// PlayerRenderExecute.cpp — render command build and execution
// Split from PlayerRender.cpp for maintainability.
//
#include "PlayerRenderInternal.h"
#include "MotionDispatch.h"
#include "MotionTraceWeb.h"
#include "PrivateMotionGLL.h"
#include "SourceCache.h"

#include <cmath>
#include <limits>

using namespace motion::internal;
using namespace motion::internal::render_detail;

namespace motion::internal::render_detail {

    tjs_int floatToSignedIntTowardZeroSaturated_guess(float value) {
        if(std::isnan(value)) {
            return 0;
        }
        constexpr float upper = 2147483648.0f;
        constexpr float lower = -2147483648.0f;
        if(value >= upper) {
            return std::numeric_limits<tjs_int>::max();
        }
        if(value <= lower) {
            return std::numeric_limits<tjs_int>::min();
        }
        return static_cast<tjs_int>(std::trunc(value));
    }

}

namespace motion {

    namespace {
        struct DispatchReleaseGuard_guess {
            iTJSDispatch2 *dispatch = nullptr;

            ~DispatchReleaseGuard_guess() {
                if(dispatch) {
                    dispatch->Release();
                }
            }
        };

        std::array<int, 4> integralClipRect(
            const std::array<float, 4> &rect) {
            return {
                floatToSignedIntTowardZeroSaturated_guess(rect[0]),
                floatToSignedIntTowardZeroSaturated_guess(rect[1]),
                floatToSignedIntTowardZeroSaturated_guess(rect[2]),
                floatToSignedIntTowardZeroSaturated_guess(rect[3])
            };
        }

        tjs_int getRenderSourcePropertyInt_guess(
            ncbPropAccessor &accessor,
            const tjs_char *member,
            tjs_uint32 *hint) {
            tTJSVariant value;
            iTJSDispatch2 *dispatch = accessor.GetDispatch();
            (void)dispatch->PropGet(0, member, hint, &value, dispatch);
            return static_cast<tjs_int>(value.AsInteger());
        }

    }

