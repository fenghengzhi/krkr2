// Frida agent for the Kirikiroid2 1.3.9 motion_playback Android oracle.
// The active render_path surface is intentionally semantic-only: it samples
// Player draw routing, prepared items, render-command construction and the
// Canvas/accurate-SLA execution envelopes.  Optional Layer/Bitmap raw-image
// probes remain disabled until those engine-internal offsets are independently
// rebased and runtime-validated for 1.3.9.

'use strict';

// Kirikiroid2 1.3.9 Android arm64-v8a (libgame.so) offsets.  MotionPlayer and
// Debug.message entries below were mapped independently in all four 1.3.9
// reference binaries; only the Android arm64 addresses are installed here.
const PLAYER_PROGRESS_COMPAT_OFF = 0x6CFE78;
const PLAYER_INIT_NON_EMOTE_OFF  = 0x6B365C;
const PLAYER_PARSE_PARAM_OFF     = 0x6B1718;
const PLAYER_PARSE_PARAM_LIST_OFF = 0x6B202C;
const PLAYER_BIND_PARAM_OFF      = 0x6C4668;
const PLAYER_EVALUATE_TIMELINE_OFF = 0x699AE4;
const PLAYER_SUB_MOTION_OFF      = 0x6BE0C0;
const PLAYER_PHASE3_LAST_OFF     = 0x6BD908;
const PLAYER_DRAW_COMPAT_OFF     = 0x6D3398;
const PLAYER_DRAW_D3D_OFF        = 0x6D2F70;
const PLAYER_DRAW_SLA_OFF        = 0x6D2A38;
const PLAYER_RENDER_PREPARE_OFF  = 0x6D2544;
const PLAYER_APPLY_PREPARED_PROJECTION_OFF = 0x6D2644;
const PLAYER_BUILD_ITEMS_OFF     = 0x6BF714;
const PLAYER_BUILD_COMMANDS_OFF  = 0x6C2208;
const PLAYER_ACCURATE_SLA_RENDER_OFF = 0x6C7088;
const PLAYER_RENDER_EXECUTE_OFF  = 0x6C4820;
const PLAYER_UPDATE_LAYER_AFTER_DRAW_OFF = 0x6CBBB8;
const DEBUG_MESSAGE_OFF          = 0xA178BC;

// Retired 1.4.4-only deep diagnostic addresses.  They are deliberately kept
// out of installHook; naming them here documents why the corresponding helper
// code must not be enabled by the semantic render_path capability.
const PLAYER_RENDER_EXECUTE_DIRECT_OPERATE_AFFINE_CALL_OFF = 0x6C8D74;
const PLAYER_RENDER_EXECUTE_DIRECT_OPERATE_AFFINE_AFTER_OFF = 0x6C8D78;
const SOFTWARE_OPERATE_RECT_HELPER_OFF = 0x85F718;
const LAYER_FILL_RECT_OFF        = 0x80EBAC;
const LAYER_SAVE_LAYER_IMAGE_OFF = 0x80963C;
const TVP_SAVE_AS_PNG_OFF        = 0x83EDA4;
const DRAW_DEVICE_UPLOAD_LAYER_TO_TEXTURE_OFF = 0x850528;
const BITMAP_GET_SCANLINE_OFF    = 0xA75DE4;
// TVPContinuousEventDispatch calls TVPGetRoughTickCount32 @0xA2A4DC at
// 0x8DECD0, then consumes the returned tick at 0x8DECD4.
// Restrict the differential clock override to that one call edge: timers,
// input, storage and worker-thread clocks must continue to see real time.
const TVP_CONTINUOUS_EVENT_TICK_RETURN_OFF = 0x8DECD4;
const TVP_GET_ROUGH_TICK_COUNT32_OFF = 0xA2A4DC;
const LAYER_CLASS_ID_OFF = 0x1ADE668;
const LAYER_NATIVE_MAIN_IMAGE_OFF = 280;
const BITMAP_NATIVE_IMPL_OFF = 88;
const TVP_ALPHA_BLEND_D_SLOT_OFF = 0x1CE1650;
const TVP_ALPHA_BLEND_DO_SLOT_OFF = 0x1CBCB68;

const STATIC_PARSE_PROJECTION = 'static_parse-semantic-v1';
const STATIC_PARSE_SAMPLE_POINTS = {
    init_non_emote_enter: 'initNonEmoteMotionLike_0x6B365C.enter',
    init_non_emote_leave: 'initNonEmoteMotionLike_0x6B365C.leave',
    parse_parameter_enter: 'appendParameterEntryLike_0x6B1718.enter',
    parse_parameter_leave: 'appendParameterEntryLike_0x6B1718.leave',
    parse_parameter_list_enter: 'parseParameterList_guess.enter',
    parse_parameter_list_leave: 'parseParameterList_guess.leave',
};

const INIT_MOTION_PROJECTION = 'init-motion-semantic-v1';
const INIT_MOTION_SAMPLE_POINTS = {
    init_non_emote_enter: 'initNonEmoteMotionLike_0x6B365C.enter',
    init_non_emote_leave: 'initNonEmoteMotionLike_0x6B365C.leave',
};

const TRACE_FLATTEN_PROJECTION = 'trace_flatten-semantic-v1';
const TRACE_FLATTEN_SAMPLE_POINT = 'progressCompat.phase3-end.pre-cleanup';
const TRACE_FLATTEN_SAMPLE_ORDER = 'phase3-return-order';
const TRACE_FLATTEN_MAX_NODES = 512;
const TRACE_FLATTEN_ABS_FLOAT_LIMIT = 1000000.0;
const FRAME_SELECTION_SPEC = __FRAME_SELECTION_PROJECTION_JSON__;
const FRAME_SELECTION_PROJECTION = FRAME_SELECTION_SPEC.projection;
const FRAME_SELECTION_SAMPLE_POINT = FRAME_SELECTION_SPEC.samplePoint;
const FRAME_SELECTION_NODE_FIELDS = FRAME_SELECTION_SPEC.nodeFields || [];

const NODE_STRIDE = 2632;
const PARAM_ENTRY_STRIDE = 56;

const STAGE_STATIC_PARSE = 'static_parse';
const STAGE_INIT_MOTION = 'init_motion';
const STAGE_VARIABLE_BINDING = 'variable_binding';
const STAGE_FRAME_SELECTION = 'frame_selection';
const STAGE_SUB_MOTION_DECISION = 'sub_motion_decision';
const STAGE_TRACE_FLATTEN = 'trace_flatten';
const STAGE_DRAW_DISPATCH = 'draw_dispatch';
const STAGE_RENDER_PREPARE = 'render_prepare';
const STAGE_RENDER_COMMANDS = 'render_commands';
const STAGE_RENDER_EXECUTE = 'render_execute';
const STAGE_LAYER_SAVE = 'layer_save';
const STAGE_LAYER_RAW_PROBE = 'layer_raw_probe';
const STAGE_LAYER_VISUAL_READBACK = 'layer_visual_readback';

const ALL_STAGES = [
    STAGE_STATIC_PARSE,
    STAGE_INIT_MOTION,
    STAGE_VARIABLE_BINDING,
    STAGE_FRAME_SELECTION,
    STAGE_SUB_MOTION_DECISION,
    STAGE_TRACE_FLATTEN,
];

const RENDER_STAGES = [
    STAGE_DRAW_DISPATCH,
    STAGE_RENDER_PREPARE,
    STAGE_RENDER_COMMANDS,
    STAGE_RENDER_EXECUTE,
];

const SUPPORTED_STAGES = new Set([
    STAGE_TRACE_FLATTEN,
    ...RENDER_STAGES,
]);

const NODE_OFF = {
    parameterEntry: 8,
    coordinateMode: 24,
    nodeType: 28,
    parentIndex: 36,
    flags: 44,
    activeSlot: 1392,
    active: 1505,
    visible: 1506,
    flipX: 1507,
    flipY: 1508,
    posX: 1512,
    posY: 1520,
    posZ: 1528,
    angleDeg: 1536,
    scaleX: 1544,
    scaleY: 1552,
    slantX: 1560,
    slantY: 1568,
    opacity: 1576,
    stencilType: 52,
};

const PARAM_OFF = {
    id: 0,
    discretization: 20,
    rangeBegin: 24,
    rangeEnd: 32,
    value: 40,
    mode: 48,
};

const PLAYER_OFF = {
    internalAssignRequested: 613,
    d3dDrawMode: 909,
};

let base = null;
let hooked = false;
let recording = false;
let events = [];
let frameCounter = 0;
let nextFrameId = 0;
let seqCounter = 0;
let startTimeMs = 0;
let enabledStages = new Set(ALL_STAGES);
let virtualContinuousTickFps = 0;
let virtualContinuousTickBase = null;
let virtualContinuousTickIndex = 0;

let inCompat = false;
let samplesInFrame = [];
let capturedObjthis = null;
let currentFrameId = null;
let capturedDeltaMs = null;
let lastCompletedFrameId = null;
let lastCompletedTopPlayer = null;
let currentRenderFrameId = null;
let currentRenderPlayer = null;
let drawIdCounter = 0;
let activeDrawContexts = [];
let recordRenderStepCheckpoints = false;
let recordLayerRawProbes = false;
let recordSaveLayerVisualReadbackProbes = false;
let saveLayerVisualReadbackFrameStart = 0;
let saveLayerVisualReadbackFrameCount = 1;
let captureFrameStart = 0;
let captureFrameCount = -1;
let renderCaseFrameBases = {};
let activeSaveLayerImageContexts = [];
let activePngSaveContexts = [];
let activeRenderExecuteContexts = [];
let lastRenderLayerObject = null;
let lastDrawTargetObject = null;
let lastSlaRenderTargetObject = null;
let lastSlaRenderNativeLayer = null;
let lastFullCanvasUploadCandidate = null;
let pendingExecutePostUploadCheckpoint = null;
let finalFramebufferExports = null;
let directOperateAffineFuncCallHookCache = {};
let nativeInstanceSupportCache = {};
let propGetFunctionCache = {};
let funcCallFunctionCache = {};
let adaptorRenderTargetCache = {};
let bitmapGetScanLineFunctionCache = {};
let textureGetPixelDataFunctionCache = {};
let textureGetPitchFunctionCache = {};

const FINAL_FRAMEBUFFER_WIDTH = 1920;
const FINAL_FRAMEBUFFER_HEIGHT = 1080;
const GL_RGBA = 0x1908;
const GL_UNSIGNED_BYTE = 0x1401;
const GL_PACK_ALIGNMENT = 0x0D05;
const GL_FRAMEBUFFER = 0x8D40;
const GL_FRAMEBUFFER_BINDING = 0x8CA6;
const GL_NO_ERROR = 0;

function ensureBase() {
    if (base !== null) return base;
    base = Module.findBaseAddress('libgame.so');
    if (base === null) {
        throw new Error('libgame.so not loaded in target process');
    }
    return base;
}

function hexOff(offset) {
    return '0x' + offset.toString(16).toUpperCase();
}

function attachAt(offset, name, callbacks) {
    try {
        Interceptor.attach(ensureBase().add(offset), callbacks);
    } catch (e) {
        throw new Error(
            'failed to hook ' + name + ' at libgame.so+' +
            hexOff(offset) + ': ' + e
        );
    }
}

function stageEnabled(stage) {
    return enabledStages.has(stage);
}

function captureFrameEnabled(frameId) {
    if (frameId === null || frameId === undefined) return false;
    const start = Math.max(0, captureFrameStart | 0);
    const count = captureFrameCount | 0;
    if (frameId < start) return false;
    return count < 0 || frameId < start + count;
}

function ptrHex(value) {
    if (value === null || value === undefined) return null;
    try {
        const p = ptr(value);
        if (p.isNull()) return null;
        return p.toString();
    } catch (e) {
        return String(value);
    }
}

function findExportAny(names, modules) {
    for (const name of names) {
        for (const moduleName of modules) {
            try {
                const addr = Module.findExportByName(moduleName, name);
                if (addr) return addr;
            } catch (e) {}
        }
    }
    return null;
}

function ensureFinalFramebufferExports() {
    if (finalFramebufferExports) {
        return { ok: true, exports: finalFramebufferExports };
    }
    const modules = [null, 'libGLESv2.so', 'libGLESv3.so'];
    const glReadPixels = findExportAny(['glReadPixels'], modules);
    if (!glReadPixels) {
        return { ok: false, error: 'glReadPixels export not found' };
    }
    const exports = {
        glReadPixels: new NativeFunction(
            glReadPixels, 'void',
            ['int', 'int', 'int', 'int', 'int', 'int', 'pointer']),
        glGetError: null,
        glGetIntegerv: null,
        glPixelStorei: null,
        glBindFramebuffer: null,
    };
    const glGetError = findExportAny(['glGetError'], modules);
    if (glGetError) {
        exports.glGetError = new NativeFunction(glGetError, 'int', []);
    }
    const glGetIntegerv = findExportAny(['glGetIntegerv'], modules);
    if (glGetIntegerv) {
        exports.glGetIntegerv = new NativeFunction(
            glGetIntegerv, 'void', ['int', 'pointer']);
    }
    const glPixelStorei = findExportAny(['glPixelStorei'], modules);
    if (glPixelStorei) {
        exports.glPixelStorei = new NativeFunction(
            glPixelStorei, 'void', ['int', 'int']);
    }
    const glBindFramebuffer = findExportAny(['glBindFramebuffer'], modules);
    if (glBindFramebuffer) {
        exports.glBindFramebuffer = new NativeFunction(
            glBindFramebuffer, 'void', ['int', 'int']);
    }
    finalFramebufferExports = exports;
    return { ok: true, exports: exports };
}

function readGlInt(gl, pname) {
    if (!gl.glGetIntegerv) return null;
    const out = Memory.alloc(4);
    gl.glGetIntegerv(pname, out);
    return out.readS32();
}

