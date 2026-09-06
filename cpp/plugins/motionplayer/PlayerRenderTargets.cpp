// PlayerRenderTargets.cpp — Layer/SLA/D3D render targets and post-draw update
// Split from PlayerRender.cpp for maintainability.
//
#include "PlayerRenderInternal.h"
#if defined(KRKR2_WASMTIME_HEADLESS)
#include "MotionTraceWeb.h"
#endif
#include "MotionRenderBackend.h"
#include "Platform.h"
#include "PrivateMotionGLL.h"
#include "RenderManager.h"
#include "ResourceManager.h"
#include "SourceCache.h"
#include "ncbind.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace motion::internal;
using namespace motion::internal::render_detail;

extern bool TVPWindowUpdateEventsDelivering;

namespace motion {
    namespace {
        using PreparedRenderItem = detail::PreparedRenderItem;

        struct DispatchReleaseGuard_guess {
            iTJSDispatch2 *dispatch = nullptr;

            DispatchReleaseGuard_guess() = default;
            DispatchReleaseGuard_guess(
                const DispatchReleaseGuard_guess &) = delete;
            DispatchReleaseGuard_guess &operator=(
                const DispatchReleaseGuard_guess &) = delete;

            ~DispatchReleaseGuard_guess() {
                if(dispatch) {
                    dispatch->Release();
                }
            }
        };

        std::array<tTVPPointD, 6> makeTextureQuad(
            const std::array<int, 4> &rect) {
            const double l = rect[0], t = rect[1];
            const double r = rect[2], b = rect[3];
            return {{
                {l, t}, {r, t}, {l, b}, {r, t}, {l, b}, {r, b},
            }};
        }

        std::array<tTVPPointD, 6> makeAffineTargetQuad(
            const PreparedRenderItem &item,
            float xOffset,
            float yOffset) {
            const tTVPPointD p0{
                static_cast<double>(item.corners[0] + xOffset),
                static_cast<double>(item.corners[1] + yOffset)};
            const tTVPPointD p1{
                static_cast<double>(item.corners[2] + xOffset),
                static_cast<double>(item.corners[3] + yOffset)};
            const tTVPPointD p2{
                static_cast<double>(item.corners[6] + xOffset),
                static_cast<double>(item.corners[7] + yOffset)};
            const tTVPPointD p3{
                p1.x - p0.x + p2.x,
                p1.y - p0.y + p2.y};
            return {{p0, p1, p2, p1, p2, p3}};
        }

        std::vector<tTVPPointD> buildOffsetMeshPoints(
            const std::vector<detail::MeshPoint> &points,
            float xOffset,
            float yOffset) {
            std::vector<tTVPPointD> out;
            out.reserve(points.size());
            for(const auto &point : points) {
                out.push_back({
                    static_cast<double>(point.x + xOffset),
                    static_cast<double>(point.y + yOffset)});
            }
            return out;
        }

        bool shouldQueuePrivateMotionGLLRenderItem_guess(
            const PreparedRenderItem &item,
            bool priorDraw) {
            if((item.blendMode & 0xF) == 6 || item.skipFlag0 ||
               item.rawFlag16 || item.opacity == 0) {
                return false;
            }
            if(priorDraw && !item.skipFlag1) {
                return false;
            }
            return !item.sourceState->blank;
        }

        int privateMotionGLLOpacity_guess(
            const PreparedRenderItem &item,
            bool priorDraw) {
            int opacity = item.opacity;
            if(priorDraw) {
                // C++ signed division truncates toward zero. The references
                // implement this as (x + signbit) arithmetic-shift-right.
                opacity /= 2;
            }
            return opacity;
        }

        std::vector<detail::MeshPoint> *
        populatePrivateMotionGLLPoints_guess(
            PreparedRenderItem &item,
            PrivateMotionGLLRenderItemInput_guess &queueItem) {
            if(item.meshType == 0) {
                queueItem.affinePoints =
                    std::array<detail::MeshPoint, 3>{ {
                        { item.corners[0], item.corners[1] },
                        { item.corners[2], item.corners[3] },
                        { item.corners[6], item.corners[7] },
                    } };
                return nullptr;
            }
            if(item.meshType == 1) {
                return &item.meshPoints;
            }
            if(item.meshType == 2) {
                return &item.commandCompositeMeshPoints;
            }
            return nullptr;
        }

        unsigned int d3dPackedColorWithOpacity(
            const PreparedRenderItem &item,
            int opacity) {
            const auto base = item.packedColors[0];
            const auto rgb = base == 0xFF808080u ? 0x00FFFFFFu
                                                 : (base & 0x00FFFFFFu);
            return rgb |
                (static_cast<unsigned int>(static_cast<std::uint8_t>(opacity))
                 << 24u);
        }

        tTVPLayerType accurateSlaLayerType_guess(int rawBlendMode) {
            switch(rawBlendMode & 0x0F) {
                case 1: return ltPsAdditive;
                case 2:
                case 5: return ltPsSubtractive;
                case 3: return ltPsMultiplicative;
                case 4: return ltPsScreen;
                default: return ltAlpha;
            }
        }

        bool accurateSlaSkipsMaskBuffer_guess(int rawBlendMode) {
            return (rawBlendMode & 0x0F) == 6;
        }

        bool shouldRenderAccurateSlaItem_guess(
            const PreparedRenderItem &item) {
            return !item.skipFlag0 && !item.rawFlag16 && item.opacity != 0;
        }

