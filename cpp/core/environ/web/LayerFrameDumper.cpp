#include "LayerFrameDumper.h"

#ifdef __EMSCRIPTEN__

#include <cstdio>
#include <string>

#include <emscripten.h>
#include <spdlog/spdlog.h>

#include "EventIntf.h"
#include "LayerIntf.h"
#include "WindowIntf.h"
#include "impl/DrawDevice.h"

namespace {

bool dumpLayersRequested() {
    return EM_ASM_INT({
               try {
                   if (typeof window === 'undefined') return 0;
                   const p = new URLSearchParams(window.location.search);
                   const v = p.get('dumpLayers');
                   if (v === '1' || v === 'true') return 1;
                   return 0;
               } catch (e) { return 0; }
           }) != 0;
}

std::string jsonEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string ttstrToUtf8(const ttstr &s) {
    return s.AsStdString();
}

class TVPLayerFrameDumper : public tTVPContinuousEventCallbackIntf {
public:
    void OnContinuousCallback(tjs_uint64 tick) override {
        ++Frame;
        tjs_int wcount = TVPGetWindowCount();
        for (tjs_int wi = 0; wi < wcount; ++wi) {
            tTJSNI_Window *win = TVPGetWindowListAt(wi);
            if (!win) continue;
            iTVPDrawDevice *dd = win->GetDrawDevice();
            if (!dd) continue;
            tTJSNI_BaseLayer *primary = dd->GetPrimaryLayer();
            if (!primary) continue;
            dumpLayer(primary, /*depth=*/0, /*parent=*/nullptr, wi, tick);
        }
    }

private:
    void dumpLayer(tTJSNI_BaseLayer *layer, int depth,
                   tTJSNI_BaseLayer *parent, tjs_int windowIdx,
                   tjs_uint64 tick) {
        if (!layer) return;
        const tTVPRect &r = layer->GetRect();
        const tjs_char *typeStr = layer->GetTypeNameString();
        std::string name = jsonEscape(ttstrToUtf8(layer->GetName()));
        std::string typeName = typeStr
            ? jsonEscape(ttstrToUtf8(ttstr(typeStr)))
            : std::string("unknown");

        char line[1024];
        std::snprintf(line, sizeof(line),
            "LAYER_FRAME {\"frame\":%llu,\"tick\":%llu,\"win\":%d,"
            "\"depth\":%d,\"ptr\":\"%p\",\"parent\":\"%p\","
            "\"name\":\"%s\",\"type\":\"%s\","
            "\"rect\":{\"l\":%d,\"t\":%d,\"r\":%d,\"b\":%d,\"w\":%d,\"h\":%d},"
            "\"image\":{\"l\":%d,\"t\":%d,\"w\":%u,\"h\":%u},"
            "\"opacity\":%d,\"visible\":%s,\"absIndex\":%d,\"hasImage\":%s}",
            static_cast<unsigned long long>(Frame),
            static_cast<unsigned long long>(tick),
            static_cast<int>(windowIdx),
            depth,
            static_cast<void *>(layer),
            static_cast<void *>(parent),
            name.c_str(), typeName.c_str(),
            r.left, r.top, r.right, r.bottom,
            r.get_width(), r.get_height(),
            layer->GetImageLeft(), layer->GetImageTop(),
            layer->GetImageWidth(), layer->GetImageHeight(),
            layer->GetOpacity(),
            layer->GetVisible() ? "true" : "false",
            layer->GetAbsoluteOrderIndex(),
            layer->GetHasImage() ? "true" : "false");

        // Use emscripten_log path via spdlog::info so the line ends up on
        // the browser console exactly once.
        spdlog::info("{}", line);

        tjs_uint count = layer->GetCount();
        for (tjs_uint i = 0; i < count; ++i) {
            tTJSNI_BaseLayer *child = layer->GetChildren(i);
            if (child) dumpLayer(child, depth + 1, layer, windowIdx, tick);
        }
    }

    tjs_uint64 Frame = 0;
};

TVPLayerFrameDumper *g_dumper = nullptr;

} // namespace

void TVPInstallLayerFrameDumperIfRequested() {
    if (g_dumper) return;
    if (!dumpLayersRequested()) return;
    g_dumper = new TVPLayerFrameDumper();
    TVPAddContinuousEventHook(g_dumper);
    spdlog::info("LAYER_FRAME_BEGIN dumpLayers=1 installed");
}

#endif // EMSCRIPTEN