function readFinalFramebufferSnapshot(frameId) {
    const resolved = ensureFinalFramebufferExports();
    const diagnostics = {
        captureMethod: 'glReadPixels-current-framebuffer',
        origin: 'bottom-left',
        channelOrder: 'RGBA',
        frameId: frameId,
    };
    if (!resolved.ok) {
        return {
            ok: false,
            error: resolved.error,
            diagnostics: diagnostics,
        };
    }
    const gl = resolved.exports;
    const width = FINAL_FRAMEBUFFER_WIDTH;
    const height = FINAL_FRAMEBUFFER_HEIGHT;
    const pitch = width * 4;
    const size = pitch * height;
    const oldPackAlignment = readGlInt(gl, GL_PACK_ALIGNMENT);
    const oldFramebuffer = readGlInt(gl, GL_FRAMEBUFFER_BINDING);
    diagnostics.oldPackAlignment = oldPackAlignment;
    diagnostics.readFramebuffer = oldFramebuffer;
    try {
        if (gl.glPixelStorei) {
            gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        }
        const pixels = Memory.alloc(size);
        gl.glReadPixels(
            0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        const glError = gl.glGetError ? gl.glGetError() : GL_NO_ERROR;
        diagnostics.glError = glError;
        if (glError !== GL_NO_ERROR) {
            return {
                ok: false,
                error: 'glReadPixels failed with GL error 0x' +
                    glError.toString(16),
                width: width,
                height: height,
                pitch: pitch,
                pixelFormat: 'rgba32-bottom-left',
                diagnostics: diagnostics,
            };
        }
        return {
            ok: true,
            width: width,
            height: height,
            pitch: pitch,
            pixelFormat: 'rgba32-bottom-left',
            data: pixels.readByteArray(size),
            diagnostics: diagnostics,
        };
    } catch (e) {
        return {
            ok: false,
            error: String(e),
            width: width,
            height: height,
            pitch: pitch,
            pixelFormat: 'rgba32-bottom-left',
            diagnostics: diagnostics,
        };
    } finally {
        try {
            if (gl.glPixelStorei && oldPackAlignment !== null) {
                gl.glPixelStorei(GL_PACK_ALIGNMENT, oldPackAlignment);
            }
        } catch (e) {}
    }
}

function sendFinalFramebufferCheckpoint(frameId, snapshot, samplePoint) {
    const payload = {
        type: 'render_image_checkpoint',
        source: 'android-frida-current-framebuffer',
        phase: 'post_draw',
        samplePoint: samplePoint,
        frameId: frameId,
        player: null,
        layerObject: null,
        ok: snapshot.ok === true,
        width: snapshot.width || FINAL_FRAMEBUFFER_WIDTH,
        height: snapshot.height || FINAL_FRAMEBUFFER_HEIGHT,
        pitch: snapshot.pitch || FINAL_FRAMEBUFFER_WIDTH * 4,
        pixelFormat: snapshot.pixelFormat || 'rgba32-bottom-left',
        diagnostics: snapshot.diagnostics || {},
    };
    if (!snapshot.ok) {
        payload.error = snapshot.error || 'final framebuffer snapshot failed';
        send(payload);
        return;
    }
    send(payload, snapshot.data);
}

function readDrawDeviceUploadTextureSnapshot(textureLikeObject) {
    const tex = ptr(textureLikeObject);
    const diagnostics = {
        captureMethod: 'DrawDevice_UploadLayerToTexture.raw-data',
        textureLikeObject: ptrHex(tex),
    };
    if (tex.isNull()) {
        return { ok: false, error: 'null upload texture object', diagnostics };
    }
    try {
        const width = readU32(tex, 12);
        const height = readU32(tex, 16);
        const sourcePitch = readU32(tex, 20);
        const format = readU32(tex, 24);
        const sourceData = readPointer(tex, 32);
        diagnostics.width = width;
        diagnostics.height = height;
        diagnostics.sourcePitch = sourcePitch;
        diagnostics.format = format;
        diagnostics.sourceData = ptrHex(sourceData);
        if (!width || !height || width <= 0 || height <= 0 ||
            width > 8192 || height > 8192) {
            return {
                ok: false,
                error: 'invalid upload texture dimensions',
                width,
                height,
                diagnostics,
            };
        }
        if (!sourceData) {
            return {
                ok: false,
                error: 'upload texture has no source data',
                width,
                height,
                diagnostics,
            };
        }
        const rowBytes = width * 4;
        if (sourcePitch < rowBytes) {
            return {
                ok: false,
                error: 'upload texture pitch is smaller than row bytes',
                width,
                height,
                pitch: sourcePitch,
                diagnostics,
            };
        }
        const packedSize = rowBytes * height;
        const packed = Memory.alloc(packedSize);
        for (let y = 0; y < height; y++) {
            Memory.copy(
                packed.add(y * rowBytes),
                sourceData.add(y * sourcePitch),
                rowBytes);
        }
        return {
            ok: true,
            width,
            height,
            pitch: rowBytes,
            pixelFormat: 'rgba32',
            data: packed.readByteArray(packedSize),
            diagnostics,
        };
    } catch (e) {
        return { ok: false, error: String(e), diagnostics };
    }
}

function rememberFullCanvasUpload(textureLikeObject) {
    if (!recordRenderStepCheckpoints || !recording ||
        !stageEnabled(STAGE_RENDER_EXECUTE)) {
        return;
    }
    const tex = ptr(textureLikeObject);
    if (tex.isNull()) return;
    let width = 0;
    let height = 0;
    let sourcePitch = 0;
    let format = 0;
    let sourceData = null;
    try {
        width = readU32(tex, 12);
        height = readU32(tex, 16);
        sourcePitch = readU32(tex, 20);
        format = readU32(tex, 24);
        sourceData = readPointer(tex, 32);
    } catch (e) {
        return;
    }
    if (width !== FINAL_FRAMEBUFFER_WIDTH ||
        height !== FINAL_FRAMEBUFFER_HEIGHT ||
        sourcePitch < width * 4 ||
        !sourceData) {
        return;
    }
    lastFullCanvasUploadCandidate = {
        textureLikeObject: tex,
        width,
        height,
        sourcePitch,
        format,
        sourceData,
        sequence: seqCounter,
    };
    if (pendingExecutePostUploadCheckpoint &&
        Number.isInteger(pendingExecutePostUploadCheckpoint.frameId)) {
        const pending = pendingExecutePostUploadCheckpoint;
        pendingExecutePostUploadCheckpoint = null;
        sendStoredCanvasUploadCheckpoint(
            pending.frameId,
            pending.markerType || 'execute_post_upload',
            'execute_post',
            pending.samplePoint ||
                'execute_leave.after-next-drawdevice-upload');
    }
}

function sendStoredCanvasUploadCheckpoint(
    frameId, markerType, phase, samplePoint) {
    const checkpointPhase = phase || 'post_draw';
    const checkpointSamplePoint = samplePoint ||
        'startup.tjs.post_draw.after_onPaint.drawdevice-upload';
    const candidate = lastFullCanvasUploadCandidate;
    if (!candidate) {
        send({
            type: 'render_image_checkpoint',
            source: 'android-frida-drawdevice-upload-texture',
            phase: checkpointPhase,
            samplePoint: checkpointSamplePoint,
            frameId,
            player: null,
            layerObject: null,
            ok: false,
            pixelFormat: 'rgba32',
            error: 'no 1920x1080 DrawDevice upload was captured before marker',
            diagnostics: {
                markerType,
                captureMethod: 'DrawDevice_UploadLayerToTexture.raw-data',
            },
        });
        return false;
    }
    const snapshot =
        readDrawDeviceUploadTextureSnapshot(candidate.textureLikeObject);
    if (!snapshot.ok) {
        send({
            type: 'render_image_checkpoint',
            source: 'android-frida-drawdevice-upload-texture',
            phase: checkpointPhase,
            samplePoint: checkpointSamplePoint,
            frameId,
            player: null,
            layerObject: null,
            ok: false,
            pixelFormat: 'rgba32',
            error: snapshot.error || 'DrawDevice upload snapshot failed',
            diagnostics: Object.assign({}, snapshot.diagnostics || {}, {
                markerType,
                rememberedTextureLikeObject:
                    ptrHex(candidate.textureLikeObject),
                rememberedSourceData: ptrHex(candidate.sourceData),
                rememberedSequence: candidate.sequence,
            }),
        });
        return false;
    }
    const diagnostics = Object.assign({}, snapshot.diagnostics || {}, {
        markerType,
        sampleTiming: checkpointSamplePoint,
        phase: checkpointPhase,
        rememberedSourceData: ptrHex(candidate.sourceData),
        uploadSequence: candidate.sequence,
    });
    send({
        type: 'render_image_checkpoint',
        source: 'android-frida-drawdevice-upload-texture',
        phase: checkpointPhase,
        samplePoint: checkpointSamplePoint,
        frameId,
        player: null,
        layerObject: null,
        ok: true,
        width: snapshot.width,
        height: snapshot.height,
        pitch: snapshot.pitch,
        pixelFormat: snapshot.pixelFormat || 'rgba32',
        diagnostics,
    }, snapshot.data);
    return true;
}

function scheduleExecutePostUploadCheckpoint(player, samplePoint) {
    const frameId = renderFrameIdFor(player);
    if (!Number.isInteger(frameId) || !captureFrameEnabled(frameId)) return;
    pendingExecutePostUploadCheckpoint = {
        frameId,
        markerType: 'execute_post',
        samplePoint,
    };
}

function canonicalPtr(value) {
    const p = ptr(value);
    if (Process.pointerSize !== 8 || p.isNull()) return p;
    // Android 11+ arm64 can hand Frida top-byte-tagged heap pointers
    // (TBI/MTE style, e.g. 0xb4...). Some Frida APIs want the canonical
    // address, while reads can still need the original tag.
    return p.and(ptr('0x00ffffffffffffff'));
}

function retagPtr(value, tagSource) {
    try {
        const low = canonicalPtr(value);
        if (Process.pointerSize !== 8 || low.isNull() ||
                tagSource === null || tagSource === undefined) {
            return low;
        }
        const tag = ptr(tagSource).and(ptr('0xff00000000000000'));
        return tag.isNull() ? low : low.or(tag);
    } catch (e) {
        return null;
    }
}

function addPtrCandidate(out, seen, value) {
    try {
        const p = ptr(value);
        if (p.isNull()) return;
        const key = p.toString();
        if (!seen[key]) {
            seen[key] = true;
            out.push(p);
        }
    } catch (e) {
    }
}

function ptrCandidates(value, options) {
    const out = [];
    const seen = {};
    addPtrCandidate(out, seen, value);
    try {
        addPtrCandidate(out, seen, canonicalPtr(value));
    } catch (e) {
    }
    if (options && options.tagSource !== undefined) {
        addPtrCandidate(out, seen, retagPtr(value, options.tagSource));
    }
    if (options && Array.isArray(options.tagSources)) {
        for (const source of options.tagSources) {
            addPtrCandidate(out, seen, retagPtr(value, source));
        }
    }
    return out;
}

function ptrHexCanonical(value) {
    if (value === null || value === undefined) return null;
    try {
        const p = canonicalPtr(value);
        if (p.isNull()) return null;
        return p.toString();
    } catch (e) {
        return ptrHex(value);
    }
}

function ptrToNumber(value) {
    try {
        return parseInt(canonicalPtr(value).toString(), 16);
    } catch (e) {
        return 0;
    }
}

function readWithCandidates(p, off, fn, options) {
    for (const base of ptrCandidates(p, options)) {
        try {
            return fn(base.add(off));
        } catch (e) {
        }
    }
    return null;
}

function readS32(p, off, options) {
    return readWithCandidates(p, off, (q) => q.readS32(), options);
}

function readU32(p, off, options) {
    return readWithCandidates(p, off, (q) => q.readU32(), options);
}

function readU8(p, off, options) {
    return readWithCandidates(p, off, (q) => q.readU8(), options);
}

function readBool(p, off) {
    const v = readU8(p, off);
    return v === null ? null : v !== 0;
}

function readDouble(p, off, options) {
    const v = readWithCandidates(p, off, (q) => q.readDouble(), options);
    if (v !== null) {
        return Number.isFinite(v) ? v : null;
    }
    return null;
}

function readFloat(p, off, options) {
    const v = readWithCandidates(p, off, (q) => q.readFloat(), options);
    if (v !== null) {
        return Number.isFinite(v) ? v : null;
    }
    return null;
}

function readPointer(p, off, options) {
    const q = readWithCandidates(p, off, (r) => r.readPointer(), options);
    return q === null || q.isNull() ? null : q;
}

function rangeHasReadableBytes(p, byteCount) {
    try {
        const range = Process.findRangeByAddress(p);
        if (range === null || range.protection.indexOf('r') < 0) return false;
        const start = ptrToNumber(p);
        const rangeStart = ptrToNumber(range.base);
        const rangeEnd = rangeStart + range.size;
        return start >= rangeStart && start + byteCount <= rangeEnd;
    } catch (e) {
        return false;
    }
}

function probeReadableBytes(p, byteCount) {
    try {
        p.readU8();
        if (byteCount > 1) p.add(byteCount - 1).readU8();
        return true;
    } catch (e) {
        return false;
    }
}

function findReadablePtr(p, byteCount, options) {
    for (const q of ptrCandidates(p, options)) {
        if (rangeHasReadableBytes(q, byteCount) ||
                probeReadableBytes(q, byteCount)) {
            return q;
        }
    }
    return null;
}

function hasReadableBytes(p, byteCount, options) {
    return findReadablePtr(p, byteCount, options) !== null;
}

function requireReadable(p, byteCount, name, options) {
    const q = findReadablePtr(p, byteCount, options);
    if (q !== null) return q;
    const tried = ptrCandidates(p, options).map((v) => v.toString()).join(',');
    throw new Error(
        name + ' is not readable: raw=' + ptrHex(p) +
        ' canonical=' + ptrHexCanonical(p) +
        (tried ? ' tried=' + tried : '')
    );
}

function readArgInt(arg) {
    try { return arg.toInt32(); } catch (e) {}
    try { return parseInt(arg.toString(), 16) | 0; } catch (e) {}
    return null;
}

function getCachedNativeFunction(cache, fnPtr, retType, argTypes) {
    const key = ptrHex(fnPtr);
    if (!key) return null;
    if (!cache[key]) {
        cache[key] = new NativeFunction(fnPtr, retType, argTypes);
    }
    return cache[key];
}

function readVariantObject(variantPtr) {
    try {
        const variant = ptr(variantPtr);
        if (variant.isNull()) {
            return {
                object: null,
                objThis: null,
                type: null,
                error: 'null variant',
            };
        }
        const type = readS32(variant, 16);
        const object = readPointer(variant, 0);
        if (object === null) {
            return {
                object: null,
                objThis: null,
                type: type,
                error: 'variant object is null',
            };
        }
        return {
            object: object,
            objThis: readPointer(variant, 8),
            type: type,
            error: null,
        };
    } catch (e) {
        return {
            object: null,
            objThis: null,
            type: null,
            error: String(e),
        };
    }
}

function readVariantScalar(variantPtr) {
    try {
        const variant = ptr(variantPtr);
        if (variant.isNull()) {
            return { type: null, int32: null, double: null, error: 'null variant' };
        }
        const type = readS32(variant, 16);
        return {
            type: type,
            int32: readS32(variant, 0),
            double: readDouble(variant, 0),
            error: null,
        };
    } catch (e) {
        return { type: null, int32: null, double: null, error: String(e) };
    }
}

function readVariantArg(argArrayPtr, index) {
    try {
        const array = ptr(argArrayPtr);
        if (array.isNull()) {
            return { variant: null, object: null, scalar: null, error: 'null arg array' };
        }
        const variant = array.add(index * Process.pointerSize).readPointer();
        if (variant.isNull()) {
            return { variant: null, object: null, scalar: null, error: 'null arg variant' };
        }
        return {
            variant: variant,
            object: readVariantObject(variant),
            scalar: readVariantScalar(variant),
            error: null,
        };
    } catch (e) {
        return { variant: null, object: null, scalar: null, error: String(e) };
    }
}

function readVariantText(variantPtr) {
    try {
        const variant = ptr(variantPtr);
        if (variant.isNull()) {
            return { value: null, type: null, error: 'null variant' };
        }
        const type = readS32(variant, 16);
        const stringPtr = readPointer(variant, 0);
        if (stringPtr === null) {
            return { value: null, type: type, error: 'variant string is null' };
        }
        return {
            value: readVariantString(stringPtr),
            type: type,
            error: null,
        };
    } catch (e) {
        return { value: null, type: null, error: String(e) };
    }
}

function ptrEqual(a, b) {
    if (!a || !b) return false;
    try {
        return ptr(a).equals(ptr(b));
    } catch (e) {
        return false;
    }
}

function readNativeLayerPixel(nativeLayer, layerObject, x, y) {
    const native = nativeLayer ? ptr(nativeLayer) : NULL;
    const diagnostics = {
        layerObject: ptrHex(layerObject),
        nativeLayer: ptrHex(native),
        captureMethod: 'bitmap-get-scanline-pixel',
        x: x,
        y: y,
    };
    if (native.isNull()) {
        return { ok: false, error: 'null native layer', diagnostics: diagnostics };
    }
    try {
        const mainImage = readPointer(native, LAYER_NATIVE_MAIN_IMAGE_OFF);
        diagnostics.mainImage = ptrHex(mainImage);
        if (mainImage === null) {
            return { ok: false, error: 'layer has no main image', diagnostics: diagnostics };
        }
        const bitmapImpl = readPointer(mainImage, BITMAP_NATIVE_IMPL_OFF);
        diagnostics.bitmapImpl = ptrHex(bitmapImpl);
        if (bitmapImpl === null) {
            return { ok: false, error: 'main image has no bitmap impl', diagnostics: diagnostics };
        }
        const width = readU32(bitmapImpl, 12);
        const height = readU32(bitmapImpl, 16);
        diagnostics.width = width;
        diagnostics.height = height;
        if (!width || !height || width <= 0 || height <= 0 ||
            width > 8192 || height > 8192) {
            return { ok: false, error: 'invalid bitmap dimensions', diagnostics: diagnostics };
        }
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return { ok: false, error: 'pixel coordinate out of range', diagnostics: diagnostics };
        }
        const getScanLinePtr = ensureBase().add(BITMAP_GET_SCANLINE_OFF);
        const getScanLineFn = getCachedNativeFunction(
            bitmapGetScanLineFunctionCache,
            getScanLinePtr,
            'pointer', ['pointer', 'uint']);
        diagnostics.getScanLineFunction = ptrHex(getScanLinePtr);
        const row = getScanLineFn(mainImage, y);
        diagnostics.rowPtr = ptrHex(row);
        if (!row || row.isNull()) {
            return { ok: false, error: 'bitmap scanline is null', diagnostics: diagnostics };
        }
        const bgra = row.add(x * 4).readU32();
        return {
            ok: true,
            bgra: '0x' + ('00000000' + bgra.toString(16)).slice(-8),
            b: bgra & 0xff,
            g: (bgra >>> 8) & 0xff,
            r: (bgra >>> 16) & 0xff,
            a: (bgra >>> 24) & 0xff,
            width: width,
            height: height,
            diagnostics: diagnostics,
        };
    } catch (e) {
        return { ok: false, error: String(e), diagnostics: diagnostics };
    }
}

function readNativeLayerFirstPixel(nativeLayer, layerObject) {
    const pixel = readNativeLayerPixel(nativeLayer, layerObject, 0, 0);
    if (pixel.diagnostics) {
        pixel.diagnostics.captureMethod = 'bitmap-get-scanline-first-pixel';
    }
    return pixel;
}

function readLayerFirstPixel(layerObject) {
    const resolved = resolveLayerNativeInstance(layerObject);
    if (!resolved.layer) {
        return {
            ok: false,
            error: resolved.error || 'no layer',
            diagnostics: {
                layerObject: ptrHex(layerObject),
                nativeLayer: null,
                classId: resolved.classId || null,
                hresult: resolved.hresult || null,
            },
        };
    }
    const pixel = readNativeLayerFirstPixel(resolved.layer, layerObject);
    if (pixel.diagnostics) {
        pixel.diagnostics.classId = resolved.classId || null;
    }
    return pixel;
}

function readLayerDrawFaceDiagnostics(layerObject) {
    const resolved = resolveLayerNativeInstance(layerObject);
    if (!resolved.layer) {
        return {
            ok: false,
            error: resolved.error || 'no layer',
            nativeLayer: null,
            classId: resolved.classId || null,
            hresult: resolved.hresult || null,
        };
    }
    return {
        ok: true,
        nativeLayer: ptrHex(resolved.layer),
        classId: resolved.classId || null,
        offset400S32: readS32(resolved.layer, 400),
        offset404S32: readS32(resolved.layer, 404),
        offset408U8: readU8(resolved.layer, 408),
        offset408S32: readS32(resolved.layer, 408),
    };
}

function readLayerPixel(layerObject, x, y) {
    const resolved = resolveLayerNativeInstance(layerObject);
    if (!resolved.layer) {
        return {
            ok: false,
            error: resolved.error || 'no layer',
            diagnostics: {
                layerObject: ptrHex(layerObject),
                nativeLayer: null,
                classId: resolved.classId || null,
                hresult: resolved.hresult || null,
                x: x,
                y: y,
            },
        };
    }
    const pixel = readNativeLayerPixel(resolved.layer, layerObject, x, y);
    if (pixel.diagnostics) {
        pixel.diagnostics.classId = resolved.classId || null;
    }
    return pixel;
}

function readLayerPixelSamples(layerObject, points) {
    const samples = [];
    for (const point of points) {
        samples.push({
            x: point[0],
            y: point[1],
            pixel: readLayerPixel(layerObject, point[0], point[1]),
        });
    }
    return samples;
}

function unpackRectPair(leftTop, rightBottom) {
    const lt = ptr(leftTop);
    const rb = ptr(rightBottom);
    return {
        left: lt.toInt32(),
        top: lt.shr(32).toInt32(),
        right: rb.toInt32(),
        bottom: rb.shr(32).toInt32(),
    };
}

function readTexturePixel(texture, x, y) {
    const tex = texture ? ptr(texture) : NULL;
    const diagnostics = {
        texture: ptrHex(tex),
        captureMethod: 'texture-pixel-data',
        x: x,
        y: y,
    };
    if (tex.isNull()) {
        return { ok: false, error: 'null texture', diagnostics: diagnostics };
    }
    try {
        const vtable = readPointer(tex, 0);
        diagnostics.vtable = ptrHex(vtable);
        const getPixelDataPtr = readPointer(vtable, 64);
        const getPitchPtr = readPointer(vtable, 80);
        diagnostics.getPixelDataFunction = ptrHex(getPixelDataPtr);
        diagnostics.getPitchFunction = ptrHex(getPitchPtr);
        const getPixelData = getCachedNativeFunction(
            textureGetPixelDataFunctionCache,
            getPixelDataPtr,
            'pointer', ['pointer']);
        const getPitch = getCachedNativeFunction(
            textureGetPitchFunctionCache,
            getPitchPtr,
            'int', ['pointer']);
        const pixelData = getPixelData(tex);
        const pitch = getPitch(tex);
        diagnostics.pixelData = ptrHex(pixelData);
        diagnostics.pitch = pitch;
        if (!pixelData || pixelData.isNull()) {
            return { ok: false, error: 'null pixel data', diagnostics: diagnostics };
        }
        if (pitch <= 0 || x < 0 || y < 0) {
            return { ok: false, error: 'invalid pitch or coordinate', diagnostics: diagnostics };
        }
        const bgra = pixelData.add(y * pitch + x * 4).readU32();
        return {
            ok: true,
            bgra: '0x' + ('00000000' + bgra.toString(16)).slice(-8),
            b: bgra & 0xff,
            g: (bgra >>> 8) & 0xff,
            r: (bgra >>> 16) & 0xff,
            a: (bgra >>> 24) & 0xff,
            diagnostics: diagnostics,
        };
    } catch (e) {
        return { ok: false, error: String(e), diagnostics: diagnostics };
    }
}