        bool computeAccurateSlaClip_guess(
            const PreparedRenderItem &item,
            int canvasWidth,
            int canvasHeight,
            RenderClipRect &out) {
            float clipLeft = std::fmax(item.paintBox[0], 0.0f);
            float clipTop = std::fmax(item.paintBox[1], 0.0f);
            const float width = static_cast<float>(canvasWidth);
            const float height = static_cast<float>(canvasHeight);
            float clipRight = item.paintBox[2] < width
                ? item.paintBox[2]
                : width;
            float clipBottom = item.paintBox[3] < height
                ? item.paintBox[3]
                : height;

            // The native item has no separate viewport-valid byte. A missing
            // viewport is represented only by its inverted numeric rectangle.
            if(item.viewport[2] >= item.viewport[0] &&
               item.viewport[3] >= item.viewport[1]) {
                const float viewportLeft = std::floor(item.viewport[0]);
                const float viewportTop = std::floor(item.viewport[1]);
                const float viewportRight = std::ceil(item.viewport[2]);
                const float viewportBottom = std::ceil(item.viewport[3]);
                clipLeft = viewportLeft < clipLeft
                    ? clipLeft
                    : viewportLeft;
                clipTop = viewportTop < clipTop
                    ? clipTop
                    : viewportTop;
                clipRight = clipRight < viewportRight
                    ? clipRight
                    : viewportRight;
                clipBottom = clipBottom < viewportBottom
                    ? clipBottom
                    : viewportBottom;
            }

            // Native uses the MI condition after FCMP: equality and unordered
            // (NaN) survive, while only a strictly reversed edge is rejected.
            if(clipRight < clipLeft || clipBottom < clipTop) {
                return false;
            }

            out.left = clipLeft;
            out.top = clipTop;
            out.right = clipRight;
            out.bottom = clipBottom;
            return true;
        }

        void setInternalWorkspaceLayerSize_guess(iTJSDispatch2 *layerObject,
                                                 int width,
                                                 int height) {
            tTJSVariant widthArg(width);
            tTJSVariant heightArg(height);
            tTJSVariant *args[] = { &widthArg, &heightArg };
            (void)layerObject->FuncCall(
                0, TJS_W("setSize"), &detail::setSizeMemberHint_guess,
                nullptr, 2, args, layerObject);
        }

        bool markD3DStencilMaskChain_guess(PreparedRenderItem *item,
                                           std::uint8_t ref) {
            bool hasDrawableMaskTarget = false;
            for(auto *ancestor = item; ancestor; ancestor = ancestor->parentItem) {
                ancestor->stencilMaskRef = ref;
                if(!ancestor->rawFlag16 && !ancestor->skipFlag0) {
                    hasDrawableMaskTarget = true;
                }
                for(auto *child : ancestor->childItems) {
                    child->stencilMaskRef = ref;
                    if(!child->rawFlag16 && !child->skipFlag0 &&
                       child->opacity != 0) {
                        hasDrawableMaskTarget = true;
                    }
                }
            }
            return hasDrawableMaskTarget;
        }

        int assignD3DStencilRefs_guess(
            std::vector<PreparedRenderItem *> &items) {
            for(auto *itemPtr : items) {
                auto &item = *itemPtr;
                item.stencilMaskRef = 0;
                item.stencilWriteRef = 0;
            }

            int stencilCount = 0;
            static bool stencilOverflowLogged = false;
            for(auto *itemPtr : items) {
                auto &item = *itemPtr;
                if((item.blendMode & 0xF) == 6 || !item.drawFlag ||
                   item.rawFlag16 || item.opacity == 0 || !item.parentItem) {
                    continue;
                }
                const int previousStencilCount = stencilCount++;
                if(previousStencilCount >= 255 && !stencilOverflowLogged) {
                    stencilOverflowLogged = true;
                    const ttstr message(
                        TJS_W("StencilCount overflow(256)"));
                    const ttstr caption(TJS_W("MMotionPlayer"));
                    (void)TVPShowSimpleMessageBox(message, caption);
                }
                const auto ref = static_cast<std::uint8_t>(stencilCount);
                item.stencilWriteRef = ref;
                if(!markD3DStencilMaskChain_guess(item.parentItem, ref)) {
                    item.stencilWriteRef = 0;
                }
            }
            return stencilCount;
        }

        bool computeD3DClip_guess(
            const PreparedRenderItem &item,
            int canvasWidth,
            int canvasHeight,
            std::array<float, 4> &out) {
            float clipLeft = std::fmax(item.paintBox[0], 0.0f);
            float clipTop = std::fmax(item.paintBox[1], 0.0f);
            const float width = static_cast<float>(canvasWidth);
            const float height = static_cast<float>(canvasHeight);
            // Keep the native compare/select operand order explicit. Besides
            // selecting the canvas bound for an unordered paint edge, this
            // makes equality select the second operand (including its zero
            // sign) rather than std::min's first argument.
            float clipRight = item.paintBox[2] < width
                ? item.paintBox[2]
                : width;
            float clipBottom = item.paintBox[3] < height
                ? item.paintBox[3]
                : height;

            if(item.viewport[2] >= item.viewport[0] &&
               item.viewport[3] >= item.viewport[1]) {
                const float viewportLeft = std::floor(item.viewport[0]);
                const float viewportTop = std::floor(item.viewport[1]);
                const float viewportRight = std::ceil(item.viewport[2]);
                const float viewportBottom = std::ceil(item.viewport[3]);
                clipLeft = viewportLeft < clipLeft
                    ? clipLeft
                    : viewportLeft;
                clipTop = viewportTop < clipTop
                    ? clipTop
                    : viewportTop;
                clipRight = clipRight < viewportRight
                    ? clipRight
                    : viewportRight;
                clipBottom = clipBottom < viewportBottom
                    ? clipBottom
                    : viewportBottom;
            }

            out = {clipLeft, clipTop, clipRight, clipBottom};
            // The native ordered >= tests reject equality and reversed edges.
            // An unordered edge would survive this gate, although the normal
            // construction above replaces or rejects every input NaN first.
            return !(clipLeft >= clipRight || clipTop >= clipBottom);
        }

        int prepareD3DRenderItems_guess(
            std::vector<PreparedRenderItem *> &items,
            int width,
            int height,
            bool priorDraw) {
            if(priorDraw) {
                return 0;
            }

            const int stencilCount = assignD3DStencilRefs_guess(items);
            for(auto *item : items) {
                if(!item->drawFlag) {
                    continue;
                }
                std::array<float, 4> clip{};
                const bool hasClip = computeD3DClip_guess(
                    *item, width, height, clip);
                if(!hasClip || item->rawFlag16) {
                    item->rawFlag21 = false;
                    continue;
                }
                item->rawFlag21 = true;
                item->clipRect = clip;
                item->leafLayer.Clear();
            }
            return stencilCount;
        }

