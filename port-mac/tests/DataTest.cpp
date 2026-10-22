// Stage E3 test for the macOS platform layer: reads the real game data without a window.
//
//   DataTest <game folder>        (the folder that contains Data/)
//
// 1. MPQ: opens the client's base archives through SFile, lists every file of locale-<locale>.MPQ and loads each
//    one, so every decompression codec in use goes through the Mac file and thread layers.
// 2. DBC: loads DBFilesClient files and checks the WDBC header against the file size.
// 3. BLP: decodes every BLP texture of that archive to ARGB8888 at every mip level.
// Exit code 0 when everything passed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Engine/Source/BLPFile/blp.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Storm/H/storm.h"

static int32_t s_failures;

static void Check(bool ok, const char* what, const char* name) {
    if (!ok) {
        std::printf("FAIL %s: %s\n", what, name);
        s_failures++;
    }
}

static bool LoadFile(const char* name, std::vector<uint8_t>& out) {
    void* data = nullptr;
    uint32_t size = 0;

    if (!SFile::LoadFile(name, &data, &size, 0, nullptr)) {
        return false;
    }

    out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    SFile::Unload(data);
    return true;
}

static int32_t CollectName(const char* fileName, void* param) {
    static_cast<std::vector<std::string>*>(param)->push_back(fileName);
    return 1;
}

static std::vector<std::string> s_blpNames;

static void TestMpq(const char* locale) {
    const char* archives[] = { "common.MPQ", "common-2.MPQ", "expansion.MPQ", "lichking.MPQ", "patch.MPQ", "patch-2.MPQ", "patch-3.MPQ" };
    int32_t priority = 100;

    for (const char* name : archives) {
        char path[260];
        SStrPrintf(path, sizeof(path), "Data\\%s", name);
        SArchive* archive;
        Check(SFile::OpenArchive(path, priority--, 0xC00, &archive) != 0, "open archive", path);
    }

    char path[260];
    SStrPrintf(path, sizeof(path), "Data\\%s\\locale-%s.MPQ", locale, locale);
    SArchive* localeArchive = nullptr;
    Check(SFile::OpenArchive(path, 200, 0xC00, &localeArchive) != 0, "open archive", path);
    if (!localeArchive) {
        return;
    }

    std::vector<std::string> names;
    SFile::List(localeArchive, CollectName, &names);
    Check(names.size() > 1000, "list archive", path);

    uint32_t loaded = 0;
    uint64_t bytes = 0;
    for (const std::string& name : names) {
        std::vector<uint8_t> data;
        if (LoadFile(name.c_str(), data)) {
            if (name.size() > 4 && !SStrCmpI(name.c_str() + name.size() - 4, ".blp", STORM_MAX_STR)) {
                s_blpNames.push_back(name);
            }
            loaded++;
            bytes += data.size();
        } else {
            Check(false, "load", name.c_str());
        }
    }

    std::printf("mpq: %s lists %zu files, loaded %u (%llu bytes)\n", path, names.size(), loaded, static_cast<unsigned long long>(bytes));
}

static void TestDbc() {
    const char* names[] = {
        "DBFilesClient\\Spell.dbc", "DBFilesClient\\Item.dbc", "DBFilesClient\\Map.dbc", "DBFilesClient\\AreaTable.dbc",
        "DBFilesClient\\ChrRaces.dbc", "DBFilesClient\\ChrClasses.dbc", "DBFilesClient\\CreatureDisplayInfo.dbc",
    };

    for (const char* name : names) {
        std::vector<uint8_t> data;
        if (!LoadFile(name, data)) {
            Check(false, "load dbc", name);
            continue;
        }

        uint32_t header[5] = {};
        if (data.size() >= sizeof(header)) {
            std::memcpy(header, data.data(), sizeof(header));
        }

        bool valid = data.size() >= sizeof(header) && !std::memcmp(data.data(), "WDBC", 4)
            && sizeof(header) + static_cast<uint64_t>(header[1]) * header[3] + header[4] == data.size();
        Check(valid, "dbc header", name);
        std::printf("dbc: %s records=%u fields=%u recordSize=%u strings=%u\n", name, header[1], header[2], header[3], header[4]);
    }
}

static void TestBlp() {
    uint32_t decoded = 0;

    for (const std::string& blpName : s_blpNames) {
        const char* name = blpName.c_str();
        CBLPFile blp;
        if (!blp.Open(name, 0)) {
            Check(false, "open blp", name);
            continue;
        }

        uint32_t levels = 0;
        bool ok = true;
        for (uint32_t level = 0; blp.IsValidMip(level); level++, levels++) {
            uint8_t* pixels = nullptr;
            uint32_t stride = 0;
            if (!blp.Lock(PIXEL_ARGB8888, level, pixels, stride) || !pixels) {
                ok = false;
                break;
            }
            blp.Unlock(level);
        }

        Check(ok && levels > 0, "decode blp", name);
        decoded += ok && levels > 0;
        blp.Close();
    }

    std::printf("blp: decoded %u of %zu textures\n", decoded, s_blpNames.size());
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("usage: %s <game folder> [locale]\n", argv[0]);
        return 2;
    }

    const char* locale = argc > 2 ? argv[2] : "enUS";

    StormRtlInitialize();
    StormInitialize();

    SFile::DisableSFileCheckDisk();
    SFile::EnableDirectAccess(0);
    SFile::SetBasePath(argv[1]);
    SFile::SetDataPath("Data\\");

    TestMpq(locale);
    TestDbc();
    TestBlp();

    std::printf("%s (%d failures)\n", s_failures ? "FAILED" : "PASSED", s_failures);
    return s_failures ? 1 : 0;
}
