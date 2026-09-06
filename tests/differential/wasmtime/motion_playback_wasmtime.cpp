// Wasmtime-only Motion playback differential glue.
//
// This file deliberately stays below the engine/platform boundary: it owns the
// exported test ABI, error buffer, framebuffer buffer, and MotionTraceWeb
// linkage symbols. Cocos, Window, FS, thread, event behavior, and differential
// trace collection must come from the normal engine sources plus
// host-provided env/WASI imports and guest-resident Wasm trace inspection.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <emscripten/emscripten.h>

#include "Application.h"
#include "EventIntf.h"
#include "LayerIntf.h"
#include "MainScene.h"
#include "RenderManager.h"
#include "SystemControl.h"
#include "WindowIntf.h"
#include "tjsError.h"
#include "motionplayer/MotionNode.h"
#include "motionplayer/MotionTraceWeb.h"
#include "motionplayer/Player.h"
#include "motionplayer/PrivateMotionGLL.h"
#include "motionplayer/RuntimeSupport.h"

void setError(const std::string &message);
void TVPWebFrameTickUpdate();

extern "C" {
int TVPWasmtimeGetContinuousEventHookSlotCount();
int TVPWasmtimeGetContinuousEventHookLiveCount();
int TVPWasmtimeGetContinuousHandlerSlotCount();
int TVPWasmtimeGetContinuousHandlerLiveCount();
int TVPWasmtimeGetContinuousHandlerAddCount();
int TVPWasmtimeGetContinuousHandlerRemoveCount();
int TVPWasmtimeGetContinuousDeliverCount();
int TVPWasmtimeGetContinuousHandlerInvokeCount();
int TVPWasmtimeGetContinuousHandlerFailureCount();
int TVPWasmtimeGetContinuousTickCount();
int TVPWasmtimeGetContinuousTickAt(int index);
}

namespace {

std::string g_error;
std::string g_stage;
std::vector<unsigned char> g_framebuffer;
int g_framebuffer_width = 0;
int g_framebuffer_height = 0;
int g_framebuffer_pitch = 0;
int g_framebuffer_format = 0;
int g_framebuffer_frame_no = 0;
std::string g_render_probe_jsonl;
std::string g_motion_trace_jsonl;
std::string g_motion_trace_frame_json;
bool g_motion_trace_first_layer = true;
int g_render_probe_seq = 0;
int g_render_draw_id = 0;
bool g_record_layer_raw_probes = false;
bool g_record_save_layer_visual_readback_probes = false;
int g_save_layer_visual_readback_frame_start = 0;
int g_save_layer_visual_readback_frame_count = 1;
int g_capture_frame_start = 0;
int g_capture_frame_count = -1;
int g_yuzulogo_frame_base = 0;
int g_m2logo_frame_base = 63;
int g_wasmtime_tick_count = 0;
int g_wasmtime_tick_diag_mask = 0;
int g_wasmtime_tick_diag_ever_mask = 0;
int g_wasmtime_window_count_peak = 0;
constexpr const char *kRenderStageCaptureRoot = "/render_stage_capture";

struct MotionLayerSample {
    int nodeType;
    int stencilType;
    motion::detail::MotionNode::AccumulatedState accumulated;
};

struct MotionPlayerSample {
    motion::Player *player;
    std::vector<MotionLayerSample> layers;
};

struct ProgressCapture {
    ProgressCapture *parent = nullptr;
    void *objthis = nullptr;
    int frameId = -1;
    int exceptionsOnEntry = 0;
    double deltaMs = std::numeric_limits<double>::quiet_NaN();
    std::vector<MotionPlayerSample> samples;
};

struct TraceState {
    ProgressCapture *progress = nullptr;
    bool inRender = false;
    int frameCounter = 0;
    int nextFrameId = 0;
    int lastCompletedFrameId = -1;
    motion::Player *lastCompletedTopPlayer = nullptr;
    void *lastCompletedRenderLayerObject = nullptr;
    bool lastCompletedRenderLayerFromAccurateSla = false;
    void *lastDrawTargetObject = nullptr;
    motion::Player *lastPostDrawLayerPlayer = nullptr;
    void *lastPostDrawLayerObject = nullptr;
    std::string lastPostDrawLayerSamplePoint;
    iTVPTexture2D *lastPostDrawCanvasTexture = nullptr;
    std::string lastPostDrawCanvasSamplePoint;
    int currentRenderFrameId = -1;
    motion::Player *currentRenderPlayer = nullptr;
    int accurateSlaRenderDepth = 0;
};

TraceState &traceState() {
    static TraceState state;
    return state;
}

std::string ptrHex(const void *ptr) {
    if(!ptr) return "null";
    std::ostringstream os;
    os << "\"0x" << std::hex
       << static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(ptr))
       << "\"";
    return os.str();
}

