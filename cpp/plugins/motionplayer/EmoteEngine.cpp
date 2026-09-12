// EmoteEngine implementation. Lifecycle-sensitive behavior is cross-checked
// against all four current reference binaries; current address tables live in
// analysis/motionplayer_lifecycle_four_binary_2026-08-11.md.
//
// Player and the seven direct controllers are single-pointer owners. The ten
// typed deque families retain their recovered element ownership, publication
// order and exception prefixes across the platform-specific STL ABIs.

#include "EmoteEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "EmotePlayer.h"  // Player + EmotePlayer + ResourceManager
#include "MsgIntf.h"
#include "MotionDispatch.h"
#include "ncbind.hpp"
#include "Player.h"
#include "RuntimeSupport.h" // detail::narrow (G2-C bind-loop label conversion)
#include "tjsArray.h"
#include "tjsDictionary.h"

namespace motion {

    namespace internal {

        double controllerSliceTime_guess(double remaining) noexcept {
            return std::min(remaining, 1.1);
        }

    } // namespace internal

    namespace {

        using detail::controllerExponentHint_guess;
        using detail::controllerFrameHint_guess;
        using detail::controllerLengthDoneHint_guess;
        using detail::controllerLengthHint_guess;
        using detail::controllerMouthHint_guess;
        using detail::controllerP0Hint_guess;
        using detail::controllerP1Hint_guess;
        using detail::controllerPhaseHint_guess;
        using detail::controllerPrevHint_guess;
        using detail::controllerRequestQueueHint_guess;
        using detail::controllerSpeedHint_guess;
        using detail::controllerTargetHint_guess;
        using detail::controllerTickHint_guess;
        using detail::controllerVHint_guess;

        // applyMetadata owns a distinct process-wide TJS member-hint slot for
        // every required property. The three optional PropGet calls pass no
        // hint, matching all four reference tables.
        tjs_uint32 metadataMirrorHint_guess = 0;
        tjs_uint32 metadataScaleHint_guess = 0;
        tjs_uint32 metadataBustControlHint_guess = 0;
        tjs_uint32 metadataHairControlHint_guess = 0;
        tjs_uint32 metadataPartsControlHint_guess = 0;
        tjs_uint32 metadataEyeControlHint_guess = 0;
        tjs_uint32 metadataEyebrowControlHint_guess = 0;
        tjs_uint32 metadataMouthControlHint_guess = 0;
        tjs_uint32 metadataTransitionControlHint_guess = 0;
        tjs_uint32 metadataLoopControlHint_guess = 0;
        tjs_uint32 metadataClampControlHint_guess = 0;
        tjs_uint32 metadataMirrorControlHint_guess = 0;
        tjs_uint32 metadataTimelineControlHint_guess = 0;

        // These TJS member hints have shared identities beyond a single helper
        // in all four binaries: enabled is shared by the leaf builders and
        // several other controller families, label is reused by
        // builders/list/state code, talkLabel belongs to the Mouth builder,
        // var_lr/var_ud by Bust/Chain/Clamp, type by clamp/timeline/runtime
        // code, frameList by two builders, and value by timeline/selector
        // state code. Clamp min/max additionally share exported slots with the
        // EmotePlayer variable-range wrapper.
        tjs_uint32 engineEnabledHint_guess = 0;
        tjs_uint32 engineLabelHint_guess = 0;
        tjs_uint32 engineTalkLabelHint_guess = 0;
        tjs_uint32 selectorOptionListHint_guess = 0;
        tjs_uint32 selectorOffValueHint_guess = 0;
        tjs_uint32 selectorOnValueHint_guess = 0;
        tjs_uint32 loopTransitionListHint_guess = 0;
        tjs_uint32 loopVarLoopHint_guess = 0;
        tjs_uint32 engineVarLrHint_guess = 0;
        tjs_uint32 engineVarUdHint_guess = 0;
        tjs_uint32 engineTypeHint_guess = 0;
        tjs_uint32 engineFrameListHint_guess = 0;
        tjs_uint32 engineValueHint_guess = 0;
        tjs_uint32 engineRemoveHint_guess = 0;

        // Bust and Chain builders share these metadata/parameter slots. The
        // nested vec3 helper's x/y slots are also consumed by shape-anchor
        // resolution; z is private to the helper.
        tjs_uint32 engineParamHint_guess = 0;
        tjs_uint32 engineOpHint_guess = 0;
        tjs_uint32 enginePHint_guess = 0;
        tjs_uint32 enginePvHint_guess = 0;
        tjs_uint32 engineOfsHint_guess = 0;
        tjs_uint32 engineBaseLayerHint_guess = 0;
        // Chain-only state fields retain distinct process-wide hint slots.
        tjs_uint32 engineBendRHint_guess = 0;
        tjs_uint32 engineBendSHint_guess = 0;
        tjs_uint32 engineBpHint_guess = 0;
        tjs_uint32 engineVarLrmHint_guess = 0;
        tjs_uint32 enginePointXHint_guess = 0;
        tjs_uint32 enginePointYHint_guess = 0;
        tjs_uint32 enginePointZHint_guess = 0;

        // Top-level state schema. Serialize and unserialize share one mutable
        // member-hint slot per child; "mouth" reuses the controller-state slot.
        tjs_uint32 engineStateTimelineHint_guess = 0;
        tjs_uint32 engineStateEyeHint_guess = 0;
        tjs_uint32 engineStateEyebrowHint_guess = 0;
        tjs_uint32 engineStateTransitionHint_guess = 0;
        tjs_uint32 engineStateSelectorHint_guess = 0;
        tjs_uint32 engineStateBaseHint_guess = 0;
        tjs_uint32 engineStateOuterForceHint_guess = 0;

        // Base/outer-force child dictionaries reuse these exact slots between
        // serialization and restoration.
        tjs_uint32 baseStateCoordHint_guess = 0;
        tjs_uint32 baseStateScaleHint_guess = 0;
        tjs_uint32 baseStateColorHint_guess = 0;
        tjs_uint32 baseStateRotateHint_guess = 0;
        tjs_uint32 outerForceStateBustHint_guess = 0;
        tjs_uint32 outerForceStateHairHint_guess = 0;
        tjs_uint32 outerForceStatePartsHint_guess = 0;

        // The controller-state serializer/restorer member-hint globals are
        // declared in MotionDispatch because `length` is also consumed by the
        // split-out bust-chain spring constructor. `frame` additionally
        // participates in buildVariableList.
        // Metadata timeline classification owns one hint shared by both the
        // MEMBERMUSTEXIST probe and the conditional value read.
        tjs_uint32 timelineDiffHint_guess = 0;

        // Mirror-control owns a distinct process-wide slot for its sole named
        // property. It is not shared with the top-level metadata mirror flag.
        tjs_uint32 mirrorVariableMatchListHint_guess = 0;

        // Timeline-state-only property hints. In all four references this
        // time/content pair is distinct from the broader node-frame
        // time/content family despite using the same member spellings.
        tjs_uint32 timelineLoopBeginHint_guess = 0;
        tjs_uint32 timelineLoopEndHint_guess = 0;
        tjs_uint32 timelineLastTimeHint_guess = 0;
        tjs_uint32 timelineVariableListHint_guess = 0;
        tjs_uint32 timelineTimeHint_guess = 0;
        tjs_uint32 timelineContentHint_guess = 0;
        tjs_uint32 timelineEasingHint_guess = 0;

        // The other playing-timeline dictionary fields have distinct slots.
        tjs_uint32 timelineInfoFlagsHint_guess = 0;
        tjs_uint32 timelineInfoBlendRatioHint_guess = 0;

        // Timeline snapshot fields. Label and flags reuse their broader Engine
        // slots; these three are shared by timeline serialize and restore.
        tjs_uint32 timelineStateCurTimeHint_guess = 0;
        tjs_uint32 timelineStateBlendRatioCtrlHint_guess = 0;
        tjs_uint32 timelineStateStopWhenBlendDoneHint_guess = 0;

        // The native Engine destroys non-trivial fields in declaration-reverse
        // phases around its pointer-sized owning fields. Move a container's
        // allocation into a temporary to make both element destruction and
        // backing-storage release happen at the corresponding native phase.
        // The member remains a valid empty object for its automatic destructor
        // after this body returns.
        template <typename Container>
        void releaseContainerStorageAtNativePhase(Container &container) {
            Container empty;
            container.swap(empty);
        }

        tTJSVariant createTJSDictionary_guess() {
            iTJSDispatch2 *dispatch = TJSCreateDictionaryObject();
            tTJSVariant result(dispatch, dispatch);
            dispatch->Release();
            return result;
        }

        tTJSArrayNI *getTJSArrayNative(const tTJSVariant &value) {
            iTJSDispatch2 *dispatch = value.AsObjectNoAddRef();
            tTJSArrayNI *native = nullptr;
            dispatch->NativeInstanceSupport(
                TJS_NIS_GETINSTANCE, TJSGetArrayClassID(),
                reinterpret_cast<iTJSNativeInstance **>(&native));
            return native;
        }

        tTJSArrayNI *tryGetTJSArrayNative(const tTJSVariant &value) {
            if(value.Type() != tvtObject) {
                return nullptr;
            }
            iTJSDispatch2 *dispatch = value.AsObjectNoAddRef();
            if(!dispatch) {
                return nullptr;
            }
            tTJSArrayNI *native = nullptr;
            if(TJS_FAILED(dispatch->NativeInstanceSupport(
                    TJS_NIS_GETINSTANCE, TJSGetArrayClassID(),
                    reinterpret_cast<iTJSNativeInstance **>(&native)))) {
                return nullptr;
            }
            return native;
        }

        void setTJSProperty(tTJSVariant &dictionary, const tjs_char *name,
                            tTJSVariant value,
                            tjs_uint32 *hint = nullptr) {
            iTJSDispatch2 *dispatch = dictionary.AsObjectNoAddRef();
            dispatch->PropSet(TJS_MEMBERENSURE, name, hint, &value, dispatch);
        }

        bool tryGetTJSProperty(const tTJSVariant &dictionary,
                               const tjs_char *name, tTJSVariant &value,
                               tjs_uint32 *hint = nullptr) {
            if(dictionary.Type() != tvtObject) {
                return false;
            }
            iTJSDispatch2 *dispatch = dictionary.AsObjectNoAddRef();
            if(!dispatch) {
                return false;
            }
            tTJSVariant probe;
            if(TJS_FAILED(dispatch->PropGet(
                    TJS_MEMBERMUSTEXIST, name, hint, &probe, dispatch))) {
                return false;
            }
            tTJSVariant committed(probe);
            value = committed;
            return true;
        }

        // Native controller restorers first materialize an ncbPropAccessor and
        // then probe through its retained dispatch.  In particular, a null
        // Object closure is not converted into a friendly "missing" result.
        bool tryGetTJSProperty(const ncbPropAccessor &dictionary,
                               const tjs_char *name, tTJSVariant &value,
                               tjs_uint32 *hint = nullptr) {
            iTJSDispatch2 *dispatch = dictionary.GetDispatch();
            tTJSVariant probe;
            if(TJS_FAILED(dispatch->PropGet(
                    TJS_MEMBERMUSTEXIST, name, hint, &probe, dispatch))) {
                return false;
            }
            tTJSVariant committed(probe);
            value = committed;
            return true;
        }

        bool tryGetTJSStringProperty(const ncbPropAccessor &dictionary,
                                     const tjs_char *name, ttstr &value,
                                     tjs_uint32 *hint = nullptr) {
            iTJSDispatch2 *dispatch = dictionary.GetDispatch();
            tTJSVariant probe;
            if(TJS_FAILED(dispatch->PropGet(
                    TJS_MEMBERMUSTEXIST, name, hint, &probe, dispatch))) {
                return false;
            }
            ttstr committed(probe);
            value = committed;
            return true;
        }

        bool tryGetTJSScalarProperty(const tTJSVariant &dictionary,
                                     const tjs_char *name,
                                     tTJSVariant &value,
                                     tjs_uint32 *hint = nullptr) {
            if(dictionary.Type() != tvtObject) {
                return false;
            }
            iTJSDispatch2 *dispatch = dictionary.AsObjectNoAddRef();
            return dispatch && TJS_SUCCEEDED(dispatch->PropGet(
                TJS_MEMBERMUSTEXIST, name, hint, &value, dispatch));
        }

        bool tryGetTJSScalarProperty(const ncbPropAccessor &dictionary,
                                     const tjs_char *name,
                                     tTJSVariant &value,
                                     tjs_uint32 *hint = nullptr) {
            iTJSDispatch2 *dispatch = dictionary.GetDispatch();
            return TJS_SUCCEEDED(dispatch->PropGet(
                TJS_MEMBERMUSTEXIST, name, hint, &value, dispatch));
        }

        void restoreIntIfPresent(const tTJSVariant &dictionary,
                                 const tjs_char *name, int32_t &field,
                                 tjs_uint32 *hint = nullptr) {
            tTJSVariant value;
            if(tryGetTJSScalarProperty(dictionary, name, value, hint)) {
                field = static_cast<int32_t>(value.AsInteger());
            }
        }

        void restoreIntIfPresent(const ncbPropAccessor &dictionary,
                                 const tjs_char *name, int32_t &field,
                                 tjs_uint32 *hint = nullptr) {
            tTJSVariant value;
            if(tryGetTJSScalarProperty(dictionary, name, value, hint)) {
                field = static_cast<int32_t>(value.AsInteger());
            }
        }

        void restoreFloatIfPresent(const tTJSVariant &dictionary,
                                   const tjs_char *name, float &field,
                                   tjs_uint32 *hint = nullptr) {
            tTJSVariant value;
            if(tryGetTJSScalarProperty(dictionary, name, value, hint)) {
                field = static_cast<float>(value.AsReal());
            }
        }

        void restoreFloatIfPresent(const ncbPropAccessor &dictionary,
                                   const tjs_char *name, float &field,
                                   tjs_uint32 *hint = nullptr) {
            tTJSVariant value;
            if(tryGetTJSScalarProperty(dictionary, name, value, hint)) {
                field = static_cast<float>(value.AsReal());
            }
        }

        void restoreDoubleIfPresent(const tTJSVariant &dictionary,
                                    const tjs_char *name, double &field) {
            tTJSVariant value;
            if(tryGetTJSScalarProperty(dictionary, name, value)) {
                field = value.AsReal();
            }
        }

        // Shared eye/eyebrow request-queue serializer on all four ABIs.
        tTJSVariant serializeRequestQueue_guess(
            const std::deque<std::pair<float, float>> &queue) {
            detail::TJSArrayWithItems_guess result =
                detail::createTJSArrayWithItems_guess();
            for(const auto &[p0, p1] : queue) {
                // The factory reference is transferred directly to the
                // accessor.  Array publication then constructs the two-pointer
                // Object closure before the accessor releases that reference.
                ncbPropAccessor item(TJSCreateDictionaryObject(), false);
                (void)item.SetValue(
                    TJS_W("p0"), p0, TJS_MEMBERENSURE,
                    &controllerP0Hint_guess);
                (void)item.SetValue(
                    TJS_W("p1"), p1, TJS_MEMBERENSURE,
                    &controllerP1Hint_guess);
                iTJSDispatch2 *const dispatch = item.GetDispatch();
                result.items->emplace_back(dispatch, dispatch);
            }
            return result.value;
        }

        void restoreRequestQueue_guess(
            std::deque<std::pair<float, float>> &queue,
            const ncbPropAccessor &object, tTJSVariant &dictionary) {
            // The native Variant-output probe reuses the by-value input slot.
            // A failed probe leaves that slot unchanged; success replaces it
            // before Array native-instance lookup.
            if(!tryGetTJSProperty(object, TJS_W("rq"), dictionary,
                                  &controllerRequestQueueHint_guess)) {
                return;
            }
            tTJSArrayNI *native = tryGetTJSArrayNative(dictionary);
            if(!native) {
                return;
            }
            queue.clear();
            for(const tTJSVariant &rawItem : native->Items) {
                tTJSVariant item(rawItem);
                item.ToObject();
                ncbPropAccessor itemObject(item);
                item.Clear();
                const float p0 = itemObject.GetValue(
                    TJS_W("p0"), ncbTypedefs::Tag<float>(), 0,
                    &controllerP0Hint_guess);
                const float p1 = itemObject.GetValue(
                    TJS_W("p1"), ncbTypedefs::Tag<float>(), 0,
                    &controllerP1Hint_guess);
                queue.emplace_back(p0, p1);
            }
        }

        // Shared variable-controller state schema on all four ABIs.
        tTJSVariant serializeVarControllerState_guess(
            const EmoteVarController *controller) {
            ncbPropAccessor result(TJSCreateDictionaryObject(), false);
            (void)result.SetValue(
                TJS_W("phase"), controller->state, TJS_MEMBERENSURE,
                &controllerPhaseHint_guess);
            (void)result.SetValue(
                TJS_W("tick"), controller->phase, TJS_MEMBERENSURE,
                &controllerTickHint_guess);
            (void)result.SetValue(
                TJS_W("speed"), controller->invDuration,
                TJS_MEMBERENSURE, &controllerSpeedHint_guess);
            (void)result.SetValue(
                TJS_W("exponent"), controller->powCount,
                TJS_MEMBERENSURE, &controllerExponentHint_guess);

            const auto makeChannels = [controller](const float *values) {
                detail::TJSArrayWithItems_guess array =
                    detail::createTJSArrayWithItems_guess();
                for(int index = 0; index < controller->count; ++index) {
                    array.items->emplace_back(values[index]);
                }
                return array.value;
            };
            (void)result.SetValue(
                TJS_W("frame"), makeChannels(controller->currentValue),
                TJS_MEMBERENSURE, &controllerFrameHint_guess);
            (void)result.SetValue(
                TJS_W("prev"), makeChannels(controller->startValue),
                TJS_MEMBERENSURE, &controllerPrevHint_guess);
            (void)result.SetValue(
                TJS_W("target"), makeChannels(controller->targetValue),
                TJS_MEMBERENSURE, &controllerTargetHint_guess);

            iTJSDispatch2 *const dispatch = result.GetDispatch();
            return tTJSVariant(dispatch, dispatch);
        }

        void restoreVarControllerState_guess(
            EmoteVarController *controller, tTJSVariant dictionary) {
            if(dictionary.Type() != tvtObject) {
                return;
            }

            tTJSVariant objectValue(dictionary);
            objectValue.ToObject();
            ncbPropAccessor object(objectValue);
            objectValue.Clear();

            restoreIntIfPresent(object, TJS_W("phase"), controller->state,
                                &controllerPhaseHint_guess);
            restoreFloatIfPresent(object, TJS_W("tick"), controller->phase,
                                  &controllerTickHint_guess);
            restoreFloatIfPresent(object, TJS_W("speed"),
                                  controller->invDuration,
                                  &controllerSpeedHint_guess);
            restoreFloatIfPresent(object, TJS_W("exponent"),
                                  controller->powCount,
                                  &controllerExponentHint_guess);

            const auto restoreChannels = [controller, &object, &dictionary](
                const tjs_char *name, float *values, tjs_uint32 *hint) {
                if(!tryGetTJSProperty(object, name, dictionary, hint)) {
                    return;
                }

                tTJSVariant arrayValue(dictionary);
                arrayValue.ToObject();
                ncbPropAccessor array(arrayValue);
                arrayValue.Clear();
                for(int index = 0; index < controller->count; ++index) {
                    values[index] = array.GetValue(
                        index, ncbTypedefs::Tag<float>(), 0);
                }
            };
            restoreChannels(TJS_W("frame"), controller->currentValue,
                            &controllerFrameHint_guess);
            restoreChannels(TJS_W("prev"), controller->startValue,
                            &controllerPrevHint_guess);
            restoreChannels(TJS_W("target"), controller->targetValue,
                            &controllerTargetHint_guess);
        }