        void appendD3DAffine_guess(
            motion::render_backend_guess::TriangleBatch_guess &batch,
            iTVPRenderMethod *method,
            const D3DTargetTextureGetter_guess &targetTextureGetter,
            const tTVPRect &targetRect,
            const PreparedRenderItem &item,
            iTVPTexture2D *sourceTexture,
            const std::array<int, 4> &sourceRect,
            float xOffset,
            float yOffset,
            std::uint32_t packedColor) {
            const auto dst = makeAffineTargetQuad(
                item, xOffset, yOffset);
            const auto src = makeTextureQuad(sourceRect);
            // The getter returns (reference, target). D3DAdaptor's callable
            // rereads its live target slot here, after the source callback and
            // method selection; D3DLayer's callable returns its captured target.
            const auto targets = targetTextureGetter(
                method->IsBlendTarget(), targetRect);
            batch.appendTriangles_guess(
                method, sourceTexture, targets.second, targets.first, targetRect,
                src.data(), dst.data(), dst.size(), packedColor);
        }

        void appendD3DMesh_guess(
            motion::render_backend_guess::TriangleBatch_guess &batch,
            iTVPRenderMethod *method,
            const D3DTargetTextureGetter_guess &targetTextureGetter,
            const tTVPRect &targetRect,
            iTVPTexture2D *sourceTexture,
            const std::array<int, 4> &sourceRect,
            const std::vector<tTVPPointD> &boundsPoints,
            const std::vector<tTVPPointD> &meshPoints,
            int meshDivX,
            int meshDivY,
            std::uint32_t packedColor) {
            tTVPRect computedBounds(targetRect);
            motion::render_backend_guess::buildAndSubmitMeshTriangles_guess(
                computedBounds, sourceTexture,
                tTVPRect(sourceRect[0], sourceRect[1],
                         sourceRect[2], sourceRect[3]),
                boundsPoints, meshPoints, meshDivX, meshDivY,
                [&](iTVPTexture2D *submittedSourceTexture,
                    const std::vector<tTVPPointD> &sourceVertices,
                    const std::vector<tTVPPointD> &destinationVertices) {
                    const auto targets = targetTextureGetter(
                        method->IsBlendTarget(), targetRect);
                    batch.appendTriangles_guess(
                        method, submittedSourceTexture,
                        targets.second, targets.first,
                        targetRect, sourceVertices.data(),
                        destinationVertices.data(),
                        destinationVertices.size(), packedColor);
                });
        }

        bool shouldSkipD3DRenderItem_guess(
            const PreparedRenderItem &item,
            bool priorDraw) {
            if((item.blendMode & 0xF) == 6) {
                return true;
            }
            if(item.skipFlag0 || item.rawFlag16) {
                return true;
            }
            return priorDraw && !item.skipFlag1;
        }
    } // namespace

    bool internal::render_detail::computeD3DClipForDifferentialTest_guess(
        const detail::PreparedRenderItem &item,
        int canvasWidth,
        int canvasHeight,
        std::array<float, 4> &out) {
        return computeD3DClip_guess(item, canvasWidth, canvasHeight, out);
    }

    bool internal::render_detail::
    computeAccurateSlaClipForDifferentialTest_guess(
        const detail::PreparedRenderItem &item,
        int canvasWidth,
        int canvasHeight,
        std::array<float, 4> &out) {
        RenderClipRect clip;
        const bool valid = computeAccurateSlaClip_guess(
            item, canvasWidth, canvasHeight, clip);
        out = {clip.left, clip.top, clip.right, clip.bottom};
        return valid;
    }

    tjs_int internal::render_detail::getInternalWorkspaceDimension_guess(
        ncbPropAccessor &object,
        const tjs_char *member,
        tjs_uint32 *hint) {
        if(!object.HasValue(member, hint)) {
            return 0;
        }
        return object.GetValue(
            member, ncbTypedefs::Tag<tjs_int>(), 0, hint);
    }

    void Player::materializeInternalRenderLayers_guess(
        const tTJSVariant &target) {
        // All four references gate only on the primary internal Layer Variant.
        // Its assignment is published before probing dimensions, sizing it,
        // and creating the work Layer. A later call therefore does not repair
        // a missing or incompletely-sized work Layer after partial failure.
        if(_internalRenderLayer.Type() != tvtVoid) {
            return;
        }

        ncbPropAccessor targetAccessor{tTJSVariant(target)};
        tTJSVariant owner = targetAccessor.GetValue(
            TJS_W("window"), ncbTypedefs::Tag<tTJSVariant>(), 0,
            &detail::windowMemberHint_guess);

        ncbPropAccessor internal{tTJSVariant(
            _internalRenderLayer =
                detail::createLayerVariant_guess(owner, target))};

        const tjs_int height = getInternalWorkspaceDimension_guess(
            targetAccessor, TJS_W("height"),
            &detail::heightMemberHint_guess);
        const tjs_int width = getInternalWorkspaceDimension_guess(
            targetAccessor, TJS_W("width"),
            &detail::widthMemberHint_guess);
        setInternalWorkspaceLayerSize_guess(
            internal.GetDispatch(), width, height);

        ncbPropAccessor work{tTJSVariant(
            _internalSourceWorkLayer_guess =
                detail::createLayerVariant_guess(owner, target))};
        setInternalWorkspaceLayerSize_guess(work.GetDispatch(), width, height);
    }

