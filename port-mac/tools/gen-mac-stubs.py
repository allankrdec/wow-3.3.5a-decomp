#!/usr/bin/env python3
"""Generates Mac/ stub sources from Windows platform sources (stage E2).

For each input file (a Win64/ source, or a Windows-only common file from mac-excluded.txt) it writes the Mac
counterpart: Foo/Win64/Bar.cpp -> Foo/Mac/Bar.cpp, Foo/Bar.cpp -> Foo/Mac/Bar.cpp. The stub keeps the portable
includes, the non-static global variables and every non-static function definition, with the body replaced by
TODO_MAC(). Existing Mac/ files are never overwritten: they are the real implementation once someone edits them.

usage: port-mac/tools/gen-mac-stubs.py [--force] file.cpp ...
"""

import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

CALLCONV = r"\b(STORMAPI|__stdcall|__cdecl|__fastcall|__thiscall|WINAPI|CALLBACK|APIENTRY)\b"

_header_cache = {}


def header_available(name):
    """True if <name> resolves with the macOS SDK and the Mac include shims."""
    if name not in _header_cache:
        result = subprocess.run(
            ["clang++", "-std=c++17", "-fsyntax-only", "-x", "c++", "-I", ROOT,
             "-I", os.path.join(ROOT, "Engine/Source/Base/Platform/Mac/Include"), "-"],
            input=f"#include <{name}>\n", capture_output=True, text=True)
        _header_cache[name] = result.returncode == 0
    return _header_cache[name]


def mac_path(path):
    parts = path.split("/")
    for i, part in enumerate(parts):
        if part in ("Win64", "Win32", "Windows"):
            parts[i] = "Mac"
            return "/".join(parts)
    return "/".join(parts[:-1] + ["Mac", parts[-1]])


def strip_arch_guard(text):
    lines = text.split("\n")
    if lines and lines[0].startswith("#if defined(WOWDECOMP_ARCH_64)"):
        end = max(i for i, line in enumerate(lines) if line.startswith("#endif"))
        lines = lines[1:end]
    return "\n".join(lines)


def split_top_level(text):
    """Yields (kind, text) items: 'pre' preprocessor line, 'comment', 'decl' (ends with ;), 'def' (with body)."""
    i, n = 0, len(text)
    pending = ""
    while i < n:
        c = text[i]
        if not pending.strip() and c == "#":
            end = text.find("\n", i)
            end = n if end < 0 else end
            while text[end - 1] == "\\":
                end = text.find("\n", end + 1)
            yield "pre", text[i:end]
            i = end + 1
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            if not pending.strip():
                yield "comment", text[i:end]
            i = end + 1
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i) + 2
            i = end
            continue
        if c in "\"'":
            j = i + 1
            while text[j] != c:
                j += 2 if text[j] == "\\" else 1
            pending += text[i:j + 1]
            i = j + 1
            continue
        if c == ";":
            yield "decl", (pending + ";").strip()
            pending = ""
            i += 1
            continue
        if c == "{":
            head = pending.strip()
            if re.match(r"^(namespace\b|extern\s+\"C\")", head):
                yield "open", head
                pending = ""
                i += 1
                continue
            depth, j = 1, i + 1
            while depth:
                if text.startswith("//", j):
                    j = text.find("\n", j)
                    continue
                if text.startswith("/*", j):
                    j = text.find("*/", j) + 2
                    continue
                if text[j] in "\"'":
                    q = text[j]
                    j += 1
                    while text[j] != q:
                        j += 2 if text[j] == "\\" else 1
                elif text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                j += 1
            rest = text[j:]
            m = re.match(r"\s*;", rest)
            if m or not re.search(r"\)\s*(const\s*)?(noexcept\s*)?(override\s*)?(:.*)?$", head, re.S):
                # Aggregate initializer, class/struct/enum definition: keep as a declaration.
                end = j + (m.end() if m else 0)
                yield "decl", (pending + text[i:end]).strip()
                pending = ""
                i = end
                continue
            yield "def", head
            pending = ""
            i = j
            continue
        if c == "}":
            if pending.strip():
                yield "decl", pending.strip()
                pending = ""
            yield "close", "}"
            i += 1
            continue
        pending += c
        i += 1