function readTexturePixelSamples(texture, points) {
    const samples = [];
    for (const point of points) {
        samples.push({
            x: point[0],
            y: point[1],
            pixel: readTexturePixel(texture, point[0], point[1]),
        });
    }
    return samples;
}

function resolveLayerNativeInstance(layerObject) {
    const obj = ptr(layerObject);
    if (obj.isNull()) return { layer: null, error: 'null layer object' };
    try {
        const vtable = obj.readPointer();
        const fnPtr = vtable.add(200).readPointer();
        const fn = getCachedNativeFunction(
            nativeInstanceSupportCache, fnPtr,
            'int64', ['pointer', 'int64', 'uint32', 'pointer']);
        if (!fn) return { layer: null, error: 'NativeInstanceSupport missing' };
        const out = Memory.alloc(Process.pointerSize);
        out.writePointer(NULL);
        const classId = ensureBase().add(LAYER_CLASS_ID_OFF).readU32();
        const hr = fn(obj, 2, classId, out);
        const layer = out.readPointer();
        if ((Number(hr) & 0x80000000) !== 0 || layer.isNull()) {
            return {
                layer: null,
                error: 'NativeInstanceSupport failed',
                hresult: String(hr),
                classId: classId,
            };
        }
        return { layer: layer, error: null, classId: classId };
    } catch (e) {
        return { layer: null, error: String(e) };
    }
}

function nativeInstanceForClassId(objectValue, classId) {
    const obj = ptr(objectValue);
    if (obj.isNull()) return { native: null, hresult: null, error: 'null object' };
    try {
        const vtable = obj.readPointer();
        const fnPtr = vtable.add(200).readPointer();
        const fn = getCachedNativeFunction(
            nativeInstanceSupportCache, fnPtr,
            'int64', ['pointer', 'int64', 'uint32', 'pointer']);
        if (!fn) {
            return { native: null, hresult: null, error: 'NativeInstanceSupport missing' };
        }
        const out = Memory.alloc(Process.pointerSize);
        out.writePointer(NULL);
        const hr = fn(obj, 2, classId, out);
        const native = out.readPointer();
        if ((Number(hr) & 0x80000000) !== 0 || native.isNull()) {
            return { native: null, hresult: String(hr), error: 'no native instance' };
        }
        return { native: native, hresult: String(hr), error: null };
    } catch (e) {
        return { native: null, hresult: null, error: String(e) };
    }
}

function propGetObject(dispatchObject, name, flag) {
    try {
        const obj = ptr(dispatchObject);
        if (obj.isNull()) return { object: null, error: 'null dispatch object' };
        const vtable = obj.readPointer();
        const fnPtr = vtable.add(32).readPointer();
        const fn = getCachedNativeFunction(
            propGetFunctionCache, fnPtr,
            'int64', ['pointer', 'uint32', 'pointer', 'pointer', 'pointer', 'pointer']);
        const result = Memory.alloc(32);
        Memory.writeByteArray(result, new Uint8Array(32));
        const memberName = Memory.allocUtf16String(name);
        const hr = fn(obj, flag || 0, memberName, NULL, result, obj);
        if ((Number(hr) & 0x80000000) !== 0) {
            return { object: null, hresult: String(hr), error: 'PropGet failed' };
        }
        const variant = readVariantObject(result);
        return {
            object: variant.object,
            objThis: variant.objThis,
            type: variant.type,
            hresult: String(hr),
            error: variant.error,
        };
    } catch (e) {
        return { object: null, error: String(e) };
    }
}

function funcCallObject(dispatchObject, name, flag) {
    try {
        const obj = ptr(dispatchObject);
        if (obj.isNull()) return { object: null, error: 'null dispatch object' };
        const vtable = obj.readPointer();
        const fnPtr = vtable.add(16).readPointer();
        const fn = getCachedNativeFunction(
            funcCallFunctionCache, fnPtr,
            'int64',
            ['pointer', 'uint32', 'pointer', 'pointer', 'pointer', 'int',
             'pointer', 'pointer']);
        const result = Memory.alloc(32);
        Memory.writeByteArray(result, new Uint8Array(32));
        const memberName = Memory.allocUtf16String(name);
        const hr = fn(obj, flag || 0, memberName, NULL, result, 0, NULL, obj);
        if ((Number(hr) & 0x80000000) !== 0) {
            return { object: null, hresult: String(hr), error: 'FuncCall failed' };
        }
        const variant = readVariantObject(result);
        return {
            object: variant.object,
            objThis: variant.objThis,
            type: variant.type,
            hresult: String(hr),
            error: variant.error,
        };
    } catch (e) {
        return { object: null, error: String(e) };
    }
}

function probeLayerImageObject(layerObject) {
    const resolved = resolveLayerNativeInstance(layerObject);
    if (!resolved.layer) {
        return {
            ok: false,
            resolved: resolved,
            error: resolved.error || 'not a Layer',
        };
    }
    try {
        const mainImage = readPointer(resolved.layer, LAYER_NATIVE_MAIN_IMAGE_OFF);
        const bitmapImpl = mainImage ? readPointer(mainImage, BITMAP_NATIVE_IMPL_OFF) : null;
        return {
            ok: mainImage !== null && bitmapImpl !== null,
            resolved: resolved,
            mainImage: ptrHex(mainImage),
            bitmapImpl: ptrHex(bitmapImpl),
            error: mainImage === null
                ? 'layer has no main image'
                : bitmapImpl === null
                    ? 'main image has no bitmap impl'
                    : null,
        };
    } catch (e) {
        return { ok: false, resolved: resolved, error: String(e) };
    }
}

function resolveLayerObjectForCheckpoint(objectValue) {
    if (!objectValue) return { object: null, diagnostics: { error: 'null object' } };
    const key = ptrHex(objectValue);
    if (key && adaptorRenderTargetCache[key]) {
        return adaptorRenderTargetCache[key];
    }
    const directProbe = probeLayerImageObject(objectValue);
    if (directProbe.ok) {
        const result = {
            object: objectValue,
            diagnostics: {
                route: 'direct_layer',
                classId: directProbe.resolved.classId || null,
                mainImage: directProbe.mainImage || null,
            },
        };
        if (key) adaptorRenderTargetCache[key] = result;
        return result;
    }

    const propDiagnostics = [];
    for (const propSpec of [
        { name: 'targetLayer', flag: 0, access: 'PropGet' },
        { name: 'layerTreeOwnerInterface', flag: 0x1000, access: 'FuncCall' },
    ]) {
        const propName = propSpec.name;
        const prop = propSpec.access === 'FuncCall'
            ? funcCallObject(objectValue, propName, propSpec.flag)
            : propGetObject(objectValue, propName, propSpec.flag);
        const propDiag = {
            property: propName,
            access: propSpec.access,
            object: ptrHex(prop.object),
            type: prop.type === undefined ? null : prop.type,
            hresult: prop.hresult || null,
            error: prop.error || null,
        };
        if (!prop.object) {
            propDiagnostics.push(propDiag);
            continue;
        }
        const resolved = probeLayerImageObject(prop.object);
        propDiag.layerProbe = {
            ok: resolved.ok,
            error: resolved.error || null,
            classId: resolved.resolved ? resolved.resolved.classId || null : null,
            hresult: resolved.resolved ? resolved.resolved.hresult || null : null,
            mainImage: resolved.mainImage || null,
            bitmapImpl: resolved.bitmapImpl || null,
        };
        propDiagnostics.push(propDiag);
        if (!resolved.ok) continue;
        const result = {
            object: prop.object,
            diagnostics: {
                route: 'dispatch_property_layer',
                sourceObject: key,
                property: propName,
                propertyType: prop.type,
                layerClassId: resolved.resolved.classId || null,
                mainImage: resolved.mainImage || null,
            },
        };
        if (key) adaptorRenderTargetCache[key] = result;
        return result;
    }

    for (let classId = 1; classId < 256; classId++) {
        const native = nativeInstanceForClassId(objectValue, classId);
        if (!native.native) continue;
        for (const variantOff of [40, 20, 0]) {
            const candidate = readVariantObject(native.native.add(variantOff));
            if (!candidate || !candidate.object) continue;
            const resolved = probeLayerImageObject(candidate.object);
            if (!resolved.ok) continue;
            const result = {
                object: candidate.object,
                diagnostics: {
                    route: 'native_variant_layer',
                    sourceObject: key,
                    nativeClassId: classId,
                    nativeObject: ptrHex(native.native),
                    variantOffset: variantOff,
                    layerClassId: resolved.resolved.classId || null,
                    mainImage: resolved.mainImage || null,
                },
            };
            if (key) adaptorRenderTargetCache[key] = result;
            return result;
        }
    }

    return {
        object: null,
        diagnostics: {
            route: 'unresolved_object',
            sourceObject: key,
            directError: directProbe.error || null,
            directClassId: directProbe.resolved ? directProbe.resolved.classId || null : null,
            directHresult: directProbe.resolved ? directProbe.resolved.hresult || null : null,
            properties: propDiagnostics,
        },
    };
}

function readNativeLayerImageSnapshot(nativeLayer, layerObject) {
    const native = nativeLayer ? ptr(nativeLayer) : NULL;
    if (native.isNull()) {
        return {
            ok: false,
            error: 'null native layer',
            diagnostics: {
                layerObject: ptrHex(layerObject),
                nativeLayer: null,
            },
        };
    }
    const diagnostics = {
        layerObject: ptrHex(layerObject),
        nativeLayer: ptrHex(native),
    };
    try {
        const mainImage = readPointer(native, LAYER_NATIVE_MAIN_IMAGE_OFF);
        diagnostics.mainImage = ptrHex(mainImage);
        if (mainImage === null) return { ok: false, error: 'layer has no main image' };
        const bitmapImpl = readPointer(mainImage, BITMAP_NATIVE_IMPL_OFF);
        diagnostics.bitmapImpl = ptrHex(bitmapImpl);
        if (bitmapImpl === null) {
            return {
                ok: false,
                error: 'main image has no bitmap impl',
                diagnostics: diagnostics,
            };
        }
        const width = readU32(bitmapImpl, 12);
        const height = readU32(bitmapImpl, 16);
        diagnostics.width = width;
        diagnostics.height = height;
        if (!width || !height || width <= 0 || height <= 0 ||
            width > 8192 || height > 8192) {
            return {
                ok: false,
                error: 'invalid bitmap dimensions',
                width: width,
                height: height,
                diagnostics: diagnostics,
            };
        }
        const vtable = bitmapImpl.readPointer();
        diagnostics.vtable = ptrHex(vtable);
        const getScanLinePtr = ensureBase().add(BITMAP_GET_SCANLINE_OFF);
        const getScanLineFn = getCachedNativeFunction(
            bitmapGetScanLineFunctionCache,
            getScanLinePtr,
            'pointer', ['pointer', 'uint']);
        const pitch = width * 4;
        const packedSize = width * height * 4;
        diagnostics.captureMethod = 'bitmap-get-scanline';
        diagnostics.getScanLineFunction = ptrHex(getScanLinePtr);
        diagnostics.pitch = pitch;
        diagnostics.packedSize = packedSize;
        const packed = Memory.alloc(packedSize);
        for (let y = 0; y < height; y++) {
            diagnostics.currentRow = y;
            const row = getScanLineFn(mainImage, y);
            diagnostics.rowPtr = ptrHex(row);
            if (!row || row.isNull()) {
                diagnostics.failedRow = y;
                return {
                    ok: false,
                    error: 'bitmap scanline is null',
                    width: width,
                    height: height,
                    pitch: pitch,
                    diagnostics: diagnostics,
                };
            }
            Memory.copy(
                packed.add(y * width * 4),
                row,
                width * 4);
        }
        delete diagnostics.currentRow;
        delete diagnostics.rowPtr;
        return {
            ok: true,
            width: width,
            height: height,
            pitch: pitch,
            pixelFormat: 'bgra32',
            data: packed.readByteArray(packedSize),
            diagnostics: diagnostics,
        };
    } catch (e) {
        return {
            ok: false,
            error: String(e),
            diagnostics: diagnostics,
        };
    }
}

function readLayerImageSnapshot(layerObject) {
    if (!layerObject) {
        return {
            ok: false,
            error: 'null layer object',
            diagnostics: {
                layerObject: null,
                nativeLayer: null,
            },
        };
    }
    const resolved = resolveLayerNativeInstance(layerObject);
    if (!resolved.layer) {
        return {
            ok: false,
            error: resolved.error || 'no layer',
            diagnostics: {
                layerObject: ptrHex(layerObject),
                nativeLayer: null,
                classId: resolved.classId || null,
                hresult: resolved.hresult || null,
            },
        };
    }
    const snapshot = readNativeLayerImageSnapshot(resolved.layer, layerObject);
    if (snapshot.diagnostics) {
        snapshot.diagnostics.classId = resolved.classId || null;
    }
    return snapshot;
}

function readResolvedLayerImageSnapshot(requestedObject) {
    const resolved = resolveLayerObjectForCheckpoint(requestedObject);
    const objectForRead = resolved.object || requestedObject;
    const snapshot = readLayerImageSnapshot(objectForRead);
    if (!snapshot.diagnostics) snapshot.diagnostics = {};
    snapshot.diagnostics.requestedLayerObject = ptrHex(requestedObject);
    snapshot.diagnostics.resolvedLayerObject = ptrHex(objectForRead);
    snapshot.diagnostics.resolution =
        resolved.diagnostics || { route: resolved.object ? 'unknown' : 'direct' };
    return {
        snapshot: snapshot,
        objectForRead: objectForRead,
    };
}

function sendRenderImageCheckpoint(player, layerObject, phase, samplePoint,
                                   frameIdOverride, fallbackLayerObject) {
    if (!recordRenderStepCheckpoints || !recording ||
        !stageEnabled(STAGE_RENDER_EXECUTE)) {
        return;
    }
    const frameId = Number.isInteger(frameIdOverride)
        ? frameIdOverride : renderFrameIdFor(player);
    if (frameId === null || frameId === undefined) return;
    if (!captureFrameEnabled(frameId)) return;
    let resolvedSnapshot = readResolvedLayerImageSnapshot(layerObject);
    let fallbackUsed = false;
    if (!resolvedSnapshot.snapshot.ok && fallbackLayerObject &&
        !ptrEqual(layerObject, fallbackLayerObject)) {
        const fallbackSnapshot =
            readResolvedLayerImageSnapshot(fallbackLayerObject);
        if (fallbackSnapshot.snapshot.ok) {
            resolvedSnapshot = fallbackSnapshot;
            fallbackUsed = true;
        }
    }
    const snapshot = resolvedSnapshot.snapshot;
    if (snapshot.diagnostics) {
        snapshot.diagnostics.fallbackLayerObject = ptrHex(fallbackLayerObject);
        snapshot.diagnostics.fallbackUsed = fallbackUsed;
    }
    const payload = {
        type: 'render_image_checkpoint',
        source: 'android-frida-layer-main-image',
        phase: phase,
        samplePoint: samplePoint,
        frameId: frameId,
        player: ptrHex(player),
        layerObject: ptrHex(resolvedSnapshot.objectForRead),
        requestedLayerObject: ptrHex(layerObject),
        fallbackLayerObject: ptrHex(fallbackLayerObject),
        fallbackUsed: fallbackUsed,
        ok: snapshot.ok === true,
        width: snapshot.width || null,
        height: snapshot.height || null,
        pitch: snapshot.pitch || null,
        pixelFormat: 'rgba32',
        diagnostics: snapshot.diagnostics || {},
    };
    if (!snapshot.ok) {
        payload.error = snapshot.error || 'snapshot failed';
        send(payload);
        return;
    }
    send(payload, snapshot.data);
}

function sendRenderNativeImageCheckpoint(player, nativeLayer, layerObject,
                                         phase, samplePoint, diagnostics,
                                         frameIdOverride) {
    if (!recordRenderStepCheckpoints || !recording ||
        !stageEnabled(STAGE_RENDER_EXECUTE)) {
        return;
    }
    const frameId = Number.isInteger(frameIdOverride)
        ? frameIdOverride : renderFrameIdFor(player);
    if (frameId === null || frameId === undefined) return;
    if (!captureFrameEnabled(frameId)) return;
    const snapshot = readNativeLayerImageSnapshot(nativeLayer, layerObject);
    const mergedDiagnostics = {};
    if (snapshot.diagnostics) {
        for (const key of Object.keys(snapshot.diagnostics)) {
            mergedDiagnostics[key] = snapshot.diagnostics[key];
        }
    }
    if (diagnostics) {
        for (const key of Object.keys(diagnostics)) {
            mergedDiagnostics[key] = diagnostics[key];
        }
    }
    const payload = {
        type: 'render_image_checkpoint',
        source: 'android-frida-native-layer-main-image',
        phase: phase,
        samplePoint: samplePoint,
        frameId: frameId,
        player: ptrHex(player),
        layerObject: ptrHex(layerObject),
        nativeLayer: ptrHex(nativeLayer),
        ok: snapshot.ok === true,
        width: snapshot.width || null,
        height: snapshot.height || null,
        pitch: snapshot.pitch || null,
        pixelFormat: 'rgba32',
        diagnostics: mergedDiagnostics,
    };
    if (!snapshot.ok) {
        payload.error = snapshot.error || 'snapshot failed';
        send(payload);
        return;
    }
    send(payload, snapshot.data);
}