    void Player::renderViaSharedD3DAdaptor(
        const tTJSVariant &targetVariant,
        detail::PreparedRenderItemList &mainList) {
        // Construction precedes target selection. A later SLA resolver,
        // Invalidate, dispatch call, or capture failure therefore leaves the
        // process-global adaptor published for subsequent Players.
        auto *sharedAdaptor = ensureSharedD3DAdaptor();

        // The inline native branch default-constructs this owner, then assigns
        // exactly one branch result. In particular, an SLA draw never first
        // retains the original adaptor Variant. The ordinary branch copies the
        // complete Variant pair, preserving an objthis distinct from object.
        tTJSVariant selectedTarget;
        iTJSDispatch2 *targetObject = targetVariant.AsObjectNoAddRef();
        if(auto *sla =
               ncbInstanceAdaptor<SeparateLayerAdaptor>::GetNativeInstance(
                   targetObject, false)) {
            // The native sticky route treats an SLA specially: rotate its two
            // trees, resolve ordinal zero without a payload/sequence advance,
            // invalidate the unused retired tree, and then clear the private
            // target plus the newly active tree. selectedTarget deliberately
            // keeps an owning reference across those invalidations.
            sla->beginLayerPass_guess();
            selectedTarget = sla->resolveLayerOrdinal_guess(0);
            sla->endLayerPass_guess();
            sla->clear();
        } else {
            selectedTarget = targetVariant;
        }

        // This path stays on the TJS dispatch boundary. It does not unwrap a
        // native Layer, branch on either HRESULT, or repair an invalidated SLA
        // Layer. Width is the first argument and height the second.
        ncbPropAccessor target{selectedTarget};
        iTJSDispatch2 *targetDispatch = target.GetDispatch();
        tTJSVariant widthArg(sharedAdaptor->getWidth());
        tTJSVariant heightArg(sharedAdaptor->getHeight());
        tTJSVariant *sizeArgs[] = {&widthArg, &heightArg};
        (void)targetDispatch->FuncCall(
            0, TJS_W("setSize"), &detail::setSizeMemberHint_guess,
            nullptr, 2, sizeArgs, targetDispatch);
        (void)target.SetValue(
            TJS_W("visible"), static_cast<tjs_int>(1), TJS_MEMBERENSURE,
            &detail::visibleMemberHint_guess);

        sharedAdaptor->renderFromPlayer_guess(this, mainList);
        sharedAdaptor->captureCanvas(selectedTarget);
    }


    int Player::buildPrivateMotionGLLCommands_guess(
        tTJSNI_BaseLayer *renderLayer,
        tjs_int canvasWidth,
        tjs_int canvasHeight,
        detail::PreparedRenderItemList &mainList,
        detail::PreparedRenderItemList &auxList) {
        const bool priorDraw = _priorDraw;
        int stencilCount = 0;
        if(!priorDraw) {
            stencilCount = assignD3DStencilRefs_guess(mainList);
            for(auto *itemPtr : mainList) {
                auto &item = *itemPtr;
                if(!item.drawFlag) {
                    continue;
                }
                std::array<float, 4> clip{};
                const bool hasClip = computeD3DClip_guess(
                    item, canvasWidth, canvasHeight, clip);
                if(!hasClip || item.rawFlag16) {
                    item.rawFlag21 = false;
                    continue;
                }
                item.rawFlag21 = true;
                item.clipRect = clip;
                item.leafLayer.Clear();
                if(!item.rawFlag20) {
                    item.renderLayerId = dispatchRequireLayerId(
                        &detail::nodeRequireLayerIdMemberHint_guess);
                    item.rawFlag20 = true;
                }
            }
        }
        (void)auxList;
        clearPrivateMotionGLLRenderQueue_guess(renderLayer);
        for(auto *itemPtr : mainList) {
            auto &item = *itemPtr;
            if(!shouldQueuePrivateMotionGLLRenderItem_guess(
                   item, priorDraw)) {
                continue;
            }
            auto *sourceTexture =
                nativeRM()->loadRenderSourceTextureFromItem_guess(
                    *this, item);
            PrivateMotionGLLRenderItemInput_guess queueItem;
            queueItem.opacity =
                privateMotionGLLOpacity_guess(item, priorDraw);
            queueItem.stencilMaskRef = item.stencilMaskRef;
            queueItem.stencilWriteRef = item.stencilWriteRef;
            queueItem.blendMode = item.blendMode;
            queueItem.geometryType = item.meshType;
            if(item.meshType == 1) {
                const auto cellDivisions =
                    renderBezierPatchCellDivisions_guess(
                        item.commandPatchDivision,
                        item.sourceState->width,
                        item.sourceState->height);
                queueItem.meshDivX = cellDivisions[0];
                queueItem.meshDivY = cellDivisions[1];
            } else if(item.meshType == 2) {
                queueItem.meshDivX = item.meshDivX;
                queueItem.meshDivY = item.meshDivY;
            }
            queueItem.packedColors = item.packedColors;
            queueItem.sourceRect = item.sourceState->textureRect;
            queueItem.sourceTexture = sourceTexture;
            auto *pointsToSwap =
                populatePrivateMotionGLLPoints_guess(item, queueItem);
            appendPrivateMotionGLLRenderItem_guess(renderLayer,
                                                   queueItem,
                                                   pointsToSwap);
        }
        return stencilCount;
    }

    void Player::computeParticleOutsideRect_guess(
        const std::array<float, 4> &targetRect) {
        _particleOutsideRect = internal::computeParticleOutsideRect_guess(
            targetRect,
            _drawAffineM11, _drawAffineM12,
            _drawAffineM21, _drawAffineM22,
            _drawAffineM14, _drawAffineM24,
            _cameraOffsetX, _cameraOffsetY,
            _outsideFactor);
    }