    // The render-command build pass materializes each leaf on the persistent
    // SeparateLayerAdaptor ordered map before the later submit-only pass. Its
    // points are already clip-local, so the copy uses zero extra translation.
    void Player::emitPreparedLeafLayerCopy_guess(
        detail::PreparedRenderItem &item) {
        // The outer clip calculation remains in call-local registers across
        // every resolver/property callback. Re-entry may mutate the persistent
        // item, but setSize and geometry offsets keep this clip snapshot while
        // corners, command vectors and SourceState are read live later.
        const float clipLeft = item.clipRect[0];
        const float clipTop = item.clipRect[1];
        const float clipRight = item.clipRect[2];
        const float clipBottom = item.clipRect[3];
        const tjs_real clipWidth = static_cast<tjs_real>(
            clipRight - clipLeft);
        const tjs_real clipHeight = static_cast<tjs_real>(
            clipBottom - clipTop);

        const tjs_int leafBlendMode = 0;
        const std::array<std::uint32_t, 4> leafColors{
            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
        };

        SeparateLayerPayload_guess payload;
        payload.completionType = _completionType;
        payload.hasOutlineOrMeshline = false;
        payload.commandSrc = item.commandSrc;
        payload.blendMode = leafBlendMode;
        payload.packedColors = leafColors;
        payload.paintAndViewport = {
            item.paintBox[0], item.paintBox[1], item.paintBox[2],
            item.paintBox[3], item.viewport[0], item.viewport[1],
            item.viewport[2], item.viewport[3]
        };
        if(item.meshType == 2) {
            payload.compositeMeshPoints = item.commandCompositeMeshPoints;
        } else if(item.meshType == 1) {
            payload.bezierPatchPoints = item.commandBezierPatchPoints;
        }
        payload.corners = item.corners;
        bool createdOrChanged = false;
        // Native publishes the resolver result first, then independently
        // retains the Object obtained from a copy of that persistent item
        // Variant. Both temporary Variants die before any later TJS callback;
        // the raw Object owner survives every descriptor/source/copy call.
        DispatchReleaseGuard_guess leafLayerObject;
        {
            tTJSVariant leafVariant =
                _renderSeparateLayerAdaptor->resolveLayerNode_guess(
                static_cast<tjs_uint32>(item.renderLayerId), payload,
                createdOrChanged);
            item.leafLayer = leafVariant;
            tTJSVariant leafLayerCopy(item.leafLayer);
            leafLayerObject.dispatch = leafLayerCopy.AsObject();
        }
        // The created/changed byte alone gates source materialization. Native
        // does not add an adaptor, extent, or leaf-object recovery branch.
        if(!createdOrChanged) {
            return;
        }

        // The normal leaf path also publishes a neutral source descriptor:
        // blendMode zero and four opaque-white color weights. The ordinary
        // execute caller uses the item's actual blend/colors instead.
        ncbPropAccessor descriptor{tTJSVariant(_sourceDescriptor)};
        descriptor.SetValue(TJS_W("key"), item.commandKey,
                            TJS_MEMBERENSURE,
                            &detail::commandKeyMemberHint_guess);
        descriptor.SetValue(TJS_W("src"), item.commandSrc,
                            TJS_MEMBERENSURE,
                            &detail::srcMemberHint_guess);
        descriptor.SetValue(TJS_W("blendMode"), leafBlendMode,
                            TJS_MEMBERENSURE,
                            &detail::blendModeMemberHint_guess);

        ncbPropAccessor color{tTJSVariant(_sourceColors)};
        for(tjs_int index = 0; index < 4; ++index) {
            color.SetValue(
                index,
                leafColors[static_cast<std::size_t>(index)],
                TJS_MEMBERENSURE);
        }

        const tTJSVariant sourceObject =
            resolveRenderSource_guess(
                item.sourceState->object);
        ncbPropAccessor sourceAccessor{tTJSVariant(sourceObject)};
        const tjs_int srcW = getRenderSourcePropertyInt_guess(
            sourceAccessor, TJS_W("width"),
            &detail::widthMemberHint_guess);
        const tjs_int srcH = getRenderSourcePropertyInt_guess(
            sourceAccessor, TJS_W("height"),
            &detail::heightMemberHint_guess);

        // The leaf path first writes Integer 0 to neutralColor with
        // TJS_MEMBERENSURE, then dispatches setSize with two Real Variants.
        // All dispatch results are ignored.
        {
            tTJSVariant neutralColor(static_cast<tjs_int>(0));
            (void)leafLayerObject.dispatch->PropSet(
                TJS_MEMBERENSURE, TJS_W("neutralColor"),
                &detail::neutralColorMemberHint_guess, &neutralColor,
                leafLayerObject.dispatch);
        }
        (void)callLayerSetSizeReal_guess(
            leafLayerObject.dispatch, clipWidth, clipHeight);
        const tTVPRect sourceRect(0, 0, srcW, srcH);
        const auto completionType =
            static_cast<tTVPBBStretchType>(_completionType);

        if(item.meshType == 0) {
            // Native promotes each corner to double before adding -0.5 and
            // subtracting the clip origin. It never publishes a float-quantized
            // local-corner snapshot on the persistent item.
            const double clipLeftReal = static_cast<double>(clipLeft);
            const double clipTopReal = static_cast<double>(clipTop);
            const std::array<tTVPPointD, 3> localPts{{
                {static_cast<double>(item.corners[0]) - 0.5 - clipLeftReal,
                 static_cast<double>(item.corners[1]) - 0.5 - clipTopReal},
                {static_cast<double>(item.corners[2]) - 0.5 - clipLeftReal,
                 static_cast<double>(item.corners[3]) - 0.5 - clipTopReal},
                {static_cast<double>(item.corners[6]) - 0.5 - clipLeftReal,
                 static_cast<double>(item.corners[7]) - 0.5 - clipTopReal},
            }};
            (void)callLayerAffineCopy_guess(
                leafLayerObject.dispatch, localPts.data(), sourceObject,
                sourceRect,
                completionType, true);
        } else if(item.meshType == 1 || item.meshType == 2) {
            std::array<tjs_int, 2> cellDivisions{
                item.meshDivX, item.meshDivY
            };
            if(item.meshType == 1) {
                cellDivisions = renderBezierPatchCellDivisions_guess(
                    item.commandPatchDivision,
                    item.sourceState->width,
                    item.sourceState->height);
            }
            const float pointOffsetX = -0.5f - clipLeft;
            const float pointOffsetY = -0.5f - clipTop;
            // The leaf helper consumes the command Bezier vector for type 1
            // and the command composite vector for type 2. It does not use the
            // ordinary mesh vector or a persistent translated sidecar.
            const auto &meshPoints = item.meshType == 1
                ? item.commandBezierPatchPoints
                : item.commandCompositeMeshPoints;
            tTJSVariant meshArray = buildMeshPointTJSArrayVariant_guess(
                meshPoints, pointOffsetX, pointOffsetY);
            if(item.meshType == 1) {
                // Bezier leaf copy uses the shared cell-division pipeline.
                (void)callLayerBezierPatchCopy_guess(
                    leafLayerObject.dispatch, sourceObject, sourceRect,
                    meshArray, cellDivisions[0], cellDivisions[1],
                    completionType, true);
            } else if(item.meshType == 2) {
                // Composite mesh leaf copy preserves the prepared divisions.
                (void)callLayerMeshCopy_guess(
                    leafLayerObject.dispatch, sourceObject, sourceRect,
                    meshArray, cellDivisions[0], cellDivisions[1],
                    completionType, true);
            }
        }

    }