function sendLayerRawProbe(player, layerObject, nativeLayer, samplePoint,
                           semanticPayload, diagnostics) {
    if (!recordLayerRawProbes || !recording ||
        !stageEnabled(STAGE_LAYER_RAW_PROBE)) {
        return;
    }
    let frameId = renderFrameIdFor(player);
    if (!captureFrameEnabled(frameId)) return;
    const snapshot = nativeLayer
        ? readNativeLayerImageSnapshot(nativeLayer, layerObject)
        : readLayerImageSnapshot(layerObject);
    const diag = {};
    const snapshotDiag = snapshot.diagnostics || {};
    for (const k of Object.keys(snapshotDiag)) diag[k] = snapshotDiag[k];
    if (diagnostics) {
        for (const k of Object.keys(diagnostics)) diag[k] = diagnostics[k];
    }
    const payload = semanticPayload || {};
    payload.schema = 'motion-render-stage-oracle-v1-event';
    payload.source = 'android-frida-layer-main-image-raw-probe';
    payload.stage = STAGE_LAYER_RAW_PROBE;
    payload.kind = 'raw_probe';
    payload.samplePoint = samplePoint || 'layer_raw_probe';
    if (frameId !== null && frameId !== undefined) {
        payload.frameId = frameId;
    }
    payload.player = ptrHex(player || currentRenderPlayer);
    payload.ok = snapshot.ok === true;
    payload.width = snapshot.width || null;
    payload.height = snapshot.height || null;
    payload.pitch = snapshot.pitch || null;
    payload.pixelFormat = snapshot.pixelFormat || 'bgra32';
    payload.nativeLayer = diag.nativeLayer || ptrHex(nativeLayer);
    payload.mainImage = diag.mainImage || null;
    payload.bitmapImpl = diag.bitmapImpl || null;
    payload.buffer = diag.buffer || null;
    payload.diagnostics = diag;
    payload.seq = seqCounter++;
    payload.timeMs = Date.now() - startTimeMs;
    if (!snapshot.ok) {
        payload.error = snapshot.error || 'snapshot failed';
        events.push(payload);
        send({
            type: 'layer_raw_probe',
            ok: false,
            seq: payload.seq,
            samplePoint: payload.samplePoint,
            frameId: payload.frameId,
            error: payload.error,
        });
        return;
    }
    events.push(payload);
    send({
        type: 'layer_raw_probe',
        ok: true,
        seq: payload.seq,
        samplePoint: payload.samplePoint,
        frameId: payload.frameId,
        width: snapshot.width,
        height: snapshot.height,
        pitch: snapshot.pitch,
        pixelFormat: snapshot.pixelFormat || 'bgra32',
    }, snapshot.data);
}

function saveLayerVisualReadbackFrameEnabled(frameId) {
    if (!recordSaveLayerVisualReadbackProbes) return false;
    if (frameId === null || frameId === undefined) return false;
    if (!captureFrameEnabled(frameId)) return false;
    const start = Math.max(0, saveLayerVisualReadbackFrameStart | 0);
    const count = saveLayerVisualReadbackFrameCount | 0;
    if (frameId < start) return false;
    return count < 0 || frameId < start + count;
}

function sendSaveLayerVisualReadbackRow(ctx, rowIndex, rowPtr) {
    if (!recordSaveLayerVisualReadbackProbes || !recording ||
        !stageEnabled(STAGE_LAYER_VISUAL_READBACK) || !ctx) {
        return;
    }
    const frameId = renderFrameIdFor(ctx.player);
    if (!saveLayerVisualReadbackFrameEnabled(frameId)) return;
    const width = ctx.width || 0;
    const height = ctx.height || 0;
    const rowBytes = ctx.rowBytes || 0;
    const payload = {
        schema: 'motion-render-stage-oracle-v1-event',
        source: 'android-frida-saveLayerImage-visual-readback',
        stage: STAGE_LAYER_VISUAL_READBACK,
        kind: 'save_layer_visual_readback_row',
        samplePoint: 'saveLayerImage_0x80963C.visual_readback_row',
        frameId: frameId,
        player: ptrHex(ctx.player),
        image: ptrHex(ctx.image),
        row: rowIndex,
        width: width,
        height: height,
        rowBytes: rowBytes,
        bpp: ctx.bpp || 32,
        pixelFormat: 'rgba32-source-row',
        rowPtr: ptrHex(rowPtr),
        ok: true,
        diagnostics: {
            nativeLayer: ptrHex(ctx.nativeLayer),
            mainImage: ptrHex(ctx.mainImage),
            bitmapImpl: ptrHex(ctx.bitmapImpl),
            saveLayerImageMainImageA1Plus280: ptrHex(ctx.mainImage),
            pngSaveFunction: hexOff(TVP_SAVE_AS_PNG_OFF),
            getScanLineFunction: hexOff(BITMAP_GET_SCANLINE_OFF),
        },
        seq: seqCounter++,
        timeMs: Date.now() - startTimeMs,
    };
    let data = null;
    try {
        if (!rowPtr || ptr(rowPtr).isNull()) {
            payload.ok = false;
            payload.error = 'visual readback row pointer is null';
        } else if (rowBytes <= 0 || width <= 0 || height <= 0 ||
                   rowIndex < 0 || rowIndex >= height) {
            payload.ok = false;
            payload.error = 'invalid visual readback row metadata';
        } else {
            data = ptr(rowPtr).readByteArray(rowBytes);
        }
    } catch (e) {
        payload.ok = false;
        payload.error = String(e);
    }
    events.push(payload);
    send({
        type: 'layer_visual_readback_probe',
        ok: payload.ok === true,
        seq: payload.seq,
        samplePoint: payload.samplePoint,
        frameId: payload.frameId,
        row: rowIndex,
        width: width,
        height: height,
        rowBytes: rowBytes,
        pixelFormat: payload.pixelFormat,
        error: payload.error || null,
    }, data);
}

function readD0(ctx) {
    try {
        const raw = ctx.d0.toString();
        if (raw.indexOf('0x') === 0 || raw.indexOf('0X') === 0) return null;
        const v = parseFloat(raw);
        return Number.isFinite(v) ? v : null;
    } catch (e) {
        return null;
    }
}

function readD0Raw(ctx) {
    try { return ctx.d0.toString(); } catch (e) { return null; }
}

function emit(stage, kind, payload) {
    if (!recording || !stageEnabled(stage)) return;
    if (inCompat && !captureFrameEnabled(currentFrameId)) return;
    const ev = payload || {};
    ev.schema = 'motion-stage-oracle-v1-event';
    ev.stage = stage;
    ev.kind = kind;
    ev.seq = seqCounter++;
    ev.timeMs = Date.now() - startTimeMs;
    if (inCompat) {
        ev.frameId = currentFrameId;
        if (stage !== STAGE_TRACE_FLATTEN) {
            ev.objthis = ptrHex(capturedObjthis);
        }
    }
    events.push(ev);
}

function emitStaticParse(kind, semanticPayload, diagnostics) {
    if (!recording || !stageEnabled(STAGE_STATIC_PARSE)) return;
    if (inCompat && !captureFrameEnabled(currentFrameId)) return;
    const diag = diagnostics || {};
    if (inCompat) {
        diag.frameId = currentFrameId;
        diag.objthis = ptrHex(capturedObjthis);
    }
    const ev = semanticPayload || {};
    ev.schema = 'motion-stage-oracle-v1-event';
    ev.stage = STAGE_STATIC_PARSE;
    ev.kind = kind;
    ev.projection = STATIC_PARSE_PROJECTION;
    ev.samplePoint = STATIC_PARSE_SAMPLE_POINTS[kind] || kind;
    ev.diagnostics = diag;
    ev.seq = seqCounter++;
    ev.timeMs = Date.now() - startTimeMs;
    events.push(ev);
}

function emitInitMotion(kind, semanticPayload, diagnostics) {
    if (!recording || !stageEnabled(STAGE_INIT_MOTION)) return;
    if (inCompat && !captureFrameEnabled(currentFrameId)) return;
    const diag = diagnostics || {};
    if (inCompat) {
        diag.frameId = currentFrameId;
        diag.objthis = ptrHex(capturedObjthis);
    }
    const ev = semanticPayload || {};
    ev.schema = 'motion-stage-oracle-v1-event';
    ev.stage = STAGE_INIT_MOTION;
    ev.kind = kind;
    ev.projection = INIT_MOTION_PROJECTION;
    ev.samplePoint = INIT_MOTION_SAMPLE_POINTS[kind] || kind;
    ev.diagnostics = diag;
    ev.seq = seqCounter++;
    ev.timeMs = Date.now() - startTimeMs;
    events.push(ev);
}

function semanticFrameSelectionNode(raw) {
    if (raw === null || raw === undefined) return null;
    const out = {};
    for (const field of FRAME_SELECTION_NODE_FIELDS) {
        out[field] = Object.prototype.hasOwnProperty.call(raw, field)
            ? raw[field]
            : null;
    }
    return out;
}

function emitFrameSelection(kind, semanticPayload, diagnostics) {
    if (!recording || !stageEnabled(STAGE_FRAME_SELECTION)) return;
    if (inCompat && !captureFrameEnabled(currentFrameId)) return;
    const diag = diagnostics || {};
    if (inCompat) {
        diag.objthis = ptrHex(capturedObjthis);
    }
    const ev = semanticPayload || {};
    ev.schema = 'motion-stage-oracle-v1-event';
    ev.stage = STAGE_FRAME_SELECTION;
    ev.kind = kind;
    ev.projection = FRAME_SELECTION_PROJECTION;
    ev.samplePoint = FRAME_SELECTION_SAMPLE_POINT;
    ev.diagnostics = diag;
    ev.seq = seqCounter++;
    ev.timeMs = Date.now() - startTimeMs;
    if (inCompat) {
        ev.frameId = currentFrameId;
    }
    events.push(ev);
}

function renderFrameIdFor(player) {
    if (currentRenderFrameId !== null && currentRenderFrameId !== undefined) {
        return currentRenderFrameId;
    }
    if (lastCompletedFrameId === null || lastCompletedFrameId === undefined) {
        return null;
    }
    if (lastCompletedTopPlayer === null || lastCompletedTopPlayer === undefined) {
        return lastCompletedFrameId;
    }
    if (player === null || player === undefined) {
        return lastCompletedFrameId;
    }
    return ptrHex(player) === ptrHex(lastCompletedTopPlayer)
        ? lastCompletedFrameId
        : null;
}

function postDrawFrameIdFromMarkerArgs(argArray, numParams) {
    if (numParams <= 2) return null;
    const caseArg = readVariantArg(argArray, 1);
    const frameArg = readVariantArg(argArray, 2);
    if (!caseArg.variant || !frameArg.scalar) return null;
    const caseText = readVariantText(caseArg.variant);
    let baseFrame = null;
    if (Object.prototype.hasOwnProperty.call(
            renderCaseFrameBases, caseText.value)) {
        baseFrame = renderCaseFrameBases[caseText.value];
    } else if (caseText.value === 'yuzulogo') {
        baseFrame = 0;
    } else if (caseText.value === 'm2logo') {
        baseFrame = 63;
    }
    if (baseFrame === null) return null;
    const localFrame = frameArg.scalar.int32;
    if (!Number.isInteger(localFrame) || localFrame < 0) return null;
    return baseFrame + localFrame;
}

function emitRender(stage, kind, semanticPayload, diagnostics, samplePoint) {
    if (!recording || !stageEnabled(stage)) return;
    let player = currentRenderPlayer;
    if (!player && diagnostics && diagnostics.player) {
        try { player = ptr(diagnostics.player); } catch (e) { player = null; }
    }
    const frameId = renderFrameIdFor(player);
    if (frameId === null || frameId === undefined) return;
    if (!captureFrameEnabled(frameId)) return;
    const diag = diagnostics || {};
    const ev = semanticPayload || {};
    ev.schema = 'motion-render-stage-oracle-v1-event';
    ev.captureContract = 'motion-render-boundaries-v1';
    ev.stage = stage;
    ev.kind = kind;
    ev.samplePoint = samplePoint || kind;
    if (stage === STAGE_RENDER_EXECUTE &&
        (kind === 'execute_enter' || kind === 'execute_leave')) {
        ev.executeBoundary = ev.accurateSla === true
            ? 'Player.renderAccurateSeparateLayerAdaptor'
            : 'Player.renderToCanvas';
        ev.samplePoint = ev.executeBoundary +
            (kind === 'execute_enter' ? '.enter' : '.leave');
    }
    ev.frameId = frameId;
    ev.player = ptrHex(player);
    ev.diagnostics = diag;
    ev.seq = seqCounter++;
    ev.timeMs = Date.now() - startTimeMs;
    events.push(ev);
}

function currentRenderExecuteContext() {
    if (activeRenderExecuteContexts.length === 0) return null;
    return activeRenderExecuteContexts[activeRenderExecuteContexts.length - 1];
}

function emitDirectExecuteProbe(ctx, phase, samplePoint, extra) {
    if (!recording || !stageEnabled(STAGE_RENDER_EXECUTE) || !ctx) return;
    const frameId = renderFrameIdFor(ctx.player);
    if (frameId === null || frameId === undefined) return;
    if (!captureFrameEnabled(frameId)) return;
    const payload = extra || {};
    payload.schema = 'motion-render-stage-oracle-v1-event';
    payload.source = 'android-frida-direct-execute-probe';
    payload.stage = STAGE_RENDER_EXECUTE;
    payload.kind = 'direct_execute_probe';
    payload.samplePoint = samplePoint || 'sub_6C7440.direct.operateAffine';
    payload.frameId = frameId;
    payload.player = ptrHex(ctx.player);
    payload.probePhase = phase;
    payload.branch = 'direct.operateAffine';
    payload.seq = seqCounter++;
    payload.timeMs = Date.now() - startTimeMs;
    events.push(payload);
}

function currentDrawContextFor(player) {
    if (activeDrawContexts.length === 0) return null;
    const playerHex = ptrHex(player);
    for (let i = activeDrawContexts.length - 1; i >= 0; --i) {
        const ctx = activeDrawContexts[i];
        if (!ctx) continue;
        if (!playerHex || ctx.playerHex === playerHex) return ctx;
    }
    return null;
}

function checkpointTargetForDrawContext(drawCtx, fallbackTarget) {
    return drawCtx && drawCtx.targetObject ? drawCtx.targetObject : fallbackTarget;
}

function drawPathSummary(ctx) {
    const route = ctx.route || (ctx.steps.length === 0 ? 'no_target' : 'failed');
    return {
        route: route,
        steps: ctx.steps.slice(),
        prepareCalled: ctx.prepareCalled,
        prepareOk: ctx.prepareOk,
        d3dDrawModeAfterPrepare: ctx.d3dDrawModeAfterPrepare,
        renderToCanvasCalled: ctx.renderToCanvasCalled,
        updateLayerAfterDrawCalled: ctx.updateLayerAfterDrawCalled,
        internalAssignRequested: ctx.internalAssignRequested,
    };
}

function beginDrawContext(player, argVariant) {
    const targetVariant = readVariantObject(argVariant);
    const ctx = {
        drawId: drawIdCounter++,
        player: player,
        playerHex: ptrHex(player),
        argVariant: argVariant,
        targetObject: targetVariant.object,
        targetObjThis: targetVariant.objThis,
        targetVariantType: targetVariant.type,
        targetVariantError: targetVariant.error,
        steps: [],
        emittedSteps: {},
        route: null,
        prepareCalled: false,
        prepareOk: null,
        d3dDrawModeAfterPrepare: null,
        renderToCanvasCalled: false,
        updateLayerAfterDrawCalled: false,
        internalAssignRequested: null,
    };
    activeDrawContexts.push(ctx);
    return ctx;
}

function finishDrawContext(ctx) {
    if (!ctx) return;
    for (let i = activeDrawContexts.length - 1; i >= 0; --i) {
        if (activeDrawContexts[i] === ctx) {
            activeDrawContexts.splice(i, 1);
            break;
        }
    }
}

function setDrawRoute(ctx, route) {
    if (ctx && route) ctx.route = route;
}

function isDirectAdaptorRoute(ctx) {
    return !!ctx && (
        ctx.route === 'd3d_adaptor' ||
        ctx.route === 'separate_layer_adaptor'
    );
}

function emitDrawStep(ctx, drawStep, outcome, route, extra) {
    if (!ctx) return;
    const stepIndex = ctx.steps.length;
    ctx.steps.push(drawStep);
    ctx.emittedSteps[drawStep] = true;
    if (route) ctx.route = route;
    const payload = {
        drawId: ctx.drawId,
        stepIndex: stepIndex,
        drawStep: drawStep,
        outcome: outcome,
    };
    if (route) payload.route = route;
    if (extra) {
        for (const k of Object.keys(extra)) payload[k] = extra[k];
    }
    emitRender(STAGE_DRAW_DISPATCH, 'draw_step', payload, {
        addr: PLAYER_DRAW_COMPAT_OFF,
        player: ctx.playerHex,
        argVariant: ptrHex(ctx.argVariant),
        targetVariantType: ctx.targetVariantType,
        targetObject: ptrHex(ctx.targetObject),
        targetObjThis: ptrHex(ctx.targetObjThis),
        targetError: ctx.targetVariantError,
    }, 'Player_draw_0x6D3398.' + drawStep);
}

function ensureDrawTargetCheckMisses(ctx) {
    if (!ctx) return;
    if (!ctx.emittedSteps.target_check_d3d) {
        emitDrawStep(ctx, 'target_check_d3d', 'miss');
    }
    if (!ctx.emittedSteps.target_check_sla) {
        emitDrawStep(ctx, 'target_check_sla', 'miss');
    }
}

function safeUtf16(ptrValue, length) {
    try {
        if (!ptrValue || ptrValue.isNull()) return null;
        if (length !== undefined && length !== null) {
            if (length < 0 || length > 2048) return null;
            return ptrValue.readUtf16String(length);
        }
        return ptrValue.readUtf16String();
    } catch (e) {
        return null;
    }
}

function readVariantString(vstr) {
    if (!vstr || vstr.isNull()) return '';

    const candidates = [
        { lenOff: 60, longOff: 8, shortOff: 16 },
        { lenOff: 64, longOff: 8, shortOff: 16 },
        { lenOff: 56, longOff: 8, shortOff: 16 },
        { lenOff: 52, longOff: 4, shortOff: 12 },
    ];

    for (const c of candidates) {
        const len = readS32(vstr, c.lenOff);
        if (len === null || len < 0 || len > 2048) continue;
        const longPtr = readPointer(vstr, c.longOff);
        if (longPtr !== null && len > 21) {
            const s = safeUtf16(longPtr, len);
            if (s !== null) return s;
        }
        const s = safeUtf16(vstr.add(c.shortOff), len);
        if (s !== null) return s;
    }
    return null;
}

function readTtstr(ttstrPtr) {
    try {
        const p = ptr(ttstrPtr);
        if (p.isNull()) return '';
        const vstr = p.readPointer();
        return readVariantString(vstr);
    } catch (e) {
        return null;
    }
}

function readParameterEntry(entryPtr, index) {
    const p = ptr(entryPtr);
    const idPtr = p.add(PARAM_OFF.id);
    return {
        index: index,
        ptr: ptrHex(p),
        idPtr: ptrHex(idPtr),
        id: readTtstr(idPtr),
        discretization: readS32(p, PARAM_OFF.discretization),
        rangeBegin: readDouble(p, PARAM_OFF.rangeBegin),
        rangeEnd: readDouble(p, PARAM_OFF.rangeEnd),
        value: readDouble(p, PARAM_OFF.value),
        mode: readS32(p, PARAM_OFF.mode),
    };
}