    void Player::renderAccurateSeparateLayerAdaptor_guess(
        SeparateLayerAdaptor *sla,
        detail::PreparedRenderItemList &mainList,
        detail::PreparedRenderItemList &auxList) {
        // The target Variant is materialized before the Layer class accessor.
        // Width and height are then read through TJS in that order, with no
        // native-instance fallback or positive-size admission gate.
        ncbPropAccessor targetLayerOwner{sla->getTargetLayer()};
        iTJSDispatch2 *targetLayerObject = targetLayerOwner.GetDispatch();
        ncbPropAccessor layerClass{TJS_W("Layer")};
        iTJSDispatch2 *layerClassObject = layerClass.GetDispatch();
        const auto readTargetDimension =
            [layerClassObject, targetLayerObject](
                const tjs_char *member, tjs_uint32 *hint) {
                tTJSVariant value;
                (void)layerClassObject->PropGet(
                    0, member, hint, &value, targetLayerObject);
                return static_cast<tjs_int>(value.AsInteger());
            };
        const tjs_int canvasWidth = readTargetDimension(
            TJS_W("width"), &detail::widthMemberHint_guess);
        const tjs_int canvasHeight = readTargetDimension(
            TJS_W("height"), &detail::heightMemberHint_guess);

        const std::array<float, 4> targetClip{
            0.0f, 0.0f,
            static_cast<float>(canvasWidth),
            static_cast<float>(canvasHeight)
        };
        computeParticleOutsideRect_guess(targetClip);

        // The external SLA pass starts before command construction. Its normal
        // tail clears retired Layers; an exception deliberately leaves them for
        // the next pass swap.
        sla->beginLayerPass_guess();
        buildRenderCommands(mainList, auxList, targetClip);

        for(auto *itemPtr : mainList) {
            if(!shouldRenderAccurateSlaItem_guess(*itemPtr)) {
                continue;
            }
            auto &item = *itemPtr;

            RenderClipRect clip;
            if(!computeAccurateSlaClip_guess(
                   item, static_cast<int>(canvasWidth),
                   static_cast<int>(canvasHeight), clip)) {
                continue;
            }

            const tjs_real clipWidth = static_cast<tjs_real>(
                clip.right - clip.left);
            const tjs_real clipHeight = static_cast<tjs_real>(
                clip.bottom - clip.top);
            const auto layerType =
                accurateSlaLayerType_guess(item.blendMode);
            const float offsetX = -0.5f - static_cast<float>(clip.left);
            const float offsetY = -0.5f - static_cast<float>(clip.top);

            SeparateLayerPayload_guess payload;
            payload.completionType = _completionType;
            payload.hasOutlineOrMeshline =
                _outline.Type() != tvtVoid || _meshline.Type() != tvtVoid;
            payload.commandSrc = item.commandSrc;
            payload.blendMode = item.blendMode;
            payload.packedColors = item.packedColors;
            payload.paintAndViewport = {
                item.paintBox[0], item.paintBox[1], item.paintBox[2],
                item.paintBox[3], item.viewport[0], item.viewport[1],
                item.viewport[2], item.viewport[3]
            };
            if(item.meshType == 2) {
                payload.compositeMeshPoints =
                    item.commandCompositeMeshPoints;
            } else if(item.meshType == 1) {
                payload.bezierPatchPoints = item.meshPoints;
            }
            payload.corners = item.corners;

            bool createdOrChanged = false;
            tTJSVariant baseLayerVariant =
                sla->resolveLayerNode_guess(
                    static_cast<tjs_uint32>(item.layerId1), payload,
                    createdOrChanged);
            // A temporary Variant CopyRef obtains one Object-only raw retain,
            // then dies before any later callback. That raw owner spans the
            // rest of the item. Masked, debug and publication phases repeat
            // the same temporary-copy acquisition independently below.
            DispatchReleaseGuard_guess baseLayerObjectOwner;
            baseLayerObjectOwner.dispatch =
                retainObjectFromVariantCopy_guess(baseLayerVariant);
            auto *baseLayerObject = baseLayerObjectOwner.dispatch;

            // The shipped payload comparator currently reports refresh for
            // every comparison, but this caller-side gate is present in all
            // four references and remains observable for compatible builds.
            if(createdOrChanged) {
                tTJSVariant sourceObject =
                    nativeRM()->loadRenderSourceLayerFromItem_guess(
                        *this, item);
                ncbPropAccessor sourceLayerOwner{sourceObject};
                auto *sourceLayerObject = sourceLayerOwner.GetDispatch();
                const auto readSourceDimension =
                    [sourceLayerObject](const tjs_char *member,
                                        tjs_uint32 *hint) {
                        tTJSVariant value;
                        (void)sourceLayerObject->PropGet(
                            0, member, hint, &value, sourceLayerObject);
                        return static_cast<tjs_int>(value.AsInteger());
                    };
                const tjs_int sourceWidth = readSourceDimension(
                    TJS_W("width"), &detail::widthMemberHint_guess);
                const tjs_int sourceHeight = readSourceDimension(
                    TJS_W("height"), &detail::heightMemberHint_guess);

                (void)callLayerSetSizeReal_guess(
                    baseLayerObject, clipWidth, clipHeight);

                const tTVPRect sourceRect(
                    0, 0, sourceWidth, sourceHeight);
                const auto completionType =
                    static_cast<tTVPBBStretchType>(_completionType);
                if(item.meshType == 0) {
                    const auto localPts = buildAffineTrianglePoints(
                        item.corners, offsetX, offsetY);
                    (void)callLayerAffineCopy_guess(
                        baseLayerObject, localPts.data(), sourceObject,
                        sourceRect, completionType, true);
                } else if(item.meshType == 1) {
                    const auto cellDivisions =
                        renderBezierPatchCellDivisions_guess(
                            item.commandPatchDivision,
                            item.sourceState->width,
                            item.sourceState->height);
                    tTJSVariant meshArray =
                        buildMeshPointTJSArrayVariant_guess(
                            item.meshPoints, offsetX, offsetY);
                    (void)callLayerBezierPatchCopy_guess(
                        baseLayerObject, sourceObject, sourceRect, meshArray,
                        cellDivisions[0], cellDivisions[1], completionType,
                        true);
                } else if(item.meshType == 2) {
                    tTJSVariant meshArray =
                        buildMeshPointTJSArrayVariant_guess(
                            item.commandCompositeMeshPoints, offsetX, offsetY);
                    (void)callLayerMeshCopy_guess(
                        baseLayerObject, sourceObject, sourceRect, meshArray,
                        item.meshDivX, item.meshDivY, completionType, true);
                }
            }

            tTJSVariant finalLayerVariant(baseLayerVariant);
            if(!accurateSlaSkipsMaskBuffer_guess(item.blendMode) &&
               item.parentItem) {
                tTJSVariant hiddenValue(static_cast<tjs_int>(0));
                (void)baseLayerObject->PropSet(
                    TJS_MEMBERENSURE, TJS_W("visible"),
                    &detail::visibleMemberHint_guess, &hiddenValue,
                    baseLayerObject);

                finalLayerVariant = sla->resolveLayerOrdinal_guess(
                    static_cast<tjs_uint32>(item.layerId2));
                DispatchReleaseGuard_guess maskedLayerObjectOwner;
                maskedLayerObjectOwner.dispatch =
                    retainObjectFromVariantCopy_guess(finalLayerVariant);
                auto *maskedLayerObject =
                    maskedLayerObjectOwner.dispatch;

                tTJSVariant baseLayerArg(baseLayerVariant);
                tTJSVariant *assignImagesArgs[] = {&baseLayerArg};
                (void)maskedLayerObject->FuncCall(
                    0, TJS_W("assignImages"),
                    &detail::assignImagesMemberHint_guess, nullptr, 1,
                    assignImagesArgs, maskedLayerObject);
                (void)callLayerSetSizeReal_guess(
                    maskedLayerObject, clipWidth, clipHeight);

                for(auto *ancestor = item.parentItem; ancestor;
                    ancestor = ancestor->parentItem) {
                    if(ancestor->rawFlag21 && !ancestor->rawFlag16) {
                        const tTJSVariant &selectedMask =
                            (ancestor->stencilComposite & 4) != 0
                                ? ancestor->composedLayer
                                : ancestor->leafLayer;
                        applyMotionAlphaMask_guess(
                            finalLayerVariant,
                            floatToSignedIntTowardZeroSaturated_guess(
                                ancestor->clipRect[0] -
                                static_cast<float>(clip.left)),
                            floatToSignedIntTowardZeroSaturated_guess(
                                ancestor->clipRect[1] -
                                static_cast<float>(clip.top)),
                            selectedMask, 0, 0,
                            floatToSignedIntTowardZeroSaturated_guess(
                                ancestor->clipRect[2] -
                                ancestor->clipRect[0]),
                            floatToSignedIntTowardZeroSaturated_guess(
                                ancestor->clipRect[3] -
                                ancestor->clipRect[1]),
                            // Bit 2 selects the mask image above; accurate
                            // SLA applies only the low two operation bits.
                            64, _maskMode, ancestor->stencilComposite & 3);
                    } else if((ancestor->stencilComposite & 3) == 1) {
                        // This deliberate argc=4 dispatch is rejected by Layer;
                        // the native caller ignores the result and stops walking.
                        (void)callLayerFillRect4_guess(
                            maskedLayerObject, clipWidth, clipHeight);
                        break;
                    }
                }
            }

            if(payload.hasOutlineOrMeshline &&
               (createdOrChanged || item.parentItem)) {
                DispatchReleaseGuard_guess debugLayerObjectOwner;
                debugLayerObjectOwner.dispatch =
                    retainObjectFromVariantCopy_guess(finalLayerVariant);
                auto *debugLayerObject =
                    debugLayerObjectOwner.dispatch;
                drawRenderItemFrame_guess(
                    debugLayerObject, debugLayerObject, item,
                    _outline, _meshline, offsetX, offsetY);
            }

            {
                DispatchReleaseGuard_guess publishLayerObjectOwner;
                publishLayerObjectOwner.dispatch =
                    retainObjectFromVariantCopy_guess(finalLayerVariant);
                auto *publishLayerObject =
                    publishLayerObjectOwner.dispatch;

                tTJSVariant leftArg(
                    static_cast<tjs_real>(clip.left));
                tTJSVariant topArg(
                    static_cast<tjs_real>(clip.top));
                tTJSVariant *positionArgs[] = {&leftArg, &topArg};
                (void)publishLayerObject->FuncCall(
                    0, TJS_W("setPos"), &detail::setPosMemberHint_guess,
                    nullptr, 2, positionArgs, publishLayerObject);

                tTJSVariant typeValue(
                    static_cast<tjs_int>(layerType));
                (void)publishLayerObject->PropSet(
                    TJS_MEMBERENSURE, TJS_W("type"),
                    &detail::typeMemberHint_guess, &typeValue,
                    publishLayerObject);

                tTJSVariant visibleValue(static_cast<tjs_int>(1));
                (void)publishLayerObject->PropSet(
                    TJS_MEMBERENSURE, TJS_W("visible"),
                    &detail::visibleMemberHint_guess, &visibleValue,
                    publishLayerObject);

                tTJSVariant opacityValue(
                    static_cast<tjs_int>(item.opacity));
                (void)publishLayerObject->PropSet(
                    TJS_MEMBERENSURE, TJS_W("opacity"),
                    &detail::opacityMemberHint_guess, &opacityValue,
                    publishLayerObject);
            }
        }

        sla->endLayerPass_guess();
    }

