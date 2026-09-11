#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <lz4.h>
#include <lz4frame.h>

#include "StorageIntf.h"
#include "ncbind.hpp"
#include "test_config.h"

extern tTJS *TVPScriptEngine;
extern void TVPGetListAt(const ttstr &, iTVPStorageLister *);

namespace {
    void LoadLzfs() {
        static bool indexed = false;
        static bool loaded = false;
        if(!TVPScriptEngine)
            TVPScriptEngine = new tTJS();
        if(!indexed) {
            ncbAutoRegister::AllRegist();
            indexed = true;
        }
        if(!loaded) {
            REQUIRE(ncbAutoRegister::LoadModule(TJS_W("lzfs.dll")));
            loaded = true;
        }
    }

    std::vector<unsigned char> ReadAll(tTJSBinaryStream &stream) {
        std::vector<unsigned char> bytes(stream.GetSize());
        stream.ReadBuffer(bytes.data(), bytes.size());
        return bytes;
    }

    std::vector<unsigned char> DecodeIncrementally(
        const std::vector<unsigned char> &input) {
        LZ4F_decompressionContext_t context = nullptr;
        REQUIRE_FALSE(LZ4F_isError(LZ4F_createDecompressionContext(&context, 100)));
        struct Guard {
            LZ4F_decompressionContext_t context;
            ~Guard() { LZ4F_freeDecompressionContext(context); }
        } guard{context};
        std::vector<unsigned char> result;
        unsigned char chunk[4096];
        size_t position = 0, hint = 1;
        while(hint != 0) {
            size_t read = std::min(size_t(8191), input.size() - position);
            size_t written = sizeof(chunk);
            hint = LZ4F_decompress(context, chunk, &written,
                                   input.data() + position, &read, nullptr);
            REQUIRE_FALSE(LZ4F_isError(hint));
            REQUIRE((read != 0 || written != 0 || hint == 0));
            result.insert(result.end(), chunk, chunk + written);
            position += read;
        }
        REQUIRE(position == input.size());
        return result;
    }

    class CountLister : public iTVPStorageLister {
    public:
        int count = 0;
        void Add(const ttstr &) override { ++count; }
    };
}

TEST_CASE("lzfs registers its storage media and keeps an ordinary file intact",
          "[lzfs]") {
    REQUIRE(LZ4_versionNumber() == 10704);
    LoadLzfs();
    REQUIRE_FALSE(ncbAutoRegister::LoadModule(TJS_W("LZFS.DLL")));
    TVPSetCurrentDirectory(ttstr(TEST_FILES_PATH "/tjs2/"));
    const ttstr name(TJS_W("lzfs://./test.tjs"));
    REQUIRE(TVPIsExistentStorage(name));
    REQUIRE_FALSE(TVPIsExistentStorage(TJS_W("lzfs://./missing-lzfs-file.tjs")));
    REQUIRE(TVPGetLocallyAccessibleName(name).IsEmpty());
    CountLister lister;
    TVPGetListAt(TJS_W("lzfs://./"), &lister);
    REQUIRE(lister.count == 0);
    std::unique_ptr<tTJSBinaryStream> ordinary(
        TVPCreateStream(TJS_W("test.tjs"), TJS_BS_READ));
    std::unique_ptr<tTJSBinaryStream> wrapped(TVPCreateStream(name, TJS_BS_READ));
    REQUIRE(ordinary);
    REQUIRE(wrapped);
    REQUIRE(wrapped->GetPosition() == 0);
    REQUIRE(ReadAll(*wrapped) == ReadAll(*ordinary));
}

TEST_CASE("lzfs opens existing LZ4 assets through the storage manager", "[lzfs]") {
    const char *fixtureRoot = std::getenv("KRKR2_LZFS_FIXTURE_DIR");
    if(!fixtureRoot || !*fixtureRoot)
        SKIP("Set KRKR2_LZFS_FIXTURE_DIR to an existing extracted asset directory");
    LoadLzfs();
    size_t checked = 0;
    for(const auto &entry : std::filesystem::recursive_directory_iterator(fixtureRoot)) {
        if(!entry.is_regular_file())
            continue;
        std::ifstream file(entry.path(), std::ios::binary);
        unsigned char magic[4];
        file.read(reinterpret_cast<char *>(magic), sizeof(magic));
        if(file.gcount() != 4 || magic[0] != 4 || magic[1] != 0x22 ||
           magic[2] != 0x4d || magic[3] != 0x18)
            continue;
        file.seekg(0);
        std::vector<unsigned char> compressed{
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        CAPTURE(entry.path().string());
        std::vector<unsigned char> expected = DecodeIncrementally(compressed);
        std::string parent = entry.path().parent_path().string() + "/";
        TVPSetCurrentDirectory(ttstr(parent.c_str()));
        ttstr storage = TJS_W("lzfs://./") + ttstr(entry.path().filename().string().c_str());
        REQUIRE(TVPIsExistentStorage(storage));
        std::unique_ptr<tTJSBinaryStream> stream(TVPCreateStream(storage, TJS_BS_READ));
        REQUIRE(stream);
        REQUIRE(stream->GetPosition() == 0);
        REQUIRE(ReadAll(*stream) == expected);
        ++checked;
    }
    REQUIRE(checked > 0);
    SUCCEED("Decoded " << checked << " existing LZ4 files");
}