function readParameterTable(playerPtr) {
    const player = ptr(playerPtr);
    const begin = readPointer(player, 384);
    const end = readPointer(player, 392);
    const out = {
        begin: ptrHex(begin),
        end: ptrHex(end),
        stride: PARAM_ENTRY_STRIDE,
        count: 0,
        entries: [],
    };
    if (begin === null || end === null) return out;

    const beginN = ptrToNumber(begin);
    const endN = ptrToNumber(end);
    if (endN < beginN) {
        out.error = 'invalid parameter vector begin/end';
        return out;
    }
    const span = endN - beginN;
    if (span % PARAM_ENTRY_STRIDE !== 0) {
        out.error = 'parameter vector span is not 56-byte aligned';
        out.span = span;
        return out;
    }
    const count = span / PARAM_ENTRY_STRIDE;
    out.count = count;
    if (count > 256) {
        out.error = 'parameter vector unexpectedly large';
        return out;
    }
    for (let i = 0; i < count; i++) {
        out.entries.push(readParameterEntry(begin.add(i * PARAM_ENTRY_STRIDE), i));
    }
    return out;
}

function semanticParameterTable(raw) {
    const entries = raw && raw.entries ? raw.entries : [];
    return {
        count: raw && typeof raw.count === 'number' ? raw.count : entries.length,
        entries: entries.map((entry) => ({
            index: entry.index,
            id: entry.id,
            discretization: entry.discretization !== 0,
            rangeBegin: entry.rangeBegin,
            rangeEnd: entry.rangeEnd,
            value: entry.value,
            mode: entry.mode,
        })),
    };
}

function parameterTableDiagnostics(raw) {
    const diag = {
        begin: raw ? raw.begin : null,
        end: raw ? raw.end : null,
        stride: raw ? raw.stride : null,
    };
    if (raw && raw.error) diag.error = raw.error;
    if (raw && raw.span !== undefined) diag.span = raw.span;
    if (raw && raw.entries && raw.entries.length > 0) {
        diag.entries = raw.entries.map((entry) => ({
            index: entry.index,
            ptr: entry.ptr,
            idPtr: entry.idPtr,
        }));
    }
    return diag;
}

function semanticPlayerOverview(raw) {
    raw = raw || {};
    return {
        nodeCount: raw.nodeCount || 0,
        parameterTable: semanticParameterTable(raw.parameterTable),
        playing: raw.playing,
        currentTime: raw.currentTime,
    };
}

function playerOverviewDiagnostics(raw) {
    raw = raw || {};
    return {
        player: raw.player,
        nodeLayout: raw.nodeLayout,
        frameTickCount: raw.frameTickCount,
        frameLastTime: raw.frameLastTime,
        parameterTable: parameterTableDiagnostics(raw.parameterTable),
    };
}

function parameterTableChanges(before, after) {
    const changes = [];
    const b = before && before.entries ? before.entries : [];
    const a = after && after.entries ? after.entries : [];
    const n = Math.max(b.length, a.length);
    for (let i = 0; i < n; i++) {
        const bi = b[i] || null;
        const ai = a[i] || null;
        if (bi === null || ai === null) {
            changes.push({ index: i, before: bi, after: ai, reason: 'entry_added_or_removed' });
            continue;
        }
        if (bi.mode !== ai.mode || bi.value !== ai.value) {
            changes.push({
                index: i,
                id: ai.id !== null ? ai.id : bi.id,
                beforeMode: bi.mode,
                afterMode: ai.mode,
                beforeValue: bi.value,
                afterValue: ai.value,
            });
        }
    }
    return changes;
}

function walkDeque(playerPtr, stride, options) {
    const strict = !!(options && options.strict);
    const fail = (message) => {
        if (strict) throw new Error(message);
        return null;
    };
    const playerRaw = ptr(playerPtr);
    const player = strict
        ? requireReadable(playerRaw, 264, 'Player')
        : (findReadablePtr(playerRaw, 264) || playerRaw);
    const startCurPtr = readPointer(player, 200);
    const startFirstPtr = readPointer(player, 208);
    const startLastPtr = readPointer(player, 216);
    const startNodePtr = readPointer(player, 224);
    const finishCurPtr = readPointer(player, 232);
    const finishFirstPtr = readPointer(player, 240);
    const finishLastPtr = readPointer(player, 248);
    const finishNodePtr = readPointer(player, 256);

    const startCur = ptrToNumber(startCurPtr);
    const startFirst = ptrToNumber(startFirstPtr);
    const startLast = ptrToNumber(startLastPtr);
    const startNode = ptrToNumber(startNodePtr);
    const finishCur = ptrToNumber(finishCurPtr);
    const finishFirst = ptrToNumber(finishFirstPtr);
    const finishLast = ptrToNumber(finishLastPtr);
    const finishNode = ptrToNumber(finishNodePtr);

    if (startCur === 0 || finishCur === 0) {
        return fail('Node deque has null current pointer');
    }
    if (startFirst === 0 || startLast === 0 ||
            finishFirst === 0 || finishLast === 0) {
        return fail('Node deque has null block boundary pointer');
    }
    if (startNode === 0 || finishNode === 0) {
        return fail('Node deque has null map iterator');
    }
    if (finishNode < startNode) {
        return fail('Node deque finish node precedes start node');
    }
    if ((finishNode - startNode) % 8 !== 0) {
        return fail('Node deque map span is not pointer-aligned');
    }
    const mapEntries = Math.floor((finishNode - startNode) / 8) + 1;
    if (mapEntries <= 0 || mapEntries > TRACE_FLATTEN_MAX_NODES) {
        return fail('Node deque map entry count out of range: ' + mapEntries);
    }
    if (!(startFirst <= startCur && startCur <= startLast)) {
        return fail('Node deque start cursor is outside its block');
    }
    if (!(finishFirst <= finishCur && finishCur <= finishLast)) {
        return fail('Node deque finish cursor is outside its block');
    }
    let startNodeReadable = startNodePtr;
    if (strict) {
        const tagOptions = { tagSource: playerRaw };
        startNodeReadable = requireReadable(
            startNodePtr, 8, 'Node deque start map iterator', tagOptions);
        requireReadable(
            finishNodePtr, 8, 'Node deque finish map iterator', tagOptions);
    }

    let bufElems = 1;
    if (startLast > startFirst) {
        const span = startLast - startFirst;
        if (span % stride === 0 && span > 0) {
            bufElems = Math.floor(span / stride);
        } else if (strict) {
            return fail('Node deque block span is not a stride multiple');
        }
    }
    if (bufElems < 1 || bufElems > TRACE_FLATTEN_MAX_NODES) {
        return fail('Node deque block element count out of range: ' + bufElems);
    }

    const nodes = [];
    let mapIter = startNodeReadable;
    let safety = 0;
    while (ptrToNumber(mapIter) <= finishNode) {
        if (++safety > TRACE_FLATTEN_MAX_NODES) {
            return fail('Node deque map walk exceeded safety limit');
        }
        const tagOptions = { tagSources: [playerRaw, startCurPtr, finishCurPtr] };
        if (strict) {
            mapIter = requireReadable(
                mapIter, 8, 'Node deque map entry', tagOptions);
        }
        const blockPtrRaw = readPointer(mapIter, 0);
        const blockRaw = ptrToNumber(blockPtrRaw);
        if (blockRaw === 0) return fail('Node deque block pointer is null');
        const blockPtr = findReadablePtr(blockPtrRaw, stride, tagOptions) ||
            blockPtrRaw;
        if (strict) {
            requireReadable(blockPtr, stride, 'Node deque block', tagOptions);
        }
        let first = 0;
        let last = bufElems;
        if (ptrToNumber(mapIter) === startNode) {
            first = Math.floor((startCur - blockRaw) / stride);
        }
        if (ptrToNumber(mapIter) === finishNode) {
            last = Math.floor((finishCur - blockRaw) / stride);
        }
        if (first < 0 || last < first || last > bufElems) {
            return fail(
                'Node deque block cursor range is invalid: first=' +
                first + ' last=' + last + ' bufElems=' + bufElems
            );
        }
        for (let k = first; k < last; k++) {
            const node = blockPtr.add(k * stride);
            if (strict) {
                requireReadable(
                    node, NODE_OFF.opacity + 4, 'Node', tagOptions);
            }
            nodes.push(node);
            if (nodes.length > TRACE_FLATTEN_MAX_NODES) {
                return fail('Node count exceeded strict trace limit');
            }
        }
        mapIter = mapIter.add(8);
    }
    if (strict && nodes.length === 0) {
        return fail('Node deque produced zero nodes');
    }
    return nodes;
}

function readNodeAccum(nodePtr) {
    const node = ptr(nodePtr);
    return {
        nodeType: readS32(node, NODE_OFF.nodeType),
        active: readBool(node, NODE_OFF.active),
        visible: readBool(node, NODE_OFF.visible),
        flipX: readBool(node, NODE_OFF.flipX),
        flipY: readBool(node, NODE_OFF.flipY),
        posX: readDouble(node, NODE_OFF.posX),
        posY: readDouble(node, NODE_OFF.posY),
        posZ: readDouble(node, NODE_OFF.posZ),
        angleDeg: readDouble(node, NODE_OFF.angleDeg),
        scaleX: readDouble(node, NODE_OFF.scaleX),
        scaleY: readDouble(node, NODE_OFF.scaleY),
        slantX: readDouble(node, NODE_OFF.slantX),
        slantY: readDouble(node, NODE_OFF.slantY),
        opacity: readS32(node, NODE_OFF.opacity),
        stencilType: readS32(node, NODE_OFF.stencilType),
    };
}

function validateTraceFlattenLayer(layer, index, nodePtr) {
    const intFields = ['nodeType', 'opacity', 'stencilType'];
    const boolFields = ['active', 'visible', 'flipX', 'flipY'];
    const numFields = [
        'posX', 'posY', 'posZ', 'angleDeg',
        'scaleX', 'scaleY', 'slantX', 'slantY',
    ];
    for (const field of intFields) {
        if (!Number.isInteger(layer[field])) {
            throw new Error(
                'Node ' + index + ' field ' + field +
                ' is not an integer at ' + ptrHex(nodePtr)
            );
        }
    }
    for (const field of boolFields) {
        if (typeof layer[field] !== 'boolean') {
            throw new Error(
                'Node ' + index + ' field ' + field +
                ' is not boolean at ' + ptrHex(nodePtr)
            );
        }
    }
    for (const field of numFields) {
        const value = layer[field];
        if (typeof value !== 'number' || !Number.isFinite(value) ||
                Math.abs(value) > TRACE_FLATTEN_ABS_FLOAT_LIMIT) {
            throw new Error(
                'Node ' + index + ' field ' + field +
                ' is invalid: ' + value + ' at ' + ptrHex(nodePtr)
            );
        }
    }
    if (layer.opacity < 0 || layer.opacity > 255) {
        throw new Error(
            'Node ' + index + ' opacity out of range: ' + layer.opacity
        );
    }
    if (layer.nodeType < 0 || layer.nodeType > 32) {
        throw new Error(
            'Node ' + index + ' nodeType out of range: ' + layer.nodeType
        );
    }
    if (Math.abs(layer.stencilType) > 1024) {
        throw new Error(
            'Node ' + index + ' stencilType out of range: ' +
            layer.stencilType
        );
    }
}

function readNodeBrief(nodePtr, index) {
    const node = ptr(nodePtr);
    const paramEntry = readPointer(node, NODE_OFF.parameterEntry);
    let param = null;
    if (paramEntry !== null) {
        param = {
            ptr: ptrHex(paramEntry),
            mode: readS32(paramEntry, PARAM_OFF.mode),
            value: readDouble(paramEntry, PARAM_OFF.value),
            id: readTtstr(paramEntry.add(PARAM_OFF.id)),
        };
    }
    return {
        index: index,
        ptr: ptrHex(node),
        parameterEntry: ptrHex(paramEntry),
        parameter: param,
        coordinateMode: readS32(node, NODE_OFF.coordinateMode),
        nodeType: readS32(node, NODE_OFF.nodeType),
        parentIndex: readS32(node, NODE_OFF.parentIndex),
        flags: readU8(node, NODE_OFF.flags),
        activeSlot: readS32(node, NODE_OFF.activeSlot),
        active: readBool(node, NODE_OFF.active),
        visible: readBool(node, NODE_OFF.visible),
        opacity: readS32(node, NODE_OFF.opacity),
    };
}

function walkNodes(playerPtr, options) {
    const strict = !!(options && options.strict);
    try {
        const nodes = walkDeque(playerPtr, NODE_STRIDE, options);
        if (nodes !== null && nodes.length > 0) {
            const layers = [];
            for (let i = 0; i < nodes.length; i++) {
                const accum = readNodeAccum(nodes[i]);
                if (strict) {
                    validateTraceFlattenLayer(accum, i, nodes[i]);
                }
                accum.index = i;
                layers.push(accum);
            }
            return { layout: 'deque', layers: layers, nodeCount: nodes.length };
        }
    } catch (e) {
        return { layout: 'deque-error', error: String(e), layers: [], nodeCount: 0 };
    }
    if (strict) {
        return {
            layout: 'deque-error',
            error: 'strict trace_flatten requires a valid Node deque',
            layers: [],
            nodeCount: 0,
        };
    }
    const rootPtr = readPointer(ptr(playerPtr), 200);
    if (rootPtr === null) return { layout: 'empty', layers: [], nodeCount: 0 };
    const accum = readNodeAccum(rootPtr);
    accum.index = 0;
    return { layout: 'root-only', layers: [accum], nodeCount: 1 };
}

function playerOverview(playerPtr) {
    const player = ptr(playerPtr);
    const walked = walkNodes(player);
    return {
        player: ptrHex(player),
        nodeLayout: walked.layout,
        nodeCount: walked.nodeCount,
        parameterTable: readParameterTable(player),
        playing: readBool(player, 1099),
        currentTime: readDouble(player, 456),
        frameTickCount: readDouble(player, 1120),
        frameLastTime: readDouble(player, 1128),
    };
}

function snapshotMotionSubNodes(playerPtr) {
    const nodes = walkDeque(playerPtr, NODE_STRIDE);
    const out = [];
    if (nodes === null) return out;
    for (let i = 0; i < nodes.length; i++) {
        const nodeType = readS32(nodes[i], NODE_OFF.nodeType);
        if (nodeType === 3) {
            out.push(readNodeBrief(nodes[i], i));
        }
    }
    return out;
}

function snapshotEvalNode(nodePtr) {
    return readNodeBrief(ptr(nodePtr), -1);
}

function semanticEvalNode(nodePtr) {
    return semanticFrameSelectionNode(snapshotEvalNode(nodePtr));
}

function classifySubMotion(beforeNode, afterNode, childSampleDelta) {
    const node = afterNode || beforeNode || {};
    const param = node.parameter || {};
    const mode = param.mode === null || param.mode === undefined ? 0 : param.mode;
    const flags = node.flags === null || node.flags === undefined ? 0 : node.flags;
    if (childSampleDelta > 0) return 'play_or_update_child';
    if (((mode & 5) !== 0) || flags !== 0) return 'gate_open_no_child_sample';
    if (node.visible === false && mode === 0) return 'skip_invisible';
    return 'skip_gate_closed';
}

function compareMotionSubSnapshots(before, after, childSampleDelta) {
    const out = [];
    const byIndex = {};
    for (const b of before || []) byIndex[b.index] = { before: b, after: null };
    for (const a of after || []) {
        if (!byIndex[a.index]) byIndex[a.index] = { before: null, after: a };
        else byIndex[a.index].after = a;
    }
    Object.keys(byIndex).sort((a, b) => Number(a) - Number(b)).forEach((k) => {
        const item = byIndex[k];
        out.push({
            index: Number(k),
            before: item.before,
            after: item.after,
            decision: classifySubMotion(item.before, item.after, childSampleDelta),
        });
    });
    return out;
}

function currentSamplePlayers() {
    return samplesInFrame.map((s) => s.player.toString());
}

function readRectF(p, off) {
    return [
        readFloat(p, off),
        readFloat(p, off + 4),
        readFloat(p, off + 8),
        readFloat(p, off + 12),
    ];
}

function readFloatArray(p, off, count) {
    const out = [];
    for (let i = 0; i < count; i++) {
        out.push(readFloat(p, off + i * 4));
    }
    return out;
}

function readRectS32(p, off) {
    return [
        readS32(p, off),
        readS32(p, off + 4),
        readS32(p, off + 8),
        readS32(p, off + 12),
    ];
}

function readRenderItem(itemPtr, index) {
    const item = ptr(itemPtr);
    const paintBox = readRectF(item, 184);
    const buildClipRect = readRectF(item, 216);
    return {
        index: index,
        item: ptrHex(item),
        flags: {
            flag16: readU8(item, 16),
            flag17: readU8(item, 17),
            flag18: readU8(item, 18),
            drawFlag19: readU8(item, 19),
            layerResolved20: readU8(item, 20),
            clipValid21: readU8(item, 21),
        },
        layerIds: {
            primary: readS32(item, 52),
            secondary: readS32(item, 56),
        },
        sortKey64: readDouble(item, 64),
        corners: readFloatArray(item, 136, 8),
        paintBox: paintBox,
        clipRect: buildClipRect,
        buildClipRect: buildClipRect,
        viewportRect: readRectF(item, 200),
        diagnostics: {
            itemPlus184PaintBox: paintBox,
            itemPlus216BuildClipRect: buildClipRect,
        },
        sourceGate232: readU32(item, 232),
        stencilType244: readU32(item, 244),
        parentItem264: ptrHex(readPointer(item, 264)),
        meshType280: readS32(item, 280),
        leafLayerVariantTag320: readU32(item, 320),
        composedLayerVariantTag340: readU32(item, 340),
    };
}

function readRenderItemVector(vectorPtr, limit) {
    const vec = ptr(vectorPtr);
    const out = {
        vector: ptrHex(vec),
        begin: null,
        end: null,
        count: 0,
        items: [],
    };
    try {
        const begin = vec.readPointer();
        const end = vec.add(8).readPointer();
        out.begin = ptrHex(begin);
        out.end = ptrHex(end);
        const beginN = ptrToNumber(begin);
        const endN = ptrToNumber(end);
        if (beginN === 0 || endN === 0 || endN < beginN) {
            out.error = 'invalid render item vector begin/end';
            return out;
        }
        const span = endN - beginN;
        if (span % 8 !== 0) {
            out.error = 'render item vector span is not pointer aligned';
            out.span = span;
            return out;
        }
        const count = span / 8;
        out.count = count;
        const n = Math.min(count, limit || 256);
        for (let i = 0; i < n; i++) {
            const itemPtr = begin.add(i * 8).readPointer();
            if (!itemPtr.isNull()) {
                out.items.push(readRenderItem(itemPtr, i));
            } else {
                out.items.push({ index: i, item: null });
            }
        }
        if (count > n) out.truncated = count - n;
    } catch (e) {
        out.error = String(e);
    }
    return out;
}