    void Player::renderToD3DAdaptor(D3DAdaptor *adaptor) {
        detail::PreparedRenderItemList mainList;
        detail::PreparedRenderItemList auxList;
        if(!prepareRenderItems(mainList, auxList)) {
            return;
        }
        applyPreparedRenderItemProjection_guess(mainList);
        adaptor->renderFromPlayer_guess(this, mainList);
    }

    // Unlike the script-facing typed draw route, D3DLayer supplies the
    // compositor's current native texture and transformed origin directly; no
    // TJS Layer or D3DAdaptor is constructed.
    void Player::drawToTexture_guess(iTVPTexture2D *target, float x, float y) {
        // Match the native D3DLayer route's strict Variant::AsObject path. It
        // AddRefs the dispatch and never releases that reference before return;
        // using nativeRM() here would silently repair the per-call leak.
        iTJSDispatch2 *resourceManagerDispatch =
            _findSourceResourceManager.AsObject();
        auto *resourceManager =
            ncbInstanceAdaptor<ResourceManager>::GetNativeInstance(
                resourceManagerDispatch);
        const ttstr motionContext =
            static_cast<ttstr>(_findMotionContextVariant);
        const auto loadedIt =
            resourceManager->_loadedModules.find(motionContext);
        if(loadedIt == resourceManager->_loadedModules.end()) {
            return;
        }
        auto *loadedResource = &loadedIt->second;

        detail::PreparedRenderItemList mainList;
        detail::PreparedRenderItemList auxList;
        if(!prepareRenderItems(mainList, auxList)) {
            return;
        }
        applyPreparedRenderItemProjection_guess(mainList);
        // The native path passes a type-erased source getter that returns the
        // persistent descriptor's current texture without the atlas retry.
        const D3DSourceTextureGetter_guess sourceTextureGetter =
            [loadedResource](detail::PreparedRenderItem &item) {
                (void)loadedResource;
                return item.sourceState->texture;
            };
        // This otherwise-unused selector call is present in both D3DLayer
        // callers before the private OpenGL target bind.
        (void)TVPIsSoftwareRenderManager();
        render_backend_guess::getPrivateOpenGLRenderManager_guess()
            ->SetRenderTarget(target);
        const tTVPRect targetRect(
            0, 0, static_cast<tjs_int>(target->GetWidth()),
            static_cast<tjs_int>(target->GetHeight()));
        const D3DTargetTextureGetter_guess targetTextureGetter =
            [target](bool, const tTVPRect &) {
                return D3DTargetTexturePair_guess(target, target);
            };
        renderPreparedItemsToD3DTexture_guess(
            target, targetTextureGetter, targetRect, sourceTextureGetter,
            mainList, x, y);
    }