def return_type(signature):
    """Return type text of a function definition head, or None for constructors/destructors."""
    head = re.sub(r"^template\s*<.*?>\s*", "", signature, flags=re.S)
    head = head.split(":")[0] if re.search(r"\)\s*:", head) else head
    depth = 0
    for k, ch in enumerate(head):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == "(" and depth == 0:
            before = head[:k].rstrip()
            break
    else:
        return None
    m = re.search(r"((?:[A-Za-z_][\w]*(?:<[^()]*>)?::)*~?[A-Za-z_]\w*|operator\s*\S+)$", before)
    ret = before[:m.start()] if m else before
    ret = re.sub(CALLCONV, "", ret)
    ret = re.sub(r"\b(inline|virtual|explicit|extern|__forceinline)\b", "", ret).strip()
    name = m.group(1) if m else ""
    if not ret:
        return None
    if re.search(r"\boperator\b", name) and "::" not in name and not ret:
        return None
    return ret



def generate(src):
    text = strip_arch_guard(open(os.path.join(ROOT, src), encoding="utf-8", errors="replace").read())
    out = [
        f"// Generated by port-mac/tools/gen-mac-stubs.py from {src}.",
        "// Stub for the macOS build: replace each TODO_MAC() with the Mac implementation.",
        "",
        '#include "Engine/Source/Base/Platform/Mac/TodoMac.h"',
    ]
    for kind, item in split_top_level(text):
        if kind == "pre":
            m = re.match(r'#include\s*([<"])([^>"]+)[>"]', item)
            if m:
                if m.group(1) == '"':
                    if not re.search(r"/(Win32|Win64|Windows)/", m.group(2)):
                        out.append(item)
                elif header_available(m.group(2)):
                    out.append(item)
            elif item.startswith("#define"):
                out.append(item)
        elif kind == "comment":
            # The address of the Windows function, without the OFFSET tag the verification harness scans for.
            m = re.match(r"//\s*OFFSET:\s*(0x[0-9A-Fa-f]+)", item)
            if m:
                out.append("")
                out.append(f"// Win64: {m.group(1)}")
        elif kind == "open":
            out.append("")
            out.append(item + " {")
        elif kind == "close":
            out.append("}")
        elif kind == "decl":
            if re.match(r"^(static\b|typedef\b|using\b|struct\b|class\b|enum\b|union\b|extern\b|#)", item):
                continue
            if "(" in item.split("=")[0] and "=" not in item:
                continue  # a function declaration
            out.append("")
            out.append(item)
        elif kind == "def":
            if re.match(r"^(template\s*<.*?>\s*)?(static|inline)\b", item, re.S):
                while out and re.match(r"// Win64:", out[-1]):
                    out.pop()
                continue
            ret = return_type(item)
            sig = item  # constructors keep their initializer list: bases may lack a default constructor
            if ret is None or re.fullmatch(r"void", ret):
                body = "    TODO_MAC();"
            else:
                body = f"    TODO_MAC();\n    return TodoMacValue<{ret}>();"
            if out and out[-1] and not out[-1].startswith("// Win64:"):
                out.append("")
            out.append(sig + " {")
            out.append(body)
            out.append("}")
    return "\n".join(re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip().split("\n")) + "\n"


def main(argv):
    force = "--force" in argv
    for src in [a for a in argv if a != "--force"]:
        src = os.path.relpath(os.path.abspath(src), ROOT)
        dst = mac_path(src)
        if os.path.exists(os.path.join(ROOT, dst)) and not force:
            print(f"skip {dst} (exists)")
            continue
        os.makedirs(os.path.dirname(os.path.join(ROOT, dst)), exist_ok=True)
        with open(os.path.join(ROOT, dst), "w") as f:
            f.write(generate(src))
        print(f"wrote {dst}")


if __name__ == "__main__":
    main(sys.argv[1:])
