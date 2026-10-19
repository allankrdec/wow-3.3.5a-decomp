# 01: Métricas e experimento de compilação com clang

## Tamanho do código

Linhas de `.cpp`, `.h` e `.c` por módulo. A tabela traz só os maiores.

| Módulo | Linhas | Observação |
|---|---:|---|
| Engine/Source/SoundEngine | 270.622 | quase tudo é o FMOD Ex em código-fonte (`fmodex/`) |
| WoW/Source/Ui | 149.801 | portável, exceto `UILCD/Windows` (~4,5 mil linhas) |
| WoW/Source/Object | 87.780 | portável |
| WoW/Source/WorldClient | 47.343 | portável |
| Engine/Source/Frame | 38.234 | portável |
| Engine/Source/BlizzardCore | 34.814 | camada de SO, com `Win32/` e `Win64/` |
| WoW/Source/DB | 24.586 | portável |
| WoW/Source/Battlenet | 24.479 | portável, exceto o loader de DLL |
| Engine/Source/Gx | 23.214 | backends D3D9 / D3D9Ex / OpenGL |
| Engine/Source/Storm | 17.548 | camada de SO, com `Win32/` e `Win64/` |
| Engine/Source/Os | 14.897 | camada de SO, quase toda em `Win32/` e `Win64/` |
| **Total Engine + WoW + Tools** | **~940.500** | |

## Código específico de plataforma

Pastas `Win64/` usadas pelo alvo x64. O alvo `Win32/` tem equivalentes de tamanho parecido.

| Pasta | Linhas |
|---|---:|
| Engine/Source/Os/Win64 | 6.845 |
| Engine/Source/Storm/Source/Win64 | 4.346 |
| Engine/Source/BlizzardCore/Source/Packages/Streaming/Win64 | 3.314 |
| Engine/Source/BlizzardCore/Source/System/File/Win64 | 1.205 |
| WoW/Source/WowServices/Win64 | 998 |
| Engine/Source/SteelseriesMouse/Win64 | 620 |
| Engine/Source/Gx/CGxDevice/Win64 | 596 |
| Engine/Source/BlizzardCore/Source/System/Thread/Win64 | 582 |
| BlizzardCore/System: URL, Time, Process, Version, Hardware, Lock, Log, Debug, Unicode (Win64) | ~1.400 |
| WoW/Source/Game/GameClient/Win64 (Warden em stub) | 114 |
| WoW/Source/Win64 (WinMain, BotCheck) | 134 |
| ComSat/Win64, Battlenet/Win64, Glue/Win64 | ~200 |
| **Total Win64 (sem o FMOD)** | **~21.600** |
| Engine/Source/SoundEngine/fmodex/win32 | 67.919 (o necessário para Mac é uma fração disso, ver doc 05) |

Fora dessas pastas, os pontos que dependem do Windows são estes:

- Os três backends Gx (`CGxDeviceD3d`, `CGxDeviceD3d9Ex`, `CGxDeviceOpenGL`), cada um cria sua própria janela Win32.
- `WoW/Source/Ui/UILCD/Windows/*` (teclado LCD da Logitech).
- `Tools/WowClient/crash_log.cpp`.
- Cerca de 43 headers que incluem `<windows.h>`. A maioria é include vestigial, por exemplo `Object.h`,
  `SpellCast.h`, `Game/Types.h` e `NetInternal.h`. O resto inclui por causa de `CRITICAL_SECTION` e `HANDLE` em
  `SCritSect.h`, `Thread.h`, `Lock.h`, `File.h` e `Time.h`.

## Contagem de idiomas MSVC

Busca em Engine, WoW e Tools, incluindo o FMOD.

| Idioma | Arquivos | Ocorrências | Impacto no macOS |
|---|---:|---:|---|
| `// OFFSET:` (funções rastreadas do binário) | 1.408 | 22.598 | nenhum, é comentário |
| Tipos Win32 (`DWORD`, `HANDLE`, `HWND`...) | 139 | 2.378 | quase tudo em `Win32/` e `Win64/` |
| `long` | 119 | 954 | baixo: quase tudo no FMOD e em libs C; o jogo usa `int32_t` |
| `wchar_t` / `WCHAR` | 34 | 702 | baixo: 0 fora de `Win*/`, UILCD e FMOD |
| `__stdcall`/`__cdecl`/`WINAPI`... | 164 | 460 | macro vazia |
| `__int64`/`__forceinline` | 41 | 383 | macro / typedef |
| intrínsecos SSE `_mm_*` | 6 | 313 | **sse2neon ou escalar** no arm64 (ver doc 07) |
| `#pragma pack` | 30 | 84 | o clang aceita igual |
| `_stricmp`/`_snprintf`/`_strdup`... | 21 | 76 | macro para `strcasecmp`/`snprintf` |
| SEH `__try`/`__except` | 15 | 38 | reescrever ou remover (só em código de plataforma) |
| `__declspec` | 13 | 33 | `__attribute__` |
| intrínsecos MSVC (`__cpuid`, `_rotl`, `_Interlocked*`) | 5 | 27 | `<cpuid.h>`/sysctl, `std::rotl`, `std::atomic` |
| `__asm` inline | 10 | 17 | todos em código x86-32 com guarda, ou `ASSERT` do FMOD (`int 3` → `__builtin_trap`) |
| `__APPLE__`/Carbon/Cocoa | 7 | 11 | só em headers de terceiros (OpenAL, ogg, FMOD) |

## Experimento: compilar no macOS com Apple clang