    void D3DAdaptor::renderFromPlayer_guess(
        Player *player,
        detail::PreparedRenderItemList &mainList) {
        // The four references gate the whole texture pipeline on the adaptor's
        // canvasCaptureEnabled byte.
        if(!getCanvasCaptureEnabled()) {
            return;
        }
        player->renderPreparedItemsToD3DTexture_guess(this, mainList);
    }

    void Player::renderPreparedItemsToD3DTexture_guess(
        D3DAdaptor *adaptor,
        detail::PreparedRenderItemList &mainList) {
        auto *targetTexture = adaptor->targetTexture();

        // The adaptor owns one static software copy per generic Layer-fallback
        // texture. KRKR atlas borrows bypass that map in both the initial and
        // retry-success branches, even when the process renderer is software.
        const D3DSourceTextureGetter_guess sourceTextureGetter =
            [this, adaptor](detail::PreparedRenderItem &item) {
                return nativeRM()->loadRenderSourceTextureForItem_guess(
                    *this, *adaptor, item);
            };
        render_backend_guess::getPrivateOpenGLRenderManager_guess()
            ->SetRenderTarget(targetTexture);
        const tTVPRect targetRect(
            0, 0, adaptor->getWidth(), adaptor->getHeight());
        const D3DTargetTextureGetter_guess targetTextureGetter =
            [adaptor](bool, const tTVPRect &) {
                auto *currentTarget = adaptor->targetTexture();
                return D3DTargetTexturePair_guess(
                    currentTarget, currentTarget);
            };
        renderPreparedItemsToD3DTexture_guess(
            targetTexture, targetTextureGetter, targetRect,
            sourceTextureGetter, mainList, 0.5f, 0.5f);
    }

    void Player::renderPreparedItemsToD3DTexture_guess(
        iTVPTexture2D *targetTexture,
        const D3DTargetTextureGetter_guess &targetTextureGetter,
        const tTVPRect &targetRect,
        const D3DSourceTextureGetter_guess &sourceTextureGetter,
        detail::PreparedRenderItemList &mainList,
        float xOffset,
        float yOffset) {
        const int stencilRefs = prepareD3DRenderItems_guess(
            mainList, targetRect.right - targetRect.left,
            targetRect.bottom - targetRect.top, _priorDraw);
        const bool stencilEnabled = stencilRefs > 0;
        motion::render_backend_guess::TriangleBatch_guess batch(
            render_backend_guess::getPrivateOpenGLRenderManager_guess());
        motion::render_backend_guess::beginStencil_guess(
            targetTexture, stencilEnabled);

        for(auto *itemPtr : mainList) {
            auto &item = *itemPtr;
            if(shouldSkipD3DRenderItem_guess(item, _priorDraw)) {
                continue;
            }
            if(item.sourceState->blank) {
                continue;
            }

            int opacity = item.opacity;
            if(_priorDraw) {
                opacity /= 2;
            }
            if(opacity <= 0 && item.stencilMaskRef == 0) {
                continue;
            }

            auto *sourceTexture = sourceTextureGetter(item);
            // The descriptor is reread only after its texture callback
            // returns, so render-time atlas writes are visible immediately.
            const auto sourceRect = item.sourceState->textureRect;
            if(sourceRect[2] <= sourceRect[0] ||
               sourceRect[3] <= sourceRect[1]) {
                continue;
            }

            const auto packedColor =
                d3dPackedColorWithOpacity(item, opacity);
            batch.setStencilState_guess(
                item.stencilWriteRef, item.stencilMaskRef);
            // All four native shared renderers pass literal true here. The
            // D3DAdaptor alphaOpAdd property is stored for script readback but
            // is not propagated into this method-selection key.
            auto *method = batch.selectMethod_guess(
                item.blendMode & 0xF,
                packedColor,
                true,
                item.stencilMaskRef != 0);
            if(!method) {
                continue;
            }

            if(item.meshType == 0) {
                appendD3DAffine_guess(
                    batch, method, targetTextureGetter, targetRect, item,
                    sourceTexture, sourceRect, xOffset, yOffset,
                    packedColor);
            } else if(item.meshType == 1) {
                const auto cellDivisions =
                    renderBezierPatchCellDivisions_guess(
                        item.commandPatchDivision,
                        item.sourceState->width,
                        item.sourceState->height);
                const auto boundsPoints = buildOffsetMeshPoints(
                    item.meshPoints, xOffset, yOffset);
                const auto meshPoints = motion::render_backend_guess::
                    tessellateBezierPatch_guess(
                        boundsPoints, cellDivisions[0], cellDivisions[1]);
                appendD3DMesh_guess(
                    batch, method, targetTextureGetter, targetRect,
                    sourceTexture, sourceRect, boundsPoints, meshPoints,
                    cellDivisions[0], cellDivisions[1], packedColor);
            } else if(item.meshType == 2) {
                const auto meshPoints =
                    buildOffsetMeshPoints(item.commandCompositeMeshPoints,
                                          xOffset, yOffset);
                appendD3DMesh_guess(
                    batch, method, targetTextureGetter, targetRect,
                    sourceTexture, sourceRect, meshPoints, meshPoints,
                    item.meshDivX, item.meshDivY, packedColor);
            }
        }

        batch.flush_guess();
        motion::render_backend_guess::endStencil_guess(stencilEnabled);
    }