function readRenderLists(mainListPtr, auxListPtr) {
    return {
        mainList: mainListPtr ? readRenderItemVector(mainListPtr, 256) : null,
        auxList: auxListPtr ? readRenderItemVector(auxListPtr, 256) : null,
    };
}

function enterRenderContext(player) {
    return {
        frameId: currentRenderFrameId,
        player: currentRenderPlayer,
        nextFrameId: lastCompletedFrameId,
    };
}

function applyRenderContext(ctx, player) {
    currentRenderFrameId = ctx.nextFrameId;
    currentRenderPlayer = player;
}

function leaveRenderContext(ctx) {
    currentRenderFrameId = ctx.frameId;
    currentRenderPlayer = ctx.player;
}

function intArgValue(arg) {
    if (!arg || !arg.scalar) return null;
    return arg.scalar.int32;
}

function doubleArgValue(arg) {
    if (!arg || !arg.scalar) return null;
    return arg.scalar.double;
}

function readUtf16StringSafe(value) {
    try {
        const p = ptr(value);
        if (p.isNull()) return null;
        return p.readUtf16String();
    } catch (e) {
        return null;
    }
}

function likelyDirectRenderItem(ctx) {
    if (!ctx || !ctx.mainList) return null;
    try {
        const mainList = readRenderItemVector(ctx.mainList, 16);
        if (!mainList || !mainList.items) return null;
        for (const item of mainList.items) {
            if (item && item.sourceGate232 && !item.parentItem264) return item;
        }
    } catch (e) {}
    return null;
}

function directOperateAffinePayload(ctx, phase, machineContext) {
    const targetObject = machineContext && machineContext.x7
        ? ptr(machineContext.x7) : (ctx ? ctx.target : null);
    const argArray = machineContext && machineContext.x6
        ? ptr(machineContext.x6) : null;
    const sourceArg = argArray ? readVariantArg(argArray, 0) : null;
    const srcLeftArg = argArray ? readVariantArg(argArray, 1) : null;
    const srcTopArg = argArray ? readVariantArg(argArray, 2) : null;
    const srcWidthArg = argArray ? readVariantArg(argArray, 3) : null;
    const srcHeightArg = argArray ? readVariantArg(argArray, 4) : null;
    const affineArg = argArray ? readVariantArg(argArray, 5) : null;
    const pointArgs = argArray
        ? [
            [doubleArgValue(readVariantArg(argArray, 6)),
             doubleArgValue(readVariantArg(argArray, 7))],
            [doubleArgValue(readVariantArg(argArray, 8)),
             doubleArgValue(readVariantArg(argArray, 9))],
            [doubleArgValue(readVariantArg(argArray, 10)),
             doubleArgValue(readVariantArg(argArray, 11))],
        ]
        : null;
    const modeArg = argArray ? readVariantArg(argArray, 12) : null;
    const opacityArg = argArray ? readVariantArg(argArray, 13) : null;
    const typeArg = argArray ? readVariantArg(argArray, 14) : null;
    const itemPtr = machineContext && machineContext.x23
        ? ptr(machineContext.x23) : null;
    let renderItem = null;
    try {
        if (itemPtr && !itemPtr.isNull()) renderItem = readRenderItem(itemPtr, null);
    } catch (e) {
        renderItem = { item: ptrHex(itemPtr), error: String(e) };
    }
    if (!renderItem) renderItem = likelyDirectRenderItem(ctx);
    const sourceObject = sourceArg && sourceArg.object
        ? sourceArg.object.object : null;
    const targetFirstPixel = targetObject
        ? readLayerFirstPixel(targetObject) : { ok: false, error: 'no target object' };
    const sourceFirstPixel = sourceObject
        ? readLayerFirstPixel(sourceObject) : { ok: false, error: 'no source layer object' };
    const targetDrawFaceDiagnostics = targetObject
        ? readLayerDrawFaceDiagnostics(targetObject)
        : { ok: false, error: 'no target object', nativeLayer: null };
    const sourceDrawFaceDiagnostics = sourceObject
        ? readLayerDrawFaceDiagnostics(sourceObject)
        : { ok: false, error: 'no source layer object', nativeLayer: null };
    const sourcePixelSamples = sourceObject
        ? readLayerPixelSamples(sourceObject, [
            [0, 0], [1, 42], [2, 42], [3, 42],
            [1, 43], [2, 43], [3, 43], [1, 49],
            [3, 49], [1, 50], [3, 50],
        ])
        : [];
    const targetPixelSamples = targetObject
        ? readLayerPixelSamples(targetObject, [
            [725, 693], [725, 694], [725, 695], [725, 696],
            [725, 697], [726, 700], [726, 701],
        ])
        : [];
    return {
        probePhase: phase,
        branch: 'direct.operateAffine',
        nodeIndex: null,
        meshType: renderItem ? renderItem.meshType280 : null,
        blendMode: intArgValue(modeArg),
        opacity: intArgValue(opacityArg),
        stretchType: intArgValue(typeArg),
        affine: intArgValue(affineArg),
        sourceRectArgs: {
            left: intArgValue(srcLeftArg),
            top: intArgValue(srcTopArg),
            width: intArgValue(srcWidthArg),
            height: intArgValue(srcHeightArg),
        },
        targetFace: targetDrawFaceDiagnostics.offset404S32,
        targetDrawFace: targetDrawFaceDiagnostics.offset400S32,
        targetHoldAlpha: targetDrawFaceDiagnostics.offset408U8,
        resolvedBltMethod: null,
        resolvedBltMethodName: null,
        target: ptrHex(targetObject),
        sourceObject: ptrHex(sourceObject),
        argArray: ptrHex(argArray),
        sourceVariant: sourceArg ? ptrHex(sourceArg.variant) : null,
        sourceVariantType: sourceArg && sourceArg.object
            ? sourceArg.object.type : null,
        sourceVariantError: sourceArg
            ? (sourceArg.error ||
               (sourceArg.object ? sourceArg.object.error : null))
            : null,
        operateAffinePointArgs: pointArgs,
        renderItem: renderItem,
        sourceFirstPixel: sourceFirstPixel,
        targetFirstPixel: targetFirstPixel,
        sourcePixelSamples: sourcePixelSamples,
        targetPixelSamples: targetPixelSamples,
        diagnostics: {
            callSite: hexOff(PLAYER_RENDER_EXECUTE_DIRECT_OPERATE_AFFINE_CALL_OFF),
            afterSite: hexOff(PLAYER_RENDER_EXECUTE_DIRECT_OPERATE_AFFINE_AFTER_OFF),
            funcCallDispatch: machineContext && machineContext.x0
                ? ptrHex(machineContext.x0) : null,
            funcCallObjThis: machineContext && machineContext.x7
                ? ptrHex(machineContext.x7) : null,
            targetPixelDiagnostics: targetFirstPixel.diagnostics || null,
            sourcePixelDiagnostics: sourceFirstPixel.diagnostics || null,
            targetDrawFaceDiagnostics: targetDrawFaceDiagnostics,
            sourceDrawFaceDiagnostics: sourceDrawFaceDiagnostics,
        },
    };
}

function ensureDirectOperateAffineFuncCallHook(fnPtr) {
    const key = ptrHex(fnPtr);
    if (!key || directOperateAffineFuncCallHookCache[key]) return;
    directOperateAffineFuncCallHookCache[key] = true;
    Interceptor.attach(ptr(fnPtr), {
        onEnter(args) {
            const ctx = currentRenderExecuteContext();
            if (!ctx) return;
            const methodName = readUtf16StringSafe(args[2]);
            const argc = readArgInt(args[5]);
            if (argc !== 15) return;
            this.directOperateAffineCtx = ctx;
            this.directOperateAffinePayload = directOperateAffinePayload(
                ctx, 'before', {
                    x0: args[0],
                    x6: args[6],
                    x23: NULL,
                });
            this.directOperateAffinePayload.target = ptrHex(ctx.target);
            this.directOperateAffinePayload.targetFirstPixel =
                readLayerFirstPixel(ctx.target);
            this.directOperateAffinePayload.funcCallMethodName = methodName;
            this.directOperateAffinePayload.funcCallTarget = ptrHex(args[0]);
            this.directOperateAffinePayload.funcCallMatchesExecuteTarget =
                ptrEqual(args[0], ctx.target);
            emitDirectExecuteProbe(
                ctx,
                'before',
                'sub_6C7440.direct.beforeOperateAffine',
                this.directOperateAffinePayload);
        },
        onLeave() {
            const ctx = this.directOperateAffineCtx;
            if (!ctx) return;
            const payload = Object.assign(
                {}, this.directOperateAffinePayload || {});
            payload.probePhase = 'after';
            payload.samplePoint = 'sub_6C7440.direct.afterOperateAffine';
            payload.targetFirstPixel = readLayerFirstPixel(ctx.target);
            payload.beforeTargetFirstPixel =
                this.directOperateAffinePayload
                    ? this.directOperateAffinePayload.targetFirstPixel || null
                    : null;
            emitDirectExecuteProbe(
                ctx,
                'after',
                'sub_6C7440.direct.afterOperateAffine',
                payload);
        },
    });
}

function removeContext(list, ctx) {
    if (!ctx) return;
    const idx = list.lastIndexOf(ctx);
    if (idx >= 0) list.splice(idx, 1);
}

function enterAccurateSlaRenderExecute(ctx) {
    if (!ctx || ctx.entered) return;
    ctx.entered = true;
    ctx.executeCtx = {
        player: ctx.player,
        target: ctx.target,
        targetVariant: ctx.targetVariant,
        targetVariantObject: ctx.targetVariantObject,
        mainList: ctx.mainList,
        auxList: ctx.auxList,
        accurateSla: true,
        slaAdaptor: ctx.slaAdaptor,
    };
    activeRenderExecuteContexts.push(ctx.executeCtx);
    sendLayerRawProbe(
        ctx.player, ctx.target, null,
        'Player_renderAccurateSeparateLayerAdaptor_0x6C7088.enter',
        {},
        {
            addr: PLAYER_ACCURATE_SLA_RENDER_OFF,
            targetVariant: ptrHex(ctx.targetVariant),
            targetVariantOffset: 20,
            target: ptrHex(ctx.target),
            targetObjThis: ctx.targetVariantObject
                ? ptrHex(ctx.targetVariantObject.objThis) : null,
            targetError: ctx.targetVariantObject
                ? ctx.targetVariantObject.error : null,
            drawTarget: ctx.drawCtx ? ptrHex(ctx.drawCtx.targetObject) : null,
            targetMatchesDrawArg: ctx.targetMatchesDrawArg,
            slaAdaptor: ptrHex(ctx.slaAdaptor),
        });
    const checkpointTarget =
        checkpointTargetForDrawContext(ctx.drawCtx, ctx.target);
    sendRenderImageCheckpoint(
        ctx.player, checkpointTarget, 'execute_pre',
        'Player_renderAccurateSeparateLayerAdaptor_0x6C7088.enter.after-target-resolve',
        undefined, ctx.target);
    emitRender(STAGE_RENDER_EXECUTE, 'execute_enter', {
        accurateSla: true,
    }, {
        addr: PLAYER_ACCURATE_SLA_RENDER_OFF,
        player: ptrHex(ctx.player),
        targetVariant: ptrHex(ctx.targetVariant),
        targetVariantOffset: 20,
        targetVariantType: ctx.targetVariantObject
            ? ctx.targetVariantObject.type : null,
        target: ptrHex(ctx.target),
        targetObjThis: ctx.targetVariantObject
            ? ptrHex(ctx.targetVariantObject.objThis) : null,
        targetError: ctx.targetVariantObject
            ? ctx.targetVariantObject.error : null,
        drawTarget: ctx.drawCtx ? ptrHex(ctx.drawCtx.targetObject) : null,
        targetMatchesDrawArg: ctx.targetMatchesDrawArg,
        slaAdaptor: ptrHex(ctx.slaAdaptor),
        mainListPtr: ptrHex(ctx.mainList),
        auxListPtr: ptrHex(ctx.auxList),
    }, 'Player_renderAccurateSeparateLayerAdaptor_0x6C7088.enter');
}

function leaveAccurateSlaRenderExecute(ctx, retval) {
    if (!ctx || !ctx.entered) return;
    const leaveTarget = ctx.target ||
        (ctx.executeCtx ? ctx.executeCtx.target : null);
    const leaveTargetVariantObject = ctx.targetVariantObject ||
        (ctx.executeCtx ? ctx.executeCtx.targetVariantObject : null);
    emitRender(STAGE_RENDER_EXECUTE, 'execute_leave', {
        accurateSla: true,
        retval: ptrHex(retval),
    }, {
        addr: PLAYER_ACCURATE_SLA_RENDER_OFF,
        player: ptrHex(ctx.player),
        targetVariant: ptrHex(ctx.targetVariant),
        targetVariantOffset: 20,
        targetVariantType: leaveTargetVariantObject
            ? leaveTargetVariantObject.type : null,
        target: ptrHex(leaveTarget),
        targetObjThis: leaveTargetVariantObject
            ? ptrHex(leaveTargetVariantObject.objThis) : null,
        targetError: leaveTargetVariantObject
            ? leaveTargetVariantObject.error : null,
        drawTarget: ctx.drawCtx ? ptrHex(ctx.drawCtx.targetObject) : null,
        targetMatchesDrawArg: ctx.targetMatchesDrawArg,
        slaAdaptor: ptrHex(ctx.slaAdaptor),
    }, 'Player_renderAccurateSeparateLayerAdaptor_0x6C7088.leave');
    sendLayerRawProbe(
        ctx.player, leaveTarget, null,
        'Player_renderAccurateSeparateLayerAdaptor_0x6C7088.leave',
        {},
        {
            addr: PLAYER_ACCURATE_SLA_RENDER_OFF,
            targetVariant: ptrHex(ctx.targetVariant),
            targetVariantOffset: 20,
            target: ptrHex(leaveTarget),
            targetObjThis: leaveTargetVariantObject
                ? ptrHex(leaveTargetVariantObject.objThis) : null,
            targetError: leaveTargetVariantObject
                ? leaveTargetVariantObject.error : null,
            drawTarget: ctx.drawCtx ? ptrHex(ctx.drawCtx.targetObject) : null,
            targetMatchesDrawArg: ctx.targetMatchesDrawArg,
            slaAdaptor: ptrHex(ctx.slaAdaptor),
        });
    const leaveCheckpointTarget =
        checkpointTargetForDrawContext(ctx.drawCtx, leaveTarget);
    sendRenderImageCheckpoint(
        ctx.player, leaveCheckpointTarget, 'execute_post',
        'Player_renderAccurateSeparateLayerAdaptor_0x6C7088.leave.before-return',
        undefined, leaveTarget);
    scheduleExecutePostUploadCheckpoint(
        ctx.player,
        'Player_renderAccurateSeparateLayerAdaptor_0x6C7088.leave.after-next-drawdevice-upload');
    if (leaveCheckpointTarget) {
        lastRenderLayerObject = leaveCheckpointTarget;
        lastSlaRenderTargetObject = leaveCheckpointTarget;
    }
    if (ctx.executeCtx) {
        removeContext(activeRenderExecuteContexts, ctx.executeCtx);
    }
}

function installTraceFlattenHooks() {
    attachAt(PLAYER_PROGRESS_COMPAT_OFF, 'Player_progressCompat', {
        onEnter(args) {
            if (!recording) return;
            this.previousCapture = {
                inCompat, samplesInFrame, capturedObjthis,
                currentFrameId, capturedDeltaMs,
            };
            inCompat = true;
            samplesInFrame = [];
            capturedObjthis = args[3];
            currentFrameId = nextFrameId++;
            capturedDeltaMs = null;
            // The fixture supplies a numeric interval. Observe its raw value;
            // never invoke a second TJS conversion from the hook.
            if (readArgInt(args[1]) > 0) {
                const arg = readVariantArg(args[2], 0);
                if (arg.scalar && arg.scalar.type === 5) {
                    capturedDeltaMs = arg.scalar.double;
                } else if (arg.scalar && arg.scalar.type === 4) {
                    capturedDeltaMs = arg.variant.readS64().toNumber();
                }
            }
        },
        onLeave() {
            if (!this.previousCapture) return;
            const restoreCapture = () => {
                ({ inCompat, samplesInFrame, capturedObjthis,
                   currentFrameId, capturedDeltaMs } = this.previousCapture);
            };
            const objthis = capturedObjthis;
            const samples = samplesInFrame;
            const deltaMs = capturedDeltaMs;
            const completedFrameId = currentFrameId;
            const completedTopPlayer =
                samples.length > 0 ? samples[samples.length - 1].player : null;
            inCompat = false;
            capturedObjthis = null;
            currentFrameId = null;
            lastCompletedFrameId = completedFrameId;
            lastCompletedTopPlayer = completedTopPlayer;
            lastRenderLayerObject = null;
            lastDrawTargetObject = null;
            lastSlaRenderTargetObject = null;
            lastSlaRenderNativeLayer = null;

            if (!recording || !stageEnabled(STAGE_TRACE_FLATTEN)) {
                restoreCapture();
                return;
            }

            const flatLayers = [];
            const diagnosticPlayers = [];
            let layoutTag = 'pre-cleanup';
            let walkError = null;
            for (const sample of samples) {
                const layerStart = flatLayers.length;
                for (const l of sample.layers) {
                    const out = Object.assign({}, l);
                    out.index = flatLayers.length;
                    flatLayers.push(out);
                }
                diagnosticPlayers.push({
                    ptr: sample.player.toString(),
                    layout: sample.layout || null,
                    layerStart: layerStart,
                    layerCount: sample.layers.length,
                    error: sample.error || null,
                });
                if (sample.layout && sample.layout !== 'deque') {
                    layoutTag = sample.layout;
                }
                walkError = walkError || sample.error;
            }
            emit(STAGE_TRACE_FLATTEN, 'frame', {
                projection: TRACE_FLATTEN_PROJECTION,
                samplePoint: TRACE_FLATTEN_SAMPLE_POINT,
                sampleOrder: TRACE_FLATTEN_SAMPLE_ORDER,
                deltaMs,
                playerLayerCounts: samples.map(sample => sample.layers.length),
                frameId: completedFrameId,
                playerCount: samples.length,
                layers: flatLayers,
                diagnostics: {
                    objthis: objthis ? objthis.toString() : null,
                    topPlayer: samples.length > 0
                        ? samples[samples.length - 1].player.toString() : null,
                    layout: layoutTag,
                    players: diagnosticPlayers,
                    error: walkError,
                },
            });
            frameCounter++;
            restoreCapture();
        },
    });

    attachAt(PLAYER_PHASE3_LAST_OFF, 'Player_phase3_last', {
        onEnter(args) {
            this.player = args[0];
        },
        onLeave() {
            if (!inCompat || !recording) return;
            const player = this.player;
            try {
                const w = walkNodes(player, { strict: true });
                samplesInFrame.push({
                    player: player,
                    layout: w.layout,
                    layers: w.layers,
                    error: w.error || null,
                });
            } catch (e) {
                samplesInFrame.push({
                    player: player,
                    layout: 'sample-error',
                    layers: [],
                    error: String(e),
                });
            }
        },
    });
}