        // Angle-controller state uses the same scalar property names as the
        // variable controller but stores one value rather than channel arrays.
        tTJSVariant serializeAngleControllerState_guess(
            const EmoteAngleController *controller) {
            ncbPropAccessor result(TJSCreateDictionaryObject(), false);
            (void)result.SetValue(
                TJS_W("phase"), controller->state, TJS_MEMBERENSURE,
                &controllerPhaseHint_guess);
            (void)result.SetValue(
                TJS_W("tick"), controller->phase, TJS_MEMBERENSURE,
                &controllerTickHint_guess);
            (void)result.SetValue(
                TJS_W("speed"), controller->invDuration,
                TJS_MEMBERENSURE, &controllerSpeedHint_guess);
            (void)result.SetValue(
                TJS_W("exponent"), controller->powCount,
                TJS_MEMBERENSURE, &controllerExponentHint_guess);
            (void)result.SetValue(
                TJS_W("frame"), controller->currentRad, TJS_MEMBERENSURE,
                &controllerFrameHint_guess);
            (void)result.SetValue(
                TJS_W("prev"), controller->startRad, TJS_MEMBERENSURE,
                &controllerPrevHint_guess);
            (void)result.SetValue(
                TJS_W("target"), controller->targetRad, TJS_MEMBERENSURE,
                &controllerTargetHint_guess);

            iTJSDispatch2 *const dispatch = result.GetDispatch();
            return tTJSVariant(dispatch, dispatch);
        }

        // All four binaries restore both "prev" and "target" into startRad;
        // targetRad itself is not restored. Preserve that shipped quirk.
        void restoreAngleControllerState_guess(
            EmoteAngleController *controller,
            const tTJSVariant &dictionary) {
            // Copy the incoming closure (including ObjThis), force Object
            // conversion, retain the Object dispatch in the accessor, then
            // release the temporary before the first property probe.
            tTJSVariant objectValue(dictionary);
            objectValue.ToObject();
            ncbPropAccessor object(objectValue);
            objectValue.Clear();

            restoreIntIfPresent(object, TJS_W("phase"), controller->state,
                                &controllerPhaseHint_guess);
            restoreFloatIfPresent(object, TJS_W("tick"), controller->phase,
                                  &controllerTickHint_guess);
            restoreFloatIfPresent(object, TJS_W("speed"),
                                  controller->invDuration,
                                  &controllerSpeedHint_guess);
            restoreFloatIfPresent(object, TJS_W("exponent"),
                                  controller->powCount,
                                  &controllerExponentHint_guess);
            restoreFloatIfPresent(object, TJS_W("frame"),
                                  controller->currentRad,
                                  &controllerFrameHint_guess);
            restoreFloatIfPresent(object, TJS_W("prev"),
                                  controller->startRad,
                                  &controllerPrevHint_guess);
            restoreFloatIfPresent(object, TJS_W("target"),
                                  controller->startRad,
                                  &controllerTargetHint_guess);
        }

        void serializeEyeControllerState(
            ncbPropAccessor &result, const ttstr &label,
            const EmoteBlinkController *controller) {
            (void)result.SetValue(
                TJS_W("label"), label, TJS_MEMBERENSURE,
                &engineLabelHint_guess);
            (void)result.SetValue(
                TJS_W("phase"), controller->trackState, TJS_MEMBERENSURE,
                &controllerPhaseHint_guess);
            (void)result.SetValue(
                TJS_W("frame"), controller->trackValue, TJS_MEMBERENSURE,
                &controllerFrameHint_guess);
            (void)result.SetValue(
                TJS_W("v"), controller->trackDir, TJS_MEMBERENSURE,
                &controllerVHint_guess);
            (void)result.SetValue(
                TJS_W("target"), controller->trackTarget, TJS_MEMBERENSURE,
                &controllerTargetHint_guess);
            (void)result.SetValue(
                TJS_W("length"), controller->trackSpan, TJS_MEMBERENSURE,
                &controllerLengthHint_guess);
            (void)result.SetValue(
                TJS_W("lengthDone"), controller->trackAccum,
                TJS_MEMBERENSURE, &controllerLengthDoneHint_guess);
            (void)result.SetValue(
                TJS_W("exponent"), controller->trackPow, TJS_MEMBERENSURE,
                &controllerExponentHint_guess);
            (void)result.SetValue(
                TJS_W("speed"), controller->trackInvDur, TJS_MEMBERENSURE,
                &controllerSpeedHint_guess);
            (void)result.SetValue(
                TJS_W("rq"),
                serializeRequestQueue_guess(controller->valueTrack8B),
                TJS_MEMBERENSURE, &controllerRequestQueueHint_guess);
        }

        void restoreEyeControllerState_guess(
            EmoteBlinkController *controller,
            tTJSVariant dictionary) {
            tTJSVariant objectValue(dictionary);
            objectValue.ToObject();
            ncbPropAccessor object(objectValue);
            objectValue.Clear();

            restoreIntIfPresent(object, TJS_W("phase"),
                                controller->trackState,
                                &controllerPhaseHint_guess);
            restoreFloatIfPresent(object, TJS_W("frame"),
                                  controller->trackValue,
                                  &controllerFrameHint_guess);
            restoreFloatIfPresent(object, TJS_W("v"), controller->trackDir,
                                  &controllerVHint_guess);
            restoreFloatIfPresent(object, TJS_W("target"),
                                  controller->trackTarget,
                                  &controllerTargetHint_guess);
            restoreFloatIfPresent(object, TJS_W("length"),
                                  controller->trackSpan,
                                  &controllerLengthHint_guess);
            restoreFloatIfPresent(object, TJS_W("lengthDone"),
                                  controller->trackAccum,
                                  &controllerLengthDoneHint_guess);
            restoreFloatIfPresent(object, TJS_W("exponent"),
                                  controller->trackPow,
                                  &controllerExponentHint_guess);
            restoreFloatIfPresent(object, TJS_W("speed"),
                                  controller->trackInvDur,
                                  &controllerSpeedHint_guess);
            restoreRequestQueue_guess(
                controller->valueTrack8B, object, dictionary);
        }

        void serializeEyebrowControllerState(
            ncbPropAccessor &result, const ttstr &label,
            const EmoteEyebrowController *controller) {
            (void)result.SetValue(
                TJS_W("label"), label, TJS_MEMBERENSURE,
                &engineLabelHint_guess);
            (void)result.SetValue(
                TJS_W("phase"), controller->trackState, TJS_MEMBERENSURE,
                &controllerPhaseHint_guess);
            (void)result.SetValue(
                TJS_W("frame"), controller->trackValue, TJS_MEMBERENSURE,
                &controllerFrameHint_guess);
            (void)result.SetValue(
                TJS_W("v"), controller->trackDir, TJS_MEMBERENSURE,
                &controllerVHint_guess);
            (void)result.SetValue(
                TJS_W("target"), controller->trackTarget, TJS_MEMBERENSURE,
                &controllerTargetHint_guess);
            (void)result.SetValue(
                TJS_W("length"), controller->trackSpan, TJS_MEMBERENSURE,
                &controllerLengthHint_guess);
            (void)result.SetValue(
                TJS_W("lengthDone"), controller->trackAccum,
                TJS_MEMBERENSURE, &controllerLengthDoneHint_guess);
            (void)result.SetValue(
                TJS_W("exponent"), controller->trackPow,
                TJS_MEMBERENSURE, &controllerExponentHint_guess);
            (void)result.SetValue(
                TJS_W("speed"), controller->trackInvDur,
                TJS_MEMBERENSURE, &controllerSpeedHint_guess);
            (void)result.SetValue(
                TJS_W("rq"),
                serializeRequestQueue_guess(controller->valueTrack8B),
                TJS_MEMBERENSURE, &controllerRequestQueueHint_guess);
        }

        void restoreEyebrowControllerState_guess(
            EmoteEyebrowController *controller,
            tTJSVariant dictionary) {
            tTJSVariant objectValue(dictionary);
            objectValue.ToObject();
            ncbPropAccessor object(objectValue);
            objectValue.Clear();

            restoreIntIfPresent(object, TJS_W("phase"),
                                controller->trackState,
                                &controllerPhaseHint_guess);
            restoreFloatIfPresent(object, TJS_W("frame"),
                                  controller->trackValue,
                                  &controllerFrameHint_guess);
            restoreFloatIfPresent(object, TJS_W("v"), controller->trackDir,
                                  &controllerVHint_guess);
            restoreFloatIfPresent(object, TJS_W("target"),
                                  controller->trackTarget,
                                  &controllerTargetHint_guess);
            restoreFloatIfPresent(object, TJS_W("length"),
                                  controller->trackSpan,
                                  &controllerLengthHint_guess);
            restoreFloatIfPresent(object, TJS_W("lengthDone"),
                                  controller->trackAccum,
                                  &controllerLengthDoneHint_guess);
            restoreFloatIfPresent(object, TJS_W("exponent"),
                                  controller->trackPow,
                                  &controllerExponentHint_guess);
            restoreFloatIfPresent(object, TJS_W("speed"),
                                  controller->trackInvDur,
                                  &controllerSpeedHint_guess);
            restoreRequestQueue_guess(
                controller->valueTrack8B, object, dictionary);
        }

        void serializeMouthControllerState(
            ncbPropAccessor &result, const ttstr &label,
            const EmoteMouthController *controller) {
            (void)result.SetValue(
                TJS_W("label"), label, TJS_MEMBERENSURE,
                &engineLabelHint_guess);
            (void)result.SetValue(
                TJS_W("phase"), controller->state, TJS_MEMBERENSURE,
                &controllerPhaseHint_guess);
            (void)result.SetValue(
                TJS_W("mouth"), controller->beginFrame, TJS_MEMBERENSURE,
                &controllerMouthHint_guess);
            (void)result.SetValue(
                TJS_W("frame"), controller->currentValue, TJS_MEMBERENSURE,
                &controllerFrameHint_guess);
            (void)result.SetValue(
                TJS_W("prev"), controller->startVal, TJS_MEMBERENSURE,
                &controllerPrevHint_guess);
            (void)result.SetValue(
                TJS_W("target"), controller->endVal, TJS_MEMBERENSURE,
                &controllerTargetHint_guess);
            (void)result.SetValue(
                TJS_W("tick"), controller->accum, TJS_MEMBERENSURE,
                &controllerTickHint_guess);
            (void)result.SetValue(
                TJS_W("exponent"), controller->powField, TJS_MEMBERENSURE,
                &controllerExponentHint_guess);
            (void)result.SetValue(
                TJS_W("speed"), controller->invDur, TJS_MEMBERENSURE,
                &controllerSpeedHint_guess);
        }

        void restoreMouthControllerState_guess(
            EmoteMouthController *controller,
            const tTJSVariant &dictionary) {
            // The collection wrapper lends its deque item directly here;
            // this local copy is the first Variant copy on that call path.
            tTJSVariant objectValue(dictionary);
            objectValue.ToObject();
            ncbPropAccessor object(objectValue);
            objectValue.Clear();

            restoreIntIfPresent(object, TJS_W("phase"), controller->state,
                                &controllerPhaseHint_guess);
            restoreIntIfPresent(object, TJS_W("mouth"),
                                controller->beginFrame,
                                &controllerMouthHint_guess);
            restoreFloatIfPresent(object, TJS_W("frame"),
                                  controller->currentValue,
                                  &controllerFrameHint_guess);
            restoreFloatIfPresent(object, TJS_W("prev"),
                                  controller->startVal,
                                  &controllerPrevHint_guess);
            restoreFloatIfPresent(object, TJS_W("target"),
                                  controller->endVal,
                                  &controllerTargetHint_guess);
            restoreFloatIfPresent(object, TJS_W("tick"), controller->accum,
                                  &controllerTickHint_guess);
            restoreFloatIfPresent(object, TJS_W("exponent"),
                                  controller->powField,
                                  &controllerExponentHint_guess);
            restoreFloatIfPresent(object, TJS_W("speed"),
                                  controller->invDur,
                                  &controllerSpeedHint_guess);
        }

        void serializeSelectorControllerState(
            ncbPropAccessor &result, const ttstr &label,
            const EmoteSelectorController *controller) {
            (void)result.SetValue(
                TJS_W("label"), label, TJS_MEMBERENSURE,
                &engineLabelHint_guess);
            (void)result.SetValue(
                TJS_W("value"), controller->selectedIndex,
                TJS_MEMBERENSURE, &engineValueHint_guess);
            (void)result.SetValue(
                TJS_W("phase"), controller->selState, TJS_MEMBERENSURE,
                &controllerPhaseHint_guess);
            (void)result.SetValue(
                TJS_W("speed"), controller->invDuration,
                TJS_MEMBERENSURE, &controllerSpeedHint_guess);
            (void)result.SetValue(
                TJS_W("tick"), controller->accum, TJS_MEMBERENSURE,
                &controllerTickHint_guess);
        }

        void restoreSelectorControllerState_guess(
            EmoteSelectorController *controller,
            const tTJSVariant &dictionary) {
            // Selector shares Mouth's borrowed-input boundary. Eye, Eyebrow
            // and Var instead receive an additional by-value caller copy.
            tTJSVariant objectValue(dictionary);
            objectValue.ToObject();
            ncbPropAccessor object(objectValue);
            objectValue.Clear();

            restoreIntIfPresent(object, TJS_W("value"),
                                controller->selectedIndex,
                                &engineValueHint_guess);
            restoreIntIfPresent(object, TJS_W("phase"), controller->selState,
                                &controllerPhaseHint_guess);
            restoreFloatIfPresent(object, TJS_W("speed"),
                                  controller->invDuration,
                                  &controllerSpeedHint_guess);
            restoreFloatIfPresent(object, TJS_W("tick"), controller->accum,
                                  &controllerTickHint_guess);
        }

    } // namespace

    // Four-reference construction follows declaration order. The ten typed
    // deques and four ttstr vectors start empty. Android old-libstdc++ allocates
    // an eager 11-bucket table for each unordered container after a request of
    // ten;
    // iOS libc++ leaves all seven unordered containers bucketless/lazy. This is
    // an STL ABI boundary, not a vector reserve and not a portable source-level
    // request that should be reproduced manually here.
    //
    // The three Variant fields start Void. Player and the seven direct
    // controllers are then constructed as one-pointer owners. The raw wind
    // owner and five cached parameters start null/zero; selectorEnabled is the
    // sole true trigger byte at that point, while directEdit, queuing, dirty and
    // debugPrint are false. Five consecutive doubles start at 1.0.
    //
    // Finally the constructor seeds position, scale, angle and color in that
    // exact order through the same zero-duration controller setters used later.
    // Each call is preceded by dirty=true. Those setters deliberately do not
    // initialize phase/invDuration/power tails while idle; open-coding a broader
    // reset would change the references' indeterminate-byte boundary.
    EmoteEngine::EmoteEngine(const tTJSVariant &rmDispatch)
        : _player(new Player(rmDispatch)),
          _ctlPosition(new EmoteVarController(2)),
          _ctlScale(new EmoteVarController(1)),
          _ctlColor(new EmoteVarController(4)),
          _ctlAngle(new EmoteAngleController()),
          _ctlBustOuterForce(new EmoteVarController(2)),
          _ctlHairOuterForce(new EmoteVarController(2)),
          _ctlPartsOuterForce(new EmoteVarController(2)) {
        // These new-expressions belong to their member initializers, not the
        // constructor body. Consequently the mirror caches and timeline-state
        // map precede them, while later variable containers follow them in
        // declaration-order construction/unwind. A controller constructor
        // failure frees its pending allocation before the already completed
        // owner prefix is destroyed in reverse order.
        // Player is the first such direct owner. Its new-expression keeps the
        // raw allocation outside the Engine slot until Player construction
        // succeeds: a Player-constructor exception invokes only operator delete
        // on that raw allocation (Player's own partial-member unwind has already
        // run), leaves the Engine owner unpublished, and never starts the first
        // controller. Failures after publication instead destroy/delete Player
        // through the completed unique owner during Engine-prefix rollback.
        //
        // The scalar/Variant/hash tail is declared after those owners. The
        // current iOS libc++ references form all four empty hash containers by
        // non-throwing zero stores, so a seed-setter exception sees the complete
        // tail and unwinds values -> refs -> ranges -> instant labels -> the
        // three Variants before the controller owners. Android old-libstdc++
        // instead eagerly allocates an eleven-bucket table while constructing
        // each of those four containers. On its arm64 EH path, failure in the
        // current container skips that still-partial member and starts at the
        // preceding completed container; the armv7 constructor has no local
        // cleanup landing. These are target STL/EH boundaries, so the portable
        // source keeps ordinary declaration-order default construction rather
        // than open-coding a reserve or a manual rollback.
        const float positionSeed[2] = {0.0f, 0.0f};
        _dirty = true;
        EmoteVarController_setTarget_guess(
            _ctlPosition.get(), positionSeed, 0.0f, 0.0f,
            _queuing);

        const float scaleSeed = 1.0f;
        _dirty = true;
        EmoteVarController_setTarget_guess(
            _ctlScale.get(), &scaleSeed, 0.0f, 0.0f,
            _queuing);

        _dirty = true;
        EmoteAngleController_setTarget_guess(
            _ctlAngle.get(), 0.0f, 0.0f, 0.0f,
            _queuing);

        const float colorSeed[4] = {128.0f, 128.0f, 128.0f, 255.0f};
        _dirty = true;
        EmoteVarController_setTarget_guess(
            _ctlColor.get(), colorSeed, 0.0f, 0.0f,
            _queuing);
    }

    // Four-reference normal destruction order. Native offset/address tables and
    // ABI-specific helper bodies live in analysis/; this body preserves their
    // common source-level phase ordering without baking one ABI into the port.
    EmoteEngine::~EmoteEngine() {
        // Wind precedes every container, direct controller, Player and spring.
        // It is the one raw direct owner: all four normal destructors issue only
        // the scalar delete and leave the dying Engine slot unchanged. Chain
        // springs merely borrow it and do not dereference that borrow later.
        delete _windEmitter;

        // Later-declared Engine members die before the pointer-sized controller
        // owners on every ABI: variable values, controller refs, ranges,
        // instant-variable labels, then the three Variants.
        releaseContainerStorageAtNativePhase(_variableValues);
        releaseContainerStorageAtNativePhase(_variableControllerRefs);
        releaseContainerStorageAtNativePhase(_variableRanges);
        releaseContainerStorageAtNativePhase(_instantVariableLabels);
        _variableFrameLists.Clear();
        _variableLabels.Clear();
        _variableLabelsBase.Clear();

        // Seven direct controller owners unwind in exact reverse construction
        // order, followed by the owned Player. Concrete slot stores are an
        // ABI/optimizer detail: Android destroys/deletes before the observed
        // null store (arm64 pipelines that store past the next owner load), while
        // iOS stores null before destruction. Keep the common unique_ptr reset
        // expression instead of encoding either target's instruction schedule.
        _ctlPartsOuterForce.reset();
        _ctlHairOuterForce.reset();
        _ctlBustOuterForce.reset();
        _ctlAngle.reset();
        _ctlColor.reset();
        _ctlScale.reset();
        _ctlPosition.reset();

        // Delete the Player after all seven direct controllers.
        _player.reset();

        // Earlier non-trivial members follow Player in reverse declaration
        // order. Release their allocations now instead of postponing them until
        // the automatic member-destruction epilogue.
        releaseContainerStorageAtNativePhase(_activeTimelineLabels);
        releaseContainerStorageAtNativePhase(_timelineDiffLabels);
        releaseContainerStorageAtNativePhase(_timelineLabels);
        releaseContainerStorageAtNativePhase(_timelineStates);
        releaseContainerStorageAtNativePhase(_mirrorMissCache);
        releaseContainerStorageAtNativePhase(_mirrorMatchCache);
        releaseContainerStorageAtNativePhase(_mirrorVariablePatterns);

        // Ten controller/spring deques are the earliest fields, so they unwind
        // last as #10 -> #1. Selector options borrow transition controllers;
        // destroying #9 before owning #8 retains the native borrow lifetime.
        releaseContainerStorageAtNativePhase(_lookupCurvesDeque10);

        releaseContainerStorageAtNativePhase(_vectorVarDeque9);

        // Entry destruction releases label, then its single-pointer controller
        // owner. Selector #9 has already been destroyed, so no borrow survives.
        releaseContainerStorageAtNativePhase(_auxVarDeque8);
        releaseContainerStorageAtNativePhase(_clampControlDeque7);

        // Entry reverse-member destruction releases talkLabel, then label,
        // then its single-pointer controller owner.
        releaseContainerStorageAtNativePhase(_compositeVarDeque6);

        // As for deque #4, entry reverse-member destruction releases label
        // before the controller owner.
        releaseContainerStorageAtNativePhase(_stateMachineDeque5);

        // Entry reverse-member destruction releases label before its
        // single-pointer controller owner, matching all four range destructors.
        releaseContainerStorageAtNativePhase(_stateMachineDeque4);

        releaseContainerStorageAtNativePhase(_bustChain2Nodes);

        releaseContainerStorageAtNativePhase(_bustChain1Nodes);

        // Reverse member destruction releases keyY, keyX and shapeLabel before
        // the single-pointer spring owner.
        releaseContainerStorageAtNativePhase(_hairPartsNodes);
    }