    void Player::renderToCanvas_guess(
        tTJSVariant target,
        detail::PreparedRenderItemList &mainList,
        detail::PreparedRenderItemList &auxList) {
        // One ncbPropAccessor for the global Layer class is constructed before
        // coercing the target. It owns the dispatch across target-size reads,
        // the submit loop and final setClip(argc=0), and dies after the target
        // object owner.
#if defined(KRKR2_WASMTIME_HEADLESS)
        // Match the complete Canvas envelope, including command construction
        // and normal destruction of the two function-local accessors.
        detail::MotionTraceRenderExecuteScope renderTrace(
            this, target.Type() == tvtObject ? target.AsObjectNoAddRef() : nullptr,
            true, mainList);
#endif
        ncbPropAccessor layerClass{TJS_W("Layer")};
        iTJSDispatch2 *layerClassObject = layerClass.GetDispatch();

        // Convert the supplied Variant directly to one raw object owner. The
        // native canvas path does not probe NativeInstanceSupport first.
        ncbPropAccessor renderTargetOwner{target};
        iTJSDispatch2 *resolvedLayerObject = renderTargetOwner.GetDispatch();

        // Non-priorDraw rendering clears the complex region before collecting
        // this frame's submitted paint boxes. Player.clear has already consumed
        // the previous frame's bound before this draw begins.
        if(!_priorDraw) {
            _drawRegion.Clear();
        }

        // Query through the class dispatch with target as objthis: width
        // strictly precedes height, and there is no positive-size gate here.
        const int canvasWidth = callLayerPropGetInt_guess(
            layerClassObject, resolvedLayerObject, TJS_W("width"),
            &detail::widthMemberHint_guess);
        const int canvasHeight = callLayerPropGetInt_guess(
            layerClassObject, resolvedLayerObject, TJS_W("height"),
            &detail::heightMemberHint_guess);

        iTJSDispatch2 *renderLayerObject = resolvedLayerObject;

        // Build leaf/composed state only when priorDraw is false. Prior-draw
        // submission consumes the retained item state directly.
        if(!_priorDraw) {
            buildRenderCommands(
                mainList, auxList,
                {0.0f, 0.0f,
                 static_cast<float>(canvasWidth),
                 static_cast<float>(canvasHeight)});
        }
        executeLayerRenderCommands(
            layerClassObject, renderLayerObject, canvasWidth, canvasHeight,
            true, mainList);
#if defined(KRKR2_WASMTIME_HEADLESS)
        renderTrace.setResult(true);
#endif
    }

    void Player::renderToSeparateLayerAdaptor(SeparateLayerAdaptor *sla) {
        detail::PreparedRenderItemList mainList;
        detail::PreparedRenderItemList auxList;
        if(!prepareRenderItems(mainList, auxList)) {
            return;
        }
        applyPreparedRenderItemProjection_guess(mainList);

        // The backend decision is process-lifetime state. All references use
        // one function-local guard and never re-read the preference later.
        static const bool accurateSla = isAccurateSlaRenderEnabled();

        if(accurateSla) {
#if defined(KRKR2_WASMTIME_HEADLESS)
            // Test-only Guest instrumentation matching the Android Frida
            // envelope at Player::renderAccurateSeparateLayerAdaptor.  It is
            // absent from every production build and does not alter the
            // four-reference renderer's control flow or object ownership.
            {
                auto *const renderTraceTarget =
                    tryResolveLayerDispatch(sla->getTargetLayer());
                struct AccurateSlaRenderTraceScope {
                    Player *player;
                    iTJSDispatch2 *target;
                    AccurateSlaRenderTraceScope(Player *p, iTJSDispatch2 *t)
                        : player(p), target(t) {
                        detail::motionTraceBeginAccurateSlaRender(player, target);
                    }
                    ~AccurateSlaRenderTraceScope() {
                        detail::motionTraceEndAccurateSlaRender(player, target);
                    }
                } accurateSlaRenderTrace{this, renderTraceTarget};
                detail::MotionTraceRenderExecuteScope renderTrace(
                    this, renderTraceTarget, false, mainList,
                    "Player.renderAccurateSeparateLayerAdaptor");
                renderAccurateSeparateLayerAdaptor_guess(
                    sla, mainList, auxList);
                renderTrace.setResult(true);
            }
#else
            renderAccurateSeparateLayerAdaptor_guess(
                sla, mainList, auxList);
#endif
            updateAccurateSLAAfterDraw(sla->getTargetLayer());
        } else {
            auto *renderLayer = ensurePrivateMotionGLL_guess(*sla);
            const tjs_int canvasWidth =
                static_cast<tjs_int>(renderLayer->GetWidth());
            const tjs_int canvasHeight =
                static_cast<tjs_int>(renderLayer->GetHeight());
            const int stencilCount = buildPrivateMotionGLLCommands_guess(
                renderLayer, canvasWidth, canvasHeight,
                mainList, auxList);
            setPrivateMotionGLLStencilCount_guess(
                renderLayer, stencilCount);
            if(!TVPWindowUpdateEventsDelivering) {
                renderLayer->Update(false);
            }
        }
    }

    void Player::updateLayerAfterDrawRecovered_guess(
        const tTJSVariant &target) {
        // All four references first snapshot the producer flag, even when it
        // is clear. Anchor type 10 reads this on the next frame to gate use of
        // the internal render Layer.
        _internalRenderLayerReady = _needsInternalAssignImages;
        if(!_needsInternalAssignImages) {
            return;
        }

        materializeInternalRenderLayers_guess(target);

        ncbPropAccessor internal{tTJSVariant(_internalRenderLayer)};
        (void)internal.FuncCall(
            0, TJS_W("assignImages"),
            &detail::assignImagesMemberHint_guess, nullptr, target);
    }

    void Player::updateAccurateSLAAfterDraw(const tTJSVariant &target) {
        // The accurate-SLA counterpart likewise snapshots the producer flag
        // unconditionally and leaves the producer untouched.
        _internalRenderLayerReady = _needsInternalAssignImages;
        if(!_needsInternalAssignImages) {
            return;
        }

        ncbPropAccessor targetAccessor{tTJSVariant(target)};
        materializeInternalRenderLayers_guess(target);
        ncbPropAccessor internal{tTJSVariant(_internalRenderLayer)};

        const tjs_int height = getInternalWorkspaceDimension_guess(
            targetAccessor, TJS_W("height"),
            &detail::heightMemberHint_guess);
        const tjs_int width = getInternalWorkspaceDimension_guess(
            targetAccessor, TJS_W("width"),
            &detail::widthMemberHint_guess);
        (void)internal.FuncCall(
            0, TJS_W("piledCopy"), &detail::piledCopyMemberHint_guess,
            nullptr, tTJSVariant(0), tTJSVariant(0), target,
            tTJSVariant(0), tTJSVariant(0), tTJSVariant(width),
            tTJSVariant(height));
    }

} // namespace motion