function installHook() {
    if (hooked) return;

    attachAt(TVP_GET_ROUGH_TICK_COUNT32_OFF, 'TVPGetRoughTickCount32', {
        onEnter() {
            this.virtualizeContinuousTick = recording &&
                virtualContinuousTickFps > 0 &&
                this.returnAddress.equals(
                    ensureBase().add(TVP_CONTINUOUS_EVENT_TICK_RETURN_OFF));
        },
        onLeave(retval) {
            if (!this.virtualizeContinuousTick) return;

            if (virtualContinuousTickBase === null) {
                virtualContinuousTickBase = uint64(retval.toString());
            }

            // Wasmtime advances its monotonic clock before the first guest
            // tick. Preserve that phase: the first handler tick is the epoch,
            // then floor(n * 1000 / fps) produces the same 67/67/66 ms cycle
            // at 15 Hz. startup.tjs still observes a zero first-frame delta
            // through its production lastTick === void guard.
            const firstGridTick = Math.floor(
                1000 / virtualContinuousTickFps);
            const gridTick = Math.floor(
                (virtualContinuousTickIndex + 1) * 1000 /
                virtualContinuousTickFps);
            const offsetMs = gridTick - firstGridTick;
            retval.replace(ptr(
                virtualContinuousTickBase.add(offsetMs).toString()));
            virtualContinuousTickIndex++;
        },
    });
    ensureBase();

    // Every render_path capture also carries the normalized frame stream used
    // to segment the two fixtures and assign stable frame ids to render events.
    installTraceFlattenHooks();
    if (enabledStages.size === 1 && stageEnabled(STAGE_TRACE_FLATTEN)) {
        hooked = true;
        return;
    }

    attachAt(PLAYER_DRAW_COMPAT_OFF, 'Player_drawCompat', {
        onEnter(args) {
            this.player = args[0];
            this.argVariant = args[1];
            this.ctx = enterRenderContext(this.player);
            applyRenderContext(this.ctx, this.player);
            this.drawCtx = beginDrawContext(this.player, this.argVariant);
            if (this.drawCtx && this.drawCtx.targetObject) {
                lastDrawTargetObject = this.drawCtx.targetObject;
            }
            sendLayerRawProbe(
                this.player, this.drawCtx.targetObject, null,
                'Player_draw_0x6D3398.enter',
                { drawId: this.drawCtx.drawId },
                {
                    addr: PLAYER_DRAW_COMPAT_OFF,
                    targetObject: ptrHex(this.drawCtx.targetObject),
                    targetObjThis: ptrHex(this.drawCtx.targetObjThis),
                    targetError: this.drawCtx.targetVariantError,
                });
            emitRender(STAGE_DRAW_DISPATCH, 'draw_enter', {
                drawId: this.drawCtx.drawId,
            }, {
                addr: PLAYER_DRAW_COMPAT_OFF,
                player: ptrHex(this.player),
                argVariant: ptrHex(this.argVariant),
                targetVariantType: this.drawCtx.targetVariantType,
                targetObject: ptrHex(this.drawCtx.targetObject),
                targetObjThis: ptrHex(this.drawCtx.targetObjThis),
                targetError: this.drawCtx.targetVariantError,
                rawArgs: {
                    arg0: ptrHex(args[0]),
                    arg1: ptrHex(args[1]),
                    arg2: ptrHex(args[2]),
                    arg3: ptrHex(args[3]),
                },
            }, 'Player_draw_0x6D3398.enter');
        },
        onLeave() {
            sendLayerRawProbe(
                this.player,
                this.drawCtx ? this.drawCtx.targetObject : null,
                null,
                'Player_draw_0x6D3398.leave',
                {
                    drawId: this.drawCtx ? this.drawCtx.drawId : null,
                },
                {
                    addr: PLAYER_DRAW_COMPAT_OFF,
                    targetObject: this.drawCtx
                        ? ptrHex(this.drawCtx.targetObject) : null,
                    targetObjThis: this.drawCtx
                        ? ptrHex(this.drawCtx.targetObjThis) : null,
                    targetError: this.drawCtx
                        ? this.drawCtx.targetVariantError : null,
                });
            emitRender(STAGE_DRAW_DISPATCH, 'draw_leave', {
                drawId: this.drawCtx ? this.drawCtx.drawId : null,
                route: this.drawCtx
                    ? drawPathSummary(this.drawCtx).route
                    : 'failed',
                drawPath: this.drawCtx ? drawPathSummary(this.drawCtx) : null,
            }, {
                addr: PLAYER_DRAW_COMPAT_OFF,
                player: ptrHex(this.player),
                argVariant: ptrHex(this.argVariant),
                targetVariantType: this.drawCtx
                    ? this.drawCtx.targetVariantType : null,
                targetObject: this.drawCtx
                    ? ptrHex(this.drawCtx.targetObject) : null,
                targetObjThis: this.drawCtx
                    ? ptrHex(this.drawCtx.targetObjThis) : null,
                targetError: this.drawCtx
                    ? this.drawCtx.targetVariantError : null,
            }, 'Player_draw_0x6D3398.leave');
            finishDrawContext(this.drawCtx);
            leaveRenderContext(this.ctx);
        },
    });

    attachAt(PLAYER_DRAW_D3D_OFF, 'Player_drawD3D', {
        onEnter(args) {
            this.player = args[0];
            const ctx = currentDrawContextFor(this.player);
            if (!ctx) return;
            emitDrawStep(ctx, 'target_check_d3d', 'hit', 'd3d_adaptor');
        },
    });

    attachAt(PLAYER_DRAW_SLA_OFF, 'Player_DrawSLA', {
        onEnter(args) {
            this.player = args[0];
            const ctx = currentDrawContextFor(this.player);
            if (!ctx) return;
            if (!ctx.emittedSteps.target_check_d3d) {
                emitDrawStep(ctx, 'target_check_d3d', 'miss');
            }
            emitDrawStep(
                ctx, 'target_check_sla', 'hit', 'separate_layer_adaptor');
        },
    });

    attachAt(PLAYER_RENDER_PREPARE_OFF, 'Player_renderPrepare', {
        onEnter(args) {
            this.player = args[0];
            this.mainList = args[1];
            this.auxList = args[2];
            emitRender(STAGE_RENDER_PREPARE, 'prepare_enter', {}, {
                addr: PLAYER_RENDER_PREPARE_OFF,
                player: ptrHex(this.player),
                mainListPtr: ptrHex(this.mainList),
                auxListPtr: ptrHex(this.auxList),
                arg3: ptrHex(args[3]),
                arg4: readArgInt(args[4]),
                arg5: readArgInt(args[5]),
            }, 'Player_prepareRenderItems.enter');
        },
        onLeave(retval) {
            const ctx = currentDrawContextFor(this.player);
            const ok = readArgInt(retval) !== 0;
            if (ctx && !isDirectAdaptorRoute(ctx)) {
                ensureDrawTargetCheckMisses(ctx);
                ctx.prepareCalled = true;
                ctx.prepareOk = ok;
                emitDrawStep(
                    ctx,
                    'prepare_render_items',
                    ok ? 'ok' : 'empty',
                    ok ? null : 'prepare_empty',
                    { prepareOk: ok });
                if (ok) {
                    const d3dDrawMode = readBool(
                        this.player, PLAYER_OFF.d3dDrawMode);
                    ctx.d3dDrawModeAfterPrepare = d3dDrawMode;
                    emitDrawStep(
                        ctx,
                        'branch_after_prepare',
                        d3dDrawMode ? 'shared_d3d' : 'ordinary',
                        d3dDrawMode
                            ? 'shared_d3d_after_prepare'
                            : 'ordinary_layer',
                        { d3dDrawModeAfterPrepare: d3dDrawMode });
                }
            }
            emitRender(STAGE_RENDER_PREPARE, 'prepare_leave', {
                ok: ok ? 1 : 0,
                renderLists: readRenderLists(this.mainList, this.auxList),
            }, {
                addr: PLAYER_RENDER_PREPARE_OFF,
                player: ptrHex(this.player),
                retval: ptrHex(retval),
            }, 'Player_prepareRenderItems.leave');
        },
    });

    attachAt(PLAYER_APPLY_PREPARED_PROJECTION_OFF,
             'Player_applyPreparedRenderItemProjection_guess', {
        onEnter(args) {
            this.player = args[0];
            this.mainList = args[1];
            emitRender(STAGE_RENDER_PREPARE, 'apply_projection_enter', {}, {
                addr: PLAYER_APPLY_PREPARED_PROJECTION_OFF,
                player: ptrHex(this.player),
                mainListPtr: ptrHex(this.mainList),
            }, 'Player_applyPreparedRenderItemProjection_guess.enter');
        },
        onLeave(retval) {
            const ctx = currentDrawContextFor(this.player);
            if (ctx && !isDirectAdaptorRoute(ctx)) {
                emitDrawStep(ctx, 'apply_prepared_projection', 'done');
            }
            emitRender(STAGE_RENDER_PREPARE, 'apply_projection_leave', {
                renderLists: readRenderLists(this.mainList, null),
            }, {
                addr: PLAYER_APPLY_PREPARED_PROJECTION_OFF,
                player: ptrHex(this.player),
                retval: ptrHex(retval),
            }, 'Player_applyPreparedRenderItemProjection_guess.leave');
        },
    });

    attachAt(PLAYER_BUILD_ITEMS_OFF, 'Player_buildRenderItems', {
        onEnter(args) {
            this.player = args[0];
            this.mainList = args[1];
            this.auxList = args[2];
            emitRender(STAGE_RENDER_COMMANDS, 'build_items_enter', {}, {
                addr: PLAYER_BUILD_ITEMS_OFF,
                player: ptrHex(this.player),
                mainListPtr: ptrHex(this.mainList),
                auxListPtr: ptrHex(this.auxList),
                defaultColor: readArgInt(args[3]),
                arg4: readArgInt(args[4]),
                arg5: readArgInt(args[5]),
            }, 'Player_appendPreparedRenderItems.enter');
        },
        onLeave(retval) {
            emitRender(STAGE_RENDER_COMMANDS, 'build_items_leave', {
                renderLists: readRenderLists(this.mainList, this.auxList),
            }, {
                addr: PLAYER_BUILD_ITEMS_OFF,
                player: ptrHex(this.player),
                retval: ptrHex(retval),
            }, 'Player_appendPreparedRenderItems.leave');
        },
    });

    attachAt(PLAYER_BUILD_COMMANDS_OFF, 'Player_buildRenderCommands', {
        onEnter(args) {
            this.player = args[0];
            this.mainList = args[1];
            this.auxList = args[2];
            emitRender(STAGE_RENDER_COMMANDS, 'build_commands_enter', {
                renderLists: readRenderLists(this.mainList, this.auxList),
            }, {
                addr: PLAYER_BUILD_COMMANDS_OFF,
                player: ptrHex(this.player),
                mainListPtr: ptrHex(this.mainList),
                auxListPtr: ptrHex(this.auxList),
                arg3: ptrHex(args[3]),
            }, 'Player_buildRenderCommands.enter');
        },
        onLeave(retval) {
            emitRender(STAGE_RENDER_COMMANDS, 'build_commands_leave', {
                renderLists: readRenderLists(this.mainList, this.auxList),
            }, {
                addr: PLAYER_BUILD_COMMANDS_OFF,
                player: ptrHex(this.player),
                retval: ptrHex(retval),
            }, 'Player_buildRenderCommands.leave');
        },
    });

    attachAt(PLAYER_ACCURATE_SLA_RENDER_OFF, 'Player_accurateSlaRender', {
        onEnter(args) {
            this.player = args[0];
            this.slaAdaptor = args[1];
            this.mainList = args[2];
            this.auxList = args[3];
            this.targetVariant = null;
            this.targetVariantObject = {
                object: null,
                objThis: null,
                type: null,
                error: null,
            };
            try {
                this.targetVariant = this.slaAdaptor.add(20);
                this.targetVariantObject =
                    readVariantObject(this.targetVariant);
            } catch (e) {
                this.targetVariantObject.error = String(e);
            }
            this.target = this.targetVariantObject.object;
            this.drawCtx = currentDrawContextFor(this.player);
            this.targetMatchesDrawArg = this.drawCtx
                ? ptrEqual(this.target, this.drawCtx.targetObject)
                : null;
            this.accurateSlaCtx = {
                player: this.player,
                slaAdaptor: this.slaAdaptor,
                targetVariant: this.targetVariant,
                targetVariantObject: this.targetVariantObject,
                target: this.target,
                mainList: this.mainList,
                auxList: this.auxList,
                drawCtx: this.drawCtx,
                targetMatchesDrawArg: this.targetMatchesDrawArg,
                entered: false,
                executeCtx: null,
            };
            enterAccurateSlaRenderExecute(this.accurateSlaCtx);
        },
        onLeave(retval) {
            leaveAccurateSlaRenderExecute(this.accurateSlaCtx, retval);
        },
    });

    attachAt(PLAYER_RENDER_EXECUTE_OFF, 'Player_renderExecute', {
        onEnter(args) {
            this.player = args[0];
            this.targetVariant = args[1];
            this.targetVariantObject = readVariantObject(this.targetVariant);
            this.target = this.targetVariantObject.object;
            this.drawCtx = currentDrawContextFor(this.player);
            this.targetMatchesDrawArg = this.drawCtx
                ? ptrEqual(this.target, this.drawCtx.targetObject)
                : null;
            this.mainList = args[2];
            this.auxList = args[3];
            this.executeCtx = {
                player: this.player,
                target: this.target,
                targetVariant: this.targetVariant,
                targetVariantObject: this.targetVariantObject,
                mainList: this.mainList,
                auxList: this.auxList,
            };
            activeRenderExecuteContexts.push(this.executeCtx);
            sendLayerRawProbe(
                this.player, this.target, null,
                'Player_renderToCanvas_0x6C4820.enter',
                {},
                {
                    addr: PLAYER_RENDER_EXECUTE_OFF,
                    targetVariant: ptrHex(this.targetVariant),
                    target: ptrHex(this.target),
                    targetObjThis: ptrHex(this.targetVariantObject.objThis),
                    targetError: this.targetVariantObject.error,
                    drawTarget: this.drawCtx
                        ? ptrHex(this.drawCtx.targetObject) : null,
                    targetMatchesDrawArg: this.targetMatchesDrawArg,
                });
            const checkpointTarget =
                checkpointTargetForDrawContext(this.drawCtx, this.target);
            sendRenderImageCheckpoint(
                this.player, checkpointTarget, 'execute_pre',
                'Player_renderToCanvas_0x6C4820.enter.after-target-resolve',
                undefined,
                this.target);
            emitRender(STAGE_RENDER_EXECUTE, 'execute_enter', {
                renderLists: readRenderLists(this.mainList, this.auxList),
            }, {
                addr: PLAYER_RENDER_EXECUTE_OFF,
                player: ptrHex(this.player),
                targetVariant: ptrHex(this.targetVariant),
                targetVariantType: this.targetVariantObject
                    ? this.targetVariantObject.type : null,
                target: ptrHex(this.target),
                targetObjThis: this.targetVariantObject
                    ? ptrHex(this.targetVariantObject.objThis) : null,
                targetError: this.targetVariantObject
                    ? this.targetVariantObject.error : null,
                drawTarget: this.drawCtx
                    ? ptrHex(this.drawCtx.targetObject) : null,
                targetMatchesDrawArg: this.targetMatchesDrawArg,
                mainListPtr: ptrHex(this.mainList),
                auxListPtr: ptrHex(this.auxList),
            }, 'Player_renderToCanvas_0x6C4820.enter');
        },
        onLeave(retval) {
            const leaveTarget = this.target ||
                (this.executeCtx ? this.executeCtx.target : null);
            const leaveTargetVariantObject =
                this.targetVariantObject ||
                (this.executeCtx ? this.executeCtx.targetVariantObject : null);
            const ctx = currentDrawContextFor(this.player);
            if (ctx) {
                ctx.renderToCanvasCalled = true;
                emitDrawStep(ctx, 'render_to_canvas', 'done', 'ordinary_layer');
            }
            emitRender(STAGE_RENDER_EXECUTE, 'execute_leave', {
                renderLists: readRenderLists(this.mainList, this.auxList),
                retval: ptrHex(retval),
            }, {
                addr: PLAYER_RENDER_EXECUTE_OFF,
                player: ptrHex(this.player),
                targetVariant: ptrHex(this.targetVariant),
                targetVariantType: leaveTargetVariantObject
                    ? leaveTargetVariantObject.type : null,
                target: ptrHex(leaveTarget),
                targetObjThis: leaveTargetVariantObject
                    ? ptrHex(leaveTargetVariantObject.objThis) : null,
                targetError: leaveTargetVariantObject
                    ? leaveTargetVariantObject.error : null,
                drawTarget: this.drawCtx
                    ? ptrHex(this.drawCtx.targetObject) : null,
                targetMatchesDrawArg: this.targetMatchesDrawArg,
            }, 'Player_renderToCanvas_0x6C4820.leave');
            sendLayerRawProbe(
                this.player, leaveTarget, null,
                'Player_renderToCanvas_0x6C4820.leave',
                {},
                {
                    addr: PLAYER_RENDER_EXECUTE_OFF,
                    targetVariant: ptrHex(this.targetVariant),
                    target: ptrHex(leaveTarget),
                    targetObjThis: leaveTargetVariantObject
                        ? ptrHex(leaveTargetVariantObject.objThis) : null,
                    targetError: leaveTargetVariantObject
                        ? leaveTargetVariantObject.error : null,
                    drawTarget: this.drawCtx
                        ? ptrHex(this.drawCtx.targetObject) : null,
                    targetMatchesDrawArg: this.targetMatchesDrawArg,
                });
            const leaveCheckpointTarget =
                checkpointTargetForDrawContext(this.drawCtx, leaveTarget);
            sendRenderImageCheckpoint(
                this.player, leaveCheckpointTarget, 'execute_post',
                'Player_renderToCanvas_0x6C4820.leave.before-return',
                undefined, leaveTarget);
            scheduleExecutePostUploadCheckpoint(
                this.player,
                'Player_renderToCanvas_0x6C4820.leave.after-next-drawdevice-upload');
            if (leaveCheckpointTarget) {
                lastRenderLayerObject = leaveCheckpointTarget;
            }
            if (this.executeCtx) {
                const idx = activeRenderExecuteContexts.lastIndexOf(this.executeCtx);
                if (idx >= 0) activeRenderExecuteContexts.splice(idx, 1);
            }
        },
    });

    attachAt(PLAYER_UPDATE_LAYER_AFTER_DRAW_OFF, 'Player_updateLayerAfterDraw', {
        onEnter(args) {
            this.player = args[0];
            this.targetVariant = args[1];
            this.targetVariantObject = readVariantObject(this.targetVariant);
            this.target = this.targetVariantObject.object;
            this.internalAssignRequested = readBool(
                this.player, PLAYER_OFF.internalAssignRequested);
            sendRenderImageCheckpoint(
                this.player, this.target, 'updateLayerAfterDraw_pre',
                'Player_updateLayerAfterDraw_0x6CBBB8.enter.after-target-resolve');
            sendLayerRawProbe(
                this.player, this.target, null,
                'Player_updateLayerAfterDraw_0x6CBBB8.enter',
                {
                    internalAssignRequested:
                        this.internalAssignRequested === true,
                },
                {
                    addr: PLAYER_UPDATE_LAYER_AFTER_DRAW_OFF,
                    targetVariant: ptrHex(this.targetVariant),
                    target: ptrHex(this.target),
                    targetObjThis: ptrHex(this.targetVariantObject.objThis),
                    targetError: this.targetVariantObject.error,
                });
        },
        onLeave() {
            sendRenderImageCheckpoint(
                this.player, this.target, 'updateLayerAfterDraw_post',
                'Player_updateLayerAfterDraw_0x6CBBB8.leave.before-return');
            sendLayerRawProbe(
                this.player, this.target, null,
                'Player_updateLayerAfterDraw_0x6CBBB8.leave',
                {
                    internalAssignRequested:
                        this.internalAssignRequested === true,
                },
                {
                    addr: PLAYER_UPDATE_LAYER_AFTER_DRAW_OFF,
                    targetVariant: ptrHex(this.targetVariant),
                    target: ptrHex(this.target),
                    targetObjThis: ptrHex(this.targetVariantObject.objThis),
                    targetError: this.targetVariantObject.error,
                });
            const ctx = currentDrawContextFor(this.player);
            if (!ctx) return;
            ctx.updateLayerAfterDrawCalled = true;
            ctx.internalAssignRequested = this.internalAssignRequested;
            emitDrawStep(
                ctx,
                'update_layer_after_draw',
                'done',
                'ordinary_layer',
                { internalAssignRequested: this.internalAssignRequested });
        },
    });

    // The active 1.3.9 capability ends at semantic render envelopes.  Do not
    // fall through to the retained 1.4.4 Layer/Bitmap/raw-probe hooks below.
    hooked = true;
    return;

    attachAt(LAYER_FILL_RECT_OFF, 'Layer_fillRect', {
        onEnter(args) {
            this.nativeLayer = args[0];
            this.player = currentRenderPlayer || lastCompletedTopPlayer;
        },
        onLeave() {
            sendLayerRawProbe(
                this.player, null, this.nativeLayer,
                'fillRect_0x80EBAC.leave',
                {},
                {
                    addr: LAYER_FILL_RECT_OFF,
                    nativeLayerArg: ptrHex(this.nativeLayer),
                });
        },
    });

    attachAt(LAYER_SAVE_LAYER_IMAGE_OFF, 'Layer_saveLayerImage', {
        onEnter(args) {
            this.nativeLayer = args[0];
            this.player = currentRenderPlayer || lastCompletedTopPlayer;
            this.mainImageAtEnter =
                readPointer(this.nativeLayer, LAYER_NATIVE_MAIN_IMAGE_OFF);
            const bitmapImpl = this.mainImageAtEnter
                ? readPointer(this.mainImageAtEnter, BITMAP_NATIVE_IMPL_OFF)
                : null;
            const width = bitmapImpl ? readU32(bitmapImpl, 12) : 0;
            const height = bitmapImpl ? readU32(bitmapImpl, 16) : 0;
            this.saveVisualCtx = {
                player: this.player,
                nativeLayer: this.nativeLayer,
                mainImage: this.mainImageAtEnter,
                bitmapImpl: bitmapImpl,
                width: width || 0,
                height: height || 0,
                rowBytes: width ? width * 4 : 0,
                bpp: 32,
            };
            activeSaveLayerImageContexts.push(this.saveVisualCtx);
            sendLayerRawProbe(
                this.player, null, this.nativeLayer,
                'saveLayerImage_0x80963C.enter',
                {},
                {
                    addr: LAYER_SAVE_LAYER_IMAGE_OFF,
                    nativeLayerArg: ptrHex(this.nativeLayer),
                    saveLayerImageMainImageA1Plus280:
                        ptrHex(this.mainImageAtEnter),
                });
        },
        onLeave() {
            if (this.saveVisualCtx) {
                const idx = activeSaveLayerImageContexts.lastIndexOf(
                    this.saveVisualCtx);
                if (idx >= 0) activeSaveLayerImageContexts.splice(idx, 1);
            }
            const mainImageAtLeave =
                readPointer(this.nativeLayer, LAYER_NATIVE_MAIN_IMAGE_OFF);
            sendLayerRawProbe(
                this.player, null, this.nativeLayer,
                'saveLayerImage_0x80963C.leave',
                {},
                {
                    addr: LAYER_SAVE_LAYER_IMAGE_OFF,
                    nativeLayerArg: ptrHex(this.nativeLayer),
                    saveLayerImageMainImageA1Plus280:
                        ptrHex(mainImageAtLeave),
                    saveLayerImageMainImageA1Plus280Enter:
                        ptrHex(this.mainImageAtEnter),
                    mainImagePointerStable:
                        ptrEqual(this.mainImageAtEnter, mainImageAtLeave),
                });
        },
    });

    attachAt(TVP_SAVE_AS_PNG_OFF, 'TVP_saveAsPNG', {
        onEnter(args) {
            this.image = args[2];
            this.ctx = null;
            const imageHex = ptrHex(this.image);
            for (let i = activeSaveLayerImageContexts.length - 1; i >= 0; --i) {
                const candidate = activeSaveLayerImageContexts[i];
                if (ptrHex(candidate.mainImage) === imageHex) {
                    this.ctx = Object.assign({}, candidate, {
                        image: this.image,
                    });
                    break;
                }
            }
            if (this.ctx) activePngSaveContexts.push(this.ctx);
        },
        onLeave() {
            if (!this.ctx) return;
            const idx = activePngSaveContexts.lastIndexOf(this.ctx);
            if (idx >= 0) activePngSaveContexts.splice(idx, 1);
        },
    });

    attachAt(BITMAP_GET_SCANLINE_OFF, 'Bitmap_getScanLineForRead', {
        onEnter(args) {
            this.ctx = null;
            this.rowIndex = readArgInt(args[1]);
            if (activePngSaveContexts.length === 0) return;
            const imageHex = ptrHex(args[0]);
            for (let i = activePngSaveContexts.length - 1; i >= 0; --i) {
                const candidate = activePngSaveContexts[i];
                if (ptrHex(candidate.image) === imageHex) {
                    this.ctx = candidate;
                    break;
                }
            }
        },
        onLeave(retval) {
            if (!this.ctx) return;
            sendSaveLayerVisualReadbackRow(
                this.ctx, this.rowIndex === null ? -1 : this.rowIndex, retval);
        },
    });

    attachAt(PLAYER_INIT_NON_EMOTE_OFF, 'Player_initNonEmoteMotion', {
        onEnter(args) {
            this.player = args[0];
            emitStaticParse('init_non_emote_enter', {}, {
                addr: PLAYER_INIT_NON_EMOTE_OFF,
                player: ptrHex(args[0]),
            });
            emitInitMotion('init_non_emote_enter', {}, {
                addr: PLAYER_INIT_NON_EMOTE_OFF,
                player: ptrHex(args[0]),
            });
        },
        onLeave(retval) {
            const overview = playerOverview(this.player);
            const rawParameterTable = overview.parameterTable;
            emitStaticParse('init_non_emote_leave', {
                parameterTable: semanticParameterTable(rawParameterTable),
            }, {
                addr: PLAYER_INIT_NON_EMOTE_OFF,
                retval: ptrHex(retval),
                player: ptrHex(this.player),
                parameterTable: parameterTableDiagnostics(rawParameterTable),
            });
            emitInitMotion('init_non_emote_leave', {
                overview: semanticPlayerOverview(overview),
            }, {
                addr: PLAYER_INIT_NON_EMOTE_OFF,
                retval: ptrHex(retval),
                player: ptrHex(this.player),
                overview: playerOverviewDiagnostics(overview),
            });
        },
    });

    attachAt(PLAYER_PARSE_PARAM_OFF, 'parse_motion_parameter', {
        onEnter(args) {
            this.arg0 = args[0];
            this.arg1 = args[1];
            emitStaticParse('parse_parameter_enter', {}, {
                addr: PLAYER_PARSE_PARAM_OFF,
                x0: ptrHex(args[0]),
                x1: ptrHex(args[1]),
            });
        },
        onLeave(retval) {
            emitStaticParse('parse_parameter_leave', {}, {
                addr: PLAYER_PARSE_PARAM_OFF,
                x0: ptrHex(this.arg0),
                x1: ptrHex(this.arg1),
                retval: ptrHex(retval),
            });
        },
    });

    attachAt(PLAYER_PARSE_PARAM_LIST_OFF, 'parse_motion_parameter_list', {
        onEnter(args) {
            this.arg0 = args[0];
            this.arg1 = args[1];
            emitStaticParse('parse_parameter_list_enter', {}, {
                addr: PLAYER_PARSE_PARAM_LIST_OFF,
                x0: ptrHex(args[0]),
                x1: ptrHex(args[1]),
            });
        },
        onLeave(retval) {
            emitStaticParse('parse_parameter_list_leave', {}, {
                addr: PLAYER_PARSE_PARAM_LIST_OFF,
                x0: ptrHex(this.arg0),
                x1: ptrHex(this.arg1),
                retval: ptrHex(retval),
            });
        },
    });

    attachAt(PLAYER_BIND_PARAM_OFF, 'bind_parameter_value', {
        onEnter(args) {
            this.player = args[0];
            this.labelPtr = args[1];
            this.mode = readArgInt(args[2]);
            this.value = readD0(this.context);
            this.valueRaw = readD0Raw(this.context);
            this.before = readParameterTable(args[0]);
            emit(STAGE_VARIABLE_BINDING, 'bind_parameter_enter', {
                addr: PLAYER_BIND_PARAM_OFF,
                player: ptrHex(args[0]),
                labelPtr: ptrHex(args[1]),
                label: readTtstr(args[1]),
                mode: this.mode,
                value: this.value,
                valueRaw: this.valueRaw,
                parameterTableBefore: this.before,
            });
        },
        onLeave(retval) {
            const after = readParameterTable(this.player);
            emit(STAGE_VARIABLE_BINDING, 'bind_parameter_leave', {
                addr: PLAYER_BIND_PARAM_OFF,
                player: ptrHex(this.player),
                labelPtr: ptrHex(this.labelPtr),
                label: readTtstr(this.labelPtr),
                mode: this.mode,
                value: this.value,
                valueRaw: this.valueRaw,
                retval: ptrHex(retval),
                changedEntries: parameterTableChanges(this.before, after),
                parameterTableAfter: after,
            });
        },
    });

    attachAt(PLAYER_EVALUATE_TIMELINE_OFF, 'Player_evaluateTimeline', {
        onEnter(args) {
            this.node = args[0];
            this.dirtyArg = readArgInt(args[1]);
            this.time = readD0(this.context);
            this.timeRaw = readD0Raw(this.context);
            this.before = semanticEvalNode(args[0]);
        },
        onLeave(retval) {
            emitFrameSelection('evaluate_timeline', {
                dirtyArg: this.dirtyArg,
                time: this.time,
                retval: readArgInt(retval),
                before: this.before,
                after: semanticEvalNode(this.node),
            }, {
                addr: PLAYER_EVALUATE_TIMELINE_OFF,
                node: ptrHex(this.node),
                timeRaw: this.timeRaw,
            });
        },
    });

    attachAt(PLAYER_SUB_MOTION_OFF, 'Player_subMotionDecision', {
        onEnter(args) {
            this.player = args[0];
            this.samplesBefore = currentSamplePlayers();
            this.before = snapshotMotionSubNodes(args[0]);
        },
        onLeave(retval) {
            const samplesAfter = currentSamplePlayers();
            const childSampleDelta = Math.max(0, samplesAfter.length - this.samplesBefore.length);
            const after = snapshotMotionSubNodes(this.player);
            emit(STAGE_SUB_MOTION_DECISION, 'sub_motion_decision', {
                addr: PLAYER_SUB_MOTION_OFF,
                player: ptrHex(this.player),
                retval: ptrHex(retval),
                childSamplesBefore: this.samplesBefore,
                childSamplesAfter: samplesAfter,
                childSampleDelta: childSampleDelta,
                decisions: compareMotionSubSnapshots(this.before, after, childSampleDelta),
            });
        },
    });

    hooked = true;
}