    // Four-reference group/composed pass. For each auxiliary group, union the
    // visible child paint boxes, intersect with the caller's four-edge target
    // clip and then the group's valid viewport. An empty target-clamped union
    // clears rawFlag21. Otherwise lazily create/reuse the composed Layer, size
    // and clear it, apply each live child leaf as an alpha mask, and publish the
    // composed clip with rawFlag21=true/rawFlag16=false.
    void Player::composePreparedGroupLayers_guess(
        detail::PreparedRenderItemList &auxList,
        const std::array<float, 4> &targetClip) {
        if(auxList.empty()) {
            return;
        }
        // The common builder retains the exact four caller-supplied edges. It
        // does not reconstruct an origin-zero rectangle from their extent.
        const float cameraLeft = targetClip[0];
        const float cameraTop = targetClip[1];
        const float cameraRight = targetClip[2];
        const float cameraBottom = targetClip[3];

        // Native trusts both pointer vectors and dereferences every element.
        // A null group or child is not an empty slot; it faults on first use.
        for(auto *grpPtr : auxList) {
            auto &grp = *grpPtr;
            // Seed with the group paint box and accumulate every child's paint
            // box whose rawFlag21 is set.
            float unionLeft = grp.paintBox[0];
            float unionTop = grp.paintBox[1];
            float unionRight = grp.paintBox[2];
            float unionBottom = grp.paintBox[3];
            for(auto *childPtr : grp.childItems) {
                if(!childPtr->rawFlag21) {
                    continue;
                }
                auto &child = *childPtr;
                // The union uses paintBox, not the already-clipped child rect.
                // Native FCSEL chooses the child operand on equal or unordered
                // comparisons; put it first so std::min/max preserves the same
                // NaN payload and signed-zero selection.
                unionLeft = std::min(child.paintBox[0], unionLeft);
                unionTop = std::min(child.paintBox[1], unionTop);
                unionRight = std::max(child.paintBox[2], unionRight);
                unionBottom = std::max(child.paintBox[3], unionBottom);
            }
            // Clamp to the camera rectangle first. These intermediate values,
            // not the later viewport-narrowed values, drive the empty test.
            const float camClampedLeft = cameraLeft < unionLeft
                ? unionLeft
                : cameraLeft;
            const float camClampedTop = cameraTop < unionTop
                ? unionTop
                : cameraTop;
            const float camClampedRight = unionRight < cameraRight
                ? unionRight
                : cameraRight;
            const float camClampedBottom = unionBottom < cameraBottom
                ? unionBottom
                : cameraBottom;

            // A valid group-owned viewport then narrows the camera result with
            // floor(left/top) and ceil(right/bottom). The narrowed values drive
            // the composed size and published clip.
            float finalLeft = camClampedLeft;
            float finalTop = camClampedTop;
            float finalRight = camClampedRight;
            float finalBottom = camClampedBottom;
            if(grp.viewport[2] >= grp.viewport[0] &&
               grp.viewport[3] >= grp.viewport[1]) {
                const float vfLeft = floorf(grp.viewport[0]);
                const float vfTop = floorf(grp.viewport[1]);
                const float vcRight = ceilf(grp.viewport[2]);
                const float vcBottom = ceilf(grp.viewport[3]);
                finalLeft = vfLeft < camClampedLeft ? camClampedLeft : vfLeft;
                finalTop = vfTop < camClampedTop ? camClampedTop : vfTop;
                finalRight = camClampedRight < vcRight ? camClampedRight
                                                       : vcRight;
                finalBottom = camClampedBottom < vcBottom ? camClampedBottom
                                                          : vcBottom;
            }

            // Deliberately test the camera-clamped values, not the viewport-
            // narrowed values.
            if(camClampedLeft > camClampedRight ||
               camClampedTop > camClampedBottom) {
                // Empty union invalidates this group's drawable marker.
                grp.rawFlag21 = false;
                continue;
            }

            const float unionLeftF = finalLeft;
            const float unionTopF = finalTop;
            const float unionRightF = finalRight;
            const float unionBottomF = finalBottom;
            const tjs_real composedWidth = static_cast<tjs_real>(
                unionRightF - unionLeftF);
            const tjs_real composedHeight = static_cast<tjs_real>(
                unionBottomF - unionTopF);

            // A Void composedLayer Variant creates the Layer lazily before it
            // is sized and cleared.  The reference re-evaluates
            // Window.mainWindow at this exact gate, then performs an
            // ignored-status primaryLayer read through the shared member hint;
            // there is no build-entry prefetch or null/native-layer recovery.
            if(grp.composedLayer.Type() == tvtVoid) {
                tTJSVariant owner;
                TVPExecuteExpression(TJS_W("Window.mainWindow"), &owner);
                ncbPropAccessor ownerAccessor{tTJSVariant(owner)};
                grp.composedLayer =
                    detail::createLayerVariant_guess(
                        owner,
                        ownerAccessor.GetValue(
                            TJS_W("primaryLayer"),
                            ncbTypedefs::Tag<tTJSVariant>(), 0,
                            &detail::primaryLayerMemberHint_guess));
            }
            ncbPropAccessor composedLayer{tTJSVariant(grp.composedLayer)};
            iTJSDispatch2 *composedLayerObject = composedLayer.GetDispatch();
            (void)callLayerSetSizeReal_guess(
                composedLayerObject, composedWidth, composedHeight);
            (void)callLayerFillRect5_guess(
                composedLayerObject, composedWidth, composedHeight);

            // Child alpha-mask loop: only rawFlag21 children with a non-Void
            // leafLayer participate. Destination and source then cross the
            // compositor boundary as by-value Variant CopyRefs. Both Player
            // maskMode and group stencilComposite are live per-child reads;
            // callbacks from an earlier child can affect a later child.
            for(auto *childPtr : grp.childItems) {
                auto &child = *childPtr;
                if(!child.rawFlag21 || child.leafLayer.Type() == tvtVoid) {
                    continue;
                }

                // The two owning call arguments are constructed in this exact
                // order. Source CopyRef reentry occurs after the destination
                // offset snapshot but before the size/mode/stencil reads.
                tTJSVariant destinationVariant(grp.composedLayer);
                const float childLeft = child.clipRect[0];
                const float childTop = child.clipRect[1];
                tTJSVariant sourceVariant(child.leafLayer);
                const int childWidth = floatToSignedIntTowardZeroSaturated_guess(
                    child.clipRect[2] - child.clipRect[0]);
                const int childHeight = floatToSignedIntTowardZeroSaturated_guess(
                    child.clipRect[3] - child.clipRect[1]);
                applyMotionAlphaMaskOwnedVariants_guess(
                    destinationVariant,
                    floatToSignedIntTowardZeroSaturated_guess(
                        childLeft - unionLeftF),
                    floatToSignedIntTowardZeroSaturated_guess(
                        childTop - unionTopF),
                    sourceVariant, 0, 0, childWidth, childHeight, 64,
                    _maskMode, grp.stencilComposite);
            }

            // Publish the viewport-narrowed union only after composition.
            grp.rawFlag21 = true;
            grp.rawFlag16 = false;
            grp.clipRect = {
                unionLeftF, unionTopF, unionRightF, unionBottomF
            };
        }
    }