    // Four-reference metadata reset and variable-container tail.
    void EmoteEngine::resetMetadataState() {
        // Clear releases every controller-ref key/node but retains the
        // implementation's bucket allocation. The variable-value map is
        // intentionally not cleared: scalar values survive metadata replacement
        // and are overwritten lazily by later
        // controller steps or setVariable fallthroughs in all four references.
        _variableControllerRefs.clear();

        _hairPartsNodes.clear();
        _bustChain1Nodes.clear();
        _bustChain2Nodes.clear();

        _stateMachineDeque4.clear();
        _stateMachineDeque5.clear();
        _compositeVarDeque6.clear();
        _clampControlDeque7.clear();
        // Metadata reset follows declaration order, unlike normal destruction:
        // #8 owners die before #9. The short-lived borrowed pointers are not
        // dereferenced while the selector entries are subsequently destroyed.
        _auxVarDeque8.clear();
        _vectorVarDeque9.clear();
        _lookupCurvesDeque10.clear();

        // The four references clear the mirror-pattern vector first, releasing
        // each element while retaining capacity, then clear the match and miss
        // caches.
        // std::vector<ttstr>::clear() reproduces that lifetime directly.
        _mirrorVariablePatterns.clear();
        _mirrorMatchCache.clear();
        _mirrorMissCache.clear();
        _timelineStates.clear();

        {
            detail::TJSArrayWithItems_guess baseLabels =
                detail::createTJSArrayWithItems_guess();
            _variableLabelsBase = baseLabels.value;
        }
        _variableLabels = _variableLabelsBase;
        _variableFrameLists = createTJSDictionary_guess();
        // Native unordered_set::clear releases each key/node and zeros the
        // bucket predecessor table, first-node and size fields. It deliberately
        // retains bucket storage/count, max-load factor and rehash policy.
        _instantVariableLabels.clear();
        _variableRanges.clear();
    }