rpc.exports = {
    setup() {
        return {
            base: ensureBase().toString(),
            oracle: 'Kirikiroid2-1.3.9-Android-arm64-v8a',
            capability: 'motion-render-semantic-v1',
            stages: [STAGE_TRACE_FLATTEN],
            renderStages: RENDER_STAGES.slice(),
            nodeStride: NODE_STRIDE,
            parameterEntryStride: PARAM_ENTRY_STRIDE,
            offsets: {
                progressCompat: PLAYER_PROGRESS_COMPAT_OFF,
                phase3Last: PLAYER_PHASE3_LAST_OFF,
                draw: PLAYER_DRAW_COMPAT_OFF,
                directD3D: PLAYER_DRAW_D3D_OFF,
                directSLA: PLAYER_DRAW_SLA_OFF,
                prepareRenderItems: PLAYER_RENDER_PREPARE_OFF,
                applyProjection: PLAYER_APPLY_PREPARED_PROJECTION_OFF,
                appendPreparedItems: PLAYER_BUILD_ITEMS_OFF,
                buildRenderCommands: PLAYER_BUILD_COMMANDS_OFF,
                accurateSLARender: PLAYER_ACCURATE_SLA_RENDER_OFF,
                canvasRender: PLAYER_RENDER_EXECUTE_OFF,
                updateLayerAfterDraw: PLAYER_UPDATE_LAYER_AFTER_DRAW_OFF,
                debugMessage: DEBUG_MESSAGE_OFF,
                continuousEventTickReturn:
                    TVP_CONTINUOUS_EVENT_TICK_RETURN_OFF,
                getRoughTickCount32: TVP_GET_ROUGH_TICK_COUNT32_OFF,
            },
        };
    },
    startRecord(stageNames, options) {
        const requested = Array.isArray(stageNames) ? stageNames : ALL_STAGES;
        const unsupported = requested.filter(
            (stage) => !SUPPORTED_STAGES.has(stage));
        if (unsupported.length > 0) {
            throw new Error(
                'Kirikiroid2 1.3.9 semantic oracle does not expose: ' +
                unsupported.join(', ')
            );
        }
        if (options && (
                options.recordRenderStepCheckpoints ||
                options.recordLayerRawProbes ||
                options.recordSaveLayerVisualReadbackProbes)) {
            throw new Error(
                'Kirikiroid2 1.3.9 render_path is semantic-only; ' +
                'Layer/Bitmap image and raw probes are not rebased'
            );
        }
        enabledStages = new Set(requested);
        enabledStages.add(STAGE_TRACE_FLATTEN);
        const requestedSimulationFps = Number(
            options && options.simulationFps);
        virtualContinuousTickFps =
            Number.isFinite(requestedSimulationFps) &&
            requestedSimulationFps > 0
                ? requestedSimulationFps : 0;
        virtualContinuousTickBase = null;
        virtualContinuousTickIndex = 0;
        recordRenderStepCheckpoints =
            !!(options && options.recordRenderStepCheckpoints);
        recordLayerRawProbes =
            !!(options && options.recordLayerRawProbes);
        recordSaveLayerVisualReadbackProbes =
            !!(options && options.recordSaveLayerVisualReadbackProbes);
        saveLayerVisualReadbackFrameStart =
            options && Number.isInteger(options.saveLayerVisualReadbackFrameStart)
                ? options.saveLayerVisualReadbackFrameStart : 0;
        saveLayerVisualReadbackFrameCount =
            options && Number.isInteger(options.saveLayerVisualReadbackFrameCount)
                ? options.saveLayerVisualReadbackFrameCount : 1;
        captureFrameStart =
            options && Number.isInteger(options.captureFrameStart)
                ? options.captureFrameStart : 0;
        captureFrameCount =
            options && Number.isInteger(options.captureFrameCount)
                ? options.captureFrameCount : -1;
        renderCaseFrameBases = {};
        if (options && options.renderCaseFrameBases &&
            typeof options.renderCaseFrameBases === 'object') {
            Object.keys(options.renderCaseFrameBases).forEach((caseId) => {
                const frameBase = options.renderCaseFrameBases[caseId];
                if (Number.isInteger(frameBase)) {
                    renderCaseFrameBases[String(caseId)] = frameBase;
                }
            });
        }
        events = [];
        frameCounter = 0;
        nextFrameId = 0;
        seqCounter = 0;
        startTimeMs = Date.now();
        lastCompletedFrameId = null;
        lastCompletedTopPlayer = null;
        currentRenderFrameId = null;
        currentRenderPlayer = null;
        lastRenderLayerObject = null;
        lastDrawTargetObject = null;
        lastSlaRenderTargetObject = null;
        lastSlaRenderNativeLayer = null;
        adaptorRenderTargetCache = {};
        drawIdCounter = 0;
        activeDrawContexts = [];
        activeSaveLayerImageContexts = [];
        activePngSaveContexts = [];
        activeRenderExecuteContexts = [];
        installHook();
        recording = true;
        return true;
    },
    stopRecord() {
        recording = false;
        return events.slice();
    },
    eventCount() {
        return frameCounter;
    },
    rawEventCount() {
        return events.length;
    },
};