    void Player::buildRenderCommands(
        detail::PreparedRenderItemList &mainList,
        detail::PreparedRenderItemList &auxList,
        const std::array<float, 4> &targetClip) {
#if defined(KRKR2_WASMTIME_HEADLESS)
        const int motionTraceCanvasWidth = static_cast<int>(
            targetClip[2] - targetClip[0]);
        const int motionTraceCanvasHeight = static_cast<int>(
            targetClip[3] - targetClip[1]);
        detail::motionTraceRenderBuildCommandsEnter(
            this, motionTraceCanvasWidth, motionTraceCanvasHeight,
            mainList, auxList);
#endif
        // Swap active/retired trees before either item loop. Keep this an
        // explicit normal-flow pair: native exception unwinding does not clear
        // the retired tree.
        bool renderLayerPassStarted = false;
        if(_renderSeparateLayerAdaptor) {
            _renderSeparateLayerAdaptor
                ->beginLayerPass_guess();
            renderLayerPassStarted = true;
        }
        // The prepared main vector has the same trusted-pointer boundary as
        // the auxiliary/group vectors above.
        for(auto *entryPtr : mainList) {
            auto &entry = *entryPtr;
            // The command builder works in-place on the recursively prepared
            // item list. It does not blanket-clear rawFlag20/rawFlag21 or the
            // stored clip rectangle: drawFlag=false leaves them untouched, and
            // failed intersections only clear rawFlag21. Native fields were
            // restored during item setup; there is no
            // persistent execute-result marker to reset here.
            RenderClipRect clipRect;
            const bool drawableGate = entry.drawFlag && !entry.rawFlag16;
            if(!entry.drawFlag) {
                // The native builder only materializes rawFlag21 and clipRect
                // for drawFlag entries. Ordinary direct items are clipped and
                // submitted by the later execute pass from paintBox/viewport.
                // This branch therefore preserves the restored native fields.
            } else if(!drawableGate ||
                      !computeRenderClipRect(entry, targetClip,
                                             clipRect, nullptr)) {
                entry.rawFlag21 = false;
            } else {
                entry.rawFlag21 = true;
                entry.clipRect = {
                    static_cast<float>(clipRect.left),
                    static_cast<float>(clipRect.top),
                    static_cast<float>(clipRect.right),
                    static_cast<float>(clipRect.bottom)
                };
                entry.dirtyRect = integralClipRect(entry.clipRect);

                // The drawable branch materializes its leaf in this build
                // loop, not in the later submit pass. The persistent adaptor
                // is created lazily from Window.mainWindow.primaryLayer. Its
                // raw pointer is stored only after construction succeeds, then
                // the new adaptor immediately begins the current layer pass.
                if(!_renderSeparateLayerAdaptor) {
                    tTJSVariant owner;
                    TVPExecuteExpression(
                        TJS_W("Window.mainWindow"), &owner);
                    ncbPropAccessor ownerAccessor{tTJSVariant(owner)};
                    // Keep this a direct new-expression. Native allocates the
                    // pending adaptor before evaluating the primaryLayer
                    // constructor argument, publishes only after construction,
                    // and destroys the GetValue temporary before begin-pass.
                    _renderSeparateLayerAdaptor =
                        new SeparateLayerAdaptor(
                            ownerAccessor.GetValue(
                                TJS_W("primaryLayer"),
                                ncbTypedefs::Tag<tTJSVariant>(), 0,
                                &detail::primaryLayerMemberHint_guess));
                    // A freshly published adaptor enters the same swap/reset
                    // sequence as one that already existed at pass entry.
                    _renderSeparateLayerAdaptor
                        ->beginLayerPass_guess();
                    renderLayerPassStarted = true;
                }
                if(!entry.rawFlag20) {
                    // The clip/adaptor prefix above is already published when
                    // this no-argument ResourceManager dispatch begins. Native
                    // neither pre-latches nor rechecks after the callback: an
                    // exception leaves rawFlag20 false and the numeric slot
                    // dormant/stale; normal return stores the converted id and
                    // then latches it. It does not look up or reuse a node id by
                    // name; every drawable item reaching a false latch gets one.
                    entry.renderLayerId = dispatchRequireLayerId();
                    entry.rawFlag20 = true;
                }

                // Emit the per-item leaf copy onto the adaptor's ordered-map
                // leaf layer here in the build pass, not in execute.
                emitPreparedLeafLayerCopy_guess(entry);
            }

        }

        // After the leaf loop, compose qualifying group items into their
        // persistent composed layers.
        composePreparedGroupLayers_guess(auxList, targetClip);
        if(renderLayerPassStarted) {
            // The native normal-only tail invalidates and destroys retired
            // entries that were not moved back into the active tree.
            _renderSeparateLayerAdaptor
                ->endLayerPass_guess();
        }
#if defined(KRKR2_WASMTIME_HEADLESS)
        detail::motionTraceRenderBuildCommandsLeave(
            this, motionTraceCanvasWidth, motionTraceCanvasHeight,
            mainList, auxList);
#endif
    }