**Objetivo:** medir quanto do código que não é de plataforma o compilador do macOS aceita.

**Universo:** 1.046 arquivos `.cpp` de `Engine/Source` e `WoW`. Ficaram de fora as pastas
`Win32/`, `Win64/`, `win32/` e `Windows/`, o FMOD Ex e os três backends `CGxDevice{D3d,D3d9Ex,OpenGL}`.

**Comando base:**

```sh
clang++ -std=c++17 -fsyntax-only -w -DWOWDECOMP_ARCH_64=1 -DNOMINMAX -I. \
  -IThirdParty/zlib -IThirdParty/expat/lib -IThirdParty/freetype/include <arquivo>
```

| Passada | Mudança | OK | Falha |
|---|---|---:|---:|
| 1 | árvore original, sem nada | 160 | 886 |
| 2 | + shim `windows.h` mínimo (~30 typedefs), `malloc.h`→`stdlib.h`, `raw_name()`→`name()`, `KEY_NONE` 32 bits, `-fms-extensions` | 582 | 464 |
| 3 | + `-fms-compatibility` | inviável: conflita com o `va_list` do SDK do macOS | |
| 4 | passada 2 + `-Wno-c++11-narrowing` + renomear 1 parâmetro em `CDataStore.h:70` | **1000** | **46** |

**Causa das falhas da passada 1.** Quase tudo vinha de três pontos em headers muito incluídos:

1. `#include <windows.h>`: 333 arquivos.
2. `typeid(T).raw_name()`, que só existe no MSVC: 462 arquivos, via `Storm/H/stpl.h` e `Base/CDataAllocator.h`.
   Basta trocar por `name()` ou por uma macro.
3. Parâmetro de template sombreado (`template <uint32_t size>` com um parâmetro de método chamado `size`) em
   `Base/CDataStore.h:61-70`.

Também aparece estreitamento em enums com `0xFFFFFFFFFFFFFFFF` num `enum : uint32_t`, nestes pontos:
`Event/Types.h:9,139,152`, `Glue/Types.h:50` e `WorldClient/Types.h:73`. O MSVC aceita, o clang não.
Correção: `0xFFFFFFFF`.

**As 46 falhas restantes da passada 4** são todas itens de header de compatibilidade:

| Causa | Arquivos | Correção |
|---|---:|---|
| `OVERLAPPED` (I/O assíncrona em Mopaq, iounit, Streaming e File) | 17 | definir um tipo próprio na camada BlizzardCore/File, com implementação POSIX |
| `Interlocked*` | 7 | `std::atomic` / `__atomic_*` |
| `_stricmp`, `_strnicmp`, `_snprintf`, `_vsnprintf`, `_strtoi64` | 10 | macros |
| `<xmmintrin.h>` em arm64 (M2Model, M2Scene, CharacterComponent, SPIRIT x2) | 5 | sse2neon ou caminho escalar |
| `_finite`, `_isnan`, `FLT_MAX` | 4 | `std::isfinite`, `std::isnan`, `<cfloat>` |
| `OutputDebugStringA`, `_cexit`, `winerror.h`, `mbstring.h`, `tagRECT`, `HRESULT` | 6 | shim ou stub |
| `vorbis/vorbisfile.h` (caminho de include) | 1 | CMake |
| `reinterpret_cast` que remove `const` (BSNEncodeInterface) | 1 | trocar o cast |

**Limitações do teste.**

- `-fsyntax-only` não gera código, não linka e não instancia todos os templates.
- Erros de ABI e de comportamento (layout, alinhamento, ponto flutuante) não aparecem.
- Com `-w`, avisos que poderiam indicar bugs ficam escondidos.

O resultado é um bom indicador de que o código de jogo é C++17 padrão. Ele não prova que o jogo funciona.

### Reproduzir

```sh
# 1) copiar a árvore para fora do repo
rsync -a --exclude .git Engine WoW ThirdParty /tmp/wowtree/
cd /tmp/wowtree
grep -rl 'raw_name()' Engine WoW | xargs sed -i '' 's/\.raw_name()/.name()/g'
sed -i '' 's/KEY_NONE = 0xFFFFFFFFFFFFFFFF/KEY_NONE = 0xFFFFFFFF/' Engine/Source/Event/Types.h
sed -i '' '70s/uint32_t\* size,/uint32_t* size_,/' Engine/Source/Base/CDataStore.h

# 2) shim mínimo em /tmp/shim/windows.h (typedefs de DWORD, BOOL, HANDLE, HWND, CRITICAL_SECTION,
#    RECT, POINT, LARGE_INTEGER, FILETIME, MAX_PATH, INVALID_HANDLE_VALUE, WINAPI/CALLBACK vazios)
#    e /tmp/shim/malloc.h, que só faz #include <stdlib.h>

# 3) listar os arquivos e compilar
find Engine/Source WoW -name '*.cpp' | grep -vE '/(Win32|Win64|win32|Windows)/|fmodex|CGxDevice(D3d|D3d9Ex|OpenGL)' > files.txt
while read f; do
  clang++ -std=c++17 -fsyntax-only -w -fms-extensions -Wno-c++11-narrowing \
    -DWOWDECOMP_ARCH_64=1 -DNOMINMAX -I/tmp/shim -I. \
    -IThirdParty/zlib -IThirdParty/expat/lib -IThirdParty/freetype/include -IThirdParty/pcre \
    "$f" >/dev/null 2>&1 && echo "OK $f" || echo "FAIL $f"
done < files.txt | sort | uniq -c
```