    void EmoteEngine::buildVariableList_guess(
        const tTJSVariant &variableList) {
        detail::TJSArrayWithItems_guess workingArray =
            detail::createTJSArrayWithItems_guess();
        _variableLabels = workingArray.value;
        std::deque<tTJSVariant> *const labels = workingArray.items;

        _variableFrameLists = createTJSDictionary_guess();

        tTJSVariant variableListValue(variableList);
        variableListValue.ToObject();
        ncbPropAccessor variableListObject(variableListValue);
        variableListValue.Clear();

        tTJSVariant frameDictionaryValue(_variableFrameLists);
        frameDictionaryValue.ToObject();
        ncbPropAccessor frameDictionary(frameDictionaryValue);
        frameDictionaryValue.Clear();

        const int count = variableListObject.GetArrayCount();
        for(int variableIndex = 0; variableIndex < count; ++variableIndex) {
            tTJSVariant itemValue = variableListObject.GetValue(
                variableIndex, ncbTypedefs::Tag<tTJSVariant>(), 0);
            itemValue.ToObject();
            ncbPropAccessor item(itemValue);
            itemValue.Clear();
            const ttstr label = item.GetValue(
                TJS_W("label"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineLabelHint_guess);

            auto [rangeIt, inserted] =
                _variableRanges.try_emplace(label, label);
            (void)inserted;
            detail::EmoteVariableRange &range = rangeIt->second;

            std::deque<tTJSVariant> *frameArray;
            {
                detail::TJSArrayWithItems_guess created =
                    detail::createTJSArrayWithItems_guess();
                workingArray.value = created.value;
                frameArray = created.items;
            }
            tjs_uint32 *const labelHint =
                const_cast<ttstr &>(label).GetHint();
            const bool hasFrameArray =
                frameDictionary.HasValue(label.c_str(), labelHint);
            if(hasFrameArray) {
                workingArray.value = frameDictionary.GetValue(
                    label.c_str(), ncbTypedefs::Tag<tTJSVariant>(), 0,
                    labelHint);
                frameArray = &getTJSArrayNative(workingArray.value)->Items;
            } else {
                labels->emplace_back(label);
                (void)frameDictionary.SetValue(
                    label.c_str(), workingArray.value,
                    TJS_MEMBERENSURE, labelHint);
            }

            tTJSVariant frameListValue = item.GetValue(
                TJS_W("frameList"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &engineFrameListHint_guess);
            frameListValue.ToObject();
            ncbPropAccessor frameList(frameListValue);
            frameListValue.Clear();
            const int frameCount = frameList.GetArrayCount();
            for(int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                const tTJSVariant frame = frameList.GetValue(
                    frameIndex, ncbTypedefs::Tag<tTJSVariant>(), 0);
                tTJSVariant frameObjectValue(frame);
                frameObjectValue.ToObject();
                ncbPropAccessor frameObject(frameObjectValue);
                frameObjectValue.Clear();
                const double frameValue = frameObject.GetValue(
                    TJS_W("frame"), ncbTypedefs::Tag<tjs_real>(), 0,
                    &controllerFrameHint_guess);

                // All four references select the newly decoded value on an
                // equal or unordered comparison. This is observably different
                // from std::min/std::max for signed zero and NaN.
                range.frameMin = range.frameMin < frameValue
                    ? range.frameMin
                    : frameValue;
                range.frameMax = frameValue < range.frameMax
                    ? range.frameMax
                    : frameValue;
                frameArray->push_back(frame);
            }
        }
    }

    void EmoteEngine::removeVariableLabel_guess(const ttstr &label) {
        tTJSVariant labelsValue(_variableLabels);
        labelsValue.ToObject();
        ncbPropAccessor labels(labelsValue);
        labelsValue.Clear();

        tTJSVariant argument(label);
        tTJSVariant *arguments[] = { &argument };
        iTJSDispatch2 *dispatch = labels.GetDispatch();
        (void)dispatch->FuncCall(
            0, TJS_W("remove"), &engineRemoveHint_guess, nullptr, 1,
            arguments, dispatch);
    }

    // Four-reference common pseudocode:
    //   base=new Array; publicKeys=base; base.Items=currentLabels.Items; dirty=1;
    //   for (entry : selectorDeque) { entry.flag=selectorEnabled;
    //     if (enabled) { entry.ctl.queue.clear(); selState=0; applySelection(0); }
    //     else std::remove(base.Items.begin(), base.Items.end(), entry.label);
    //     for (target : entry.targets) if (enabled) removeVariableLabel_guess(target.label);
    //       else setTarget(target.ctl, zero, duration=0, power=0, append=false);
    //   }
    void EmoteEngine::syncSelectorControls_guess() {
        // The factory-return owner deliberately spans the complete selector
        // pass. Native code publishes the fresh closure before copying Items;
        // the member therefore already names the new Array while that deque
        // assignment is in progress.
        detail::TJSArrayWithItems_guess baseLabels =
            detail::createTJSArrayWithItems_guess();
        _variableLabelsBase = baseLabels.value;

        tTJSArrayNI *currentLabels = getTJSArrayNative(_variableLabels);
        *baseLabels.items = currentLabels->Items;

        _dirty = true;
        for (EmoteSelectorControlEntry_Deque9& entry : _vectorVarDeque9) {
            entry.flag = _selectorEnabled;
            if (_selectorEnabled) {
                entry.ctl->commandTrack12B.queue.clear();
                entry.ctl->selState = 0;
                EmoteSelectorController_applySelection(
                    entry.ctl.get(), 0, 0.0f, 0.0f);
            } else {
                const tTJSVariant label(entry.label);
                // The returned new-end iterator is deliberately ignored: the
                // four binaries compact by Variant copy-assignment but do not
                // erase, destroy, or shrink the resulting tail.
                (void)std::remove(baseLabels.items->begin(),
                                  baseLabels.items->end(), label);
            }

            for (EmoteTransitionControlEntry_Deque8 *target : entry.targets) {
                if (_selectorEnabled) {
                    removeVariableLabel_guess(target->label);
                } else {
                    const float zero = 0.0f;
                    EmoteVarController_setTarget_guess(
                        target->ctl.get(), &zero, 0.0f, 0.0f, false);
                }
            }
        }
    }

    bool EmoteEngine::isSelectorTarget(ttstr label) {
        // targets stays empty in a normally constructed native object. Preserve
        // the loop because the gate writes are part of the shipped query body.
        for (EmoteSelectorControlEntry_Deque9 &entry : _vectorVarDeque9) {
            entry.flag = _selectorEnabled;
            for (EmoteTransitionControlEntry_Deque8 *target : entry.targets) {
                if (target->label == label) {
                    return true;
                }
            }
        }
        return false;
    }

    void EmoteEngine::activateSelectorTarget(ttstr label) {
        // The native plugin has no targets writer, so this is normally inert.
        for (EmoteSelectorControlEntry_Deque9 &entry : _vectorVarDeque9) {
            for (std::size_t index = 0; index < entry.targets.size(); ++index) {
                if (entry.targets[index]->label != label) {
                    continue;
                }

                entry.ctl->commandTrack12B.queue.clear();
                entry.ctl->selState = 0;
                EmoteSelectorController_applySelection(
                    entry.ctl.get(), static_cast<int>(index), 0.0f, 0.0f);
                entry.flag = 0;

                for (EmoteSelectorControlEntry_Deque9 &selector :
                     _vectorVarDeque9) {
                    float value;
                    EmoteSelectorController_step(
                        selector.ctl.get(), &value, 0.0f);
                    _variableValues[selector.label] = value;
                }
                for (EmoteTransitionControlEntry_Deque8 &transition :
                     _auxVarDeque8) {
                    float value;
                    EmoteVarController_step(
                        transition.ctl.get(), &value, 0.0f);
                    _variableValues[transition.label] = value;
                }
                return;
            }
        }
    }

    void EmoteEngine::deactivateSelectorTarget(ttstr label) {
        // The native plugin has no targets writer, so this is normally inert.
        for (EmoteSelectorControlEntry_Deque9 &entry : _vectorVarDeque9) {
            for (std::size_t index = 0; index < entry.targets.size(); ++index) {
                if (entry.targets[index]->label != label) {
                    continue;
                }

                entry.ctl->commandTrack12B.queue.clear();
                entry.ctl->selState = 0;
                EmoteSelectorController_applySelection(
                    entry.ctl.get(), static_cast<int>(index), 0.0f, 0.0f);
                entry.flag = 1;

                for (EmoteSelectorControlEntry_Deque9 &selector :
                     _vectorVarDeque9) {
                    float value;
                    EmoteSelectorController_step(
                        selector.ctl.get(), &value, 0.0f);
                    _variableValues[selector.label] = value;
                }
                for (EmoteTransitionControlEntry_Deque8 &transition :
                     _auxVarDeque8) {
                    float value;
                    EmoteVarController_step(
                        transition.ctl.get(), &value, 0.0f);
                    _variableValues[transition.label] = value;
                }
                return;
            }
        }
    }

    // The order is observable because selector applySelection may enqueue into
    // transition controllers that are reset immediately afterward.
    void EmoteEngine::resetControllers_guess() {
        std::size_t activeIndex = 0;
        while(activeIndex < _activeTimelineLabels.size()) {
            detail::EmoteTimelineState &state =
                _timelineStates[_activeTimelineLabels[activeIndex]];
            if(state.loopBegin >= 0.0) {
                // The active-timeline phase owns the null gate; the native
                // reset helper is not called for a missing blend owner.
                if(state.blendController) {
                    EmoteVarController_reset_guess(
                        state.blendController.get());
                }
                ++activeIndex;
            } else {
                applyTimelineWindow_guess(
                    state, true, state.lastTime);
                _activeTimelineLabels.erase(
                    _activeTimelineLabels.begin() +
                    static_cast<std::ptrdiff_t>(activeIndex));
            }
        }

        EmoteVarController_reset_guess(_ctlBustOuterForce.get());
        EmoteVarController_reset_guess(_ctlHairOuterForce.get());
        EmoteVarController_reset_guess(_ctlPartsOuterForce.get());

        for(EmoteHairPartsNode48B &node : _hairPartsNodes) {
            node.spring->firstFlag = 1;
            node.initFlag = 1;
        }
        for(EmoteBustChain1Node56B &node : _bustChain1Nodes) {
            node.spring->firstFlag = 1;
            node.initFlag = 1;
        }
        for(EmoteBustChain2Node56B &node : _bustChain2Nodes) {
            node.spring->firstFlag = 1;
            node.initFlag = 1;
        }

        for(EmoteEyeControlEntry_Deque4 &entry : _stateMachineDeque4) {
            EmoteBlinkController_reset_guess(entry.ctl.get());
        }
        for(EmoteEyebrowControlEntry_Deque5 &entry : _stateMachineDeque5) {
            EmoteEyebrowController_reset_guess(entry.ctl.get());
        }
        for(EmoteMouthControlEntry_Deque6 &entry : _compositeVarDeque6) {
            EmoteMouthController *ctl = entry.ctl.get();
            if(!ctl->valueTrack12B.empty()) {
                ctl->state = 0;
                ctl->currentValue = ctl->valueTrack12B.back().endRad;
                ctl->valueTrack12B.clear();
            } else if(ctl->state != 0) {
                ctl->state = 0;
                ctl->currentValue = ctl->endVal;
            }
        }
        for(EmoteSelectorControlEntry_Deque9 &entry : _vectorVarDeque9) {
            EmoteSelectorController_reset_guess(entry.ctl.get());
        }
        for(EmoteTransitionControlEntry_Deque8 &entry : _auxVarDeque8) {
            EmoteVarController_reset_guess(entry.ctl.get());
        }

        EmoteVarController_reset_guess(_ctlPosition.get());
        EmoteVarController_reset_guess(_ctlScale.get());
        if(!_ctlAngle->queue.empty()) {
            _ctlAngle->state = 0;
            _ctlAngle->currentRad = _ctlAngle->queue.back().endRad;
            _ctlAngle->queue.clear();
        } else if(_ctlAngle->state != 0) {
            float value = _ctlAngle->targetRad;
            _ctlAngle->state = 0;
            while(value < 0.0f) {
                value += 6.2832f;
            }
            while(value >= 6.2832f) {
                value -= 6.2832f;
            }
            _ctlAngle->currentRad = value;
        }
        EmoteVarController_reset_guess(_ctlColor.get());
    }

    void EmoteEngine::setMirror_guess(bool mirror) {
        _mirrorRequested = mirror;
        _mirrorChanged = (_mirrorRequested != _mirrorBase);
        _player->setFlipX(_mirrorChanged);
        resetControllers_guess();
    }

    void EmoteEngine::setOuterForceTarget_guess(
        const ttstr &label, double x, double y, double duration,
        double power) {
        const float values[2] = {
            static_cast<float>(x), static_cast<float>(y)
        };
        EmoteVarController *controller = nullptr;
        if(label == TJS_W("bust")) {
            controller = _ctlBustOuterForce.get();
        } else if(label == TJS_W("hair")) {
            controller = _ctlHairOuterForce.get();
        } else if(label == TJS_W("parts")) {
            controller = _ctlPartsOuterForce.get();
        } else {
            return;
        }

        EmoteVarController_setTarget_guess(
            controller, values, static_cast<float>(duration),
            static_cast<float>(power), _queuing);
    }

    bool EmoteEngine::getAnimating_guess() const {
        const auto varControllerActive = [](const EmoteVarController *ctl) {
            return ctl->state != 0 || !ctl->queue.empty();
        };

        if(varControllerActive(_ctlPosition.get()) ||
           varControllerActive(_ctlScale.get()) ||
           _ctlAngle->state != 0 || !_ctlAngle->queue.empty()) {
            return true;
        }

        std::unordered_set<ttstr, detail::ttstr_hash, detail::ttstr_equal>
            timelineDrivenLabels;
        for(const ttstr &label : _activeTimelineLabels) {
            const auto found = _timelineStates.find(label);
            if(found == _timelineStates.end()) {
                continue;
            }

            const detail::EmoteTimelineState &state = found->second;
            // A mapped active label without parsed timeline data is skipped as
            // a whole. In particular, its blend owner and loop marker do not
            // contribute to animating until the timeline data exists.
            if(!state.timelineData) {
                continue;
            }
            for(const detail::EmoteTimelineTrack &track :
                state.timelineData->variableList) {
                timelineDrivenLabels.insert(track.label);
            }

            // Once timeline data exists, the reference directly dereferences
            // the blend controller. Preserve that narrower lifetime
            // precondition instead of turning an invalid state into idle.
            if(varControllerActive(state.blendController.get()) ||
               state.loopBegin < 0.0) {
                return true;
            }
        }

        const auto timelineDrives = [&timelineDrivenLabels](const ttstr &label) {
            return timelineDrivenLabels.find(label) !=
                   timelineDrivenLabels.end();
        };

        for(const EmoteSelectorControlEntry_Deque9 &entry :
            _vectorVarDeque9) {
            if((entry.ctl->selState != 0 ||
                !entry.ctl->commandTrack12B.queue.empty()) &&
               !timelineDrives(entry.label)) {
                return true;
            }
        }
        for(const EmoteTransitionControlEntry_Deque8 &entry : _auxVarDeque8) {
            if(varControllerActive(entry.ctl.get()) &&
               !timelineDrives(entry.label)) {
                return true;
            }
        }
        for(const EmoteEyeControlEntry_Deque4 &entry : _stateMachineDeque4) {
            if((entry.ctl->trackState != 0 ||
                !entry.ctl->valueTrack12B.empty()) &&
               !timelineDrives(entry.label)) {
                return true;
            }
        }
        for(const EmoteEyebrowControlEntry_Deque5 &entry :
            _stateMachineDeque5) {
            if((entry.ctl->trackState != 0 ||
                !entry.ctl->valueTrack12B.empty()) &&
               !timelineDrives(entry.label)) {
                return true;
            }
        }
        for(const EmoteMouthControlEntry_Deque6 &entry :
            _compositeVarDeque6) {
            if((entry.ctl->state != 0 ||
                !entry.ctl->valueTrack12B.empty()) &&
               !timelineDrives(entry.label) &&
               !timelineDrives(entry.talkLabel)) {
                return true;
            }
        }
        return false;
    }

    void EmoteEngine::applyMetadata_guess(tTJSVariant metadata) {
        // All four current references clear the metadata-owned state before
        // making the core's second Variant copy. The typed NCB adapter already
        // owns its by-value argument, so reset cannot invalidate an aliased
        // source dispatch before this copy is made.
        resetMetadataState();

        // Native lifetime: copy the by-value input again, ToObject, retain its
        // dispatch in an accessor/closure, then clear the second Variant before
        // the first property read. The accessor releases the dispatch on every
        // normal and exceptional exit.
        tTJSVariant metadataValue(metadata);
        metadataValue.ToObject();
        ncbPropAccessor metadataAccessor(metadataValue);
        metadataValue.Clear();
        iTJSDispatch2 *metadataDispatch = metadataAccessor.GetDispatch();

        const auto propGet = [metadataDispatch](const tjs_char *name,
                                                tjs_uint32 *hint) {
            tTJSVariant value;
            (void)metadataDispatch->PropGet(
                0, name, hint, &value, metadataDispatch);
            return value;
        };
        const auto tryPropGet = [metadataDispatch](
            const tjs_char *name, tTJSVariant &value) {
            return TJS_SUCCEEDED(metadataDispatch->PropGet(
                TJS_MEMBERMUSTEXIST, name, nullptr, &value,
                metadataDispatch));
        };

        _mirrorBase = propGet(
            TJS_W("mirror"), &metadataMirrorHint_guess).operator bool();
        _mirrorChanged = (_mirrorRequested != _mirrorBase);
        _player->setFlipX(_mirrorChanged);
        resetControllers_guess();
        _player->progressFrames_guess(nullptr, 0.0);

        _metadataScale = propGet(
            TJS_W("scale"), &metadataScaleHint_guess).AsReal();
        float controllerScale = 0.0f;
        EmoteVarController_step(_ctlScale.get(), &controllerScale, 0.0f);
        _inverseCombinedScale =
            1.0 / (_metadataScale * controllerScale);

        // The native body reuses one Variant slot for all three optional
        // members. A successful later PropGet releases/replaces the earlier
        // value; a missing member does not invoke its builder.
        tTJSVariant optionalValue;
        if(tryPropGet(TJS_W("variableList"), optionalValue)) {
            buildVariableList_guess(optionalValue);
        }

        // Each required control Variant dies immediately after its builder;
        // the four binaries do not retain all of these dispatches until the
        // end of applyMetadata.
        buildBustControl_guess(propGet(
            TJS_W("bustControl"), &metadataBustControlHint_guess));
        buildChainControl_guess(_bustChain1Nodes, propGet(
            TJS_W("hairControl"), &metadataHairControlHint_guess), 1);
        buildChainControl_guess(_bustChain2Nodes, propGet(
            TJS_W("partsControl"), &metadataPartsControlHint_guess), 2);
        buildEyeControl_guess(propGet(
            TJS_W("eyeControl"), &metadataEyeControlHint_guess));
        buildEyebrowControl_guess(propGet(
            TJS_W("eyebrowControl"), &metadataEyebrowControlHint_guess));
        buildMouthControl_guess(propGet(
            TJS_W("mouthControl"), &metadataMouthControlHint_guess));
        buildTransitionControl_guess(propGet(
            TJS_W("transitionControl"),
            &metadataTransitionControlHint_guess));

        if(tryPropGet(TJS_W("selectorControl"), optionalValue)) {
            buildSelectorControl_guess(optionalValue);
        }

        buildLoopControl_guess(propGet(
            TJS_W("loopControl"), &metadataLoopControlHint_guess));
        buildClampControl_guess(propGet(
            TJS_W("clampControl"), &metadataClampControlHint_guess));
        buildMirrorControl_guess(propGet(
            TJS_W("mirrorControl"), &metadataMirrorControlHint_guess));

        if(tryPropGet(TJS_W("instantVariableList"), optionalValue)) {
            buildInstantVariableList_guess(optionalValue);
        }

        buildTimelineControl_guess(propGet(
            TJS_W("timelineControl"), &metadataTimelineControlHint_guess));

        syncSelectorControls_guess();
    }

    // Four-reference root-controller helper. Exact ABI addresses, field offsets
    // and sink call chains are recorded in analysis/; all targets share the
    // source-level order position -> color -> scale -> angle. The seven direct
    // controllers are constructor invariants, so these four pointers are
    // intentionally dereferenced without null guards just as in native code.
    void EmoteEngine::stepRootControllers_guess(float dt) {
        // One four-float stack range is reused by every controller. Each native
        // step fills its controller's complete channel count before the sink.
        float out[4];

        EmoteVarController_step(_ctlPosition.get(), out, dt);
        _player->setCoord(out[0], out[1]);

        EmoteVarController_step(_ctlColor.get(), out, dt);
        const std::uint32_t argb =
              (std::uint32_t)(std::uint8_t)(int)out[0]
            | ((std::uint32_t)(std::uint8_t)(int)out[1] << 8)
            | ((std::uint32_t)(std::uint8_t)(int)out[2] << 16)
            | ((std::uint32_t)(std::uint8_t)(int)out[3] << 24);
        // The Player sink swaps packed R/B exactly once before storing it.
        _player->setColorWeight((tjs_int)argb);

        EmoteVarController_step(_ctlScale.get(), out, dt);
        // No zero/finite guard: IEEE division produces inf/NaN as native does.
        _inverseCombinedScale = 1.0 / (_metadataScale * out[0]);
        // This is the root zoom/scale pair, not the independent slant pair.
        _player->setZoom(out[0], out[0]);

        EmoteAngleController_step(_ctlAngle.get(), out, dt);
        // Angle-controller storage/output is radians; the Player sink converts
        // it to degrees before direct-edit normalization or root-delta compare.
        _player->setAngleRad(out[0]);
    }

    // ------------------------------------------------------------------------
    // Physics-pass helper shared by the hair/parts and bust passes.
    // ------------------------------------------------------------------------
    // All four references pass the deque entry's embedded label as const ttstr&,
    // resolve a LayerGetter, fetch its `shape` object, accept only point
    // geometry, and leave both outputs untouched on failure.
    bool EmoteEngine::resolveShapeAnchor_guess(const ttstr &label,
                                                float *outX, float *outY) {
        Player *const player = _player.get();
        tTJSVariant resolved = player->getLayerGetter(label);
        if(resolved.Type() != tvtObject) {
            return false;
        }
        iTJSDispatch2 *obj = resolved.AsObjectNoAddRef();
        if(!obj) {
            return false;
        }

        tTJSVariant shapeVar;
        if(TJS_FAILED(obj->PropGet(
               0, TJS_W("shape"), nullptr, &shapeVar, obj)) ||
           shapeVar.Type() != tvtObject) {
            return false;
        }
        iTJSDispatch2 *shape = shapeVar.AsObjectNoAddRef();
        if(!shape) {
            return false;
        }

        tTJSVariant typeVar;
        tjs_int type = 0;
        if(shape->PropGet(
               0, TJS_W("type"), nullptr, &typeVar, shape) == TJS_S_OK) {
            type = static_cast<tjs_int>(typeVar.AsInteger());
        }
        if(type != 0) {
            return false;
        }

        double x = 0.0;
        double y = 0.0;
        tTJSVariant xVar;
        tTJSVariant yVar;
        if(shape->PropGet(
               0, TJS_W("x"), nullptr, &xVar, shape) == TJS_S_OK) {
            x = xVar.AsReal();
        }
        if(shape->PropGet(
               0, TJS_W("y"), nullptr, &yVar, shape) == TJS_S_OK) {
            y = yVar.AsReal();
        }

        const double rootX = player->getX();
        const double rootY = player->getY();
        const double r = _inverseCombinedScale;

        // The output slots retain the native X/Y crossover verbatim.
        *outX = static_cast<float>(rootY + (y - rootY) * r);
        *outY = static_cast<float>(rootX + (x - rootX) * r);
        return true;
    }

    // Common four-reference simple-spring wrapper:
    //   copy the two-value bust outer-force controller output;
    //   for (node in deque#1) {
    //       resolve the node's shape anchor;
    //       if (node->initFlag) {
    //           clear it and step once for the full interval;
    //       } else if (dt - 0.0001 > 0) {
    //           interpolate from the previous anchor in <=1.1-unit substeps;
    //       }
    //       persist the resolved anchor and upsert both output angles in the
    //       variable-value map;
    //   }
    void EmoteEngine::stepHairParts(float dt) {
        Player* const player = _player.get();
        EmoteVarController* const ctl = _ctlBustOuterForce.get();

        float currentForce[2];
        const int count = ctl->count;
        if (count >= 1) {
            std::memcpy(currentForce, ctl->currentValue,
                        sizeof(float) * static_cast<std::size_t>(count));
        }

        const float stepThreshold = dt - 0.0001f;

        for (EmoteHairPartsNode48B& node : _hairPartsNodes) {
            float anchorX = node.anchorX;
            float anchorY = node.anchorY;
            resolveShapeAnchor_guess(node.shapeLabel, &anchorX, &anchorY);

            // Deliberately uninitialized. Every reference stores these slots
            // even when a non-first node skips stepping at dt <= 0.0001.
            float outX;
            float outY;
            if (node.initFlag) {
                node.initFlag = 0;
                const float angleRad =
                    static_cast<float>(player->getAngleRad());
                EmotePhysics_springStep_guess(
                    node.spring.get(), &outX, &outY,
                    anchorX, anchorY, currentForce[0], currentForce[1], dt,
                    static_cast<float>(_bustScale), angleRad);
            } else if (stepThreshold > 0.0f) {
                // sub-stepped integration toward the resolved anchor.
                const float prevX = node.anchorX;
                const float prevY = node.anchorY;
                float elapsed = 0.0f;
                do {
                    const float substep = std::fmin(dt - elapsed, 1.1f);
                    elapsed = elapsed + substep;
                    const float currentWeight = elapsed / dt;
                    const float previousWeight = 1.0f - currentWeight;
                    const float interpolatedAnchorX =
                        (previousWeight * prevX) +
                        (currentWeight * anchorX);
                    const float interpolatedAnchorY =
                        (previousWeight * prevY) +
                        (currentWeight * anchorY);
                    const float angleRad =
                        static_cast<float>(player->getAngleRad());
                    EmotePhysics_springStep_guess(
                        node.spring.get(), &outX, &outY,
                        interpolatedAnchorX, interpolatedAnchorY,
                        currentForce[0], currentForce[1], substep,
                        static_cast<float>(_bustScale), angleRad);
                } while (stepThreshold > elapsed);
            }

            node.anchorX = anchorX;
            node.anchorY = anchorY;

            _variableValues[node.keyX] = outX;
            _variableValues[node.keyY] = outY;
        }
    }

    // Chain-control wrapper shared by the bust/hair/parts paths.  The direct
    // count-sized copy and uninitialized output locals are intentional shipped
    // boundaries: all four references retain them, including the skipped-step
    // path when dt <= 0.0001f.  ARM32 and both iOS references call the separate
    // post-bend helper; Android ARM64 inlines that helper twice.
    void EmoteEngine::stepBust(EmoteVarController* ctlTarget,
                               std::deque<EmoteBustChain1Node56B>& chainNodes,
                               double scale, float dt) {
        Player* const player = _player.get();

        float currentForce[2];
        const int count = ctlTarget->count;
        if (count >= 1) {
            std::memcpy(currentForce, ctlTarget->currentValue,
                        sizeof(float) * static_cast<size_t>(count));
        }

        const float outputScale = static_cast<float>(scale);
        const float stepThreshold = dt - 0.0001f;

        for (EmoteBustChain1Node56B& node : chainNodes) {
            float anchorX = node.anchorX;
            float anchorY = node.anchorY;
            resolveShapeAnchor_guess(node.shapeLabel, &anchorX, &anchorY);

            node.spring->collisionCurve = _windEmitter;

            float outSegment0;
            float outSegment1;
            float outSelectedY;

            if (node.initFlag) {
                node.initFlag = 0;
                const float angleRad =
                    static_cast<float>(player->getAngleRad());
                EmoteBustChainSpring_step_guess(
                    node.spring.get(), anchorX, anchorY,
                    &outSegment0, &outSegment1, &outSelectedY,
                    currentForce[0], currentForce[1],
                    dt, outputScale, angleRad);
                EmoteBustChainSpring_postBend_guess(
                    node.spring.get(), outSelectedY,
                    &outSegment0, &outSegment1, dt);
            } else if (stepThreshold > 0.0f) {
                const float prevX = node.anchorX;
                const float prevY = node.anchorY;
                float elapsed = 0.0f;
                do {
                    const float substep = std::fmin(dt - elapsed, 1.1f);
                    elapsed = elapsed + substep;
                    const float currentWeight = elapsed / dt;
                    const float previousWeight = 1.0f - currentWeight;
                    const float interpolatedAnchorX =
                        (previousWeight * prevX) +
                        (currentWeight * anchorX);
                    const float interpolatedAnchorY =
                        (previousWeight * prevY) +
                        (currentWeight * anchorY);
                    const float angleRad =
                        static_cast<float>(player->getAngleRad());
                    EmoteBustChainSpring_step_guess(
                        node.spring.get(),
                        interpolatedAnchorX, interpolatedAnchorY,
                        &outSegment0, &outSegment1, &outSelectedY,
                        currentForce[0], currentForce[1],
                        substep, outputScale, angleRad);
                    EmoteBustChainSpring_postBend_guess(
                        node.spring.get(), outSelectedY,
                        &outSegment0, &outSegment1, substep);
                } while (stepThreshold > elapsed);
            }

            node.anchorX = anchorX;
            node.anchorY = anchorY;

            _variableValues[node.keyA] = outSegment1;
            _variableValues[node.keyB] = outSegment0;
            _variableValues[node.keyC] = outSelectedY;
        }
    }

    // Enabled metadata entries append an owning controller with an initially
    // empty label, assign the retained label afterward, then publish/overwrite
    // the label's type-4 reference. The numeric reference is the original
    // metadata index, so disabled entries leave holes and duplicate labels let
    // the later enabled entry replace the earlier map value.
    void EmoteEngine::buildEyeControl_guess(
        const tTJSVariant& eyeControl) {
        ncbPropAccessor controlObject{tTJSVariant(eyeControl)};
        const int count = static_cast<int>(controlObject.GetArrayCount());
        for (int metadataIndex = 0;
             metadataIndex < count; ++metadataIndex) {
            const tTJSVariant element =
                controlObject.GetValue(
                    metadataIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor elementObject{tTJSVariant(element)};
            if (!elementObject.GetValue(
                    TJS_W("enabled"), ncbTypedefs::Tag<bool>(), 0,
                    &engineEnabledHint_guess)) {
                continue;
            }

            EmoteBlinkController *controller =
                new EmoteBlinkController(element);

            // Ownership starts only after the destination deque element has
            // been constructed. A deque growth failure after the controller
            // constructor therefore leaks the raw pointer as in the references.
            _stateMachineDeque4.emplace_back(controller);
            _stateMachineDeque4.back().label = elementObject.GetValue(
                TJS_W("label"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineLabelHint_guess);

            detail::EmoteVarRef& controllerRef =
                _variableControllerRefs[_stateMachineDeque4.back().label];
            controllerRef.type = 4;
            controllerRef.index = metadataIndex;
        }
    }

    // Deque #5 independently repeats the Eye builder's publication sequence:
    // filter by enabled, append a raw controller owner with empty label, assign
    // label, then publish/overwrite type 5 with the un-compacted metadata index.
    void EmoteEngine::buildEyebrowControl_guess(
        const tTJSVariant& eyebrowControl) {
        ncbPropAccessor controlObject{tTJSVariant(eyebrowControl)};
        const int count = static_cast<int>(controlObject.GetArrayCount());
        for (int metadataIndex = 0;
             metadataIndex < count; ++metadataIndex) {
            const tTJSVariant element =
                controlObject.GetValue(
                    metadataIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor elementObject{tTJSVariant(element)};
            if (!elementObject.GetValue(
                    TJS_W("enabled"), ncbTypedefs::Tag<bool>(), 0,
                    &engineEnabledHint_guess)) {
                continue;
            }

            EmoteEyebrowController *controller =
                new EmoteEyebrowController(element);

            _stateMachineDeque5.emplace_back(controller);
            _stateMachineDeque5.back().label = elementObject.GetValue(
                TJS_W("label"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineLabelHint_guess);

            detail::EmoteVarRef& controllerRef =
                _variableControllerRefs[_stateMachineDeque5.back().label];
            controllerRef.type = 5;
            controllerRef.index = metadataIndex;
        }
    }

    // Mouth entries are unique among the leaf control categories: one
    // controller owns two published values and therefore registers two
    // controller references. The metadata loop index is preserved even when
    // disabled elements were skipped; it is not replaced with the deque index.
    void EmoteEngine::buildMouthControl_guess(
        const tTJSVariant& mouthControl) {
        ncbPropAccessor controlObject{tTJSVariant(mouthControl)};
        const int metadataCount =
            static_cast<int>(controlObject.GetArrayCount());
        for (int metadataIndex = 0;
             metadataIndex < metadataCount;
             ++metadataIndex) {
            const tTJSVariant element = controlObject.GetValue(
                metadataIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor elementObject{tTJSVariant(element)};
            if (!elementObject.GetValue(
                    TJS_W("enabled"), ncbTypedefs::Tag<bool>(), 0,
                    &engineEnabledHint_guess)) {
                continue;
            }

            // The binary first pushes {ctl,0,0}, then CopyRefs both strings into
            // the newly appended slot. It passes a raw controller pointer and
            // does not keep a source controller owner across deque growth, so
            // growth failure after a successful controller construction
            // retains the shipped leak.
            EmoteMouthController *controller =
                new EmoteMouthController(element);
            _compositeVarDeque6.emplace_back(controller);
            EmoteMouthControlEntry_Deque6& entry =
                _compositeVarDeque6.back();

            // label receives beginFrame; talkLabel receives currentValue.
            entry.label = elementObject.GetValue(
                TJS_W("label"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineLabelHint_guess);
            entry.talkLabel = elementObject.GetValue(
                TJS_W("talkLabel"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineTalkLabelHint_guess);

            detail::EmoteVarRef& labelRef =
                _variableControllerRefs[entry.label];
            labelRef.type = 6;
            labelRef.index = metadataIndex;

            detail::EmoteVarRef& talkLabelRef =
                _variableControllerRefs[entry.talkLabel];
            talkLabelRef.type = 6;
            talkLabelRef.index = metadataIndex;
        }
    }

    // Four-reference common builder. The metadata dispatcher constructs the
    // transition deque immediately before calling this function. Each matching
    // option borrows the first transition controller with the same label,
    // disables that transition entry's direct setVariable gate, and removes the
    // label from the raw variable-binding list. Missing matches remain null and
    // are skipped by applySelection. The controller-ref map deliberately stores
    // the metadata loop index, including indices occupied by disabled entries.
    void EmoteEngine::buildSelectorControl_guess(
        const tTJSVariant &selectorControl) {
        ncbPropAccessor controlObject{tTJSVariant(selectorControl)};
        const int metadataCount =
            static_cast<int>(controlObject.GetArrayCount());
        for (int metadataIndex = 0;
             metadataIndex < metadataCount;
             ++metadataIndex) {
            const tTJSVariant element = controlObject.GetValue(
                metadataIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor elementObject{tTJSVariant(element)};

            // label = element["label"] (variable-value output key and
            // controller-ref key).
            const ttstr label = elementObject.GetValue(
                TJS_W("label"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineLabelHint_guess);

            // A disabled selector also removes its raw variable binding.
            if (!elementObject.GetValue(
                    TJS_W("enabled"), ncbTypedefs::Tag<bool>(), 0,
                    &engineEnabledHint_guess)) {
                removeVariableLabel_guess(label);
                continue;
            }

            // Assemble optionList[] from element["optionList"].
            std::vector<EmoteSelectorOption_guess> optionList;
            ncbPropAccessor optionMetadata{elementObject.GetValue(
                TJS_W("optionList"),
                ncbTypedefs::Tag<tTJSVariant>(), 0,
                &selectorOptionListHint_guess)};
            const int optionCount =
                static_cast<int>(optionMetadata.GetArrayCount());
            for (int optionIndex = 0;
                 optionIndex < optionCount;
                 ++optionIndex) {
                ncbPropAccessor option{optionMetadata.GetValue(
                    optionIndex, ncbTypedefs::Tag<tTJSVariant>())};

                // optionLabel = option["label"].
                const ttstr optionLabel = option.GetValue(
                    TJS_W("label"), ncbTypedefs::Tag<ttstr>(), 0,
                    &engineLabelHint_guess);

                // Linear first-match scan over the already-built transition
                // deque. Every option restarts at the deque head. With no
                // matching label, transitionController stays null.
                EmoteVarController* transitionController = nullptr;
                for (EmoteTransitionControlEntry_Deque8& transitionEntry :
                     _auxVarDeque8) {
                    if (transitionEntry.label == optionLabel) {
                        transitionController = transitionEntry.ctl.get();
                        transitionEntry.flag = 0;
                        removeVariableLabel_guess(optionLabel);
                        break;
                    }
                }

                const float offValue = option.GetValue(
                    TJS_W("offValue"), ncbTypedefs::Tag<float>(), 0,
                    &selectorOffValueHint_guess);
                const float onValue = option.GetValue(
                    TJS_W("onValue"), ncbTypedefs::Tag<float>(), 0,
                    &selectorOnValueHint_guess);

                EmoteSelectorOption_guess selectorOption;
                selectorOption.refCtl = transitionController;
                selectorOption.offValue = offValue;
                selectorOption.onValue = onValue;
                optionList.push_back(selectorOption);
            }

            // The deque entry owns the selector. Its option vector stores only
            // borrowed transition-controller pointers. The controller ctor
            // immediately applies selection index 0 in option order.
            EmoteSelectorController* controller =
                new EmoteSelectorController(std::move(optionList));

            // Push {controller, empty label, indeterminate gate, empty targets}
            // first, then copy the label into back().label.
            _vectorVarDeque9.emplace_back(controller);
            _vectorVarDeque9.back().label = label;

            // Controller ref {type=8,index=original metadataIndex}.
            detail::EmoteVarRef& controllerRef =
                _variableControllerRefs[_vectorVarDeque9.back().label];
            controllerRef.type = 8;
            controllerRef.index = metadataIndex;
        }
    }

    // Four-reference common transition builder. Every enabled metadata element
    // owns one scalar EmoteVarController and starts with its direct-write flag
    // set. Disabled elements are skipped but still consume their metadata loop
    // index. The following selector builder may borrow the controller and clear
    // the flag, transferring write authority without transferring ownership.
    void EmoteEngine::buildTransitionControl_guess(
        const tTJSVariant &transitionControl) {
        ncbPropAccessor controlObject{tTJSVariant(transitionControl)};
        const int metadataCount =
            static_cast<int>(controlObject.GetArrayCount());
        for (int metadataIndex = 0;
             metadataIndex < metadataCount;
             ++metadataIndex) {
            const tTJSVariant element = controlObject.GetValue(
                metadataIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor elementObject{tTJSVariant(element)};

            if (!elementObject.GetValue(
                    TJS_W("enabled"), ncbTypedefs::Tag<bool>(), 0,
                    &engineEnabledHint_guess)) {
                continue;
            }

            // A completed new-expression is held as a raw pointer until direct
            // emplacement. Thus constructor failure frees the allocation, while
            // deque growth failure after construction retains native leak behavior.
            EmoteVarController* controller = new EmoteVarController(1);

            // Push {controller, null-label, flag=1} first, then assign label to the
            // pushed entry, preserving the common source order.
            _auxVarDeque8.emplace_back(controller);
            _auxVarDeque8.back().label = elementObject.GetValue(
                TJS_W("label"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineLabelHint_guess);

            // Controller ref {type=7,index=original metadataIndex}.
            detail::EmoteVarRef& controllerRef =
                _variableControllerRefs[_auxVarDeque8.back().label];
            controllerRef.type = 7;
            controllerRef.index = metadataIndex;
        }
    }

    // Append one controller for every enabled loopControl element. Each
    // transitionList triple is narrowed to the common 12-byte float POD before
    // the {controller,label} entry is appended. `var_loop` is both the deque
    // label/variable-value output key and the controller-ref key. The
    // controller-ref map stores the original metadata index, so disabled
    // elements still create holes in that index sequence.
    // This builder does not clear any existing state.
    void EmoteEngine::buildLoopControl_guess(
        const tTJSVariant &loopControl) {
        ncbPropAccessor controlObject{tTJSVariant(loopControl)};
        const int count = static_cast<int>(controlObject.GetArrayCount());
        for(int metadataIndex = 0; metadataIndex < count; ++metadataIndex) {
            const tTJSVariant element = controlObject.GetValue(
                metadataIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor elementObject{tTJSVariant(element)};
            if(!elementObject.GetValue(
                   TJS_W("enabled"), ncbTypedefs::Tag<bool>(), 0,
                   &engineEnabledHint_guess)) {
                continue;
            }

            ncbPropAccessor transitionList{elementObject.GetValue(
                TJS_W("transitionList"),
                ncbTypedefs::Tag<tTJSVariant>(), 0,
                &loopTransitionListHint_guess)};
            const int kfCount =
                static_cast<int>(transitionList.GetArrayCount());

            auto *ctl = new EmoteLoopController();
            ctl->keys.resize(static_cast<size_t>(kfCount));

            for(int keyIndex = 0; keyIndex < kfCount; ++keyIndex) {
                ncbPropAccessor frame{transitionList.GetValue(
                    keyIndex, ncbTypedefs::Tag<tTJSVariant>())};
                auto &dst = ctl->keys[static_cast<size_t>(keyIndex)];
                dst.startValue_guess = static_cast<float>(
                    frame.GetValue(0, ncbTypedefs::Tag<tjs_real>()));
                dst.endValue_guess = static_cast<float>(
                    frame.GetValue(1, ncbTypedefs::Tag<tjs_real>()));
                dst.span = static_cast<float>(
                    frame.GetValue(2, ncbTypedefs::Tag<tjs_real>()));
            }

            _lookupCurvesDeque10.emplace_back(ctl);
            _lookupCurvesDeque10.back().label = elementObject.GetValue(
                TJS_W("var_loop"), ncbTypedefs::Tag<ttstr>(), 0,
                &loopVarLoopHint_guess);

            detail::EmoteVarRef& ref =
                _variableControllerRefs[_lookupCurvesDeque10.back().label];
            ref.type = 3;
            ref.index = metadataIndex;
        }
    }

    // All four references snapshot Count once and append only enabled entries;
    // disabled metadata therefore leaves no placeholder and its original index
    // is not retained anywhere. A new entry is zeroed before being populated in
    // type, var_lr, var_ud, min, max source order. The builder neither clears the
    // deque nor registers a controller ref, and a later property/conversion
    // failure does not roll the already-appended, possibly partial entry back.
    void EmoteEngine::buildClampControl_guess(
        const tTJSVariant &clampControl) {
        ncbPropAccessor controlObject{tTJSVariant(clampControl)};
        const int metadataCount =
            static_cast<int>(controlObject.GetArrayCount());
        for(int metadataIndex = 0;
            metadataIndex < metadataCount;
            ++metadataIndex) {
            const tTJSVariant metadata = controlObject.GetValue(
                metadataIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor metadataObject{tTJSVariant(metadata)};
            if(!metadataObject.GetValue(
                   TJS_W("enabled"), ncbTypedefs::Tag<bool>(), 0,
                   &engineEnabledHint_guess)) {
                continue;
            }

            _clampControlDeque7.emplace_back();
            EmoteClampControlEntry_Deque7 &entry =
                _clampControlDeque7.back();

            entry.type = static_cast<int>(metadataObject.GetValue(
                TJS_W("type"), ncbTypedefs::Tag<tjs_int>(), 0,
                &engineTypeHint_guess));
            entry.varLr = metadataObject.GetValue(
                TJS_W("var_lr"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineVarLrHint_guess);
            entry.varUd = metadataObject.GetValue(
                TJS_W("var_ud"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineVarUdHint_guess);
            entry.minValue = metadataObject.GetValue(
                TJS_W("min"), ncbTypedefs::Tag<tjs_real>(), 0,
                &detail::emoteVariableRangeMinHint_guess);
            entry.maxValue = metadataObject.GetValue(
                TJS_W("max"), ncbTypedefs::Tag<tjs_real>(), 0,
                &detail::emoteVariableRangeMaxHint_guess);
        }
    }

    // There is no enabled gate, empty-string filter, deduplication or
    // builder-local clear. Every item in variableMatchList is appended as ttstr;
    // all four references use one dedicated process-wide hint for this read.
    // The copied input and returned list each own an ncb accessor; Count is
    // snapshotted once and the nested accessor is released before the root.
    void EmoteEngine::buildMirrorControl_guess(
        const tTJSVariant &mirrorControl) {
        ncbPropAccessor controlObject{tTJSVariant(mirrorControl)};
        ncbPropAccessor variableMatchList{controlObject.GetValue(
            TJS_W("variableMatchList"), ncbTypedefs::Tag<tTJSVariant>(), 0,
            &mirrorVariableMatchListHint_guess)};
        const int count =
            static_cast<int>(variableMatchList.GetArrayCount());
        for(int patternIndex = 0; patternIndex < count; ++patternIndex) {
            _mirrorVariablePatterns.push_back(variableMatchList.GetValue(
                patternIndex, ncbTypedefs::Tag<ttstr>()));
        }
    }

    // Match is intentionally `first IndexOf >= 1`: a first occurrence at the
    // beginning is a miss even if the pattern occurs again later. Both results
    // remain cached across mirror-gate toggles until metadata reset.
    bool EmoteEngine::shouldMirrorLabel_guess(
        const ttstr &label) {
        if(!_mirrorChanged) {
            return false;
        }
        if(_mirrorMatchCache.find(label) !=
           _mirrorMatchCache.end()) {
            return true;
        }
        if(_mirrorMissCache.find(label) !=
           _mirrorMissCache.end()) {
            return false;
        }

        for(const ttstr &pattern : _mirrorVariablePatterns) {
            if(label.IndexOf(pattern, 0) >= 1) {
                _mirrorMatchCache.insert(label);
                return true;
            }
        }
        _mirrorMissCache.insert(label);
        return false;
    }

    // Runs once after the ordinary variable-value bind loop. Missing
    // variable-value keys start at zero; timeline contributions are then
    // accumulated into those local values.
    // A zero [min,max] range is not guarded and therefore preserves native
    // floating-point NaN/Inf propagation.
    void EmoteEngine::applyClampControls_guess() {
        Player &embeddedPlayer = player();
        for(const EmoteClampControlEntry_Deque7 &entry : _clampControlDeque7) {
            double lrValue = 0.0;
            double udValue = 0.0;
            if(const auto it = _variableValues.find(entry.varLr);
               it != _variableValues.end()) {
                lrValue = it->second;
            }
            if(const auto it = _variableValues.find(entry.varUd);
               it != _variableValues.end()) {
                udValue = it->second;
            }

            accumulateTimelineContribution_guess(
                entry.varLr, lrValue);
            accumulateTimelineContribution_guess(
                entry.varUd, udValue);

            const double range = entry.maxValue - entry.minValue;
            double lrNorm = ((lrValue - entry.minValue) / range) * 2.0 - 1.0;
            double udNorm = ((udValue - entry.minValue) / range) * 2.0 - 1.0;
            if(lrNorm != 0.0 && udNorm != 0.0) {
                if(entry.type != 0) {
                    if(entry.type == 1 &&
                       std::sqrt(lrNorm * lrNorm + udNorm * udNorm) > 1.0) {
                        const double angle = std::atan2(udNorm, lrNorm);
                        lrNorm = std::cos(angle);
                        udNorm = std::sin(angle);
                    }
                } else {
                    const double rawRatio = std::abs(lrNorm / udNorm);
                    const double ratio = rawRatio <= 1.0
                        ? rawRatio : 1.0 / rawRatio;
                    const double invLen = 1.0 / std::sqrt(ratio * ratio + 1.0);
                    const double projectedX = lrNorm * invLen;
                    const double projectedY = invLen * udNorm;
                    const double projectedLength = std::sqrt(
                        projectedX * projectedX + projectedY * projectedY);
                    const double scale =
                        (1.0 - std::cos(ratio * 1.57079633)) *
                            (std::sin(projectedLength * 1.57079633) /
                                 projectedLength - 1.0) +
                        1.0;
                    lrNorm = projectedX * scale;
                    udNorm = scale * projectedY;
                }
            }

            const double lrFinal = entry.minValue +
                range * (lrNorm + 1.0) * 0.5;
            const double udFinal = entry.minValue +
                range * (udNorm + 1.0) * 0.5;
            embeddedPlayer.bindParameterValue_guess(
                entry.varLr, 0,
                shouldMirrorLabel_guess(entry.varLr)
                    ? -lrFinal : lrFinal);
            embeddedPlayer.bindParameterValue_guess(
                entry.varUd, 0, udFinal);
        }
    }

    // Four-reference common builder. The optional-property gate belongs to
    // applyMetadata; this helper appends converted keys to the existing set.
    // Duplicate insertion is an STL boundary: old libstdc++ allocates and
    // retains a candidate before lookup, whereas libc++ allocates only on miss.
    void EmoteEngine::buildInstantVariableList_guess(
        const tTJSVariant &instantVariableList) {
        ncbPropAccessor controlObject{tTJSVariant(instantVariableList)};
        const int count = static_cast<int>(controlObject.GetArrayCount());
        for(int index = 0; index < count; ++index) {
            const ttstr value = controlObject.GetValue(
                index, ncbTypedefs::Tag<ttstr>());
            _instantVariableLabels.insert(value);
        }
    }

    // Rebuild only the declared-label vectors; retain timeline states and active
    // labels. Folder traversal extends the references' flat metadata reader.
    void EmoteEngine::buildTimelineControl_guess(
        const tTJSVariant &timelineControl) {
        _timelineLabels.clear();
        _timelineDiffLabels.clear();
        appendTimelineControlEntries(timelineControl);
    }

    void EmoteEngine::appendTimelineControlEntries(
        const tTJSVariant &timelineControl) {
        ncbPropAccessor controlObject{tTJSVariant(timelineControl)};
        const int count = static_cast<int>(controlObject.GetArrayCount());
        for(int index = 0; index < count; ++index) {
            const tTJSVariant elem = controlObject.GetValue(
                index, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor elementObject{tTJSVariant(elem)};

            // Newer models group timelines into folders. Only their children
            // are playable; registering folder labels loses the actual loops.
            tTJSVariant type;
            if(elementObject.checkVariant(TJS_W("type"), type) &&
               type.Type() == tvtString && ttstr(type) == TJS_W("folder")) {
                appendTimelineControlEntries(elementObject.GetValue(
                    TJS_W("children"), ncbTypedefs::Tag<tTJSVariant>()));
                continue;
            }

            // HasValue destroys its MEMBERMUSTEXIST probe Variant before the
            // second typed read, so a getter can observe two independent calls.
            const bool hasDiff = elementObject.HasValue(
                TJS_W("diff"), &timelineDiffHint_guess);

            std::vector<ttstr> &target =
                hasDiff && elementObject.GetValue(
                    TJS_W("diff"), ncbTypedefs::Tag<bool>(), 0,
                    &timelineDiffHint_guess)
                    ? _timelineDiffLabels
                    : _timelineLabels;

            const ttstr label = elementObject.GetValue(
                TJS_W("label"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineLabelHint_guess);
            target.push_back(label);

            // Duplicate labels stay duplicated in the vectors. operator[]
            // returns the existing mapped value, so only its raw metadata owner
            // is replaced; decoded data, controllers, flags and times survive.
            _timelineStates[label].rawElement = elem;
        }
    }

    // ------------------------------------------------------------------------
    // Spring-physics deque builders (population path). The helper reads a raw
    // dictionary dispatch's x/y/z values and narrows them to floats.
    // ------------------------------------------------------------------------
    namespace {

        // Copy the source Variant into the helper-owned ncb accessor for the
        // full x -> y -> z sequence, then narrow each TJS real to float.
        void springVec3FromVariant_guess(
            const tTJSVariant& dict, float out[3]) {
            ncbPropAccessor object{tTJSVariant(dict)};
            out[0] = static_cast<float>(object.GetValue(
                TJS_W("x"), ncbTypedefs::Tag<tjs_real>(), 0,
                &enginePointXHint_guess));
            out[1] = static_cast<float>(object.GetValue(
                TJS_W("y"), ncbTypedefs::Tag<tjs_real>(), 0,
                &enginePointYHint_guess));
            out[2] = static_cast<float>(object.GetValue(
                TJS_W("z"), ncbTypedefs::Tag<tjs_real>(), 0,
                &enginePointZHint_guess));
        }

    } // namespace

    // `bustControl` populates deque #1, whose simple spring is consumed by
    // stepHairParts. The native loop snapshots Count once, skips disabled
    // metadata without compacting its index, constructs the spring from the
    // outer metadata element, and only then overwrites its dynamic state from
    // the nested `param` dictionary.
    void EmoteEngine::buildBustControl_guess(
        const tTJSVariant& bustControl) {
        ncbPropAccessor controlObject{tTJSVariant(bustControl)};
        const int metadataCount =
            static_cast<int>(controlObject.GetArrayCount());
        for (int metadataIndex = 0;
             metadataIndex < metadataCount;
             ++metadataIndex) {
            const tTJSVariant metadata = controlObject.GetValue(
                metadataIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor metadataObject{tTJSVariant(metadata)};
            if (!metadataObject.GetValue(
                    TJS_W("enabled"), ncbTypedefs::Tag<bool>(), 0,
                    &engineEnabledHint_guess)) {
                continue;
            }

            ncbPropAccessor parameters{metadataObject.GetValue(
                TJS_W("param"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &engineParamHint_guess)};

            // A constructor throw is covered by the new-expression's allocation
            // rollback. After it returns, this intentionally remains a raw
            // pointer until emplace succeeds; property/grow failures leak it.
            EmoteSpringState* spring = new EmoteSpringState(metadata);

            // op/p/pv vec3 (dict x/y/z) overwrite stored/pos/vel.
            float vector[3];
            springVec3FromVariant_guess(parameters.GetValue(
                TJS_W("op"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &engineOpHint_guess), vector);
            spring->storedX = vector[0];
            spring->storedY = vector[1];
            spring->storedZ = vector[2];
            springVec3FromVariant_guess(parameters.GetValue(
                TJS_W("p"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &enginePHint_guess), vector);
            spring->posX = vector[0];
            spring->posY = vector[1];
            spring->posZ = vector[2];
            springVec3FromVariant_guess(parameters.GetValue(
                TJS_W("pv"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &enginePvHint_guess), vector);
            spring->velX = vector[0];
            spring->velY = vector[1];
            spring->velZ = vector[2];
            spring->biasY = static_cast<float>(parameters.GetValue(
                TJS_W("ofs"), ncbTypedefs::Tag<tjs_real>(), 0,
                &engineOfsHint_guess));

            // Raw-pointer emplace is the exact ownership hand-off. It copies the
            // pointer; the source local is deliberately not cleared. The raw
            // entry constructor sets initFlag=1, empties all three strings and
            // zeros both anchors, but does not write ABI alignment padding.
            _hairPartsNodes.emplace_back(spring);
            EmoteHairPartsNode48B& entry = _hairPartsNodes.back();
            entry.shapeLabel = metadataObject.GetValue(
                TJS_W("baseLayer"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineBaseLayerHint_guess);
            entry.keyX = metadataObject.GetValue(
                TJS_W("var_lr"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineVarLrHint_guess);
            entry.keyY = metadataObject.GetValue(
                TJS_W("var_ud"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineVarUdHint_guess);

            // Both keys accept empty/equal/duplicate values. Publication is
            // sequential, so later metadata (and var_ud after var_lr within one
            // element) overwrites the mapped ref while every deque owner stays.
            detail::EmoteVarRef& lrRef = _variableControllerRefs[entry.keyX];
            lrRef.type = 0;
            lrRef.index = metadataIndex;
            detail::EmoteVarRef& udRef = _variableControllerRefs[entry.keyY];
            udRef.type = 0;
            udRef.index = metadataIndex;
        }
    }

    // Metadata builder shared by `hairControl` (type 1 / deque #2) and
    // `partsControl` (type 2 / deque #3). Count is snapshotted once and disabled
    // rows are skipped without compacting the original metadata index.
    void EmoteEngine::buildChainControl_guess(
        std::deque<EmoteBustChain1Node56B>& chainNodes,
        const tTJSVariant& chainControl, int typeTag) {
        ncbPropAccessor controlObject{tTJSVariant(chainControl)};
        const int metadataCount =
            static_cast<int>(controlObject.GetArrayCount());
        for (int metadataIndex = 0;
             metadataIndex < metadataCount;
             ++metadataIndex) {
            const tTJSVariant metadata = controlObject.GetValue(
                metadataIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor metadataObject{tTJSVariant(metadata)};
            if (!metadataObject.GetValue(
                    TJS_W("enabled"), ncbTypedefs::Tag<bool>(), 0,
                    &engineEnabledHint_guess)) {
                continue;
            }

            ncbPropAccessor parameters{metadataObject.GetValue(
                TJS_W("param"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &engineParamHint_guess)};

            // Constructor failure is covered by new-expression rollback. Once
            // construction succeeds the spring remains raw through every
            // nested property/array read and any deque growth, so failure in
            // that interval retains the native leak window.
            EmoteBustChainSpring* spring =
                new EmoteBustChainSpring(metadata);

            float vector[3];
            springVec3FromVariant_guess(parameters.GetValue(
                TJS_W("op"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &engineOpHint_guess), vector);
            spring->op[0] = vector[0];
            spring->op[1] = vector[1];
            spring->op[2] = vector[2];
            spring->ofs = static_cast<float>(parameters.GetValue(
                TJS_W("ofs"), ncbTypedefs::Tag<tjs_real>(), 0,
                &engineOfsHint_guess));
            spring->bendR = static_cast<float>(parameters.GetValue(
                TJS_W("bendR"), ncbTypedefs::Tag<tjs_real>(), 0,
                &engineBendRHint_guess));
            spring->bendS = static_cast<float>(parameters.GetValue(
                TJS_W("bendS"), ncbTypedefs::Tag<tjs_real>(), 0,
                &engineBendSHint_guess));

            // Three independent array owners coexist in bp -> p -> pv
            // construction order. The serialized names describe the recovered
            // snapshot roles: bp restores target p, p restores current pv, and
            // pv restores velocity bp. Scope exit releases them in reverse.
            ncbPropAccessor basePositions{parameters.GetValue(
                TJS_W("bp"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &engineBpHint_guess)};
            ncbPropAccessor positions{parameters.GetValue(
                TJS_W("p"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &enginePHint_guess)};
            ncbPropAccessor velocities{parameters.GetValue(
                TJS_W("pv"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &enginePvHint_guess)};
            springVec3FromVariant_guess(
                basePositions.GetValue(
                    0, ncbTypedefs::Tag<tTJSVariant>()), spring->p[0]);
            springVec3FromVariant_guess(
                basePositions.GetValue(
                    1, ncbTypedefs::Tag<tTJSVariant>()), spring->p[1]);
            springVec3FromVariant_guess(
                positions.GetValue(
                    0, ncbTypedefs::Tag<tTJSVariant>()), spring->pv[0]);
            springVec3FromVariant_guess(
                positions.GetValue(
                    1, ncbTypedefs::Tag<tTJSVariant>()), spring->pv[1]);
            springVec3FromVariant_guess(
                velocities.GetValue(
                    0, ncbTypedefs::Tag<tTJSVariant>()), spring->bp[0]);
            springVec3FromVariant_guess(
                velocities.GetValue(
                    1, ncbTypedefs::Tag<tTJSVariant>()), spring->bp[1]);

            // This is the ownership-transfer boundary. The raw entry constructor
            // copies the pointer, leaves initFlag and ABI padding untouched,
            // creates four empty strings, and zeros both anchors.
            chainNodes.emplace_back(spring);
            EmoteBustChain1Node56B& entry = chainNodes.back();
            entry.shapeLabel = metadataObject.GetValue(
                TJS_W("baseLayer"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineBaseLayerHint_guess);
            entry.keyA = metadataObject.GetValue(
                TJS_W("var_lr"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineVarLrHint_guess);
            entry.keyB = metadataObject.GetValue(
                TJS_W("var_lrm"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineVarLrmHint_guess);
            entry.keyC = metadataObject.GetValue(
                TJS_W("var_ud"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineVarUdHint_guess);

            // Empty/equal/duplicate keys are accepted. Sequential lr -> lrm ->
            // ud publication means the later map write wins, while all deque
            // entries retain their independent spring owners.
            detail::EmoteVarRef& lrRef = _variableControllerRefs[entry.keyA];
            lrRef.type = typeTag;
            lrRef.index = metadataIndex;
            detail::EmoteVarRef& lrmRef = _variableControllerRefs[entry.keyB];
            lrmRef.type = typeTag;
            lrmRef.index = metadataIndex;
            detail::EmoteVarRef& udRef = _variableControllerRefs[entry.keyC];
            udRef.type = typeTag;
            udRef.index = metadataIndex;
        }
    }

    // ========================================================================
    // setVariable value-dispatch in the four current references and
    // the 5 per-category enqueue functions it calls. Each enqueue pushes a
    // transition keyframe {value, easing, factor} into the controller's internal
    // keyframe std::deque (its libc++/libstdc++ map/block projection varies by
    // reference ABI and is documented outside compiled source),
    // or — on the instant path (easing <= 0) — clears the deque and snaps the
    // controller's scalar state. Element fields are the controllers' named
    // keyframe types (EmoteAngleKeyValue12B / EmoteVarKeyValue20B). The source
    // tuple {value, easing, factor} is stored as {value, duration, powCount} in
    // raw float fields (the step reads powCount as float, without integer
    // conversion).
    // ========================================================================
    namespace {

        // durationFrames is the third binary floating-point argument (TJS
        // "ease"). All four builds use this same piecewise transform.
        double emoteTransitionFactor_guess(double durationFrames) {
            if (durationFrames == 0.0) {
                return 1.0;
            }
            if (durationFrames > 0.0) {
                return durationFrames + 1.0;
            }
            return 1.0 / (1.0 - durationFrames);
        }

        // The mouth-label route uses FCVTZS W,D / VCVT.S32.F64 in the four
        // references. Spell out that target-instruction boundary so malformed
        // script values cannot reach C++'s undefined float-to-int conversion.
        int32_t emoteSignedInt32TowardZeroSaturated_guess(double value) {
            constexpr double lower = -0x1p31;
            constexpr double upper = 0x1p31;
            if(std::isnan(value)) {
                return 0;
            }
            if(value >= upper) {
                return std::numeric_limits<int32_t>::max();
            }
            if(value <= lower) {
                return std::numeric_limits<int32_t>::min();
            }
            return static_cast<int32_t>(value);
        }

    } // namespace

    double EmoteEngine::getVariable(ttstr label) {
        Player &embeddedPlayer = player();
        if(embeddedPlayer.hasVariableLabelScope_guess(label)) {
            return embeddedPlayer.getVariable(label);
        }
        return embeddedPlayer.readSnapshotOrBoundParameterValue_guess(label);
    }

    // General Engine variable router in all four current references. Timeline
    // seek/window call this same method; there is no timeline-only wrapper.
    void EmoteEngine::setVariable(const ttstr& key, double value, double easing,
                                  double durationFrames) {
        // Empty keys are still looked up. A miss falls through to the
        // variable-value map.
        auto it = _variableControllerRefs.find(key);
        if (it != _variableControllerRefs.end()) {
            const detail::EmoteVarRef& ref = it->second;
            const double factor =
                emoteTransitionFactor_guess(durationFrames);

            _dirty = true;

            switch (ref.type) {
                case 0:
                case 1:
                case 2:
                    // These types reach the variable-value map only in
                    // direct-edit mode; otherwise
                    // their spring/constant feed is handled by a separate pass.
                    if (_directEdit) {
                        break;
                    }
                    return;
                case 4: {
                    // deque#4[ref.index] -> the Blink value-track enqueue helper.
                    EmoteEyeControlEntry_Deque4& entry =
                        _stateMachineDeque4[ref.index];
                    const float vValue = static_cast<float>(value);
                    const float vEasing = static_cast<float>(easing);
                    const float vFactor = static_cast<float>(factor);
                    EmoteBlinkController_enqueueValue_guess(
                        entry.ctl.get(), vValue, vEasing, vFactor, _queuing);
                    return;
                }
                case 5: {
                    // deque#5[ref.index] -> the Eyebrow value-track enqueue helper.
                    EmoteEyebrowControlEntry_Deque5& entry =
                        _stateMachineDeque5[ref.index];
                    const float vValue = static_cast<float>(value);
                    const float vEasing = static_cast<float>(easing);
                    const float vFactor = static_cast<float>(factor);
                    EmoteEyebrowController_enqueueValue_guess(
                        entry.ctl.get(), vValue, vEasing, vFactor, _queuing);
                    return;
                }
                case 6: {
                    // deque#6[ref.index]. Dual-key controller: if `key` equals the
                    //   element's "label" -> write beginFrame directly. If `key` equals the
                    //   element's "talkLabel" -> enqueue the talk ramp through
                    //   the mouth controller's shared target setter. (The
                    //   binaries compare pointer-eq before the string compare.)
                    EmoteMouthControlEntry_Deque6& entry =
                        _compositeVarDeque6[ref.index];
                    if (entry.label == key) {
                        entry.ctl->beginFrame =
                            emoteSignedInt32TowardZeroSaturated_guess(value);
                        return;
                    }
                    if (entry.talkLabel == key) {
                        const float vValue = static_cast<float>(value);
                        const float vEasing = static_cast<float>(easing);
                        const float vFactor = static_cast<float>(factor);
                        EmoteMouthController_setTarget_guess(
                            entry.ctl.get(), vValue, vEasing, vFactor,
                            _queuing);
                    }
                    return;
                }
                case 7: {
                    // Transition controllers borrowed by selectors have their
                    // direct-write gate cleared.
                    EmoteTransitionControlEntry_Deque8& entry =
                        _auxVarDeque8[ref.index];
                    if (!entry.flag) {
                        return;
                    }
                    const float vValue = static_cast<float>(value);
                    const float vEasing = static_cast<float>(easing);
                    const float vFactor = static_cast<float>(factor);
                    EmoteVarController_setTarget_guess(
                        entry.ctl.get(), &vValue, vEasing, vFactor,
                        _queuing);
                    return;
                }
                case 8: {
                    EmoteSelectorControlEntry_Deque9& entry =
                        _vectorVarDeque9[ref.index];
                    // The native builder leaves this enqueue gate
                    // indeterminate; the local entry preserves that boundary.
                    if (!entry.flag) {
                        return;
                    }
                    const float vValue = static_cast<float>(value);
                    const float vEasing = static_cast<float>(easing);
                    const float vFactor = static_cast<float>(factor);
                    EmoteSelectorController_enqueue_guess(
                        entry.ctl.get(), vValue, vEasing, vFactor, _queuing);
                    return;
                }
                default:
                    return;
            }
        }

        // Reached on controller-ref miss, or type 0/1/2 with directEdit set.
        _variableValues[key] = value;
    }

    // Lazily materializes a decoded timeline state. See
    // analysis/motionplayer_timeline_internal_four_binary_2026-08-12.md.
    void EmoteEngine::initializeTimelineState_guess(
        detail::EmoteTimelineState &state) {
        detail::EmoteTimelineData *timelineData =
            new detail::EmoteTimelineData();
        state.timelineData.reset(timelineData);

        ncbPropAccessor stateObject{tTJSVariant(state.rawElement)};
        state.loopBegin = stateObject.GetValue(
            TJS_W("loopBegin"), ncbTypedefs::Tag<tjs_real>(), 0,
            &timelineLoopBeginHint_guess);
        state.loopEnd = stateObject.GetValue(
            TJS_W("loopEnd"), ncbTypedefs::Tag<tjs_real>(), 0,
            &timelineLoopEndHint_guess);
        state.lastTime = stateObject.GetValue(
            TJS_W("lastTime"), ncbTypedefs::Tag<tjs_real>(), 0,
            &timelineLastTimeHint_guess);
        state.blendWeight = 1.0f;
        state.autoStop = 0.0;

        EmoteVarController *blendController = new EmoteVarController(1);
        state.blendController.reset(blendController);
        EmoteVarController_setTarget_guess(
            blendController, &state.blendWeight, 0.0f, 0.0f, false);

        ncbPropAccessor variableListObject{stateObject.GetValue(
            TJS_W("variableList"), ncbTypedefs::Tag<tTJSVariant>(), 0,
            &timelineVariableListHint_guess)};
        const int variableCount =
            static_cast<int>(variableListObject.GetArrayCount());
        double maxFrameTime = 0.0;
        for(int variableIndex = 0; variableIndex < variableCount;
            ++variableIndex) {
            const tTJSVariant variable = variableListObject.GetValue(
                variableIndex, ncbTypedefs::Tag<tTJSVariant>());
            ncbPropAccessor variableObject{tTJSVariant(variable)};
            ncbPropAccessor frameListObject{variableObject.GetValue(
                TJS_W("frameList"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &engineFrameListHint_guess)};
            const int frameCount =
                static_cast<int>(frameListObject.GetArrayCount());

            timelineData->variableList.emplace_back();
            detail::EmoteTimelineTrack &track =
                timelineData->variableList.back();
            track.label = variableObject.GetValue(
                TJS_W("label"), ncbTypedefs::Tag<ttstr>(), 0,
                &engineLabelHint_guess);
            // Membership is snapshotted once when the native Track is built;
            // Later set mutations do not retroactively change this Track flag.
            track.instantVariable =
                _instantVariableLabels.find(track.label) !=
                _instantVariableLabels.end();

            for(int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                const tTJSVariant rawFrame = frameListObject.GetValue(
                    frameIndex, ncbTypedefs::Tag<tTJSVariant>());
                track.frameList.emplace_back();
                detail::EmoteTimelineFrame24B &frame =
                    track.frameList.back();
                ncbPropAccessor frameObject{tTJSVariant(rawFrame)};
                frame.time = frameObject.GetValue(
                    TJS_W("time"), ncbTypedefs::Tag<tjs_real>(), 0,
                    &timelineTimeHint_guess);
                const int type = frameObject.GetValue(
                    TJS_W("type"), ncbTypedefs::Tag<tjs_int>(), 0,
                    &engineTypeHint_guess);
                if(frame.time > maxFrameTime) {
                    maxFrameTime = frame.time;
                }
                frame.typeZero = type == 0;
                if(type != 0) {
                    ncbPropAccessor contentObject{frameObject.GetValue(
                        TJS_W("content"),
                        ncbTypedefs::Tag<tTJSVariant>(), 0,
                        &timelineContentHint_guess)};
                    frame.value = static_cast<float>(
                        contentObject.GetValue(
                            TJS_W("value"),
                            ncbTypedefs::Tag<tjs_real>(), 0,
                            &engineValueHint_guess));
                    const double easing = contentObject.GetValue(
                        TJS_W("easing"),
                        ncbTypedefs::Tag<tjs_real>(), 0,
                        &timelineEasingHint_guess);
                    frame.easingWeight = easing == 0.0
                        ? 1.0
                        : easing > 0.0 ? easing + 1.0
                                       : 1.0 / (1.0 - easing);
                }
            }
        }
        if(state.lastTime < 0.0) {
            state.lastTime = maxFrameTime;
        }
    }

    void EmoteEngine::initializeTimelineControllers_guess(
        detail::EmoteTimelineState &state, tjs_uint32 flags) {
        // Flags commit before the parallel-controller gate. Empty and instant
        // tracks preserve any existing controller; eligible existing owners
        // are reset in place, while missing owners are constructed count=1.
        state.flags = flags;
        if((flags & 2u) == 0) {
            return;
        }
        for(detail::EmoteTimelineTrack &track :
            state.timelineData->variableList) {
            if(track.frameList.empty() || track.instantVariable) {
                continue;
            }
            if(!track.controller) {
                track.controller.reset(new EmoteVarController(1));
            } else {
                const float zero = 0.0f;
                EmoteVarController_setTarget_guess(
                    track.controller.get(), &zero, 0.0f, 0.0f, false);
            }
        }
    }

    void EmoteEngine::seekTimeline_guess(
        detail::EmoteTimelineState &state, double time) {
        state.frameCursors.clear();
        for(detail::EmoteTimelineTrack &track :
            state.timelineData->variableList) {
            // The native function does not append a cursor for this branch.
            if((state.flags & 4u) != 0 && track.instantVariable) {
                continue;
            }
            const bool internalRoute =
                (state.flags & 2u) != 0 && !track.instantVariable;
            std::size_t cursor = 0;
            int lastActionFrame = -1;
            if(track.frameList.size() >= 2) {
                const std::size_t scanCount = track.frameList.size() - 1;
                for(cursor = 0; cursor < scanCount; ++cursor) {
                    const detail::EmoteTimelineFrame24B &frame =
                        track.frameList[cursor];
                    if(!frame.typeZero) {
                        lastActionFrame = static_cast<int>(cursor);
                    }
                    if(frame.time <= time &&
                       track.frameList[cursor + 1].time > time) {
                        break;
                    }
                }
            }
            state.frameCursors.push_back(static_cast<int32_t>(cursor));
            if(lastActionFrame < 0) {
                continue;
            }

            const detail::EmoteTimelineFrame24B &frame =
                track.frameList[static_cast<std::size_t>(lastActionFrame)];
            const double transitionRaw =
                track.frameList[static_cast<std::size_t>(lastActionFrame) + 1].time -
                    time - 1.0;
            // All four seek bodies use an ordered <= zero select. It converts
            // negative zero to positive zero while allowing NaN to propagate.
            const double transition =
                transitionRaw <= 0.0 ? 0.0 : transitionRaw;
            if(internalRoute) {
                EmoteVarController_setTarget_guess(
                    track.controller.get(), &frame.value,
                    static_cast<float>(transition),
                    static_cast<float>(frame.easingWeight),
                    _queuing);
            } else {
                setVariable(track.label, frame.value, transition,
                            frame.easingWeight);
            }
        }
        state.currentTime = time;
    }

    void EmoteEngine::applyTimelineWindow_guess(
        detail::EmoteTimelineState &state, bool inclusive,
        double targetTime) {
        std::size_t trackIndex = 0;
        // A null decoded-data owner is a valid no-track window: native skips
        // every cursor/track access but still commits currentTime below.
        if(state.timelineData) {
            for(detail::EmoteTimelineTrack &track :
                state.timelineData->variableList) {
                if((state.flags & 4u) != 0 && track.instantVariable) {
                    // This physical-index increment intentionally disagrees with
                    // seekTimeline_guess's compact cursor vector. Preserve it.
                    ++trackIndex;
                    continue;
                }
                const bool internalRoute =
                    (state.flags & 2u) != 0 && !track.instantVariable;
                int32_t cursor = state.frameCursors[trackIndex];
                const std::size_t frameCount = track.frameList.size();
                if(cursor < static_cast<int32_t>(frameCount) - 1) {
                    auto crossed = [inclusive, targetTime](double frameTime) {
                        return inclusive ? frameTime <= targetTime
                                         : frameTime < targetTime;
                    };
                    while(crossed(track.frameList[
                                      static_cast<std::size_t>(cursor) + 1].time)) {
                        const std::size_t nextIndex =
                            static_cast<std::size_t>(cursor) + 1;
                        const detail::EmoteTimelineFrame24B &next =
                            track.frameList[nextIndex];
                        if(!next.typeZero && nextIndex + 1 < frameCount) {
                            const double transition = std::max(
                                track.frameList[nextIndex + 1].time -
                                    targetTime - 1.0,
                                0.0);
                            if(internalRoute) {
                                EmoteVarController_setTarget_guess(
                                    track.controller.get(), &next.value,
                                    static_cast<float>(transition),
                                    static_cast<float>(next.easingWeight),
                                    _queuing);
                            } else {
                                setVariable(track.label, next.value, transition,
                                            next.easingWeight);
                            }
                        }
                        cursor = static_cast<int32_t>(nextIndex);
                        if(nextIndex >= frameCount - 1) {
                            break;
                        }
                    }
                }
                state.frameCursors[trackIndex] = cursor;
                ++trackIndex;
            }
        }
        state.currentTime = targetTime;
    }

    void EmoteEngine::playTimeline_guess(
        const ttstr &label, tjs_uint32 flags) {
        if((flags & 1u) != 0) {
            stopTimeline_guess(ttstr());
        }
        const auto found = _timelineStates.find(label);
        if(found == _timelineStates.end()) {
            // The four native play bodies use the ordinary one-argument log
            // path here. A replace-mode stop above is already committed and
            // is deliberately not rolled back when the label is absent.
            TVPAddLog(
                ttstr(TJS_W("timeline label not found '")) + label +
                TJS_W("'."));
            return;
        }
        // Native code instantiates the full-range std::count algorithm on all
        // four ABIs; it does not stop after the first equal active label.
        if(std::count(_activeTimelineLabels.begin(),
                      _activeTimelineLabels.end(), label) == 0) {
            _activeTimelineLabels.push_back(label);
        }
        detail::EmoteTimelineState &state = found->second;
        if(!state.timelineData) {
            initializeTimelineState_guess(state);
        }
        initializeTimelineControllers_guess(state, flags);
        seekTimeline_guess(state, 0.0);
    }

    void EmoteEngine::stopTimeline_guess(const ttstr &label) {
        if(label.IsEmpty()) {
            _activeTimelineLabels.clear();
            return;
        }
        const auto found = std::find(_activeTimelineLabels.begin(),
                                     _activeTimelineLabels.end(), label);
        if(found != _activeTimelineLabels.end()) {
            _activeTimelineLabels.erase(found);
        }
    }

    bool EmoteEngine::isTimelinePlaying_guess(const ttstr &label) const {
        if(label.IsEmpty()) {
            return !_activeTimelineLabels.empty();
        }
        return std::find(_activeTimelineLabels.begin(),
                         _activeTimelineLabels.end(), label) !=
               _activeTimelineLabels.end();
    }

    void EmoteEngine::setTimelineBlendController_guess(
        const ttstr &label, float value, float transition,
        float easingWeight, bool autoStop) {
        const auto found = _timelineStates.find(label);
        if(found == _timelineStates.end()) {
            return;
        }
        detail::EmoteTimelineState &state = found->second;
        if(!state.timelineData) {
            initializeTimelineState_guess(state);
        }
        EmoteVarController_setTarget_guess(
            state.blendController.get(), &value, transition, easingWeight,
            _queuing);
        state.autoStop = autoStop ? 1.0 : 0.0;
    }

    void EmoteEngine::fadeInTimeline_guess(
        const ttstr &label, float duration, float easingWeight) {
        if(!isTimelinePlaying_guess(label)) {
            // play has no success result. On an unknown label it logs after
            // its flags=3 clear; the two blend calls below then miss silently.
            playTimeline_guess(label, 3u);
            setTimelineBlendController_guess(
                label, 0.0f, 0.0f, 1.0f, false);
        }
        setTimelineBlendController_guess(
            label, 1.0f, duration, easingWeight, false);
    }

    double EmoteEngine::getTimelineBlendRatio_guess(ttstr label) const {
        const auto found = _timelineStates.find(label);
        if(found != _timelineStates.end() && found->second.timelineData) {
            return found->second.blendWeight;
        }
        return 0.0;
    }

    tjs_int EmoteEngine::countMainTimelines_guess() const {
        return static_cast<tjs_int>(_timelineLabels.size());
    }

    ttstr EmoteEngine::getMainTimelineLabelAt_guess(
        tjs_uint32 index) const {
        if(index >= _timelineLabels.size()) {
            // The native empty-literal constructor also produces a null-backed
            // ttstr, so the default value is the exact out-of-range result.
            return ttstr();
        }
        return _timelineLabels[index];
    }

    tjs_int EmoteEngine::countDiffTimelines_guess() const {
        return static_cast<tjs_int>(_timelineDiffLabels.size());
    }

    ttstr EmoteEngine::getDiffTimelineLabelAt_guess(
        tjs_uint32 index) const {
        if(index >= _timelineDiffLabels.size()) {
            return ttstr();
        }
        return _timelineDiffLabels[index];
    }

    tjs_int EmoteEngine::countPlayingTimelines_guess() const {
        return static_cast<tjs_int>(_activeTimelineLabels.size());
    }

    ttstr EmoteEngine::getPlayingTimelineLabelAt_guess(
        tjs_uint32 index) const {
        if(index >= _activeTimelineLabels.size()) {
            return ttstr();
        }
        return _activeTimelineLabels[index];
    }

    tjs_int EmoteEngine::getPlayingTimelineFlagsAt_guess(
        tjs_uint32 index) const {
        // Keep the lookup even when the label getter returns its null-backed
        // empty value. A pre-existing empty-label state is observable for an
        // out-of-range index in all four references.
        const ttstr label = getPlayingTimelineLabelAt_guess(index);
        const auto found = _timelineStates.find(label);
        if(found == _timelineStates.end()) {
            return 0;
        }
        return static_cast<tjs_int>(found->second.flags);
    }

    bool EmoteEngine::getLoopTimeline_guess(ttstr label) const {
        const auto found = _timelineStates.find(label);
        if(found != _timelineStates.end()) {
            return found->second.loopBegin >= 0.0;
        }

        // All four references concatenate this exact diagnostic, send it to
        // the ordinary one-argument TVP log wrapper, and then return false.
        // A missing label is deliberately not an exception on this API.
        TVPAddLog(
            ttstr(TJS_W("timeline label not found '")) + label + TJS_W("'."));
        return false;
    }

    double EmoteEngine::getTimelineTotalFrameCount_guess(
        ttstr label) const {
        const auto found = _timelineStates.find(label);
        if(found != _timelineStates.end() &&
           found->second.loopBegin >= 0.0) {
            return found->second.lastTime;
        }
        return 0.0;
    }

    tTJSVariant EmoteEngine::getMainTimelineLabelList_guess() const {
        // The helper owns one fresh Array while exposing its borrowed native
        // deque.  Each append CopyRefs the source ttstr directly as a String
        // Variant; no script call, filtering, reserve, or intermediate vector
        // occurs.  Returning the value CopyRefs that Array before the helper
        // releases its local owner.
        detail::TJSArrayWithItems_guess result =
            detail::createTJSArrayWithItems_guess();
        for(const ttstr &label : _timelineLabels) {
            result.items->emplace_back(label);
        }
        return result.value;
    }

    tTJSVariant EmoteEngine::getDiffTimelineLabelList_guess() const {
        // This is the same fresh-Array owner/borrowed-Items/return-CopyRef
        // pipeline as the main-label getter; only the source vector differs.
        detail::TJSArrayWithItems_guess result =
            detail::createTJSArrayWithItems_guess();
        for(const ttstr &label : _timelineDiffLabels) {
            result.items->emplace_back(label);
        }
        return result.value;
    }

    tTJSVariant EmoteEngine::getPlayingTimelineInfoList_guess() const {
        detail::TJSArrayWithItems_guess result =
            detail::createTJSArrayWithItems_guess();
        for(const ttstr &label : _activeTimelineLabels) {
            const auto found = _timelineStates.find(label);
            if(found == _timelineStates.end()) {
                continue;
            }

            const detail::EmoteTimelineState &state = found->second;
            tTJSVariant dictionary = createTJSDictionary_guess();
            tTJSVariant objectValue(dictionary);
            objectValue.ToObject();
            ncbPropAccessor item(objectValue);
            objectValue.Clear();
            (void)item.SetValue(
                TJS_W("label"), label, TJS_MEMBERENSURE,
                &engineLabelHint_guess);
            (void)item.SetValue(
                TJS_W("flags"), static_cast<tjs_int>(state.flags),
                TJS_MEMBERENSURE, &timelineInfoFlagsHint_guess);
            (void)item.SetValue(
                TJS_W("blendRatio"),
                static_cast<double>(state.blendWeight),
                TJS_MEMBERENSURE, &timelineInfoBlendRatioHint_guess);
            result.items->push_back(dictionary);
        }
        return result.value;
    }

    void EmoteEngine::passTimelines_guess() {
        std::size_t activeIndex = 0;
        while(activeIndex < _activeTimelineLabels.size()) {
            const ttstr &timelineLabel =
                _activeTimelineLabels[activeIndex];
            detail::EmoteTimelineState &state =
                _timelineStates.at(timelineLabel);

            // Looping timelines, and parallel timelines which are already in
            // their pass-triggered fade, are left untouched.
            if(state.loopBegin >= 0.0 ||
               ((state.flags & 2u) != 0 && (state.flags & 4u) != 0)) {
                ++activeIndex;
                continue;
            }

            if((state.flags & 2u) != 0) {
                setTimelineBlendController_guess(
                    timelineLabel, 0.0f, 20.0f, 0.0f, true);
                state.flags |= 4u;
            }

            std::size_t trackIndex = 0;
            for(detail::EmoteTimelineTrack &track :
                state.timelineData->variableList) {
                if((state.flags & 2u) == 0 || track.instantVariable) {
                    // Both arm64 references add in Wn and SXTW before the
                    // size_t comparison; the 32-bit references use the same
                    // wrapping signed cursor. Keep that width boundary rather
                    // than widening the stored cursor before adding one.
                    int32_t frameIndex = static_cast<int32_t>(
                        static_cast<uint32_t>(
                            state.frameCursors[trackIndex]) + 1u);
                    while(static_cast<std::size_t>(frameIndex) <
                          track.frameList.size()) {
                        const detail::EmoteTimelineFrame24B &frame =
                            track.frameList[
                                static_cast<std::size_t>(frameIndex)];
                        if(!frame.typeZero) {
                            setVariable(track.label, frame.value, frame.time,
                                        frame.easingWeight);
                        }
                        frameIndex = static_cast<int32_t>(
                            static_cast<uint32_t>(frameIndex) + 1u);
                    }
                }
                ++trackIndex;
            }

            if((state.flags & 4u) != 0) {
                ++activeIndex;
            } else {
                _activeTimelineLabels.erase(
                    _activeTimelineLabels.begin() +
                    static_cast<std::ptrdiff_t>(activeIndex));
            }
        }
    }

    // Adds active, non-instant variable-track output into the supplied value.
    // The track product is rounded to float before being added to the double,
    // matching all four references. Timeline-state access is bounds-checked:
    // a stale active label propagates unordered_map::at failure without
    // materializing a default state.
    void EmoteEngine::accumulateTimelineContribution_guess(
        const ttstr &label, double &value) {
        for(const ttstr &timelineLabel : _activeTimelineLabels) {
            detail::EmoteTimelineState &state =
                _timelineStates.at(timelineLabel);
            if((state.flags & 2u) == 0) {
                continue;
            }
            for(const detail::EmoteTimelineTrack &track :
                state.timelineData->variableList) {
                if(track.instantVariable || track.frameList.empty()) {
                    continue;
                }
                if(track.label == label) {
                    value += static_cast<float>(
                        track.output * state.blendWeight);
                }
            }
        }
    }

    // Advances timeline windows before the per-controller progress loop. The
    // entry gate, active-vector erasure and timeline-state subscript behavior are shared by
    // all four current references.
    void EmoteEngine::preProgress_guess(bool force, double dt) {
        // Four-reference entry gate:
        //   if (dt != 0.0 || (force & 1) != 0) { ... }
        if(dt == 0.0 && !force) {
            return;
        }

        // The residual is shared across the ordered active-label scan. A loop
        // wrap consumes it, so later timelines receive only the remainder.
        double remaining = dt;
        std::size_t activeIndex = 0;
        while(activeIndex < _activeTimelineLabels.size()) {
            detail::EmoteTimelineState &state =
                _timelineStates[_activeTimelineLabels[activeIndex]];
            // Native branches on ordered >= 0. NaN therefore belongs to the
            // non-loop path together with negative loop markers.
            const bool nonLoop = !(state.loopBegin >= 0.0);
            if(nonLoop) {
                applyTimelineWindow_guess(
                    state, true, state.currentTime + remaining);
            } else {
                while(state.currentTime + remaining >= state.loopEnd) {
                    remaining -= state.loopEnd - state.currentTime;
                    applyTimelineWindow_guess(
                        state, false, state.loopEnd);
                    seekTimeline_guess(state, state.loopBegin);
                }
                applyTimelineWindow_guess(
                    state, true,
                    state.currentTime + std::fmax(remaining, 0.0));
            }

            if((state.flags & 2u) != 0) {
                const float step = static_cast<float>(remaining);
            EmoteVarController_step(
                    state.blendController.get(), &state.blendWeight, step);
                for(detail::EmoteTimelineTrack &track :
                    state.timelineData->variableList) {
                    if(!track.frameList.empty() && !track.instantVariable) {
                        EmoteVarController_step(
                            track.controller.get(), &track.output, step);
                    }
                }
            }

            // lastTime completion is exclusive to the non-loop path and is
            // tested before autoStop dereferences the blend owner.
            const bool reachedLastTime =
                nonLoop && state.lastTime <= state.currentTime;
            const bool blendFinished = !reachedLastTime &&
                state.autoStop != 0.0 &&
                state.blendController->state == 0 &&
                state.blendController->queue.empty();
            if(reachedLastTime || blendFinished) {
                _activeTimelineLabels.erase(
                    _activeTimelineLabels.begin() +
                    static_cast<std::ptrdiff_t>(activeIndex));
            } else {
                ++activeIndex;
            }
        }
    }

    tTJSVariant EmoteEngine::serializeTimelineState_guess() {
        detail::TJSArrayWithItems_guess result =
            detail::createTJSArrayWithItems_guess();
        for(const ttstr &label : _activeTimelineLabels) {
            // All four binaries use unordered_map::operator[] here. A stale
            // active label therefore materializes a default state before the
            // serializer dereferences its controller; it is not skipped.
            const detail::EmoteTimelineState &state = _timelineStates[label];
            ncbPropAccessor item(TJSCreateDictionaryObject(), false);
            (void)item.SetValue(
                TJS_W("label"), label, TJS_MEMBERENSURE,
                &engineLabelHint_guess);
            (void)item.SetValue(
                TJS_W("flags"), static_cast<tjs_int>(state.flags | 1u),
                TJS_MEMBERENSURE, &timelineInfoFlagsHint_guess);
            (void)item.SetValue(
                TJS_W("curTime"), state.currentTime, TJS_MEMBERENSURE,
                &timelineStateCurTimeHint_guess);
            (void)item.SetValue(
                TJS_W("blendRatioCtrl"),
                serializeVarControllerState_guess(
                    state.blendController.get()),
                TJS_MEMBERENSURE,
                &timelineStateBlendRatioCtrlHint_guess);
            (void)item.SetValue(
                TJS_W("stopWhenBlendDone"), state.autoStop,
                TJS_MEMBERENSURE,
                &timelineStateStopWhenBlendDoneHint_guess);
            iTJSDispatch2 *const dispatch = item.GetDispatch();
            result.items->emplace_back(dispatch, dispatch);
        }
        return result.value;
    }

    tTJSVariant EmoteEngine::serializeEyeState_guess() const {
        detail::TJSArrayWithItems_guess result =
            detail::createTJSArrayWithItems_guess();
        for(const EmoteEyeControlEntry_Deque4 &entry :
            _stateMachineDeque4) {
            ncbPropAccessor item(TJSCreateDictionaryObject(), false);
            serializeEyeControllerState(
                item, entry.label, entry.ctl.get());
            iTJSDispatch2 *const dispatch = item.GetDispatch();
            result.items->emplace_back(dispatch, dispatch);
        }
        return result.value;
    }

    tTJSVariant EmoteEngine::serializeEyebrowState_guess() const {
        detail::TJSArrayWithItems_guess result =
            detail::createTJSArrayWithItems_guess();
        for(const EmoteEyebrowControlEntry_Deque5 &entry :
            _stateMachineDeque5) {
            ncbPropAccessor item(TJSCreateDictionaryObject(), false);
            serializeEyebrowControllerState(
                item, entry.label, entry.ctl.get());
            iTJSDispatch2 *const dispatch = item.GetDispatch();
            result.items->emplace_back(dispatch, dispatch);
        }
        return result.value;
    }

    tTJSVariant EmoteEngine::serializeMouthState_guess() const {
        detail::TJSArrayWithItems_guess result =
            detail::createTJSArrayWithItems_guess();
        for(const EmoteMouthControlEntry_Deque6 &entry :
            _compositeVarDeque6) {
            ncbPropAccessor item(TJSCreateDictionaryObject(), false);
            serializeMouthControllerState(
                item, entry.label, entry.ctl.get());
            iTJSDispatch2 *const dispatch = item.GetDispatch();
            result.items->emplace_back(dispatch, dispatch);
        }
        return result.value;
    }

    tTJSVariant EmoteEngine::serializeTransitionState_guess() const {
        detail::TJSArrayWithItems_guess result =
            detail::createTJSArrayWithItems_guess();
        for(const EmoteTransitionControlEntry_Deque8 &entry : _auxVarDeque8) {
            tTJSVariant itemValue =
                serializeVarControllerState_guess(entry.ctl.get());
            itemValue.ToObject();
            ncbPropAccessor item(itemValue);
            itemValue.Clear();
            (void)item.SetValue(
                TJS_W("label"), entry.label, TJS_MEMBERENSURE,
                &engineLabelHint_guess);
            iTJSDispatch2 *const dispatch = item.GetDispatch();
            result.items->emplace_back(dispatch, dispatch);
        }
        return result.value;
    }

    // Saves only label/value/phase/speed/tick. The command deque, options,
    // entry gate and dormant targets vector are deliberately absent.
    tTJSVariant EmoteEngine::serializeSelectorState_guess() const {
        detail::TJSArrayWithItems_guess result =
            detail::createTJSArrayWithItems_guess();
        for(const EmoteSelectorControlEntry_Deque9 &entry :
            _vectorVarDeque9) {
            ncbPropAccessor item(TJSCreateDictionaryObject(), false);
            serializeSelectorControllerState(
                item, entry.label, entry.ctl.get());
            iTJSDispatch2 *const dispatch = item.GetDispatch();
            result.items->emplace_back(dispatch, dispatch);
        }
        return result.value;
    }

    tTJSVariant EmoteEngine::serializeBaseState_guess() const {
        ncbPropAccessor result(TJSCreateDictionaryObject(), false);
        (void)result.SetValue(
            TJS_W("coord"),
            serializeVarControllerState_guess(_ctlPosition.get()),
            TJS_MEMBERENSURE, &baseStateCoordHint_guess);
        (void)result.SetValue(
            TJS_W("scale"),
            serializeVarControllerState_guess(_ctlScale.get()),
            TJS_MEMBERENSURE, &baseStateScaleHint_guess);
        (void)result.SetValue(
            TJS_W("color"),
            serializeVarControllerState_guess(_ctlColor.get()),
            TJS_MEMBERENSURE, &baseStateColorHint_guess);
        (void)result.SetValue(
            TJS_W("rotate"),
            serializeAngleControllerState_guess(_ctlAngle.get()),
            TJS_MEMBERENSURE, &baseStateRotateHint_guess);

        iTJSDispatch2 *const dispatch = result.GetDispatch();
        return tTJSVariant(dispatch, dispatch);
    }

    // Literal key order and controller roles are common to all four binaries.
    tTJSVariant EmoteEngine::serializeOuterForceState_guess() const {
        ncbPropAccessor result(TJSCreateDictionaryObject(), false);
        (void)result.SetValue(
            TJS_W("bust"),
            serializeVarControllerState_guess(_ctlBustOuterForce.get()),
            TJS_MEMBERENSURE, &outerForceStateBustHint_guess);
        (void)result.SetValue(
            TJS_W("hair"),
            serializeVarControllerState_guess(_ctlHairOuterForce.get()),
            TJS_MEMBERENSURE, &outerForceStateHairHint_guess);
        (void)result.SetValue(
            TJS_W("parts"),
            serializeVarControllerState_guess(_ctlPartsOuterForce.get()),
            TJS_MEMBERENSURE, &outerForceStatePartsHint_guess);

        iTJSDispatch2 *const dispatch = result.GetDispatch();
        return tTJSVariant(dispatch, dispatch);
    }

    tTJSVariant EmoteEngine::serializeState_guess() {
        preProgress_guess(true, 0.0);

        for(EmoteEyeControlEntry_Deque4 &entry : _stateMachineDeque4) {
            float value;
            EmoteBlinkController_step(entry.ctl.get(), &value, 0.0f);
            _variableValues[entry.label] = value;
        }
        for(EmoteEyebrowControlEntry_Deque5 &entry : _stateMachineDeque5) {
            float value;
            EmoteEyebrowController_step(entry.ctl.get(), &value, 0.0f);
            _variableValues[entry.label] = value;
        }
        for(EmoteMouthControlEntry_Deque6 &entry : _compositeVarDeque6) {
            float mouth;
            float talk;
            EmoteMouthController_step(entry.ctl.get(), &mouth, &talk, 0.0f);
            _variableValues[entry.label] = mouth;
            _variableValues[entry.talkLabel] = talk;
        }
        for(EmoteSelectorControlEntry_Deque9 &entry : _vectorVarDeque9) {
            float value;
            EmoteSelectorController_step(entry.ctl.get(), &value, 0.0f);
            _variableValues[entry.label] = value;
        }
        for(EmoteTransitionControlEntry_Deque8 &entry : _auxVarDeque8) {
            float value;
            EmoteVarController_step(entry.ctl.get(), &value, 0.0f);
            _variableValues[entry.label] = value;
        }
        stepRootControllers_guess(0.0f);

        ncbPropAccessor result(TJSCreateDictionaryObject(), false);
        (void)result.SetValue(
            TJS_W("timeline"), serializeTimelineState_guess(),
            TJS_MEMBERENSURE, &engineStateTimelineHint_guess);
        (void)result.SetValue(
            TJS_W("eye"), serializeEyeState_guess(), TJS_MEMBERENSURE,
            &engineStateEyeHint_guess);
        (void)result.SetValue(
            TJS_W("eyebrow"), serializeEyebrowState_guess(),
            TJS_MEMBERENSURE, &engineStateEyebrowHint_guess);
        (void)result.SetValue(
            TJS_W("mouth"), serializeMouthState_guess(), TJS_MEMBERENSURE,
            &controllerMouthHint_guess);
        (void)result.SetValue(
            TJS_W("transition"), serializeTransitionState_guess(),
            TJS_MEMBERENSURE, &engineStateTransitionHint_guess);
        (void)result.SetValue(
            TJS_W("selector"), serializeSelectorState_guess(),
            TJS_MEMBERENSURE, &engineStateSelectorHint_guess);
        (void)result.SetValue(
            TJS_W("base"), serializeBaseState_guess(), TJS_MEMBERENSURE,
            &engineStateBaseHint_guess);
        (void)result.SetValue(
            TJS_W("outerforce"), serializeOuterForceState_guess(),
            TJS_MEMBERENSURE, &engineStateOuterForceHint_guess);

        iTJSDispatch2 *const dispatch = result.GetDispatch();
        return tTJSVariant(dispatch, dispatch);
    }

    void EmoteEngine::restoreTimelineState_guess(tTJSVariant value) {
        stopTimeline_guess(ttstr());
        tTJSArrayNI *array = tryGetTJSArrayNative(value);
        if(!array) {
            return;
        }
        for(const tTJSVariant &rawItem : array->Items) {
            if(rawItem.Type() != tvtObject) {
                continue;
            }

            tTJSVariant itemValue(rawItem);
            itemValue.ToObject();
            ncbPropAccessor item(itemValue);
            itemValue.Clear();

            ttstr label;
            if(!tryGetTJSStringProperty(item, TJS_W("label"), label,
                                        &engineLabelHint_guess)) {
                continue;
            }
            auto found = _timelineStates.find(label);
            if(found == _timelineStates.end()) {
                continue;
            }

            tjs_uint32 flags = 0;
            double curTime = 0.0;
            tTJSVariant field;
            if(tryGetTJSScalarProperty(
                    item, TJS_W("flags"), field,
                    &timelineInfoFlagsHint_guess)) {
                flags = static_cast<tjs_uint32>(field.AsInteger());
            }
            if(tryGetTJSScalarProperty(
                    item, TJS_W("curTime"), field,
                    &timelineStateCurTimeHint_guess)) {
                curTime = field.AsReal();
            }
            playTimeline_guess(label, flags);
            detail::EmoteTimelineState &state = found->second;
            applyTimelineWindow_guess(state, true, curTime);
            if(tryGetTJSScalarProperty(
                    item, TJS_W("stopWhenBlendDone"), field,
                    &timelineStateStopWhenBlendDoneHint_guess)) {
                state.autoStop = field.AsReal();
            }
            if(tryGetTJSProperty(
                    item, TJS_W("blendRatioCtrl"), value,
                    &timelineStateBlendRatioCtrlHint_guess)) {
                restoreVarControllerState_guess(
                    state.blendController.get(), value);
            }
        }
    }

    void EmoteEngine::restoreEyeState_guess(const tTJSVariant &value) {
        tTJSArrayNI *array = tryGetTJSArrayNative(value);
        if(!array) {
            return;
        }
        for(const tTJSVariant &rawItem : array->Items) {
            if(rawItem.Type() != tvtObject) {
                continue;
            }

            tTJSVariant itemValue(rawItem);
            itemValue.ToObject();
            ncbPropAccessor item(itemValue);
            itemValue.Clear();

            ttstr label;
            if(!tryGetTJSStringProperty(item, TJS_W("label"), label,
                                        &engineLabelHint_guess)) {
                continue;
            }
            const auto found = std::find_if(
                _stateMachineDeque4.begin(), _stateMachineDeque4.end(),
                [&label](const EmoteEyeControlEntry_Deque4 &entry) {
                    return entry.label == label;
                });
            // Eye is the sole controller category whose four native restore
            // functions compare the result with end() before dereferencing.
            if(found != _stateMachineDeque4.end()) {
                restoreEyeControllerState_guess(found->ctl.get(), rawItem);
            }
        }
    }

    void EmoteEngine::restoreEyebrowState_guess(
        const tTJSVariant &value) {
        tTJSArrayNI *array = tryGetTJSArrayNative(value);
        if(!array) {
            return;
        }
        for(const tTJSVariant &rawItem : array->Items) {
            if(rawItem.Type() != tvtObject) {
                continue;
            }

            tTJSVariant itemValue(rawItem);
            itemValue.ToObject();
            ncbPropAccessor item(itemValue);
            itemValue.Clear();

            ttstr label;
            if(!tryGetTJSStringProperty(item, TJS_W("label"), label,
                                        &engineLabelHint_guess)) {
                continue;
            }
            const auto found = std::find_if(
                _stateMachineDeque5.begin(), _stateMachineDeque5.end(),
                [&label](const EmoteEyebrowControlEntry_Deque5 &entry) {
                    return entry.label == label;
                });
            // Unlike Eye, all four binaries dereference the result even when
            // it equals end(); an unknown label therefore reaches native UB.
            restoreEyebrowControllerState_guess(found->ctl.get(), rawItem);
        }
    }

    void EmoteEngine::restoreMouthState_guess(const tTJSVariant &value) {
        tTJSArrayNI *array = tryGetTJSArrayNative(value);
        if(!array) {
            return;
        }
        for(const tTJSVariant &rawItem : array->Items) {
            if(rawItem.Type() != tvtObject) {
                continue;
            }

            tTJSVariant itemValue(rawItem);
            itemValue.ToObject();
            ncbPropAccessor item(itemValue);
            itemValue.Clear();

            ttstr label;
            if(!tryGetTJSStringProperty(item, TJS_W("label"), label,
                                        &engineLabelHint_guess)) {
                continue;
            }
            const auto found = std::find_if(
                _compositeVarDeque6.begin(), _compositeVarDeque6.end(),
                [&label](const EmoteMouthControlEntry_Deque6 &entry) {
                    return entry.label == label;
                });
            // Like Eyebrow, Transition and Selector, an unknown label reaches
            // an unchecked end-iterator dereference in every reference build.
            restoreMouthControllerState_guess(found->ctl.get(), rawItem);
        }
    }

    void EmoteEngine::restoreTransitionState_guess(
        const tTJSVariant &value) {
        tTJSArrayNI *array = tryGetTJSArrayNative(value);
        if(!array) {
            return;
        }
        for(const tTJSVariant &rawItem : array->Items) {
            if(rawItem.Type() != tvtObject) {
                continue;
            }

            tTJSVariant itemValue(rawItem);
            itemValue.ToObject();
            ncbPropAccessor item(itemValue);
            itemValue.Clear();

            ttstr label;
            if(!tryGetTJSStringProperty(item, TJS_W("label"), label,
                                        &engineLabelHint_guess)) {
                continue;
            }
            const auto found = std::find_if(
                _auxVarDeque8.begin(), _auxVarDeque8.end(),
                [&label](const EmoteTransitionControlEntry_Deque8 &entry) {
                    return entry.label == label;
                });
            // Transition preserves the same unchecked unknown-label boundary.
            restoreVarControllerState_guess(found->ctl.get(), rawItem);
        }
    }

    void EmoteEngine::restoreSelectorState_guess(
        const tTJSVariant &value) {
        tTJSArrayNI *array = tryGetTJSArrayNative(value);
        if(!array) {
            return;
        }
        for(const tTJSVariant &rawItem : array->Items) {
            if(rawItem.Type() != tvtObject) {
                continue;
            }

            tTJSVariant itemValue(rawItem);
            itemValue.ToObject();
            ncbPropAccessor item(itemValue);
            itemValue.Clear();

            ttstr label;
            if(!tryGetTJSStringProperty(item, TJS_W("label"), label,
                                        &engineLabelHint_guess)) {
                continue;
            }
            const auto found = std::find_if(
                _vectorVarDeque9.begin(), _vectorVarDeque9.end(),
                [&label](const EmoteSelectorControlEntry_Deque9 &entry) {
                    return entry.label == label;
                });
            // The four binaries unconditionally dereference the search result.
            // Unknown labels therefore hit native end-iterator UB; do not turn
            // that boundary into a silent skip.
            restoreSelectorControllerState_guess(found->ctl.get(), rawItem);
        }
    }

    void EmoteEngine::restoreBaseState_guess(const tTJSVariant &value) {
        if(value.Type() != tvtObject) {
            return;
        }

        tTJSVariant objectValue(value);
        objectValue.ToObject();
        ncbPropAccessor object(objectValue);
        objectValue.Clear();

        restoreVarControllerState_guess(
            _ctlPosition.get(), object.GetValue(
                TJS_W("coord"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &baseStateCoordHint_guess));
        restoreVarControllerState_guess(
            _ctlScale.get(), object.GetValue(
                TJS_W("scale"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &baseStateScaleHint_guess));
        restoreVarControllerState_guess(
            _ctlColor.get(), object.GetValue(
                TJS_W("color"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &baseStateColorHint_guess));
        restoreAngleControllerState_guess(
            _ctlAngle.get(), object.GetValue(
                TJS_W("rotate"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &baseStateRotateHint_guess));
    }

    void EmoteEngine::restoreOuterForceState_guess(
        const tTJSVariant &value) {
        if(value.Type() != tvtObject) {
            return;
        }

        tTJSVariant objectValue(value);
        objectValue.ToObject();
        ncbPropAccessor object(objectValue);
        objectValue.Clear();

        restoreVarControllerState_guess(
            _ctlBustOuterForce.get(),
            object.GetValue(
                TJS_W("bust"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &outerForceStateBustHint_guess));
        restoreVarControllerState_guess(
            _ctlHairOuterForce.get(),
            object.GetValue(
                TJS_W("hair"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &outerForceStateHairHint_guess));
        restoreVarControllerState_guess(
            _ctlPartsOuterForce.get(),
            object.GetValue(
                TJS_W("parts"), ncbTypedefs::Tag<tTJSVariant>(), 0,
                &outerForceStatePartsHint_guess));
    }

    void EmoteEngine::unserializeState_guess(tTJSVariant data) {
        data.ToObject();
        iTJSDispatch2 *dispatch = data.AsObject();
        data.Clear();

        const auto getChild = [dispatch](const tjs_char *name,
                                         tjs_uint32 *hint = nullptr) {
            tTJSVariant value;
            (void)dispatch->PropGet(0, name, hint, &value, dispatch);
            return value;
        };
        try {
            restoreTimelineState_guess(getChild(
                TJS_W("timeline"), &engineStateTimelineHint_guess));
            restoreEyeState_guess(getChild(
                TJS_W("eye"), &engineStateEyeHint_guess));
            restoreEyebrowState_guess(getChild(
                TJS_W("eyebrow"), &engineStateEyebrowHint_guess));
            restoreMouthState_guess(getChild(
                TJS_W("mouth"), &controllerMouthHint_guess));
            restoreTransitionState_guess(getChild(
                TJS_W("transition"), &engineStateTransitionHint_guess));
            restoreSelectorState_guess(getChild(
                TJS_W("selector"), &engineStateSelectorHint_guess));
            restoreBaseState_guess(getChild(
                TJS_W("base"), &engineStateBaseHint_guess));
            restoreOuterForceState_guess(getChild(
                TJS_W("outerforce"), &engineStateOuterForceHint_guess));
        } catch(...) {
            dispatch->Release();
            throw;
        }
        dispatch->Release();
    }

    void EmoteEngine::progress(double dt) {
        // The shared Engine core is entered unconditionally by Motion.EmotePlayer
        // after its millisecond conversion. Only the D3D shell has a dt==0 gate.
        // Keep the original double for the Player bridge and physics tail while
        // draining a separate working copy through float controller slices.
        const double originalDt = dt;

        preProgress_guess(false, originalDt);

        // Four-reference dt-slice loop. Positive remaining time guarantees a
        // slice; zero/negative remaining time still runs while dirty is set.
        // Each iteration clears dirty before stepping, selects
        // std::min(remaining, 1.1) with the live remaining interval as the
        // first operand, subtracts that same double slice afterward, and
        // repeats if either condition became true again.
        while (dt > 0.0 || _dirty) {
            const double slice =
                internal::controllerSliceTime_guess(dt);
            const float step = static_cast<float>(slice);
            _dirty = false;

            // Six active controller deques are iterated per slice. Each stores
            // an owned controller pointer plus its category-specific output
            // labels. The loop calls the per-controller step, then upserts the
            // result into the variable-value map under those labels:
            //     #4 Blink step, #5 Eyebrow step, #6 Mouth step,
            //     #8 Var step, #9 Selector step, #10 inline curve lookup.
            // Population comes from metadata builders. setVariable does not
            // push deque entries; it looks up an existing EmoteVarRef, reads
            // its category tag and stored deque index, and for type 4 indexes
            // into the already-built eye deque to drive the controller. The initial
            //   builders allocate controllers, push their typed records and
            //   register HM entries. applyMetadata_guess invokes these
            //   builders in category order, so these deques are live. Restore
            //   paths reload saved state into the already-built controllers.
            //
            // Deque#4 (eye) step. All four references call the blink step for
            // each {ctl,label} entry, then upsert the scalar result into the
            // variable-value map.
            for (EmoteEyeControlEntry_Deque4& entry : _stateMachineDeque4) {
                float out = 0.0f;
                EmoteBlinkController_step(entry.ctl.get(), &out, step);
                _variableValues[entry.label] = out;
            }
            // Deque#5 (eyebrow) step: for every {ctl,label}, step the slim
            // controller and upsert its scalar result into the variable-value
            // map under label.
            for (EmoteEyebrowControlEntry_Deque5& entry : _stateMachineDeque5) {
                float out = 0.0f;
                EmoteEyebrowController_step(entry.ctl.get(), &out, step);
                _variableValues[entry.label] = out;
            }
            // Deque#6 (mouth) step. Each {ctl,label,talkLabel} entry publishes
            // two outputs; it is the only metadata-controller deque that feeds
            // two variable-value keys per element.
            for (EmoteMouthControlEntry_Deque6& entry : _compositeVarDeque6) {
                float outBeginFrame   = 0.0f;
                float outCurrentValue = 0.0f;
                EmoteMouthController_step(entry.ctl.get(), &outBeginFrame,
                                          &outCurrentValue, step);
                _variableValues[entry.label]     = outBeginFrame;
                _variableValues[entry.talkLabel] = outCurrentValue;
            }
            // All four engines step selectors before transitions. The selector
            // output is the selected option index as a float and is published to
            // the variable-value map under the entry label.
            for (EmoteSelectorControlEntry_Deque9& entry : _vectorVarDeque9) {
                float out = 0.0f;
                EmoteSelectorController_step(entry.ctl.get(), &out, step);
                _variableValues[entry.label] = out;
            }
            // Transition controllers are scalar. Their direct-write gate is not
            // inspected by the per-frame step; it only controls external writes.
            for (EmoteTransitionControlEntry_Deque8& entry : _auxVarDeque8) {
                float out = 0.0f;
                EmoteVarController_step(entry.ctl.get(), &out, step);
                _variableValues[entry.label] = out;
            }
            // Loop controllers run last among the metadata controller deques.
            // The float result is widened to double by the variable-value-map
            // assignment.
            for (EmoteLoopControlEntry_Deque10& entry : _lookupCurvesDeque10) {
                float out = 0.0f;
                EmoteLoopController_step_guess(entry.ctl.get(), &out, step);
                _variableValues[entry.label] = out;
            }

            // Apply the 4 direct controllers (pos/scale/color/angle).
            stepRootControllers_guess(step);

            // Wind consumes the same clamped per-slice delta as the controllers.
            if (_windEmitter && _windEmitter->gate) {
                _windEmitter->step(step);
            }

            dt -= slice;
        }

        // Post-loop variable-value bind: add active timeline contributions in
        // place, apply cached mirror classification, then publish to the
        // Player's two query
        // maps. libstdc++ and libc++ expose different unordered-map iteration
        // orders, but every iteration writes a distinct label, so this boundary
        // has no inter-label result dependency.
        Player& p = player();
        for (auto& kv : _variableValues) {
            const ttstr& label = kv.first;
            double& value = kv.second;
            accumulateTimelineContribution_guess(label, value);

            const double accumulated = value;
            const bool negate = shouldMirrorLabel_guess(label);
            p.bindParameterValue_guess(label, 0, negate ? -accumulated
                                                        : accumulated);
        }

        // Clamp controls run once after the ordinary variable-value bind and
        // before Player progress. They read the Engine variable-value map again
        // rather than the values just bound
        // into the Player.
        applyClampControls_guess();

        // Engine passes the original frame-unit delta exactly once to Player;
        // millisecond conversion belongs only to the script-facing wrapper.
        player().progressFrames_guess(nullptr, originalDt);

        // The four references gate the physics-only pass on the original delta
        // and direct-edit state, then cast that original delta to float once.
        // The controller-slice loop has already drained its local `dt`, so it
        // must not be used for this pass.
        if (originalDt != 0.0 && !_directEdit) {
            // Reuse the single native-shaped double-to-float conversion.
            const float physDt = static_cast<float>(originalDt);

            // Step the three physics-target controllers into an intentionally
            // unused scratch sink. Their state, rather than this output buffer,
            // feeds the following spring passes.
            float scratch[8] = {};
            EmoteVarController_step(_ctlBustOuterForce.get(),  scratch, physDt);
            EmoteVarController_step(_ctlHairOuterForce.get(),  scratch, physDt);
            EmoteVarController_step(_ctlPartsOuterForce.get(), scratch, physDt);

            // Metadata builders populate the three spring-node deques. Empty
            // control metadata naturally leaves these passes inert.
            stepHairParts(physDt);
            stepBust(_ctlHairOuterForce.get(), _bustChain1Nodes,
                     _hairScale, physDt);
            stepBust(_ctlPartsOuterForce.get(), _bustChain2Nodes,
                     _partsScale, physDt);
        }
    }

} // namespace motion