    void Player::executeLayerRenderCommands(
        iTJSDispatch2 *layerClassObject,
        iTJSDispatch2 *renderLayerObject,
        tjs_int canvasWidth,
        tjs_int canvasHeight,
        bool skipUpdate,
        detail::PreparedRenderItemList &mainList) {
        using PreparedRenderItem = detail::PreparedRenderItem;

        struct ResolvedSourceObject {
            tTJSVariant object;
            tjs_int width = 0;
            tjs_int height = 0;
        };

        auto applyTargetLayerClip_guess =
            [&](const PreparedRenderItem &item) -> bool {
            // The native phase rejects only ordered right<left or bottom<top.
            // Unordered comparisons therefore remain valid.
            if(!(item.viewport[2] < item.viewport[0]) &&
               !(item.viewport[3] < item.viewport[1])) {
                const float clipLeft =
                    std::max(item.paintBox[0], floorf(item.viewport[0]));
                const float clipTop =
                    std::max(item.paintBox[1], floorf(item.viewport[1]));
                const float clipRight =
                    std::min(item.paintBox[2], ceilf(item.viewport[2]));
                const float clipBottom =
                    std::min(item.paintBox[3], ceilf(item.viewport[3]));
                if(clipLeft > clipRight || clipTop > clipBottom) {
                    return false;
                }
                // Native subtracts in float and promotes each final value to
                // Real only while constructing the four argument Variants.
                const float clipWidth = clipRight - clipLeft;
                const float clipHeight = clipBottom - clipTop;
                // The canvas submitter supplies four Real Variants.
                // Layer.setClip owns the later integer
                // conversion; this caller performs no native GetClip readback.
                (void)callLayerSetClip_guess(
                    layerClassObject, renderLayerObject,
                    static_cast<tjs_real>(clipLeft),
                    static_cast<tjs_real>(clipTop),
                    static_cast<tjs_real>(clipWidth),
                    static_cast<tjs_real>(clipHeight));
                return true;
            }

            (void)callLayerResetClip_guess(
                layerClassObject, renderLayerObject);
            return true;
        };

        // The sorted main vector is a borrowed-pointer sequence with no null
        // slot representation. Native reads the first item flags immediately.
        for(auto *itemPtr : mainList) {
            auto &item = *itemPtr;

            // Check raw item fields before any clip/source work. Only raw zero
            // opacity is a gate; negative and >255 values survive to the
            // submitted Integer Variant.
            const tjs_int rawOpacity = item.opacity;
            if(item.skipFlag0 || item.rawFlag16 || rawOpacity == 0) {
                continue;
            }

            if(!applyTargetLayerClip_guess(item)) {
                continue;
            }
            if(_priorDraw && !item.skipFlag1) {
                continue;
            }

            // C++ signed division preserves the common native `/ 2`
            // rounding-toward-zero boundary.
            const tjs_int opa = _priorDraw ? rawOpacity / 2 : rawOpacity;

            // The submit pass narrows all four paint-box coordinates toward
            // zero and ORs that rectangle into the persistent draw region.
            // This region survives until the next Player.clear call, so pixels
            // occupied only by the previous frame are still erased.
            _drawRegion.Or(tTVPRect(
                floatToSignedIntTowardZeroSaturated_guess(item.paintBox[0]),
                floatToSignedIntTowardZeroSaturated_guess(item.paintBox[1]),
                floatToSignedIntTowardZeroSaturated_guess(item.paintBox[2]),
                floatToSignedIntTowardZeroSaturated_guess(item.paintBox[3])));

            // The native submitter constructs these owners in descriptor ->
            // color -> source Variant -> source accessor order and keeps all
            // four alive across the selected direct/buffered copy and the
            // debug-frame dispatch. Every ordinary item exit, including the
            // direct branch's continue, then releases accessor -> source ->
            // color -> descriptor.
            ncbPropAccessor descriptor{tTJSVariant(_sourceDescriptor)};
            descriptor.SetValue(TJS_W("key"), item.commandKey,
                                TJS_MEMBERENSURE,
                                &detail::commandKeyMemberHint_guess);
            descriptor.SetValue(TJS_W("src"), item.commandSrc,
                                TJS_MEMBERENSURE,
                                &detail::srcMemberHint_guess);
            descriptor.SetValue(TJS_W("blendMode"),
                                static_cast<tjs_int>(item.blendMode),
                                TJS_MEMBERENSURE,
                                &detail::blendModeMemberHint_guess);

            ncbPropAccessor color{tTJSVariant(_sourceColors)};
            for(tjs_int index = 0; index < 4; ++index) {
                color.SetValue(
                    index,
                    item.packedColors[static_cast<std::size_t>(index)],
                    TJS_MEMBERENSURE);
            }

            ResolvedSourceObject source;
            source.object = resolveRenderSource_guess(
                item.sourceState->object);
            // Native constructs a call-local Variant CopyRef for the source
            // accessor, retains only its Object, and destroys that temporary
            // closure before the width read. Do not pre-convert the persistent
            // source Variant with AsObjectNoAddRef: that loses the observable
            // Object/ObjThis AddRef/Release prefix on both success and failure.
            ncbPropAccessor sourceAccessor{tTJSVariant(source.object)};
            source.width = getRenderSourcePropertyInt_guess(
                sourceAccessor, TJS_W("width"),
                &detail::widthMemberHint_guess);
            source.height = getRenderSourcePropertyInt_guess(
                sourceAccessor, TJS_W("height"),
                &detail::heightMemberHint_guess);

            // Exactly one shared source resolve per item. Its Object owner
            // must span both direct and buffered branches.
            const tTVPRect sourceRect(0, 0, source.width, source.height);
            const auto blendMode =
                resolveBlendOperationMode_guess(item.blendMode);
            const bool useDirectRenderPath =
                shouldUseDirectRenderPath_guess(
                    item, _completionType);

            if(useDirectRenderPath) {
                if(item.meshType == 0) {
                    const auto worldPts =
                        buildAffineTrianglePoints(item.corners,
                                                 -0.5f, -0.5f);
                    (void)callLayerOperateAffine_guess(
                        layerClassObject, renderLayerObject,
                        worldPts.data(), source.object, sourceRect,
                        blendMode, opa);
                } else {
                    const auto &renderMeshPoints = item.meshType == 2
                        ? item.commandCompositeMeshPoints
                        : item.meshPoints;
                    std::array<tjs_int, 2> cellDivisions{
                        item.meshDivX, item.meshDivY
                    };
                    if(item.meshType == 1) {
                        cellDivisions =
                            renderBezierPatchCellDivisions_guess(
                                item.commandPatchDivision,
                                item.sourceState->width,
                                item.sourceState->height);
                    } else if(item.meshType != 2) {
                        continue;
                    }
                    // Direct mesh/bezier branches build the point array
                    // with a -0.5,-0.5 world offset and dispatch through
                    // the Layer class accessor with the target render layer
                    // as objthis. Their clear flag is Integer 0.
                    tTJSVariant meshArray =
                        buildMeshPointTJSArrayVariant_guess(
                            renderMeshPoints, -0.5f, -0.5f);
                    if(item.meshType == 1) {
                        (void)callLayerOperateBezierPatch_guess(
                            layerClassObject, renderLayerObject,
                            source.object, sourceRect,
                            meshArray, cellDivisions[0], cellDivisions[1],
                            blendMode, opa, false);
                    } else if(item.meshType == 2) {
                        (void)callLayerOperateMesh_guess(
                            layerClassObject, renderLayerObject,
                            source.object, sourceRect,
                            meshArray, cellDivisions[0], cellDivisions[1],
                            blendMode, opa, false);
                    }
                }
                drawRenderItemFrame_guess(
                    layerClassObject, renderLayerObject, item,
                    _outline, _meshline);
                continue;
            }

            {
                // Preserve the three nested owners exactly: ResourceManager
                // raw dispatch owner -> bufLayer Variant -> buf raw
                // dispatch owner. GetValue is inlined on Android arm64 but
                // remains an out-of-line template helper on the other three
                // references.
                ncbPropAccessor resourceManager{ tTJSVariant(
                    _sourceCacheObject) };
                tTJSVariant bufLayer = resourceManager.GetValue(
                    TJS_W("bufLayer"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                    &detail::bufLayerMemberHint_guess);
                ncbPropAccessor buffer{ tTJSVariant(bufLayer) };
                iTJSDispatch2 *bufferObject = buffer.GetDispatch();

                // The target width/height are TJS property reads performed
                // after acquiring bufLayer; cached native dimensions are
                // not substituted for this observable dispatch sequence.
                const tjs_int targetWidthInteger =
                    callLayerPropGetInt_guess(
                        layerClassObject, renderLayerObject, TJS_W("width"),
                        &detail::widthMemberHint_guess);
                const float targetWidth =
                    static_cast<float>(targetWidthInteger);
                const tjs_int targetHeightInteger =
                    callLayerPropGetInt_guess(
                        layerClassObject, renderLayerObject,
                        TJS_W("height"), &detail::heightMemberHint_guess);
                const float targetHeight =
                    static_cast<float>(targetHeightInteger);

                // The native buffered path uses numeric-max semantics only
                // for left/top and ordered compare/select for right/bottom.
                // It has only a right<left image-phase skip; a vertically
                // inverted or zero extent deliberately reaches
                // Layer.setSize. The debug-frame phase remains outside this
                // buffered owner scope.
                const float bufferLeft = std::fmax(item.paintBox[0], 0.0f);
                const float bufferTop = std::fmax(item.paintBox[1], 0.0f);
                const float bufferRight = item.paintBox[2] < targetWidth
                    ? item.paintBox[2]
                    : targetWidth;
                const float bufferBottom = item.paintBox[3] < targetHeight
                    ? item.paintBox[3]
                    : targetHeight;
                if(!(bufferRight < bufferLeft)) {
                    const tjs_real bufferWidth =
                        static_cast<tjs_real>(bufferRight - bufferLeft);
                    const tjs_real bufferHeight =
                        static_cast<tjs_real>(bufferBottom - bufferTop);
                    (void)callLayerSetSizeReal_guess(
                        bufferObject, bufferWidth, bufferHeight);

                    const float pointOffsetX = -0.5f - bufferLeft;
                    const float pointOffsetY = -0.5f - bufferTop;
                    const auto completionType =
                        static_cast<tTVPBBStretchType>(_completionType);
                    if(item.meshType == 0) {
                        const auto localPoints = buildAffineTrianglePoints(
                            item.corners, pointOffsetX, pointOffsetY);
                        (void)callLayerAffineCopy_guess(
                            bufferObject, localPoints.data(), source.object,
                            sourceRect, completionType, true);
                    } else if(item.meshType == 1 || item.meshType == 2) {
                        const auto &renderMeshPoints = item.meshType == 2
                            ? item.commandCompositeMeshPoints
                            : item.meshPoints;
                        std::array<tjs_int, 2> cellDivisions{
                            item.meshDivX, item.meshDivY
                        };
                        if(item.meshType == 1) {
                            cellDivisions =
                                renderBezierPatchCellDivisions_guess(
                                    item.commandPatchDivision,
                                    item.sourceState->width,
                                    item.sourceState->height);
                        }
                        tTJSVariant meshArray =
                            buildMeshPointTJSArrayVariant_guess(
                                renderMeshPoints, pointOffsetX,
                                pointOffsetY);
                        if(item.meshType == 1) {
                            (void)callLayerBezierPatchCopy_guess(
                                bufferObject, source.object, sourceRect,
                                meshArray, cellDivisions[0],
                                cellDivisions[1], completionType, true);
                        } else {
                            (void)callLayerMeshCopy_guess(
                                bufferObject, source.object, sourceRect,
                                meshArray, cellDivisions[0],
                                cellDivisions[1], completionType, true);
                        }
                    }

                    // Walk the parent pointer chain. Each mask invocation
                    // receives independently owning dst/src Variant copies;
                    // declaration order makes src die before dst.
                    for(auto *ancestor = item.parentItem; ancestor;
                        ancestor = ancestor->parentItem) {
                        if(ancestor->rawFlag21 && !ancestor->rawFlag16) {
                            const tTJSVariant &selectedMask =
                                (ancestor->stencilComposite & 4) != 0
                                ? ancestor->composedLayer
                                : ancestor->leafLayer;
                            applyMotionAlphaMask_guess(
                                bufLayer,
                                floatToSignedIntTowardZeroSaturated_guess(
                                    ancestor->clipRect[0] - bufferLeft),
                                floatToSignedIntTowardZeroSaturated_guess(
                                    ancestor->clipRect[1] - bufferTop),
                                selectedMask, 0, 0,
                                floatToSignedIntTowardZeroSaturated_guess(
                                    ancestor->clipRect[2] -
                                    ancestor->clipRect[0]),
                                floatToSignedIntTowardZeroSaturated_guess(
                                    ancestor->clipRect[3] -
                                    ancestor->clipRect[1]),
                                64, _maskMode, ancestor->stencilComposite);
                        } else if((ancestor->stencilComposite & 3) == 1) {
                            // Layer.fillRect rejects this deliberate argc=4
                            // call. The native submitter ignores the error
                            // and still terminates the ancestor walk before
                            // operateRect.
                            (void)callLayerFillRect4_guess(
                                bufferObject, bufferWidth, bufferHeight);
                            break;
                        }
                    }

                    // Exact argv types: Real L/T, Object CopyRef(bufLayer),
                    // two Integer zeros, Real W/H, Integer mode/opacity.
                    // Return ignored.
                    (void)callLayerOperateRect_guess(
                        layerClassObject, renderLayerObject,
                        static_cast<tjs_real>(bufferLeft),
                        static_cast<tjs_real>(bufferTop), bufLayer,
                        bufferWidth, bufferHeight, blendMode, opa);
                }
            }
            drawRenderItemFrame_guess(layerClassObject, renderLayerObject,
                                      item, _outline, _meshline);
        }

        // Once the top-level item walk completes, reset the target clip through
        // the script-visible no-argument setClip call. This native phase has no
        // Layer.Update call; updating belongs to the post-draw wrapper.
        callLayerResetClip_guess(
            layerClassObject, renderLayerObject);
        (void)skipUpdate;
    }

} // namespace motion