void appendJsonString(std::string &out, const std::string &value) {
    out.push_back('"');
    for(char ch : value) {
        switch(ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if(static_cast<unsigned char>(ch) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    out.push_back('"');
}

void appendJsonBoolOrNull(std::string &out, bool known, bool value) {
    if(!known) {
        out += "null";
        return;
    }
    out += value ? "true" : "false";
}

void appendJsonDouble(std::string &out, double value) {
    if(!std::isfinite(value)) {
        out += "null";
        return;
    }
    std::ostringstream os;
    os << std::setprecision(17) << value;
    out += os.str();
}

template <typename T, size_t N>
void appendNumberArray(std::string &out, const std::array<T, N> &values) {
    out.push_back('[');
    for(size_t i = 0; i < N; ++i) {
        if(i) out.push_back(',');
        if(!std::isfinite(static_cast<double>(values[i]))) {
            out += "null";
            continue;
        }
        std::ostringstream os;
        os << std::setprecision(9) << values[i];
        out += os.str();
    }
    out.push_back(']');
}

std::uint32_t rotr32(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

class Sha256 {
public:
    void update(const unsigned char *data, std::size_t len) {
        _totalBytes += len;
        if(_bufferSize != 0) {
            const auto take = std::min<std::size_t>(64 - _bufferSize, len);
            std::memcpy(_buffer + _bufferSize, data, take);
            _bufferSize += take;
            data += take;
            len -= take;
            if(_bufferSize == 64) {
                transform(_buffer);
                _bufferSize = 0;
            }
        }
        while(len >= 64) {
            transform(data);
            data += 64;
            len -= 64;
        }
        if(len > 0) {
            std::memcpy(_buffer, data, len);
            _bufferSize = len;
        }
    }

    std::string finalHex() {
        const std::uint64_t bitLen = _totalBytes * 8u;
        _buffer[_bufferSize++] = 0x80u;
        if(_bufferSize > 56) {
            while(_bufferSize < 64) _buffer[_bufferSize++] = 0;
            transform(_buffer);
            _bufferSize = 0;
        }
        while(_bufferSize < 56) _buffer[_bufferSize++] = 0;
        for(int i = 7; i >= 0; --i) {
            _buffer[_bufferSize++] =
                static_cast<unsigned char>((bitLen >> (i * 8)) & 0xffu);
        }
        transform(_buffer);

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for(std::uint32_t word : _state) {
            for(int shift = 24; shift >= 0; shift -= 8) {
                const auto byte =
                    static_cast<unsigned char>((word >> shift) & 0xffu);
                out.push_back(kHex[byte >> 4]);
                out.push_back(kHex[byte & 0x0f]);
            }
        }
        return out;
    }

private:
    void transform(const unsigned char block[64]) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        std::uint32_t m[64];
        for(int i = 0; i < 16; ++i) {
            m[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for(int i = 16; i < 64; ++i) {
            const auto s0 = rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18) ^
                (m[i - 15] >> 3);
            const auto s1 = rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19) ^
                (m[i - 2] >> 10);
            m[i] = m[i - 16] + s0 + m[i - 7] + s1;
        }

        std::uint32_t a = _state[0], b = _state[1], c = _state[2],
            d = _state[3], e = _state[4], f = _state[5],
            g = _state[6], h = _state[7];
        for(int i = 0; i < 64; ++i) {
            const auto s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + ch + k[i] + m[i];
            const auto s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        _state[0] += a;
        _state[1] += b;
        _state[2] += c;
        _state[3] += d;
        _state[4] += e;
        _state[5] += f;
        _state[6] += g;
        _state[7] += h;
    }

    std::uint32_t _state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    unsigned char _buffer[64] = {};
    std::size_t _bufferSize = 0;
    std::uint64_t _totalBytes = 0;
};

std::string rgbaSha256FromBgraRows(const unsigned char *pixels, int width,
                                   int height, int pitch) {
    Sha256 sha;
    std::vector<unsigned char> row(static_cast<std::size_t>(width) * 4u);
    for(int y = 0; y < height; ++y) {
        const auto *src = pixels + static_cast<std::size_t>(y) *
            static_cast<std::size_t>(pitch);
        for(int x = 0; x < width; ++x) {
            const auto dst = static_cast<std::size_t>(x) * 4u;
            row[dst] = src[x * 4 + 2];
            row[dst + 1u] = src[x * 4 + 1];
            row[dst + 2u] = src[x * 4];
            row[dst + 3u] = src[x * 4 + 3];
        }
        sha.update(row.data(), row.size());
    }
    return sha.finalHex();
}

std::string sha256Bytes(const unsigned char *data, std::size_t len) {
    Sha256 sha;
    sha.update(data, len);
    return sha.finalHex();
}

int renderFrameIdFor(motion::Player *player) {
    auto &state = traceState();
    if(state.inRender && state.currentRenderFrameId >= 0) {
        return state.currentRenderFrameId;
    }
    if(state.lastCompletedFrameId < 0) {
        return -1;
    }
    if(!state.lastCompletedTopPlayer || !player ||
       state.lastCompletedTopPlayer == player) {
        return state.lastCompletedFrameId;
    }
    return -1;
}

bool captureFrameEnabled(int frameId) {
    if(frameId < 0) return false;
    const int start = std::max(0, g_capture_frame_start);
    if(frameId < start) return false;
    return g_capture_frame_count < 0 || frameId < start + g_capture_frame_count;
}

motion::Player *renderPlayerFor(motion::Player *player) {
    auto &state = traceState();
    if(player) return player;
    if(state.currentRenderPlayer) return state.currentRenderPlayer;
    return state.lastCompletedTopPlayer;
}

bool isCurrentRenderPlayer(motion::Player *player) {
    auto &state = traceState();
    return state.inRender && player && state.currentRenderPlayer == player;
}

void *variantLayerObjectAt(tjs_int numparams, tTJSVariant **param,
                           tjs_int index) {
    if(index < 0 || index >= numparams || !param || !param[index] ||
       param[index]->Type() != tvtObject) {
        return nullptr;
    }
    auto *object = param[index]->AsObjectNoAddRef();
    if(!object) return nullptr;
    tTJSNI_BaseLayer *layer = nullptr;
    if(TJS_FAILED(object->NativeInstanceSupport(
           TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
           reinterpret_cast<iTJSNativeInstance **>(&layer))) || !layer) {
        return nullptr;
    }
    return object;
}

void appendRenderEventForFrame(int frameId,
                               motion::Player *player,
                               const char *stage,
                               const char *kind,
                               const char *samplePoint,
                               const std::string &payload,
                               const std::string &diagnostics) {
    if(!captureFrameEnabled(frameId)) return;
    motion::Player *eventPlayer = renderPlayerFor(player);
    std::string ev;
    ev.reserve(payload.size() + diagnostics.size() + 256);
    ev += "{\"schema\":\"motion-render-stage-wasmtime-v1-event\"";
    ev += ",\"source\":\"wasmtime-port-render-stage\"";
    ev += ",\"captureContract\":\"motion-render-boundaries-v1\"";
    ev += ",\"stage\":\"";
    ev += stage;
    ev += "\",\"kind\":\"";
    ev += kind;
    ev += "\",\"samplePoint\":\"";
    ev += samplePoint;
    ev += "\",\"frameId\":";
    ev += std::to_string(frameId);
    ev += ",\"player\":";
    ev += ptrHex(eventPlayer);
    ev += ",\"seq\":";
    ev += std::to_string(g_render_probe_seq++);
    if(!payload.empty()) {
        ev.push_back(',');
        ev += payload;
    }
    ev += ",\"diagnostics\":";
    ev += diagnostics.empty() ? "{}" : diagnostics;
    ev += "}\n";
    g_render_probe_jsonl += ev;
}

void appendRenderEvent(motion::Player *player,
                       const char *stage,
                       const char *kind,
                       const char *samplePoint,
                       const std::string &payload,
                       const std::string &diagnostics) {
    appendRenderEventForFrame(renderFrameIdFor(player), player, stage, kind,
                              samplePoint, payload, diagnostics);
}

std::string activeMotionPath(const motion::Player *player) {
    return player ? player->matchedMotionPath() : std::string{};
}

using PreparedItemList = motion::detail::PreparedRenderItemList;

int preparedIndexFor(const PreparedItemList *mainList,
                     const PreparedItemList *auxList,
                     const motion::detail::PreparedRenderItem *item) {
    if(!item) return -1;
    if(mainList) {
        const auto found = std::find(mainList->begin(), mainList->end(), item);
        if(found != mainList->end()) {
            return static_cast<int>(std::distance(mainList->begin(), found));
        }
    }
    if(auxList) {
        const auto found = std::find(auxList->begin(), auxList->end(), item);
        if(found != auxList->end()) {
            const auto mainCount = mainList ? mainList->size() : 0;
            return static_cast<int>(
                mainCount + std::distance(auxList->begin(), found));
        }
    }
    return -1;
}

void appendPreparedItemJson(
    std::string &out,
    const motion::detail::PreparedRenderItem &item,
    size_t index,
    bool prependComma,
    const PreparedItemList *mainList,
    const PreparedItemList *auxList) {
    if(prependComma) out.push_back(',');
    out += "{\"index\":";
    out += std::to_string(index);
    out += ",\"nodeIndex\":";
    out += std::to_string(item.nodeIndex);
    out += ",\"sourceKey\":";
    // PreparedRenderItem now preserves the reference layout's borrowed
    // SourceState pointer instead of duplicating its path into a Web-only
    // std::string.  Keep the trace schema stable while sampling the actual
    // render-time source owner.
    appendJsonString(
        out,
        item.sourceState ? item.sourceState->path.AsStdString()
                         : std::string{});
    out += ",\"flags\":{\"flag16\":";
    out += item.rawFlag16 ? "1" : "0";
    out += ",\"flag17\":";
    out += item.skipFlag0 ? "1" : "0";
    out += ",\"flag18\":";
    out += item.skipFlag1 ? "1" : "0";
    out += ",\"drawFlag19\":";
    out += item.drawFlag ? "1" : "0";
    out += ",\"layerResolved20\":";
    out += item.rawFlag20 ? "1" : "0";
    out += ",\"clipValid21\":";
    out += item.rawFlag21 ? "1" : "0";
    out += "},\"layerIds\":{\"primary\":";
    out += std::to_string(item.layerId1);
    out += ",\"secondary\":";
    out += std::to_string(item.layerId2);
    out += "},\"sortKey64\":";
    out += std::to_string(item.sortKey);
    out += ",\"paintBox\":";
    appendNumberArray(out, item.paintBox);
    out += ",\"corners\":";
    appendNumberArray(out, item.corners);
    out += ",\"clipRect\":";
    appendNumberArray(out, item.clipRect);
    out += ",\"buildClipRect\":";
    appendNumberArray(out, item.clipRect);
    out += ",\"dirtyRect\":";
    appendNumberArray(out, item.dirtyRect);
    out += ",\"localCorners\":";
    // Diagnostic-only projection. Native keeps these values as call-local Real
    // arguments and has no persistent item field for them.
    std::array<float, 8> localCorners{};
    for(std::size_t ci = 0; ci < item.corners.size(); ci += 2) {
        localCorners[ci] =
            item.corners[ci] - 0.5f - item.clipRect[0];
        localCorners[ci + 1] =
            item.corners[ci + 1] - 0.5f - item.clipRect[1];
    }
    appendNumberArray(out, localCorners);
    out += ",\"viewportRect\":";
    appendNumberArray(out, item.viewport);
    out += ",\"sourceGate232\":";
    out += std::to_string(item.opacity);
    out += ",\"packedColors\":[";
    for(size_t ci = 0; ci < item.packedColors.size(); ++ci) {
        if(ci) out.push_back(',');
        out += std::to_string(item.packedColors[ci]);
    }
    out += "]";
    out += ",\"stencilType244\":";
    out += std::to_string(item.stencilComposite);
    out += ",\"parentItemIndex\":";
    out += std::to_string(
        preparedIndexFor(mainList, auxList, item.parentItem));
    out += ",\"childItemCount\":";
    out += std::to_string(item.childItems.size());
    out += ",\"meshType280\":";
    out += std::to_string(item.meshType);
    out += ",\"leafLayerVariantTag\":";
    out += std::to_string(static_cast<int>(item.leafLayer.Type()));
    out += ",\"composedLayerVariantTag\":";
    out += std::to_string(static_cast<int>(item.composedLayer.Type()));
    out += ",\"leafBuilt\":";
    out += item.leafLayer.Type() != tvtVoid ? "true" : "false";
    out += ",\"composedBuilt\":";
    out += item.composedLayer.Type() != tvtVoid ? "true" : "false";
    // V158 removed the non-native persistent route marker. The route is only
    // observable in the scoped execute trace, not from a later item snapshot.
    out += ",\"executedDirect\":null";
    out += "}";
}

void appendPreparedItemList(
    std::string &out,
    const char *name,
    const PreparedItemList *items,
    const PreparedItemList *mainList,
    const PreparedItemList *auxList) {
    constexpr size_t kLimit = 256;
    out += ",\"";
    out += name;
    out += "\":[";
    if(items) {
        size_t emitted = 0;
        for(size_t i = 0; i < items->size(); ++i) {
            const auto *item = (*items)[i];
            if(!item) continue;
            if(emitted >= kLimit) {
                break;
            }
            appendPreparedItemJson(
                out, *item, i, emitted > 0, mainList, auxList);
            ++emitted;
        }
    }
    out.push_back(']');
}

void appendRenderListHeader(std::string &out,
                            const void *vectorPtr,
                            const void *beginPtr,
                            const void *endPtr,
                            size_t count) {
    out += "{\"vector\":";
    out += ptrHex(vectorPtr);
    out += ",\"begin\":";
    out += ptrHex(beginPtr);
    out += ",\"end\":";
    out += ptrHex(endPtr);
    out += ",\"count\":";
    out += std::to_string(count);
    out += ",\"items\":[";
}

void appendPreparedPointerRenderList(
    std::string &out,
    const PreparedItemList *items,
    const PreparedItemList *mainList,
    const PreparedItemList *auxList) {
    constexpr size_t kLimit = 256;
    const size_t count = items ? items->size() : 0;
    const void *beginPtr = count ? static_cast<const void *>(items->data()) : nullptr;
    const void *endPtr = count
        ? static_cast<const void *>(items->data() + items->size())
        : nullptr;
    appendRenderListHeader(out, items, beginPtr, endPtr, count);
    if(items) {
        const size_t n = std::min(count, kLimit);
        size_t emitted = 0;
        for(size_t i = 0; i < n; ++i) {
            const auto *item = (*items)[i];
            if(!item) continue;
            // Oracle list indices are local to this exact main/aux vector;
            // combined diagnostic identity must not leak into sorted main.
            appendPreparedItemJson(
                out, *item, i, emitted > 0, mainList, auxList);
            ++emitted;
        }
        out.push_back(']');
        if(count > n) {
            out += ",\"itemsTruncated\":";
            out += std::to_string(count - n);
        }
        out.push_back('}');
        return;
    }
    out += "]}";
}

void appendPreparedRenderListsPayload(
    std::string &out,
    const PreparedItemList *mainList,
    const PreparedItemList *auxList,
    bool includeAuxList = true) {
    out += "\"renderLists\":{\"mainList\":";
    appendPreparedPointerRenderList(out, mainList, mainList, auxList);
    out += ",\"auxList\":";
    if(includeAuxList) {
        appendPreparedPointerRenderList(out, auxList, mainList, auxList);
    } else {
        out += "null";
    }
    out.push_back('}');
}

void appendCommandRenderListsPayload(
    std::string &out,
    const PreparedItemList *mainList,
    const PreparedItemList *auxList) {
    out += "\"renderLists\":{\"mainList\":";
    appendPreparedPointerRenderList(out, mainList, mainList, auxList);
    out += ",\"auxList\":";
    appendPreparedPointerRenderList(out, auxList, mainList, auxList);
    out.push_back('}');
}

void appendRenderItemsPayload(std::string &out,
                              const PreparedItemList *mainList,
                              const PreparedItemList *auxList) {
    const size_t mainCount = mainList ? mainList->size() : 0;
    const size_t auxCount = auxList ? auxList->size() : 0;
    const size_t preparedCount = mainCount + auxCount;
    size_t validDrawableCount = 0;
    size_t leafBuiltCount = 0;
    size_t composedBuiltCount = 0;
    auto hasValidClipRect = [](const std::array<float, 4> &rect) {
        return rect[0] < rect[2] && rect[1] < rect[3];
    };
    auto hasValidPaintOrViewportRect =
        [](const motion::detail::PreparedRenderItem &item) {
        if(item.rawFlag16 || item.skipFlag0 || item.opacity <= 0) return false;
        float left = item.paintBox[0];
        float top = item.paintBox[1];
        float right = item.paintBox[2];
        float bottom = item.paintBox[3];
        if(item.hasViewport && item.viewport[2] >= item.viewport[0] &&
           item.viewport[3] >= item.viewport[1]) {
            left = std::max(left, std::floor(item.viewport[0]));
            top = std::max(top, std::floor(item.viewport[1]));
            right = std::min(right, std::ceil(item.viewport[2]));
            bottom = std::min(bottom, std::ceil(item.viewport[3]));
        }
        return left < right && top < bottom;
    };
    if(mainList) {
        for(const auto *itemPtr : *mainList) {
            if(!itemPtr) continue;
            const auto &item = *itemPtr;
            if((item.rawFlag21 && hasValidClipRect(item.clipRect)) ||
               (!item.rawFlag21 && hasValidPaintOrViewportRect(item))) {
                ++validDrawableCount;
            }
            if(item.leafLayer.Type() != tvtVoid) ++leafBuiltCount;
        }
    }
    if(auxList) {
        for(const auto *item : *auxList) {
            if(item && item->composedLayer.Type() != tvtVoid) {
                ++composedBuiltCount;
            }
        }
    }
    out += "\"preparedItemCount\":";
    out += std::to_string(preparedCount);
    out += ",\"inputItemCount\":";
    out += std::to_string(mainCount);
    out += ",\"builtItemCount\":";
    out += std::to_string(mainCount);
    out += ",\"validDrawableItemCount\":";
    out += std::to_string(validDrawableCount);
    out += ",\"leafBuiltCount\":";
    out += std::to_string(leafBuiltCount);
    out += ",\"composedBuiltCount\":";
    out += std::to_string(composedBuiltCount);
    appendPreparedItemList(
        out, "mainListSemanticItems", mainList, mainList, auxList);
    appendPreparedItemList(
        out, "auxListSemanticItems", auxList, mainList, auxList);
}

std::string playerDiagnostics(motion::Player *player) {
    std::string diag = "{\"player\":";
    diag += ptrHex(player);
    diag += ",\"activeMotion\":";
    appendJsonString(diag, activeMotionPath(player));
    diag += ",\"samplingMode\":\"guest-cpp-probe\"}";
    return diag;
}

bool directoryExists(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool fileExists(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string framePath(const char *phase, int frameId,
                      const char *rawSuffix = "bgra") {
    char name[128];
    std::snprintf(name, sizeof(name), "%s/_execute/%s/frame_%04d.%s",
                  kRenderStageCaptureRoot, phase ? phase : "unknown",
                  frameId, rawSuffix ? rawSuffix : "raw");
    return std::string(name);
}

void appendImageCheckpointEvent(motion::Player *player, const char *phase,
                                const char *samplePoint, bool ok,
                                const std::string &path,
                                const std::string &error,
                                int width = 0,
                                int height = 0,
                                int pitch = 0,
                                const std::string &diagnostics = {},
                                int frameIdOverride = -1,
                                const char *pixelFormat = "bgra32") {
    std::string payload = "\"phase\":";
    appendJsonString(payload, phase ? phase : "");
    payload += ",\"ok\":";
    payload += ok ? "true" : "false";
    if(width > 0) {
        payload += ",\"width\":";
        payload += std::to_string(width);
    }
    if(height > 0) {
        payload += ",\"height\":";
        payload += std::to_string(height);
    }
    if(pitch > 0) {
        payload += ",\"pitch\":";
        payload += std::to_string(pitch);
    }
    payload += ",\"pixelFormat\":";
    appendJsonString(payload, pixelFormat ? pixelFormat : "bgra32");
    if(!path.empty()) {
        payload += ",\"guestPath\":";
        appendJsonString(payload, path);
    }
    if(!error.empty()) {
        payload += ",\"error\":";
        appendJsonString(payload, error);
    }
    const std::string diag =
        diagnostics.empty() ? playerDiagnostics(player) : diagnostics;
    if(frameIdOverride >= 0) {
        appendRenderEventForFrame(
            frameIdOverride, player, "render_execute",
            "execute_image_checkpoint",
            samplePoint ? samplePoint : "execute_image_checkpoint",
            payload, diag);
    } else {
        appendRenderEvent(
            player, "render_execute", "execute_image_checkpoint",
            samplePoint ? samplePoint : "execute_image_checkpoint",
            payload, diag);
    }
}

std::string checkpointDiagnostics(motion::Player *player,
                                  const char *captureMethod,
                                  const void *mainImage,
                                  int rowBytes,
                                  int failedRow = -1) {
    std::string diag = playerDiagnostics(player);
    if(!diag.empty() && diag.back() == '}') {
        diag.pop_back();
    }
    diag += ",\"captureMethod\":";
    appendJsonString(diag, captureMethod ? captureMethod : "");
    diag += ",\"mainImage\":";
    diag += ptrHex(mainImage);
    if(rowBytes > 0) {
        diag += ",\"rowBytes\":";
        diag += std::to_string(rowBytes);
    }
    if(failedRow >= 0) {
        diag += ",\"failedRow\":";
        diag += std::to_string(failedRow);
    }
    diag.push_back('}');
    return diag;
}

tTJSNI_BaseLayer *resolveTraceLayerLikeNative(iTJSDispatch2 *layerObject) {
    if(!layerObject) return nullptr;
    tTJSNI_BaseLayer *layer = nullptr;
    if(TJS_SUCCEEDED(layerObject->NativeInstanceSupport(
           TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
           reinterpret_cast<iTJSNativeInstance **>(&layer))) && layer) {
        return layer;
    }
    return motion::resolvePrivateMotionGLLNative_guess(layerObject);
}

bool writePackedRowsFromScanLines(const std::string &path,
                                  const tTVPBaseTexture *mainImage,
                                  int width,
                                  int height,
                                  int *failedRow) {
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if(!file) return false;
    bool ok = true;
    const auto rowBytes = static_cast<std::size_t>(width) * 4u;
    for(int y = 0; y < height; ++y) {
        const auto *row = static_cast<const unsigned char *>(
            mainImage->GetScanLine(static_cast<tjs_uint>(y)));
        if(!row) {
            if(failedRow) *failedRow = y;
            ok = false;
            break;
        }
        if(std::fwrite(row, 1, rowBytes, file) != rowBytes) {
            if(failedRow) *failedRow = y;
            ok = false;
            break;
        }
    }
    if(std::fclose(file) != 0) ok = false;
    return ok;
}

bool writePackedRowsFromTexture(const std::string &path,
                                iTVPTexture2D *texture,
                                int width,
                                int height,
                                int *failedRow) {
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if(!file) return false;
    bool ok = true;
    const auto rowBytes = static_cast<std::size_t>(width) * 4u;
    for(int y = 0; y < height; ++y) {
        const auto *row = static_cast<const unsigned char *>(
            texture->GetScanLineForRead(static_cast<tjs_uint>(y)));
        if(!row) {
            if(failedRow) *failedRow = y;
            ok = false;
            break;
        }
        if(std::fwrite(row, 1, rowBytes, file) != rowBytes) {
            if(failedRow) *failedRow = y;
            ok = false;
            break;
        }
    }
    if(std::fclose(file) != 0) ok = false;
    return ok;
}

std::string canvasTextureDiagnostics(const char *captureMethod,
                                     iTVPTexture2D *texture,
                                     const char *recordSamplePoint,
                                     int rowBytes,
                                     int sourcePitch,
                                     int failedRow = -1) {
    std::string diag = "{\"player\":null,\"activeMotion\":\"\"";
    diag += ",\"samplingMode\":\"guest-cpp-probe\"";
    diag += ",\"captureMethod\":";
    appendJsonString(diag, captureMethod ? captureMethod : "");
    diag += ",\"texture\":";
    diag += ptrHex(texture);
    diag += ",\"recordSamplePoint\":";
    appendJsonString(diag, recordSamplePoint ? recordSamplePoint : "");
    diag += ",\"rowBytes\":";
    diag += std::to_string(rowBytes);
    diag += ",\"sourcePitch\":";
    diag += std::to_string(sourcePitch);
    if(texture) {
        diag += ",\"format\":";
        diag += std::to_string(static_cast<int>(texture->GetFormat()));
    }
    if(failedRow >= 0) {
        diag += ",\"failedRow\":";
        diag += std::to_string(failedRow);
    }
    diag.push_back('}');
    return diag;
}

iTJSDispatch2 *resolveCheckpointLayerObject(void *object,
                                            tTJSVariant &propertyStorage) {
    propertyStorage.Clear();
    if(!object) return nullptr;
    auto *dispatch = static_cast<iTJSDispatch2 *>(object);
    if(resolveTraceLayerLikeNative(dispatch)) {
        return dispatch;
    }
    if(TJS_SUCCEEDED(dispatch->PropGet(
           0, TJS_W("targetLayer"), nullptr, &propertyStorage, dispatch)) &&
       propertyStorage.Type() == tvtObject) {
        auto *candidate = propertyStorage.AsObjectNoAddRef();
        if(resolveTraceLayerLikeNative(candidate)) {
            return candidate;
        }
    }
    propertyStorage.Clear();
    if(TJS_SUCCEEDED(dispatch->FuncCall(
           0x1000, TJS_W("layerTreeOwnerInterface"), nullptr,
           &propertyStorage, 0, nullptr, dispatch)) &&
       propertyStorage.Type() == tvtObject) {
        auto *candidate = propertyStorage.AsObjectNoAddRef();
        if(resolveTraceLayerLikeNative(candidate)) {
            return candidate;
        }
    }
    return nullptr;
}

void motionTraceRenderAccurateSlaExecutePostLayerCheckpoint(
    motion::Player *player, void *renderLayerObject, const char *samplePoint) {
    const int frameId = renderFrameIdFor(player);
    if(frameId < 0 || !captureFrameEnabled(frameId)) return;
    const char *phase = "execute_post";
    const std::string phaseDir =
        std::string(kRenderStageCaptureRoot) + "/_execute/" + phase;
    if(!directoryExists(phaseDir)) return;

    auto &state = traceState();
    void *candidateObject =
        state.lastDrawTargetObject ? state.lastDrawTargetObject
                                   : renderLayerObject;
    tTJSVariant propertyStorage;
    auto *layerObject =
        resolveCheckpointLayerObject(candidateObject, propertyStorage);
    if(!layerObject && candidateObject != renderLayerObject) {
        layerObject =
            resolveCheckpointLayerObject(renderLayerObject, propertyStorage);
    }

    auto diagnostics = [&](const char *captureMethod,
                           void *sourceObject,
                           iTJSDispatch2 *resolvedObject,
                           tTJSNI_BaseLayer *nativeLayer,
                           const void *image,
                           int rowBytes,
                           int sourcePitch,
                           int failedRow = -1) {
        std::string diag = "{\"player\":";
        diag += ptrHex(player);
        diag += ",\"activeMotion\":";
        appendJsonString(diag, activeMotionPath(player));
        diag += ",\"samplingMode\":\"guest-cpp-probe\"";
        diag += ",\"captureMethod\":";
        appendJsonString(diag, captureMethod ? captureMethod : "");
        diag += ",\"sourceObject\":";
        diag += ptrHex(sourceObject);
        diag += ",\"resolvedLayerObject\":";
        diag += ptrHex(resolvedObject);
        diag += ",\"nativeLayer\":";
        diag += ptrHex(nativeLayer);
        diag += ",\"image\":";
        diag += ptrHex(image);
        diag += ",\"rowBytes\":";
        diag += std::to_string(rowBytes);
        diag += ",\"sourcePitch\":";
        diag += std::to_string(sourcePitch);
        if(failedRow >= 0) {
            diag += ",\"failedRow\":";
            diag += std::to_string(failedRow);
        }
        diag.push_back('}');
        return diag;
    };
    auto fail = [&](const std::string &message,
                    iTJSDispatch2 *resolvedObject = nullptr,
                    tTJSNI_BaseLayer *nativeLayer = nullptr,
                    const void *image = nullptr,
                    int width = 0,
                    int height = 0,
                    int pitch = 0,
                    int sourcePitch = 0) {
        appendImageCheckpointEvent(
            player, phase, samplePoint, false, {}, message,
            width, height, pitch,
            diagnostics("target-layer-main-image.execute-post",
                        candidateObject, resolvedObject, nativeLayer,
                        image, pitch, sourcePitch),
            frameId, "rgba32");
    };

    if(!layerObject) {
        fail("accurate SLA execute_post source did not resolve to a Layer");
        return;
    }
    auto *nativeLayer = resolveTraceLayerLikeNative(layerObject);
    if(!nativeLayer) {
        fail("accurate SLA execute_post resolved object has no Layer native instance",
             layerObject);
        return;
    }
    auto *manager = nativeLayer->GetManager();
    if(!manager) {
        fail("accurate SLA execute_post Layer has no LayerManager",
             layerObject, nativeLayer);
        return;
    }
    struct CallOnPaintSuppressor {
        tTJSNI_BaseLayer *layer = nullptr;
        bool saved = false;
        explicit CallOnPaintSuppressor(tTJSNI_BaseLayer *l) : layer(l) {
            if(layer) {
                saved = layer->GetCallOnPaint();
                layer->SetCallOnPaint(false);
            }
        }
        ~CallOnPaintSuppressor() {
            if(layer) {
                layer->SetCallOnPaint(saved);
            }
        }
    } suppressOnPaint(nativeLayer);
    manager->UpdateToDrawDevice();

    auto *drawBuffer = manager->GetDrawBuffer();
    if(!drawBuffer) {
        fail("accurate SLA execute_post draw buffer is null",
             layerObject, nativeLayer);
        return;
    }
    auto *texture = drawBuffer->GetTexture();
    if(!texture) {
        fail("accurate SLA execute_post draw buffer texture is null",
             layerObject, nativeLayer, drawBuffer);
        return;
    }

    const int width = static_cast<int>(texture->GetWidth());
    const int height = static_cast<int>(texture->GetHeight());
    const int sourcePitch = static_cast<int>(texture->GetPitch());
    const int rowBytes = width * 4;
    if(width != 1920 || height != 1080 || sourcePitch < rowBytes) {
        fail("accurate SLA execute_post texture has invalid dimensions",
             layerObject, nativeLayer, texture, width, height, rowBytes,
             sourcePitch);
        return;
    }

    const auto path = framePath(phase, frameId, "rgba");
    int failedRow = -1;
    const bool ok = writePackedRowsFromTexture(
        path, texture, width, height, &failedRow);
    appendImageCheckpointEvent(
        player, phase, samplePoint, ok, path,
        ok ? std::string()
           : std::string("failed to write accurate SLA execute_post draw buffer checkpoint"),
        width, height, rowBytes,
        diagnostics("LayerManager.UpdateToDrawDevice.execute-post.no-onPaint",
                    candidateObject, layerObject, nativeLayer, texture,
                    rowBytes, sourcePitch, failedRow),
        frameId, "rgba32");
}

void appendLayerRawProbeEvent(motion::Player *player,
                              const char *samplePoint,
                              const tTJSNI_BaseLayer *layer,
                              bool ok,
                              const std::string &error,
                              const std::string &rgbaSha256,
                              int width,
                              int height,
                              int pitch,
                              const void *mainImage,
                              const void *bitmapImpl,
                              const void *buffer) {
    const int frameId = renderFrameIdFor(player);
    if(frameId < 0) return;
    motion::Player *eventPlayer = renderPlayerFor(player);
    const int seq = g_render_probe_seq++;
    std::string ev;
    ev += "{\"schema\":\"motion-render-stage-wasmtime-v1-event\"";
    ev += ",\"source\":\"wasmtime-port-layer-main-image-raw-probe\"";
    ev += ",\"stage\":\"layer_raw_probe\"";
    ev += ",\"kind\":\"raw_probe\"";
    ev += ",\"samplePoint\":";
    appendJsonString(ev, samplePoint ? samplePoint : "layer_raw_probe");
    ev += ",\"frameId\":";
    if(frameId >= 0) {
        ev += std::to_string(frameId);
    } else {
        ev += "null";
    }
    ev += ",\"player\":";
    ev += ptrHex(eventPlayer);
    ev += ",\"seq\":";
    ev += std::to_string(seq);
    ev += ",\"ok\":";
    ev += ok ? "true" : "false";
    if(width > 0) {
        ev += ",\"width\":";
        ev += std::to_string(width);
    }
    if(height > 0) {
        ev += ",\"height\":";
        ev += std::to_string(height);
    }
    if(pitch > 0) {
        ev += ",\"pitch\":";
        ev += std::to_string(pitch);
    }
    ev += ",\"pixelFormat\":\"bgra32\"";
    if(!rgbaSha256.empty()) {
        ev += ",\"rgbaSha256\":";
        appendJsonString(ev, rgbaSha256);
    }
    if(!error.empty()) {
        ev += ",\"error\":";
        appendJsonString(ev, error);
    }
    ev += ",\"nativeLayer\":";
    ev += ptrHex(layer);
    ev += ",\"mainImage\":";
    ev += ptrHex(mainImage);
    ev += ",\"bitmapImpl\":";
    ev += ptrHex(bitmapImpl);
    ev += ",\"buffer\":";
    ev += ptrHex(buffer);
    ev += ",\"diagnostics\":{\"samplingMode\":\"guest-cpp-raw-no-sync\"";
    ev += ",\"nativeLayer\":";
    ev += ptrHex(layer);
    ev += ",\"mainImage\":";
    ev += ptrHex(mainImage);
    ev += ",\"bitmapImpl\":";
    ev += ptrHex(bitmapImpl);
    ev += ",\"buffer\":";
    ev += ptrHex(buffer);
    ev += "}}\n";
    g_render_probe_jsonl += ev;
}

extern "C" __attribute__((noinline, used))
void krkr2_wasm_motion_frame_begin(std::int32_t frameId,
                                   const void *objthis,
                                   const motion::Player *topPlayer,
                                   std::int32_t playerCount) {
    g_motion_trace_frame_json = "{\"frameId\":";
    g_motion_trace_frame_json += std::to_string(frameId);
    g_motion_trace_frame_json += ",\"objthis\":";
    g_motion_trace_frame_json += ptrHex(objthis);
    g_motion_trace_frame_json += ",\"topPlayer\":";
    g_motion_trace_frame_json += ptrHex(topPlayer);
    g_motion_trace_frame_json += ",\"playerCount\":";
    g_motion_trace_frame_json += std::to_string(playerCount);
    g_motion_trace_frame_json +=
        ",\"samplePoint\":\"progressCompat.phase3-end.pre-cleanup\""
        ",\"sampleOrder\":\"phase3-return-order\",\"deltaMs\":";
    const auto &capture = *traceState().progress;
    appendJsonDouble(g_motion_trace_frame_json, capture.deltaMs);
    g_motion_trace_frame_json += ",\"playerLayerCounts\":[";
    for(size_t i = 0; i < capture.samples.size(); ++i) {
        if(i) g_motion_trace_frame_json.push_back(',');
        g_motion_trace_frame_json +=
            std::to_string(capture.samples[i].layers.size());
    }
    g_motion_trace_frame_json.push_back(']');
    g_motion_trace_frame_json +=
        ",\"layout\":\"wasmtime-guest-trace\",\"layers\":[";
    g_motion_trace_first_layer = true;
}

extern "C" __attribute__((noinline, used))
void krkr2_wasm_motion_append_layer(std::int32_t frameId,
                                    std::int32_t index,
                                    std::uint64_t nodeFlags,
                                    std::uint64_t opacityBlend,
                                    double posX,
                                    double posY,
                                    double posZ,
                                    double angleDeg,
                                    double scaleX,
                                    double scaleY,
                                    double slantX,
                                    double slantY) {
    (void)frameId;
    if(g_motion_trace_frame_json.empty()) return;
    if(!g_motion_trace_first_layer) {
        g_motion_trace_frame_json.push_back(',');
    }
    g_motion_trace_first_layer = false;
    const auto flags = static_cast<std::uint32_t>(nodeFlags);
    const auto nodeType = static_cast<std::int32_t>(nodeFlags >> 32);
    const auto opacity = static_cast<std::int32_t>(opacityBlend >> 32);
    const auto blendMode = static_cast<std::int32_t>(opacityBlend);
    auto &out = g_motion_trace_frame_json;
    out += "{\"index\":";
    out += std::to_string(index);
    out += ",\"label\":\"\",\"currentImage\":\"\",\"nodeType\":";
    out += std::to_string(nodeType);
    out += ",\"opacity\":";
    out += std::to_string(opacity);
    out += ",\"blendMode\":";
    out += std::to_string(blendMode);
    out += ",\"visible\":";
    out += (flags & (1u << 0)) ? "true" : "false";
    out += ",\"active\":";
    out += (flags & (1u << 1)) ? "true" : "false";
    out += ",\"flipX\":";
    out += (flags & (1u << 2)) ? "true" : "false";
    out += ",\"flipY\":";
    out += (flags & (1u << 3)) ? "true" : "false";
    out += ",\"posX\":";
    appendJsonDouble(out, posX);
    out += ",\"posY\":";
    appendJsonDouble(out, posY);
    out += ",\"posZ\":";
    appendJsonDouble(out, posZ);
    out += ",\"angleDeg\":";
    appendJsonDouble(out, angleDeg);
    out += ",\"scaleX\":";
    appendJsonDouble(out, scaleX);
    out += ",\"scaleY\":";
    appendJsonDouble(out, scaleY);
    out += ",\"slantX\":";
    appendJsonDouble(out, slantX);
    out += ",\"slantY\":";
    appendJsonDouble(out, slantY);
    out.push_back('}');
}

extern "C" __attribute__((noinline, used))
void krkr2_wasm_motion_frame_end(std::int32_t frameId) {
    (void)frameId;
    if(g_motion_trace_frame_json.empty()) return;
    g_motion_trace_frame_json += "]}\n";
    g_motion_trace_jsonl += g_motion_trace_frame_json;
    g_motion_trace_frame_json.clear();
}

void emitLayerSample(int frameId,
                     int flatIndex,
                     const MotionLayerSample &node) {
    const auto &accum = node.accumulated;
    const std::uint64_t opacityBlend =
        (static_cast<std::uint64_t>(
             static_cast<std::uint32_t>(accum.opacity)) << 32) |
        static_cast<std::uint32_t>(node.stencilType);
    int flags = 0;
    if(accum.visible) flags |= 1 << 0;
    if(accum.active) flags |= 1 << 1;
    if(accum.flipX) flags |= 1 << 2;
    if(accum.flipY) flags |= 1 << 3;
    const std::uint64_t nodeFlags =
        (static_cast<std::uint64_t>(
             static_cast<std::uint32_t>(node.nodeType)) << 32) |
        static_cast<std::uint32_t>(flags);
    krkr2_wasm_motion_append_layer(
        frameId, flatIndex, nodeFlags, opacityBlend, accum.posX, accum.posY,
        accum.posZ, accum.angle, accum.scaleX, accum.scaleY, accum.slantX,
        accum.slantY);
}

void emitProgressSample(const ProgressCapture &capture) {
    auto &state = traceState();
    const int frameId = capture.frameId;
    motion::Player *topPlayer =
        capture.samples.empty() ? nullptr : capture.samples.back().player;
    state.lastCompletedFrameId = frameId;
    state.lastCompletedTopPlayer = topPlayer;
    state.lastCompletedRenderLayerObject = nullptr;
    state.lastCompletedRenderLayerFromAccurateSla = false;
    state.lastDrawTargetObject = nullptr;
    state.lastPostDrawLayerPlayer = nullptr;
    state.lastPostDrawLayerObject = nullptr;
    state.lastPostDrawLayerSamplePoint.clear();
    state.lastPostDrawCanvasTexture = nullptr;
    state.lastPostDrawCanvasSamplePoint.clear();
    krkr2_wasm_motion_frame_begin(
        frameId, capture.objthis, topPlayer,
        static_cast<std::int32_t>(capture.samples.size()));

    int flatIndex = 0;
    for(const auto &sample : capture.samples) {
        for(const auto &layer : sample.layers) {
            emitLayerSample(frameId, flatIndex++, layer);
        }
    }
    krkr2_wasm_motion_frame_end(frameId);
    ++state.frameCounter;
}

template <typename Fn>
int runWithErrors(Fn &&fn) {
    try {
        fn();
        return 1;
    } catch(const TJS::eTJSScriptError &e) {
        std::string msg = ttstr(e.GetMessage()).AsStdString();
        if(e.GetBlockName()) {
            msg += " at ";
            msg += ttstr(e.GetBlockName()).AsStdString();
            msg += ":";
            msg += std::to_string(e.GetSourceLine());
        }
        msg += " pos ";
        msg += std::to_string(e.GetPosition());
        const auto trace = ttstr(e.GetTrace()).AsStdString();
        if(!trace.empty()) {
            msg += "\n";
            msg += trace;
        }
        setError(msg);
    } catch(const TJS::eTJS &e) {
        setError(ttstr(e.GetMessage()).AsStdString());
    } catch(const std::exception &e) {
        setError(e.what());
    } catch(const tjs_char *e) {
        setError(ttstr(e).AsStdString());
    } catch(const char *e) {
        setError(e);
    } catch(...) {
        setError("unknown C++ exception");
    }
    return 0;
}

} // namespace

void resetState() {
    g_error.clear();
    g_stage.clear();
    g_framebuffer.clear();
    g_framebuffer_width = 0;
    g_framebuffer_height = 0;
    g_framebuffer_pitch = 0;
    g_framebuffer_format = 0;
    g_framebuffer_frame_no = 0;
    g_render_probe_jsonl.clear();
    g_motion_trace_jsonl.clear();
    g_motion_trace_frame_json.clear();
    g_motion_trace_first_layer = true;
    g_wasmtime_tick_count = 0;
    g_wasmtime_tick_diag_mask = 0;
    g_wasmtime_tick_diag_ever_mask = 0;
    g_wasmtime_window_count_peak = 0;
    g_render_probe_seq = 0;
    g_render_draw_id = 0;
    g_record_layer_raw_probes = false;
    auto &state = traceState();
    state.progress = nullptr;
    state.inRender = false;
    state.frameCounter = 0;
    state.nextFrameId = 0;
    state.lastCompletedFrameId = -1;
    state.lastCompletedTopPlayer = nullptr;
    state.lastCompletedRenderLayerObject = nullptr;
    state.lastCompletedRenderLayerFromAccurateSla = false;
    state.lastDrawTargetObject = nullptr;
    state.lastPostDrawLayerPlayer = nullptr;
    state.lastPostDrawLayerObject = nullptr;
    state.lastPostDrawLayerSamplePoint.clear();
    state.lastPostDrawCanvasTexture = nullptr;
    state.lastPostDrawCanvasSamplePoint.clear();
    state.currentRenderFrameId = -1;
    state.currentRenderPlayer = nullptr;
    state.accurateSlaRenderDepth = 0;
}

void setError(const std::string &message) {
    if(g_stage.empty()) {
        g_error = message;
    } else {
        g_error = g_stage + ": " + message;
    }
}

void setStage(const char *stage) {
    g_stage = stage ? stage : "";
}

void setStageString(const std::string &stage) {
    g_stage = stage;
}

void TVPWasmtimeTickMainScene(float deltaSeconds) {
    ++g_wasmtime_tick_count;
    // The browser calls this from TVPMainScene::update at every RAF boundary.
    // A headless Director step does not guarantee that scheduled scene update
    // runs on every host tick, so publish the host's deterministic timestamp
    // explicitly.  A later scene-side call in the same tick sees the same RAF
    // timestamp and the production PLL discards it as a duplicate.
    TVPWebFrameTickUpdate();
    // The Emscripten Application::mainLoop wrapper is registered as the
    // browser callback by Application::run; invoking that wrapper manually in
    // the headless Wasmtime host does not step the Director after run()
    // returns.  The driver already owns the deterministic host loop, so enter
    // the same Director frame boundary directly.
    auto *director = cocos2d::Director::getInstance();
    auto *scene = TVPMainScene::GetInstance();
    int diag = 0;
    if(director) diag |= 1 << 0;
    if(scene) diag |= 1 << 1;
    if(director && director->getRunningScene()) diag |= 1 << 2;
    if(scene && scene->isRunning()) diag |= 1 << 3;
    if(scene && scene->isScheduled("startup")) diag |= 1 << 4;
    if(TVPSystemControlAlive && TVPSystemControl) diag |= 1 << 5;
    if(TVPProcessContinuousHandlerEventFlag) diag |= 1 << 6;
    if(::Application) diag |= 1 << 7;
    g_wasmtime_tick_diag_ever_mask |= diag;
    g_wasmtime_window_count_peak = std::max(
        g_wasmtime_window_count_peak, static_cast<int>(TVPGetWindowCount()));
    const int motionFramesBeforeTick = traceState().frameCounter;
    if(director && director->getScheduler()) {
        // runWithScene publishes a pending scene that Director::mainLoop must
        // promote before its scheduled callbacks become live.  The explicit
        // delta overload advances the Scheduler itself; a second update here
        // would invoke SystemControl/continuous handlers twice at one tick.
        director->mainLoop(deltaSeconds);
    }
    if(scene && scene->isScheduled("startup")) diag |= 1 << 10;
    if(TVPProcessContinuousHandlerEventFlag) diag |= 1 << 11;
    g_wasmtime_tick_diag_ever_mask |= diag;
    g_wasmtime_window_count_peak = std::max(
        g_wasmtime_window_count_peak, static_cast<int>(TVPGetWindowCount()));
    // EventIntf requires each platform's continuous-event implementation to
    // call TVPDeliverContinuousEvent while handlers are live.  Browser RAF
    // normally supplies that pump; the Wasmtime host has no RAF callback.  If
    // the scene/SystemControl path already produced a progress sample, do not
    // deliver again.  Otherwise provide exactly one host-tick delivery.
    if(traceState().frameCounter == motionFramesBeforeTick) {
        TVPDeliverContinuousEvent();
    }
    if(director && director->getRunningScene()) diag |= 1 << 8;
    if(scene && scene->isRunning()) diag |= 1 << 9;
    if(scene && scene->isScheduled("startup")) diag |= 1 << 10;
    if(TVPProcessContinuousHandlerEventFlag) diag |= 1 << 11;
    if(traceState().frameCounter > 0) diag |= 1 << 12;
    g_wasmtime_tick_diag_mask = diag;
    g_wasmtime_tick_diag_ever_mask |= diag;
    g_wasmtime_window_count_peak = std::max(
        g_wasmtime_window_count_peak, static_cast<int>(TVPGetWindowCount()));
}

namespace motion::detail {

MotionTraceProgressScope::MotionTraceProgressScope(Player *player,
                                                   void *objthis) :
    _player(player) {
    auto &state = traceState();
    auto capture = std::make_unique<ProgressCapture>();
    capture->parent = state.progress;
    capture->objthis = objthis;
    capture->frameId = state.nextFrameId++;
    capture->exceptionsOnEntry = std::uncaught_exceptions();
    state.progress = capture.get();
    _capture = capture.release();
}

MotionTraceProgressScope::~MotionTraceProgressScope() {
    auto &state = traceState();
    std::unique_ptr<ProgressCapture> capture(
        static_cast<ProgressCapture *>(_capture));
    // Frida onLeave observes normal returns, not C++ stack unwinding.
    if(std::uncaught_exceptions() == capture->exceptionsOnEntry) {
        emitProgressSample(*capture);
    }
    state.progress = capture->parent;
}

void MotionTraceProgressScope::setDeltaMs(double deltaMs) {
    static_cast<ProgressCapture *>(_capture)->deltaMs = deltaMs;
}

void motionTraceSnapshotPhase3(Player *player) {
    auto *capture = traceState().progress;
    if(!capture || !player) return;
    MotionPlayerSample sample{player, {}};
    sample.layers.reserve(player->nodes().size());
    for(const auto &node : player->nodes()) {
        sample.layers.push_back(
            {node.nodeType, node.stencilType, node.accumulated});
    }
    capture->samples.push_back(std::move(sample));
}

MotionTraceRenderDrawScope::MotionTraceRenderDrawScope(
    Player *player, void *argVariant, void *targetObject) :
    _player(player),
    _argVariant(argVariant),
    _targetObject(targetObject) {
    _drawId = g_render_draw_id++;
    auto &state = traceState();
    state.inRender = true;
    state.currentRenderFrameId = state.lastCompletedFrameId;
    state.currentRenderPlayer = player;
    state.lastDrawTargetObject = targetObject;
    state.lastPostDrawLayerPlayer = nullptr;
    state.lastPostDrawLayerObject = nullptr;
    state.lastPostDrawLayerSamplePoint.clear();
    state.lastPostDrawCanvasTexture = nullptr;
    state.lastPostDrawCanvasSamplePoint.clear();
    std::string payload = "\"drawId\":";
    payload += std::to_string(_drawId);
    std::string diagnostics = "{\"argVariant\":";
    diagnostics += ptrHex(argVariant);
    diagnostics += ",\"targetObject\":";
    diagnostics += ptrHex(targetObject);
    diagnostics += ",\"sampling\":\"guest-cpp-Player-draw\"}";
    appendRenderEvent(player, "draw_dispatch", "draw_enter",
                      "Player::draw.enter", payload,
                      diagnostics);
    motionTraceLayerRawProbe(
        player, targetObject, "Player_draw.enter");
}

MotionTraceRenderDrawScope::~MotionTraceRenderDrawScope() {
    motionTraceLayerRawProbe(
        _player, _targetObject, "Player_draw.leave");
    const char *route = _route ? _route : (_steps.empty() ? "no_target" : "failed");
    std::string payload = "\"route\":";
    appendJsonString(payload, route);
    payload += ",\"drawId\":";
    payload += std::to_string(_drawId);
    payload += ",\"drawPath\":{\"route\":";
    appendJsonString(payload, route);
    payload += ",\"steps\":[";
    for(size_t i = 0; i < _steps.size(); ++i) {
        if(i) payload.push_back(',');
        appendJsonString(payload, _steps[i]);
    }
    payload += "],\"prepareCalled\":";
    payload += _prepareCalled ? "true" : "false";
    payload += ",\"prepareOk\":";
    appendJsonBoolOrNull(payload, _prepareOkKnown, _prepareOk);
    payload += ",\"d3dDrawModeAfterPrepare\":";
    appendJsonBoolOrNull(
        payload, _d3dDrawModeAfterPrepareKnown,
        _d3dDrawModeAfterPrepare);
    payload += ",\"renderToCanvasCalled\":";
    payload += _renderToCanvasCalled ? "true" : "false";
    payload += ",\"updateLayerAfterDrawCalled\":";
    payload += _updateLayerAfterDrawCalled ? "true" : "false";
    payload += ",\"internalAssignRequested\":";
    appendJsonBoolOrNull(
        payload, _internalAssignRequestedKnown,
        _internalAssignRequested);
    payload += "}";
    std::string diagnostics = "{\"argVariant\":";
    diagnostics += ptrHex(_argVariant);
    diagnostics += ",\"targetObject\":";
    diagnostics += ptrHex(_targetObject);
    diagnostics += ",\"sampling\":\"guest-cpp-Player-draw\"}";
    appendRenderEvent(_player, "draw_dispatch", "draw_leave",
                      "Player::draw.leave", payload,
                      diagnostics);
    auto &state = traceState();
    state.inRender = false;
    state.currentRenderFrameId = -1;
    state.currentRenderPlayer = nullptr;
}

void MotionTraceRenderDrawScope::setRoute(const char *route) {
    _route = route;
}

void MotionTraceRenderDrawScope::emitStep(
    const char *drawStep,
    const char *outcome,
    const char *route,
    const char *extraPayload) {
    if(!drawStep) return;
    if(route) _route = route;
    _steps.emplace_back(drawStep);
    std::string payload = "\"drawId\":";
    payload += std::to_string(_drawId);
    payload += ",\"stepIndex\":";
    payload += std::to_string(_stepIndex++);
    payload += ",\"drawStep\":";
    appendJsonString(payload, drawStep);
    payload += ",\"outcome\":";
    appendJsonString(payload, outcome ? outcome : "");
    if(route) {
        payload += ",\"route\":";
        appendJsonString(payload, route);
    }
    if(extraPayload && extraPayload[0] != '\0') {
        payload.push_back(',');
        payload += extraPayload;
    }
    std::string diagnostics = "{\"argVariant\":";
    diagnostics += ptrHex(_argVariant);
    diagnostics += ",\"targetObject\":";
    diagnostics += ptrHex(_targetObject);
    diagnostics += ",\"sampling\":\"guest-cpp-Player-draw\"}";
    std::string samplePoint = "Player::draw.";
    samplePoint += drawStep;
    appendRenderEvent(_player, "draw_dispatch", "draw_step",
                      samplePoint.c_str(), payload, diagnostics);
}

void MotionTraceRenderDrawScope::recordTargetCheckD3D(bool hit) {
    emitStep("target_check_d3d", hit ? "hit" : "miss",
             hit ? "d3d_adaptor" : nullptr);
}

void MotionTraceRenderDrawScope::recordTargetCheckSLA(bool hit) {
    emitStep("target_check_sla", hit ? "hit" : "miss",
             hit ? "separate_layer_adaptor" : nullptr);
}

void MotionTraceRenderDrawScope::recordPrepareResult(bool ok) {
    _prepareCalled = true;
    _prepareOk = ok;
    _prepareOkKnown = true;
    emitStep("prepare_render_items", ok ? "ok" : "empty",
             ok ? nullptr : "prepare_empty",
             ok ? "\"prepareOk\":true" : "\"prepareOk\":false");
}

void MotionTraceRenderDrawScope::recordBranchAfterPrepare(bool d3dDrawMode) {
    _d3dDrawModeAfterPrepare = d3dDrawMode;
    _d3dDrawModeAfterPrepareKnown = true;
    emitStep(
        "branch_after_prepare",
        d3dDrawMode ? "shared_d3d" : "ordinary",
        d3dDrawMode ? "shared_d3d_after_prepare" : "ordinary_layer",
        d3dDrawMode
            ? "\"d3dDrawModeAfterPrepare\":true"
            : "\"d3dDrawModeAfterPrepare\":false");
}

void MotionTraceRenderDrawScope::recordApplyPreparedProjection() {
    emitStep("apply_prepared_projection", "done");
}

void MotionTraceRenderDrawScope::recordRenderToCanvas(bool ok) {
    _renderToCanvasCalled = true;
    emitStep("render_to_canvas", ok ? "done" : "failed", "ordinary_layer");
}

void MotionTraceRenderDrawScope::recordUpdateLayerAfterDraw(
    bool internalAssignRequested, bool ok) {
    _updateLayerAfterDrawCalled = true;
    _internalAssignRequested = internalAssignRequested;
    _internalAssignRequestedKnown = true;
    emitStep(
        "update_layer_after_draw",
        ok ? "done" : "failed",
        "ordinary_layer",
        internalAssignRequested
            ? "\"internalAssignRequested\":true"
            : "\"internalAssignRequested\":false");
}

MotionTraceRenderExecuteScope::MotionTraceRenderExecuteScope(
    Player *player, void *renderLayerObject, bool skipUpdate,
    const PreparedItemList &mainList, const char *boundary) :
    _player(player),
    _renderLayerObject(renderLayerObject),
    _skipUpdate(skipUpdate),
    _boundary(boundary),
    _mainList(&mainList) {
    std::string payload;
    appendRenderItemsPayload(payload, _mainList, nullptr);
    payload += ",\"renderLayerObject\":";
    payload += ptrHex(renderLayerObject);
    payload += ",\"skipUpdate\":";
    payload += skipUpdate ? "true" : "false";
    payload += ",\"executeBoundary\":";
    appendJsonString(payload, _boundary);
    std::string diagnostics = playerDiagnostics(player);
    motionTraceRenderImageCheckpoint(
        player, renderLayerObject, "execute_pre",
        (std::string(_boundary) + ".enter").c_str());
    motionTraceLayerRawProbe(
        player, renderLayerObject, "Player::renderExecute.enter");
    appendRenderEvent(player, "render_execute", "execute_enter",
                      (std::string(_boundary) + ".enter").c_str(),
                      payload, diagnostics);
}

MotionTraceRenderExecuteScope::~MotionTraceRenderExecuteScope() {
    motionTraceLayerRawProbe(
        _player, _renderLayerObject, "Player::renderExecute.leave");
    std::string payload;
    appendRenderItemsPayload(payload, _mainList, nullptr);
    payload += ",\"renderLayerObject\":";
    payload += ptrHex(_renderLayerObject);
    payload += ",\"skipUpdate\":";
    payload += _skipUpdate ? "true" : "false";
    payload += ",\"ok\":";
    payload += _ok ? "true" : "false";
    payload += ",\"executeBoundary\":";
    appendJsonString(payload, _boundary);
    std::string diagnostics = playerDiagnostics(_player);
    motionTraceRenderImageCheckpoint(
        _player, _renderLayerObject, "execute_post",
        (std::string(_boundary) + ".leave").c_str());
    const bool accurateSla = traceState().accurateSlaRenderDepth > 0;
    if(_renderLayerObject) {
        auto &state = traceState();
        state.lastCompletedRenderLayerObject = _renderLayerObject;
        state.lastCompletedRenderLayerFromAccurateSla =
            accurateSla;
    }
    appendRenderEvent(_player, "render_execute", "execute_leave",
                      (std::string(_boundary) + ".leave").c_str(),
                      payload, diagnostics);
    // Android's accurate-SLA execute_post checkpoint is the next DrawDevice
    // upload, not the LayerManager no-onPaint buffer here. The post_draw
    // checkpoint path backfills execute_post from that upload.
}

void MotionTraceRenderExecuteScope::setResult(bool ok) {
    _ok = ok;
}

void motionTraceBeginAccurateSlaRender(Player *, void *) {
    auto &state = traceState();
    ++state.accurateSlaRenderDepth;
}

void motionTraceEndAccurateSlaRender(Player *, void *) {
    auto &state = traceState();
    if(state.accurateSlaRenderDepth > 0) {
        --state.accurateSlaRenderDepth;
    }
}

bool motionTraceIsAccurateSlaRenderActive() {
    return traceState().accurateSlaRenderDepth > 0;
}

void motionTraceRenderImageCheckpointAtFrame(Player *player,
                                             void *renderLayerObject,
                                             const char *phase,
                                             const char *samplePoint,
                                             int frameId);

void motionTraceRecordPostDrawLayerCandidate(Player *player, void *layerObject,
                                             const char *samplePoint) {
    if(!layerObject) return;
    auto &state = traceState();
    state.lastPostDrawLayerPlayer = renderPlayerFor(player);
    state.lastPostDrawLayerObject = layerObject;
    state.lastPostDrawLayerSamplePoint = samplePoint ? samplePoint : "";
}

void motionTraceRecordPostDrawCanvasTexture(iTVPTexture2D *texture,
                                            const char *samplePoint) {
    if(!texture) return;
    const int width = static_cast<int>(texture->GetWidth());
    const int height = static_cast<int>(texture->GetHeight());
    const int pitch = static_cast<int>(texture->GetPitch());
    if(width != 1920 || height != 1080 || pitch < width * 4) {
        return;
    }
    if(!texture->GetScanLineForRead(0)) {
        return;
    }
    auto &state = traceState();
    state.lastPostDrawCanvasTexture = texture;
    state.lastPostDrawCanvasSamplePoint =
        samplePoint ? samplePoint : "";
}

bool forcePostDrawDrawDeviceShow() {
    if(!TVPMainWindow) return false;
    TVPMainWindow->DeliverDrawDeviceShow();
    return true;
}

void motionTraceRenderPostDrawLayerManagerCheckpointAtFrame(
    int frameId, void *markerBaseLayerObject) {
    const std::string phase = "post_draw";
    const std::string samplePoint =
        "startup.tjs.post_draw.after_onPaint.layer-manager-draw-buffer";
    auto diagnostics = [](const char *captureMethod,
                          void *layerObject,
                          tTJSNI_BaseLayer *nativeLayer,
                          iTVPLayerManager *manager,
                          iTVPBaseBitmap *drawBuffer,
                          iTVPTexture2D *texture,
                          int rowBytes,
                          int sourcePitch,
                          int failedRow = -1) {
        std::string diag = "{\"player\":null,\"activeMotion\":\"\"";
        diag += ",\"samplingMode\":\"guest-cpp-probe\"";
        diag += ",\"captureMethod\":";
        appendJsonString(diag, captureMethod ? captureMethod : "");
        diag += ",\"layerObject\":";
        diag += ptrHex(layerObject);
        diag += ",\"nativeLayer\":";
        diag += ptrHex(nativeLayer);
        diag += ",\"manager\":";
        diag += ptrHex(manager);
        diag += ",\"drawBuffer\":";
        diag += ptrHex(drawBuffer);
        diag += ",\"texture\":";
        diag += ptrHex(texture);
        diag += ",\"rowBytes\":";
        diag += std::to_string(rowBytes);
        diag += ",\"sourcePitch\":";
        diag += std::to_string(sourcePitch);
        if(texture) {
            diag += ",\"format\":";
            diag += std::to_string(static_cast<int>(texture->GetFormat()));
        }
        if(failedRow >= 0) {
            diag += ",\"failedRow\":";
            diag += std::to_string(failedRow);
        }
        diag.push_back('}');
        return diag;
    };
    auto fail = [&](const std::string &message,
                    void *layerObject = nullptr,
                    tTJSNI_BaseLayer *nativeLayer = nullptr,
                    iTVPLayerManager *manager = nullptr,
                    iTVPBaseBitmap *drawBuffer = nullptr,
                    iTVPTexture2D *texture = nullptr,
                    int width = 0,
                    int height = 0,
                    int pitch = 0) {
        appendImageCheckpointEvent(
            nullptr, phase.c_str(), samplePoint.c_str(), false, {},
            message, width, height, pitch,
            diagnostics("LayerManager.GetDrawBuffer.texture-scanline",
                        layerObject, nativeLayer, manager, drawBuffer,
                        texture, pitch, texture ? texture->GetPitch() : 0),
            frameId, "rgba32");
    };

    if(!markerBaseLayerObject) {
        fail("post_draw marker did not provide a base Layer object");
        return;
    }
    auto *layerObject = static_cast<iTJSDispatch2 *>(markerBaseLayerObject);
    tTJSNI_BaseLayer *layer = nullptr;
    if(TJS_FAILED(layerObject->NativeInstanceSupport(
           TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
           reinterpret_cast<iTJSNativeInstance **>(&layer))) || !layer) {
        fail("post_draw marker base object did not resolve to Layer native instance",
             layerObject);
        return;
    }
    auto *manager = layer->GetManager();
    if(!manager) {
        fail("post_draw marker base Layer has no LayerManager",
             layerObject, layer);
        return;
    }
    manager->UpdateToDrawDevice();
    auto *drawBuffer = manager->GetDrawBuffer();
    if(!drawBuffer) {
        fail("LayerManager draw buffer is null",
             layerObject, layer, manager);
        return;
    }
    auto *texture = drawBuffer->GetTexture();
    if(!texture) {
        fail("LayerManager draw buffer texture is null",
             layerObject, layer, manager, drawBuffer);
        return;
    }

    const int width = static_cast<int>(texture->GetWidth());
    const int height = static_cast<int>(texture->GetHeight());
    const int sourcePitch = static_cast<int>(texture->GetPitch());
    const int rowBytes = width * 4;
    if(width != 1920 || height != 1080 || sourcePitch < rowBytes) {
        fail("LayerManager draw buffer texture has invalid dimensions",
             layerObject, layer, manager, drawBuffer, texture,
             width, height, rowBytes);
        return;
    }

    const auto path = framePath(phase.c_str(), frameId, "rgba");
    int failedRow = -1;
    const bool ok = writePackedRowsFromTexture(
        path, texture, width, height, &failedRow);
    appendImageCheckpointEvent(
        nullptr, phase.c_str(), samplePoint.c_str(), ok, path,
        ok ? std::string()
           : std::string("failed to write LayerManager draw buffer RGBA checkpoint"),
        width, height, rowBytes,
        diagnostics("LayerManager.GetDrawBuffer.texture-scanline",
                    layerObject, layer, manager, drawBuffer, texture,
                    rowBytes, sourcePitch, failedRow),
        frameId, "rgba32");
    if(ok) {
        const char *executePhase = "execute_post";
        const std::string executePhaseDir =
            std::string(kRenderStageCaptureRoot) + "/_execute/" + executePhase;
        if(directoryExists(executePhaseDir)) {
            const auto executePath = framePath(executePhase, frameId, "rgba");
            if(fileExists(executePath)) {
                return;
            }
            int executeFailedRow = -1;
            const bool executeOk = writePackedRowsFromTexture(
                executePath, texture, width, height, &executeFailedRow);
            appendImageCheckpointEvent(
                nullptr, executePhase,
                "startup.tjs.post_draw.after_onPaint.layer_manager_accurate_sla_execute_post",
                executeOk, executePath,
                executeOk ? std::string()
                          : std::string("failed to write accurate SLA execute_post layer-manager checkpoint"),
                width, height, rowBytes,
                diagnostics("LayerManager.GetDrawBuffer.texture-scanline",
                            layerObject, layer, manager, drawBuffer, texture,
                            rowBytes, sourcePitch, executeFailedRow),
                frameId, "rgba32");
        }
    }
}

void motionTraceRenderPostDrawCanvasTextureCheckpointAtFrame(
    int frameId, void *markerBaseLayerObject) {
    if(frameId < 0 || !captureFrameEnabled(frameId)) return;
    const std::string phase = "post_draw";
    const std::string phaseDir =
        std::string(kRenderStageCaptureRoot) + "/_execute/" + phase;
    if(!directoryExists(phaseDir)) return;

    auto &state = traceState();
    auto *texture = state.lastPostDrawCanvasTexture;
    const std::string samplePoint =
        "startup.tjs.post_draw.after_onPaint.drawdevice-upload";
    if(!texture) {
        forcePostDrawDrawDeviceShow();
        texture = state.lastPostDrawCanvasTexture;
    }
    const char *recordSamplePoint =
        state.lastPostDrawCanvasSamplePoint.empty()
            ? nullptr
            : state.lastPostDrawCanvasSamplePoint.c_str();
    if(!texture && markerBaseLayerObject) {
        motionTraceRenderPostDrawLayerManagerCheckpointAtFrame(
            frameId, markerBaseLayerObject);
        return;
    }
    if(!texture) {
        appendImageCheckpointEvent(
            nullptr, phase.c_str(), samplePoint.c_str(), false, {},
            "no 1920x1080 DrawDevice canvas texture or marker base Layer was recorded before marker",
            0, 0, 0,
            canvasTextureDiagnostics(
                "DrawDevice_UpdateDrawBuffer.texture-scanline",
                nullptr, recordSamplePoint, 0, 0),
            frameId, "rgba32");
        return;
    }

    const int width = static_cast<int>(texture->GetWidth());
    const int height = static_cast<int>(texture->GetHeight());
    const int sourcePitch = static_cast<int>(texture->GetPitch());
    const int rowBytes = width * 4;
    const auto diagnostics = [&](int failedRow = -1) {
        return canvasTextureDiagnostics(
            "DrawDevice_UpdateDrawBuffer.texture-scanline",
            texture, recordSamplePoint, rowBytes, sourcePitch, failedRow);
    };
    if(width != 1920 || height != 1080 || sourcePitch < rowBytes) {
        appendImageCheckpointEvent(
            nullptr, phase.c_str(), samplePoint.c_str(), false, {},
            "recorded DrawDevice canvas texture has invalid dimensions",
            width, height, rowBytes, diagnostics(), frameId, "rgba32");
        return;
    }

    const auto path = framePath(phase.c_str(), frameId, "rgba");
    int failedRow = -1;
    const bool ok = writePackedRowsFromTexture(
        path, texture, width, height, &failedRow);
    appendImageCheckpointEvent(
        nullptr, phase.c_str(), samplePoint.c_str(), ok, path,
        ok ? std::string()
           : std::string("failed to write DrawDevice texture RGBA checkpoint"),
        width, height, rowBytes, diagnostics(failedRow), frameId, "rgba32");

    if(ok) {
        const char *executePhase = "execute_post";
        const std::string executePhaseDir =
            std::string(kRenderStageCaptureRoot) + "/_execute/" + executePhase;
        if(directoryExists(executePhaseDir)) {
            const auto executePath = framePath(executePhase, frameId, "rgba");
            if(fileExists(executePath)) {
                return;
            }
            int executeFailedRow = -1;
            const bool executeOk = writePackedRowsFromTexture(
                executePath, texture, width, height, &executeFailedRow);
            appendImageCheckpointEvent(
                nullptr, executePhase,
                "startup.tjs.post_draw.after_onPaint.accurate_sla_execute_post",
                executeOk, executePath,
                executeOk ? std::string()
                          : std::string("failed to write accurate SLA execute_post drawdevice checkpoint"),
                width, height, rowBytes, diagnostics(executeFailedRow),
                frameId, "rgba32");
        }
    }
}

void motionTraceRenderPrepareEnter(Player *player) {
    if(!isCurrentRenderPlayer(player)) return;
    std::string diagnostics = playerDiagnostics(player);
    appendRenderEvent(player, "render_prepare", "prepare_enter",
                      "Player::prepareRenderItems.enter", "", diagnostics);
}

void motionTraceRenderPrepareLeave(
    Player *player, bool ok,
    const PreparedItemList &mainList,
    const PreparedItemList &auxList) {
    if(!isCurrentRenderPlayer(player)) return;
    std::string payload;
    payload += "\"ok\":";
    payload += ok ? "1" : "0";
    payload.push_back(',');
    appendPreparedRenderListsPayload(payload, &mainList, &auxList);
    std::string diagnostics = playerDiagnostics(player);
    appendRenderEvent(player, "render_prepare", "prepare_leave",
                      "Player::prepareRenderItems.leave", payload,
                      diagnostics);
}

void motionTraceRenderApplyProjectionEnter(Player *player) {
    if(!isCurrentRenderPlayer(player)) return;
    std::string diagnostics = playerDiagnostics(player);
    appendRenderEvent(player, "render_prepare", "apply_projection_enter",
                      "Player::applyPreparedRenderItemProjection_guess.enter",
                      "", diagnostics);
}

void motionTraceRenderApplyProjectionLeave(
    Player *player,
    const PreparedItemList &mainList) {
    if(!isCurrentRenderPlayer(player)) return;
    std::string payload;
    // The native post-prepare pass receives only main; aux is not an empty
    // vector argument, it is absent from this call boundary.
    appendPreparedRenderListsPayload(payload, &mainList, nullptr, false);
    std::string diagnostics = playerDiagnostics(player);
    appendRenderEvent(player, "render_prepare", "apply_projection_leave",
                      "Player::applyPreparedRenderItemProjection_guess.leave",
                      payload, diagnostics);
}

void motionTraceRenderBuildItemsEnter(Player *player,
                                      std::uint32_t inheritedColor,
                                      bool inheritedDrawFlag19,
                                      bool inheritedFlag18) {
    if(!traceState().inRender) return;
    std::string diagnostics = playerDiagnostics(player);
    if(!diagnostics.empty() && diagnostics.back() == '}') {
        diagnostics.pop_back();
    }
    diagnostics += ",\"defaultColor\":";
    diagnostics += std::to_string(static_cast<std::int32_t>(inheritedColor));
    diagnostics += ",\"arg4\":";
    diagnostics += inheritedDrawFlag19 ? "1" : "0";
    diagnostics += ",\"arg5\":";
    diagnostics += inheritedFlag18 ? "1" : "0";
    diagnostics += "}";
    appendRenderEvent(nullptr, "render_commands", "build_items_enter",
                      "Player_appendPreparedRenderItems.enter", "",
                      diagnostics);
}

void motionTraceRenderBuildItemsLeave(
    Player *player,
    const PreparedItemList &mainList,
    const PreparedItemList &auxList) {
    if(!traceState().inRender) return;
    std::string payload;
    appendPreparedRenderListsPayload(payload, &mainList, &auxList);
    std::string diagnostics = playerDiagnostics(player);
    appendRenderEvent(nullptr, "render_commands", "build_items_leave",
                      "Player_appendPreparedRenderItems.leave", payload,
                      diagnostics);
}

void motionTraceRenderBuildCommandsEnter(Player *player,
                                         int canvasWidth,
                                         int canvasHeight,
                                         const PreparedItemList &mainList,
                                         const PreparedItemList &auxList) {
    if(!isCurrentRenderPlayer(player)) return;
    std::string payload;
    appendPreparedRenderListsPayload(payload, &mainList, &auxList);
    payload += ",\"canvas\":{\"width\":";
    payload += std::to_string(canvasWidth);
    payload += ",\"height\":";
    payload += std::to_string(canvasHeight);
    payload += "}";
    std::string diagnostics = playerDiagnostics(player);
    appendRenderEvent(player, "render_commands", "build_commands_enter",
                      "Player::buildRenderCommands.enter", payload,
                      diagnostics);
}

void motionTraceRenderBuildCommandsLeave(Player *player,
                                         int canvasWidth,
                                         int canvasHeight,
                                         const PreparedItemList &mainList,
                                         const PreparedItemList &auxList) {
    if(!isCurrentRenderPlayer(player)) return;
    std::string payload;
    appendCommandRenderListsPayload(payload, &mainList, &auxList);
    payload.push_back(',');
    appendRenderItemsPayload(payload, &mainList, &auxList);
    payload += ",\"canvas\":{\"width\":";
    payload += std::to_string(canvasWidth);
    payload += ",\"height\":";
    payload += std::to_string(canvasHeight);
    payload += "}";
    std::string diagnostics = playerDiagnostics(player);
    appendRenderEvent(player, "render_commands", "build_commands_leave",
                      "Player::buildRenderCommands.leave", payload,
                      diagnostics);
}

void motionTraceRenderImageCheckpointAtFrame(Player *player,
                                             void *renderLayerObject,
                                             const char *phase,
                                             const char *samplePoint,
                                             int frameId) {
    if(frameId < 0 || !captureFrameEnabled(frameId) ||
       !phase || !renderLayerObject) {
        return;
    }
    const std::string phaseDir =
        std::string(kRenderStageCaptureRoot) + "/_execute/" + phase;
    if(!directoryExists(phaseDir)) {
        return;
    }
    auto *layerObject = static_cast<iTJSDispatch2 *>(renderLayerObject);
    tTJSNI_BaseLayer *layer = resolveTraceLayerLikeNative(layerObject);
    if(!layer) {
        appendImageCheckpointEvent(
            player, phase, samplePoint, false, {},
            "renderLayerObject did not resolve to Layer/private native instance",
            0, 0, 0,
            checkpointDiagnostics(
                player, "main-image-get-scanline", nullptr, 0),
            frameId);
        return;
    }

    if(phase && std::strcmp(phase, "execute_post") == 0 &&
       traceState().accurateSlaRenderDepth > 0) {
        // Accurate-SLA execute_post matches Android's next DrawDevice upload,
        // not the target Layer main image at the accurate-SLA renderer leave.
        return;
    }

    const auto path = framePath(phase, frameId, "rgba");
    const int width = static_cast<int>(layer->GetWidth());
    const int height = static_cast<int>(layer->GetHeight());
    auto *mainImage = layer->GetMainImage();
    const int rowBytes = width * 4;
    const auto diagnostics = [&](int failedRow = -1) {
        return checkpointDiagnostics(
            player, "main-image-get-scanline", mainImage, rowBytes, failedRow);
    };
    if(width <= 0 || height <= 0) {
        appendImageCheckpointEvent(
            player, phase, samplePoint, false, path,
            "Layer main image has invalid dimensions", width, height, rowBytes,
            diagnostics(), frameId);
        return;
    }
    if(!mainImage) {
        appendImageCheckpointEvent(
            player, phase, samplePoint, false, path,
            "Layer main image is null", width, height, rowBytes, diagnostics(),
            frameId);
        return;
    }
    if(static_cast<int>(mainImage->GetWidth()) != width ||
       static_cast<int>(mainImage->GetHeight()) != height) {
        appendImageCheckpointEvent(
            player, phase, samplePoint, false, path,
            "Layer main image dimensions differ from layer dimensions",
            width, height, rowBytes, diagnostics(), frameId);
        return;
    }

    int failedRow = -1;
    bool ok = writePackedRowsFromScanLines(
        path, mainImage, width, height, &failedRow);
    appendImageCheckpointEvent(
        player, phase, samplePoint, ok, path,
        ok ? std::string()
           : std::string("failed to write scanline RGBA checkpoint"),
        width, height, rowBytes, diagnostics(failedRow), frameId, "rgba32");
}

void motionTraceRenderImageCheckpoint(Player *player,
                                      void *renderLayerObject,
                                      const char *phase,
                                      const char *samplePoint) {
    motionTraceRenderImageCheckpointAtFrame(
        player, renderLayerObject, phase, samplePoint,
        renderFrameIdFor(player));
}

void motionTraceRenderDirectExecuteProbe(Player *player,
                                         const char *samplePoint,
                                         const char *payload) {
    if(!isCurrentRenderPlayer(player)) return;
    appendRenderEvent(
        player, "render_execute", "direct_execute_probe",
        samplePoint ? samplePoint : "Player::executeLayerRenderCommands.direct",
        payload ? payload : "", playerDiagnostics(player));
}

void motionTracePrivateMotionGLLDraw(void *nativeLayer,
                                     int queuedItems,
                                     int clipLeft,
                                     int clipTop,
                                     int clipRight,
                                     int clipBottom,
                                     int targetLeft,
                                     int targetTop,
                                     int targetRight,
                                     int targetBottom,
                                     bool visibleCheck) {
    auto &state = traceState();
    if(!state.inRender) return;
    Player *player = renderPlayerFor(nullptr);
    std::string payload;
    payload += "\"nativeLayer\":";
    payload += ptrHex(nativeLayer);
    payload += ",\"queuedItems\":";
    payload += std::to_string(queuedItems);
    payload += ",\"clipRect\":[";
    payload += std::to_string(clipLeft);
    payload += ",";
    payload += std::to_string(clipTop);
    payload += ",";
    payload += std::to_string(clipRight);
    payload += ",";
    payload += std::to_string(clipBottom);
    payload += "],\"targetRect\":[";
    payload += std::to_string(targetLeft);
    payload += ",";
    payload += std::to_string(targetTop);
    payload += ",";
    payload += std::to_string(targetRight);
    payload += ",";
    payload += std::to_string(targetBottom);
    payload += "],\"visibleCheck\":";
    payload += visibleCheck ? "true" : "false";
    appendRenderEvent(
        player, "private_motion_gll", "draw_gpu",
        "__Private_Motion_GLLayer::Draw_GPU",
        payload, playerDiagnostics(player));
}

void motionTraceLayerRawProbeNative(Player *player, const void *nativeLayer,
                                    const char *samplePoint) {
    if(!g_record_layer_raw_probes || !nativeLayer) return;
    const int frameId = renderFrameIdFor(player);
    if(!captureFrameEnabled(frameId)) return;

    // Mirror of the Android oracle's frida agent sampling: the agent reads
    // mainImage via the binary's tTVPNativeBaseBitmap::GetScanLine (live
    // bitmap). GetMainImagePixelBuffer()/GetMainImagePixelBufferPitch() are
    // the upstream accessors over exactly that path (GetScanLine(0) /
    // GetPitchBytes).
    auto *layer = const_cast<tTJSNI_BaseLayer *>(
        static_cast<const tTJSNI_BaseLayer *>(nativeLayer));
    const auto *mainImage = layer->GetMainImage();
    const auto *bitmapImpl = mainImage ? mainImage->GetTexture() : nullptr;
    const int width = mainImage ? static_cast<int>(mainImage->GetWidth()) : 0;
    const int height = mainImage ? static_cast<int>(mainImage->GetHeight()) : 0;
    const int pitch = static_cast<int>(layer->GetMainImagePixelBufferPitch());
    const auto *pixels = static_cast<const unsigned char *>(
        layer->GetMainImagePixelBuffer());
    if(!mainImage || width <= 0 || height <= 0) {
        appendLayerRawProbeEvent(
            player, samplePoint, layer, false,
            "Layer has no raw main image", {}, width, height, pitch,
            mainImage, bitmapImpl, pixels);
        return;
    }
    if(pitch < width * 4) {
        appendLayerRawProbeEvent(
            player, samplePoint, layer, false,
            "Layer raw main image has invalid pitch", {}, width, height, pitch,
            mainImage, bitmapImpl, pixels);
        return;
    }
    if(!pixels) {
        appendLayerRawProbeEvent(
            player, samplePoint, layer, false,
            "Layer raw main image pixel buffer is null", {}, width, height,
            pitch,
            mainImage, bitmapImpl, pixels);
        return;
    }
    appendLayerRawProbeEvent(
        player, samplePoint, layer, true, {},
        rgbaSha256FromBgraRows(pixels, width, height, pitch),
        width, height, pitch, mainImage, bitmapImpl, pixels);
}

void motionTraceLayerRawProbe(Player *player, void *renderLayerObject,
                              const char *samplePoint) {
    if(!g_record_layer_raw_probes || !renderLayerObject) return;
    auto *layerObject = static_cast<iTJSDispatch2 *>(renderLayerObject);
    tTJSNI_BaseLayer *layer = resolveTraceLayerLikeNative(layerObject);
    if(!layer) return;
    motionTraceLayerRawProbeNative(player, layer, samplePoint);
}

bool saveLayerVisualReadbackFrameEnabled(int frameId) {
    if(!g_record_save_layer_visual_readback_probes || frameId < 0) {
        return false;
    }
    if(!captureFrameEnabled(frameId)) return false;
    const int start = std::max(0, g_save_layer_visual_readback_frame_start);
    const int count = g_save_layer_visual_readback_frame_count;
    if(frameId < start) return false;
    return count < 0 || frameId < start + count;
}

void motionTraceSaveLayerVisualReadbackRow(const void *image,
                                           int y,
                                           const void *row,
                                           int rowBytes,
                                           int width,
                                           int height,
                                           int bpp) {
    motion::Player *player = renderPlayerFor(nullptr);
    const int frameId = renderFrameIdFor(player);
    if(!saveLayerVisualReadbackFrameEnabled(frameId)) return;

    std::string payload;
    payload += "\"sourceDetail\":\"wasmtime-port-saveLayerImage-visual-readback\"";
    payload += ",\"image\":";
    payload += ptrHex(image);
    payload += ",\"row\":";
    payload += std::to_string(y);
    payload += ",\"width\":";
    payload += std::to_string(width);
    payload += ",\"height\":";
    payload += std::to_string(height);
    payload += ",\"rowBytes\":";
    payload += std::to_string(rowBytes);
    payload += ",\"bpp\":";
    payload += std::to_string(bpp);
    payload += ",\"pixelFormat\":\"rgba32-source-row\"";
    payload += ",\"rowPtr\":";
    payload += ptrHex(row);
    if(!row || rowBytes <= 0 || width <= 0 || height <= 0 ||
       y < 0 || y >= height) {
        payload += ",\"ok\":false";
        payload += ",\"error\":\"invalid visual readback row\"";
    } else {
        const auto *bytes = static_cast<const unsigned char *>(row);
        payload += ",\"ok\":true";
        payload += ",\"rowSha256\":";
        appendJsonString(
            payload,
            sha256Bytes(bytes, static_cast<std::size_t>(rowBytes)));
    }
    appendRenderEvent(player, "layer_visual_readback",
                      "save_layer_visual_readback_row",
                      "saveLayerImage_0x80963C.visual_readback_row",
                      payload, playerDiagnostics(player));
}

} // namespace motion::detail

extern "C" void krkr2_wasm_motion_trace_layer_raw_probe_native(
    const char *samplePoint, const void *nativeLayer) {
    motion::detail::motionTraceLayerRawProbeNative(
        nullptr, nativeLayer, samplePoint);
}

extern "C" void krkr2_wasm_motion_trace_save_layer_visual_readback_row(
    const void *image, int y, const void *row, int rowBytes, int width,
    int height, int bpp) {
    motion::detail::motionTraceSaveLayerVisualReadbackRow(
        image, y, row, rowBytes, width, height, bpp);
}

namespace {

int postDrawFrameIdFromMarker(tjs_int numparams, tTJSVariant **param) {
    if(numparams <= 2 || !param || !param[1] || !param[2]) return -1;
    const ttstr caseId(*param[1]);
    int base = -1;
    if(caseId == TJS_W("yuzulogo")) {
        base = g_yuzulogo_frame_base;
    } else if(caseId == TJS_W("m2logo")) {
        base = g_m2logo_frame_base;
    }
    if(base < 0) return -1;
    const auto localFrame = static_cast<int>(param[2]->AsInteger());
    if(localFrame < 0) return -1;
    return base + localFrame;
}

} // namespace

extern "C" bool krkr2_wasm_motion_trace_debug_message_probe(
    tjs_int numparams, tTJSVariant **param) {
    if(numparams < 1 || !param || !param[0]) return false;
    const ttstr marker(*param[0]);
    if(marker != TJS_W("__krkr2_motion_post_draw")) return false;
    const int frameId = postDrawFrameIdFromMarker(numparams, param);
    if(frameId >= 0 && captureFrameEnabled(frameId)) {
        void *markerBaseLayerObject = variantLayerObjectAt(
            numparams, param, 5);
        appendRenderEventForFrame(
            frameId, nullptr, "render_execute", "post_draw_marker",
            "startup.tjs.post_draw.after_onPaint",
            "\"phase\":\"post_draw\",\"ok\":true",
            "{\"captureMethod\":\"startup.tjs Debug.message marker\"}");
        motion::detail::motionTraceRenderPostDrawCanvasTextureCheckpointAtFrame(
            frameId, markerBaseLayerObject);
    }
    return true;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_render_probe_ptr() {
    if(g_render_probe_jsonl.empty())
        return 0;
    return static_cast<int>(
        reinterpret_cast<std::uintptr_t>(g_render_probe_jsonl.data()));
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_render_probe_len() {
    return static_cast<int>(g_render_probe_jsonl.size());
}

EMSCRIPTEN_KEEPALIVE
void krkr2_wasm_clear_render_probe() {
    g_render_probe_jsonl.clear();
    g_render_probe_seq = 0;
}

EMSCRIPTEN_KEEPALIVE
void krkr2_wasm_set_record_layer_raw_probes(int enabled) {
    g_record_layer_raw_probes = enabled != 0;
}

EMSCRIPTEN_KEEPALIVE
void krkr2_wasm_set_record_save_layer_visual_readback_probes(
    int enabled, int frame_start, int frame_count) {
    g_record_save_layer_visual_readback_probes = enabled != 0;
    g_save_layer_visual_readback_frame_start = frame_start;
    g_save_layer_visual_readback_frame_count = frame_count;
}

EMSCRIPTEN_KEEPALIVE
void krkr2_wasm_set_render_capture_frame_filter(int frame_start,
                                                int frame_count) {
    g_capture_frame_start = frame_start;
    g_capture_frame_count = frame_count;
}

EMSCRIPTEN_KEEPALIVE
void krkr2_wasm_set_render_case_frame_base(const char *case_id,
                                           int case_id_len,
                                           int frame_base) {
    if(!case_id || case_id_len <= 0) return;
    const std::string id(case_id, static_cast<std::size_t>(case_id_len));
    if(id == "yuzulogo") {
        g_yuzulogo_frame_base = frame_base;
    } else if(id == "m2logo") {
        g_m2logo_frame_base = frame_base;
    }
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_motion_trace_frame_count() {
    return traceState().frameCounter;
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_motion_trace_ptr() {
    if(g_motion_trace_jsonl.empty()) return 0;
    return static_cast<int>(
        reinterpret_cast<std::uintptr_t>(g_motion_trace_jsonl.data()));
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_motion_trace_len() {
    return static_cast<int>(g_motion_trace_jsonl.size());
}

EMSCRIPTEN_KEEPALIVE
void krkr2_wasm_clear_motion_trace() {
    g_motion_trace_jsonl.clear();
    g_motion_trace_frame_json.clear();
    g_motion_trace_first_layer = true;
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_tick_count() {
    return g_wasmtime_tick_count;
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_tick_diag_mask() {
    return g_wasmtime_tick_diag_mask;
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_tick_diag_ever_mask() {
    return g_wasmtime_tick_diag_ever_mask;
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_window_count() {
    return static_cast<int>(TVPGetWindowCount());
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_window_count_peak() {
    return g_wasmtime_window_count_peak;
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_event_hook_slot_count() {
    return TVPWasmtimeGetContinuousEventHookSlotCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_event_hook_live_count() {
    return TVPWasmtimeGetContinuousEventHookLiveCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_handler_slot_count() {
    return TVPWasmtimeGetContinuousHandlerSlotCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_handler_live_count() {
    return TVPWasmtimeGetContinuousHandlerLiveCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_handler_add_count() {
    return TVPWasmtimeGetContinuousHandlerAddCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_handler_remove_count() {
    return TVPWasmtimeGetContinuousHandlerRemoveCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_deliver_count() {
    return TVPWasmtimeGetContinuousDeliverCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_handler_invoke_count() {
    return TVPWasmtimeGetContinuousHandlerInvokeCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_handler_failure_count() {
    return TVPWasmtimeGetContinuousHandlerFailureCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_tick_count() {
    return TVPWasmtimeGetContinuousTickCount();
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_continuous_tick_at(int index) {
    return TVPWasmtimeGetContinuousTickAt(index);
}

EMSCRIPTEN_KEEPALIVE
int krkr2_wasm_get_event_disabled() {
    return TVPEventDisabled ? 1 : 0;
}

} // extern "C"

int wasmtimeStartupFrom(const char *path, int len) {
    if(!path || len <= 0) {
        setError("empty xp3 path");
        return 0;
    }

    const std::string xp3Path(path, static_cast<size_t>(len));
    return runWithErrors([&]() {
        setStage("TVPMainScene::startupFrom");
        auto *scene = TVPMainScene::GetInstance();
        if(!scene)
            scene = TVPMainScene::CreateInstance();
        if(!scene)
            throw std::runtime_error("TVPMainScene is unavailable");
        if(!scene->startupFrom(xp3Path)) {
            throw std::runtime_error("TVPMainScene::startupFrom returned false");
        }
        setStage("");
    });
}

int wasmtimeGetErrorPtr() {
    return static_cast<int>(reinterpret_cast<uintptr_t>(g_error.c_str()));
}

int wasmtimeGetErrorLen() {
    return static_cast<int>(g_error.size());
}
