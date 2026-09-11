// Reconstructed from all four references; evidence and unresolved source names:
// analysis/lzfs_four_binary_reconstruction_2026-09-12.md
#define NCB_MODULE_NAME TJS_W("lzfs.dll")
#include "ncbind.hpp"

#include "DebugIntf.h"
#include "StorageIntf.h"
#include "UtilStreams.h"

#include <lz4.h>
#include <lz4frame.h>
#include <vector>

static_assert(LZ4_VERSION_NUMBER == 10704,
              "lzfs requires the reference LZ4 1.7.4 dependency");

namespace {
    // The references retain a pointer-sized RAII holder, but no original name.
    struct LzfsContext_guess {
        LZ4F_decompressionContext_t context;

        LzfsContext_guess() : context(nullptr) {
            LZ4F_createDecompressionContext(&context, 100);
        }

        ~LzfsContext_guess() { LZ4F_freeDecompressionContext(context); }
    };

    class LzfsMedia_guess : public iTVPStorageMedia {
        int ref;

    public:
        LzfsMedia_guess() : ref(1) {}
        ~LzfsMedia_guess() override = default;

        void AddRef() override { ++ref; }

        void Release() override {
            if(ref == 1)
                delete this;
            else
                --ref;
        }

        void GetName(ttstr &name) override { name = TJS_W("lzfs"); }
        void NormalizeDomainName(ttstr &) override {}
        void NormalizePathName(ttstr &) override {}

        bool CheckExistentStorage(const ttstr &name) override {
            return TVPIsExistentStorage(name);
        }

        tTJSBinaryStream *Open(const ttstr &name, tjs_uint32 flags) override;

        void GetListAt(const ttstr &, iTVPStorageLister *) override {}
        void GetLocallyAccessibleName(ttstr &name) override { name.Clear(); }
    };

    tTJSBinaryStream *LzfsMedia_guess::Open(const ttstr &name,
                                           tjs_uint32 flags) {
        tTJSBinaryStream *stream = TVPCreateStream(name, flags);
        if(!stream)
            TVPThrowExceptionMessage(TJS_W("Cannot open lz4 file: %1"), name);

        tjs_uint32 magic = stream->ReadI32LE();
        stream->SetPosition(0);
        if(magic == 0x184d2204) {
            size_t fileSize = stream->GetSize();
            std::vector<unsigned char> input;
            input.resize(fileSize);
            stream->ReadBuffer(input.data(), fileSize);

            LzfsContext_guess context;
            LZ4F_frameInfo_t info;
            info.contentSize = 0;
            size_t remaining = input.size();
            size_t consumed = remaining;
            size_t result = LZ4F_getFrameInfo(context.context, &info,
                                              input.data(), &consumed);
            if(LZ4F_isError(result) || consumed == 0) {
                TVPAddLog(ttstr(TJS_W("Fail to get lz4 header info: ")) + name);
                stream->SetPosition(0);
            } else {
                delete stream;
                unsigned char *source = input.data() + consumed;
                remaining -= consumed;
                consumed = remaining;
                tTVPMemoryStream *output = new tTVPMemoryStream();
                if(info.contentSize != 0) {
                    output->SetSize(info.contentSize);
                    size_t produced = info.contentSize;
                    result = LZ4F_decompress(context.context,
                                             output->GetInternalBuffer(),
                                             &produced, source, &consumed,
                                             nullptr);
                    // A positive next-input hint is accepted when both byte
                    // counts match. The references do not require result==0.
                    if(LZ4F_isError(result) || consumed != remaining ||
                       produced != info.contentSize) {
                        delete output;
                        TVPThrowExceptionMessage(TJS_W(
                            "lzfs: Decompress fail, data may be damaged."));
                        return nullptr;
                    }
                } else {
                    std::vector<unsigned char> chunk;
                    chunk.resize(128 * 1024);
                    while(result != 0) {
                        size_t produced = chunk.size();
                        result = LZ4F_decompress(context.context, chunk.data(),
                                                 &produced, source, &consumed,
                                                 nullptr);
                        if(LZ4F_isError(result)) {
                            delete output;
                            TVPThrowExceptionMessage(TJS_W(
                                "lzfs: Decompress fail, data may be damaged."));
                            return nullptr;
                        }
                        size_t advance = consumed;
                        remaining -= consumed;
                        consumed = remaining;
                        output->WriteBuffer(chunk.data(), produced);
                        source += advance;
                    }
                    // Preserve the original comparison with contentSize=0
                    // and the redundant consumed/remaining comparison.
                    if(LZ4F_isError(result) || consumed != remaining ||
                       output->GetSize() != info.contentSize) {
                        delete output;
                        TVPThrowExceptionMessage(TJS_W(
                            "lzfs: Decompress fail, data may be damaged."));
                        return nullptr;
                    }
                }
                stream = output;
            }
        }
        // Only the context and vectors have unwind guards. The original raw
        // stream lifetime is deliberately retained for exceptions above.
        return stream;
    }

    void InitLzfs_guess() {
        TVPRegisterStorageMedia(new LzfsMedia_guess());
    }

    NCB_PRE_REGIST_CALLBACK(InitLzfs_guess);
}
