// PlayerFrameProgress.cpp — frameProgress timeline/control stepping
// Split from PlayerRender.cpp for maintainability.
//
#include "PlayerInternal.h"
#include "EmotePlayer.h" // for EmoteEngine (back-pointer deref)
#include "MotionDispatch.h"
#include "MotionTraceWeb.h"
#include "ncbind.hpp"

using namespace motion::internal;

namespace {
    tjs_int signedIndexFromBits32_guess(std::uint32_t bits) noexcept {
        static_assert(sizeof(tjs_int) == sizeof(bits),
                      "motion numeric properties require a 32-bit tjs_int");
        tjs_int value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    tjs_int addIndexWrapping32_guess(
        tjs_int value, std::uint32_t delta) noexcept {
        return signedIndexFromBits32_guess(
            static_cast<std::uint32_t>(value) + delta);
    }

    bool binary64IsNaNBits_guess(double value) noexcept {
        static_assert(sizeof(double) == sizeof(std::uint64_t));
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & UINT64_C(0x7FF0000000000000)) ==
                   UINT64_C(0x7FF0000000000000) &&
               (bits & UINT64_C(0x000FFFFFFFFFFFFF)) != 0;
    }

    bool vfpUnorderedOrGreaterEqual_guess(
        double left, double right) noexcept {
        return binary64IsNaNBits_guess(left) ||
               binary64IsNaNBits_guess(right) || left >= right;
    }

    bool vfpUnorderedOrLessEqual_guess(
        double left, double right) noexcept {
        return binary64IsNaNBits_guess(left) ||
               binary64IsNaNBits_guess(right) || left <= right;
    }

} // anonymous namespace

namespace motion {
namespace internal {
    double evaluateVariableTrackEasing_guess(
        const tTJSVariant &easing, double t) {
        // One retained root accessor owns both named reads. The resulting x/y
        // Variants then back two retained array accessors through every count
        // and numeric read. The x/y hints are the same process-wide slots used
        // by point dictionaries, LayerGetter vertices, and camera offsets.
        ncbPropAccessor easingObject{tTJSVariant(easing)};
        const tTJSVariant x = easingObject.GetValue(
            TJS_W("x"), ncbTypedefs::Tag<tTJSVariant>(), 0,
            &detail::xMemberHint_guess);
        const tTJSVariant y = easingObject.GetValue(
            TJS_W("y"), ncbTypedefs::Tag<tTJSVariant>(), 0,
            &detail::yMemberHint_guess);
        ncbPropAccessor xObject{tTJSVariant(x)};
        ncbPropAccessor yObject{tTJSVariant(y)};

        const tjs_int count = xObject.GetArrayCount();
        const tjs_real firstX = xObject.GetValue(
            0, ncbTypedefs::Tag<tjs_real>(), 0);
        // The four ARM products branch with PL after their floating compare:
        // unordered therefore selects the first endpoint as well as >=.
        if(vfpUnorderedOrGreaterEqual_guess(firstX, t)) {
            return yObject.GetValue(
                0, ncbTypedefs::Tag<tjs_real>(), 0);
        }
        const tjs_int last = addIndexWrapping32_guess(
            count, UINT32_MAX);
        const tjs_real lastX = xObject.GetValue(
            last, ncbTypedefs::Tag<tjs_real>(), 0);
        // LE also accepts the unordered condition in each reference.
        if(vfpUnorderedOrLessEqual_guess(lastX, t)) {
            return yObject.GetValue(
                last, ncbTypedefs::Tag<tjs_real>(), 0);
        }

        tjs_int segmentEnd = 0;
        do {
            segmentEnd = addIndexWrapping32_guess(segmentEnd, 3);
        } while(xObject.GetValue(
                    segmentEnd, ncbTypedefs::Tag<tjs_real>(), 0) < t);

        double values[4];
        tjs_int index = addIndexWrapping32_guess(
            segmentEnd, UINT32_MAX - 2);
        for(int i = 0; i < 4; ++i) {
            // The x read is intentionally retained even though only y feeds
            // the polynomial; dynamic dispatch makes the read observable.
            (void)xObject.GetValue(
                index, ncbTypedefs::Tag<tjs_real>(), 0);
            values[i] = yObject.GetValue(
                index, ncbTypedefs::Tag<tjs_real>(), 0);
            index = addIndexWrapping32_guess(index, 1);
        }
        const double oneMinus = 1.0 - t;
        return oneMinus * (oneMinus * oneMinus) * values[0] +
            oneMinus * (oneMinus * 3.0) * t * values[1] +
            oneMinus * 3.0 * t * t * values[2] +
            t * t * t * values[3];
    }

    // Out-of-line in every reference and shared by incremental and absolute
    // variable-track cursor paths. The retained source accessor outlives the
    // indexed getter, and the indexed result directly backs the frame accessor.
    void stepVariableTrackSlot_guess(
        detail::VarTrackSlot &slot, const tTJSVariant &frameSource,
        std::uint32_t index) {
        slot.frameIndex = index;
        ncbPropAccessor frameSourceObject{tTJSVariant(frameSource)};
        ncbPropAccessor frameObject{frameSourceObject.GetValue(
            signedIndexFromBits32_guess(index),
            ncbTypedefs::Tag<tTJSVariant>(), 0)};
        slot.time = frameObject.GetValue(
            TJS_W("time"), ncbTypedefs::Tag<tjs_real>(), 0);
        slot.merged = false;
    }

    void mergeVariableTrackSlot_guess(
        detail::VarTrackSlot &slot, const tTJSVariant &frameSource) {
        slot.merged = true;
        ncbPropAccessor frameSourceObject{tTJSVariant(frameSource)};
        ncbPropAccessor frameObject{frameSourceObject.GetValue(
            signedIndexFromBits32_guess(slot.frameIndex),
            ncbTypedefs::Tag<tTJSVariant>(), 0)};
        const tjs_int type = frameObject.GetValue(
            TJS_W("type"), ncbTypedefs::Tag<tjs_int>(), 0);
        if(type == 0) {
            slot.typeZeroFlag = true;
            return;
        }
        slot.typeZeroFlag = false;
        if(type == 2) {
            slot.interpFlag = 0;
        } else if(type == 3) {
            slot.interpFlag = 1;
        }

        // The content accessor remains alive through the later frame-level
        // easing read and assignment; normal teardown is content, frame, root.
        ncbPropAccessor contentObject{frameObject.GetValue(
            TJS_W("content"), ncbTypedefs::Tag<tTJSVariant>(), 0)};
        slot.interval = static_cast<std::uint32_t>(contentObject.GetValue(
            TJS_W("interval"), ncbTypedefs::Tag<tjs_int>(), 0));
        slot.value = contentObject.GetValue(
            TJS_W("value"), ncbTypedefs::Tag<tjs_real>(), 0);
        slot.easing = frameObject.GetValue(
            TJS_W("easing"), ncbTypedefs::Tag<tTJSVariant>(), 0);
    }
} // namespace internal

namespace {
    int rawFrameCount(const tTJSVariant &frames) {
        return detail::motionPropGetCount(frames);
    }

    tTJSVariant rawFrameAt(const tTJSVariant &frames, int index) {
        return detail::motionPropGetByNum(frames, index);
    }

    double rawFrameTime(const tTJSVariant &frame) {
        return detail::motionPropGetDouble(frame, TJS_W("time"));
    }

    // The native clamp arithmetic is performed in a 32-bit register. Rebuild
    // that modulo-2^32 subtraction explicitly so malformed dynamic counts do
    // not enter C++ signed-overflow undefined behavior.
    int subtractTwoWrapping32_guess(int value) noexcept {
        const std::uint32_t bits =
            static_cast<std::uint32_t>(value) - UINT32_C(2);
        return static_cast<int>(signedIndexFromBits32_guess(bits));
    }

    int incrementWrapping32_guess(int value) noexcept {
        const std::uint32_t bits =
            static_cast<std::uint32_t>(value) + UINT32_C(1);
        return signedIndexFromBits32_guess(bits);
    }

    int decrementWrapping32_guess(int value) noexcept {
        const std::uint32_t bits =
            static_cast<std::uint32_t>(value) - UINT32_C(1);
        return signedIndexFromBits32_guess(bits);
    }

    int rawFrameType(const tTJSVariant &frame) {
        return detail::motionPropGetInt(frame, TJS_W("type"));
    }

    tTJSVariant rawFrameContent(const tTJSVariant &frame) {
        return detail::motionPropGet(frame, TJS_W("content"));
    }
} // anonymous namespace

    void detail::PerNodeLayerState::
        invalidateRetainedChildrenBeforeClear_guess() {
        // The native code tests only the Variant tag before making the virtual
        // call. It passes Object (not ObjThis) as both the dispatch and objthis.
        if(childPlayerSnapshot.Type() == tvtObject) {
            iTJSDispatch2 *object = childPlayerSnapshot.AsObjectNoAddRef();
            (void)object->Invalidate(0, nullptr, nullptr, object);
        }

        // Every non-void value is converted to an object and read through the
        // ordinary count/PropGetByNum dispatch path. Thus a malformed non-object
        // value keeps the reference implementation's conversion exception.
        if(particleArraySnapshot.Type() != tvtVoid) {
            const tTJSVariant particleArray = particleArraySnapshot;
            const tjs_int count = detail::motionPropGetCount(particleArray);
            for(tjs_int index = 0; index < count; ++index) {
                const tTJSVariant child =
                    detail::motionPropGetByNum(particleArray, index);
                if(child.Type() == tvtObject) {
                    iTJSDispatch2 *object = child.AsObjectNoAddRef();
                    (void)object->Invalidate(0, nullptr, nullptr, object);
                }
            }
        }
    }

    void Player::clearJoinSnapshotMaps_guess() {
        // Native order is observable: invalidate every HM3 retained child,
        // destroy HM4, then destroy HM3. Do not fold the invalidation into the
        // value destructor: matched entries are erased on the restore path and
        // have already transferred/cleared their child Variants.
        for(auto &entry : _perNodeLayerStateMap) {
            entry.second.invalidateRetainedChildrenBeforeClear_guess();
        }
        _variableSnapshotMap.clear();
        _perNodeLayerStateMap.clear();
    }

    void Player::advanceLayerEventStreamPhase_guess(
        const tTJSVariant &frames) {
        const int count = rawFrameCount(frames);
        if(count >= 1) {
            const int limit = subtractTwoWrapping32_guess(count);
            while(_layerFrameCursor < limit) {
                if(_clampedEvalTime < _layerNextTime) break;
                _layerFrameCursor =
                    incrementWrapping32_guess(_layerFrameCursor);
                const auto frame = rawFrameAt(frames, _layerFrameCursor);
                _layerCurTime = rawFrameTime(frame);
                _layerNextTime = rawFrameTime(
                    rawFrameAt(frames, incrementWrapping32_guess(
                        _layerFrameCursor)));
                if(rawFrameType(frame) != 1) continue;

                const auto content = rawFrameContent(frame);
                if(_syncActive) {
                    if(detail::motionPropGetBool(content, TJS_W("align"))) {
                        _motionCompleted = true;
                        _clampedEvalTime = _layerCurTime;
                        _frameTickCount = _layerCurTime;
                    }
                    if(_syncActive && detail::motionPropGetBool(
                           content, TJS_W("sync"))) {
                        _syncWaiting = true;
                        _clampedEvalTime = _layerCurTime;
                        _frameTickCount = _layerCurTime;
                        enqueueSyncEvent_guess();
                    }
                }
                const auto action = detail::motionPropGetString(
                    content, TJS_W("action"));
                if(!action.IsEmpty()) {
                    enqueueActionEvent_guess(tTJSVariant(), action);
                }
            }
        }
    }

    void Player::rewindLayerEventStreamPhase_guess(
        const tTJSVariant &frames) {
        const int count = rawFrameCount(frames);
        if(count != 0 && _layerCurTime > _clampedEvalTime) {
            do {
                _layerFrameCursor =
                    decrementWrapping32_guess(_layerFrameCursor);
                const auto frame = rawFrameAt(frames, _layerFrameCursor);
                _layerCurTime = rawFrameTime(frame);
                _layerNextTime = rawFrameTime(
                    rawFrameAt(frames, incrementWrapping32_guess(
                        _layerFrameCursor)));
                if(rawFrameType(frame) == 1) {
                    const auto content = rawFrameContent(frame);
                    if(_syncActive) {
                        if(detail::motionPropGetBool(content, TJS_W("align"))) {
                            _motionCompleted = true;
                            _clampedEvalTime = _layerCurTime;
                            _frameTickCount = _layerCurTime;
                        }
                        if(_syncActive && detail::motionPropGetBool(
                               content, TJS_W("sync"))) {
                            _syncWaiting = true;
                            _clampedEvalTime = _layerCurTime;
                            _frameTickCount = _layerCurTime;
                            enqueueSyncEvent_guess();
                        }
                    }
                    const auto action = detail::motionPropGetString(
                        content, TJS_W("action"));
                    if(!action.IsEmpty()) {
                        enqueueActionEvent_guess(tTJSVariant(), action);
                    }
                }
            } while(_layerCurTime > _clampedEvalTime);
        }
    }

    void Player::advanceRootContentStreamPhase_guess(
        const tTJSVariant &frames) {
        const int count = rawFrameCount(frames);
        const int limit = subtractTwoWrapping32_guess(count);
        while(_rootFrameCursor < limit) {
            if(_clampedEvalTime < _rootNextTime) break;
            _rootFrameCursor = incrementWrapping32_guess(_rootFrameCursor);
            _rootContentVariant = rawFrameContent(
                rawFrameAt(frames, _rootFrameCursor));
            _rootCurTime = _rootNextTime;
            _rootNextTime = rawFrameTime(
                rawFrameAt(frames, incrementWrapping32_guess(
                    _rootFrameCursor)));
        }
    }

    void Player::rewindRootContentStreamPhase_guess(
        const tTJSVariant &frames) {
        if(_rootCurTime > _clampedEvalTime) {
            do {
                _rootFrameCursor =
                    decrementWrapping32_guess(_rootFrameCursor);
                const auto frame = rawFrameAt(frames, _rootFrameCursor);
                _rootContentVariant = rawFrameContent(frame);
                _rootNextTime = _rootCurTime;
                _rootCurTime = rawFrameTime(frame);
            } while(_rootCurTime > _clampedEvalTime);
        }
    }

    void Player::advanceVariableTracksPhase_guess() {
        // Forward stream ③. Each item owns its persistent raw frame Variant,
        // while this path makes one additional owner only for the dynamic
        // count read. Step/merge deliberately re-read the persistent field.

        for (auto &item : _variableLabelScopes) {
            const tTJSVariant frameSourceOwner = item.frameSource;
            const int count = detail::motionPropGetCount(frameSourceOwner);

            const int cursor = item.activeSlotCursor;
            detail::VarTrackSlot *active = &item.slot[cursor];
            detail::VarTrackSlot *other =
                &item.slot[(cursor & 1) == 0];
            const int limit = subtractTwoWrapping32_guess(count);
            while (signedIndexFromBits32_guess(active->frameIndex) < limit) {
                // Native spells this as an ordered less-than break. In
                // particular, an unordered/NaN Player cursor does not break.
                if(_clampedEvalTime < other->time) break;
                const std::uint32_t nextIdx = other->frameIndex + 1;
                item.activeSlotCursor =
                    (item.activeSlotCursor & 1) == 0;
                stepVariableTrackSlot_guess(
                    *active, item.frameSource, nextIdx);
                detail::VarTrackSlot *tmp = active;
                active = other;
                other = tmp;
            }
            // Four-reference boundary: inspect both merged flags, but pass
            // slot[0] to merge in both branches. Do not correct the second call
            // to slot[1]; backward and absolute paths deliberately differ.
            if (!item.slot[0].merged) {
                mergeVariableTrackSlot_guess(
                    item.slot[0], item.frameSource);
            }
            if (!item.slot[1].merged) {
                mergeVariableTrackSlot_guess(
                    item.slot[0], item.frameSource);
            }
        }
    }

    void Player::rewindVariableTracksPhase_guess() {
        // Reverse stream ③. Unlike the forward path, its post-loop merge walks
        // the two physical slots and therefore really writes slot[0], slot[1].
        // It creates no per-item frame-source owner.

        for (auto &item : _variableLabelScopes) {
            const int cursor = item.activeSlotCursor;
            detail::VarTrackSlot *active = &item.slot[cursor];
            detail::VarTrackSlot *other =
                &item.slot[(cursor & 1) == 0];
            // The decrement is intentionally unsigned. Invalid cursor state can
            // underflow and is passed verbatim to PropGetByNum.
            while (active->time > _clampedEvalTime) {
                const std::uint32_t prevIdx = active->frameIndex - 1u;
                item.activeSlotCursor =
                    (item.activeSlotCursor & 1) == 0;
                stepVariableTrackSlot_guess(
                    *other, item.frameSource, prevIdx);
                detail::VarTrackSlot *tmp = active;
                active = other;
                other = tmp;
            }
            if (!item.slot[0].merged) {
                mergeVariableTrackSlot_guess(
                    item.slot[0], item.frameSource);
            }
            if (!item.slot[1].merged) {
                mergeVariableTrackSlot_guess(
                    item.slot[1], item.frameSource);
            }
        }
    }

    void Player::reseedVariableTracks_guess() {
        // Absolute re-seed: scan to the last frame not greater than the target,
        // clamp against count-2, rebuild both slots and reset cursor parity.

        for (auto &item : _variableLabelScopes) {
            // Each iteration retains an independent source Variant before its
            // first dynamic dispatch and releases it only after both slot
            // merges and the cursor reset. A re-entrant getter may clear the
            // persistent field without invalidating the rest of this pass.
            const tTJSVariant frameSource = item.frameSource;
            const int count = detail::motionPropGetCount(frameSource);

            int index = 0;
            if(count >= 1) {
                for(index = 0; index < count; ++index) {
                    const tTJSVariant frame = detail::motionPropGetByNum(
                        frameSource, index);
                    const double frameTime = detail::motionPropGetDouble(
                        frame, TJS_W("time"));
                    // Match the inlined native phase: reload the Player field
                    // after every dynamic time getter instead of snapshotting
                    // one value before the variable-track walk.
                    const double evaluationTime = _clampedEvalTime;
                    if(frameTime == evaluationTime) {
                        break;
                    }
                    if(frameTime <= evaluationTime) {
                        continue;
                    }
                    --index;
                    break;
                }
            }
            // count 0/1 deliberately produces a negative seed. At INT_MIN and
            // INT_MIN+1 the native 32-bit subtraction wraps positive before
            // the signed min comparison. The chosen bit pattern is then
            // forwarded through the numeric-property ABI as uint32_t.
            const int seedIndex = std::min(
                index, subtractTwoWrapping32_guess(count));
            stepVariableTrackSlot_guess(
                item.slot[0], frameSource,
                static_cast<std::uint32_t>(seedIndex));
            mergeVariableTrackSlot_guess(
                item.slot[0], frameSource);
            stepVariableTrackSlot_guess(
                item.slot[1], frameSource,
                static_cast<std::uint32_t>(seedIndex) + UINT32_C(1));
            mergeVariableTrackSlot_guess(
                item.slot[1], frameSource);
            item.activeSlotCursor = 0;
        }
    }

    // Non-incremental full reseek. It rescans the layer and root streams from
    // scratch, absolutely seeds both variable-track slots and every non-root
    // node timeline, restores/prunes HM4 and HM3, then rebuilds every HM1
    // entry's cached child-node matches. The layer scan intentionally performs
    // a double increment when time < target; the root and variable scans do not.
    void Player::reseekTimelineCursors() {
        // ---- STEP 1: LAYER coarse scan over the persistent motion["tag"] owner ----
        // This owner remains live through the entire full-reseek tail. A
        // re-entrant getter may clear the Player field without invalidating any
        // later phase or changing the final reverse-destruction order.
        const tTJSVariant tagFrames = _tagFrameSourceVariant;
        const int tagCount = rawFrameCount(tagFrames);

        if(tagCount >= 1) {
            int index = 0;
            for(; index < tagCount; ++index) {
                const tTJSVariant frame = rawFrameAt(tagFrames, index);
                const double frameTime = rawFrameTime(frame);
                // The native member has no target argument. It reloads the
                // Player field after the script time getter, then reuses that
                // value for both ordered comparisons.
                const double evaluationTime = _clampedEvalTime;
                if(frameTime <= evaluationTime) {
                    if(frameTime < evaluationTime) {
                        // The for-loop update supplies the second increment.
                        ++index;
                        continue;
                    }
                } else {
                    --index;
                }
                break;
            }

            _layerFrameCursor = std::min(index, tagCount - 2);
            // Both cached times intentionally round-trip through int.
            const tTJSVariant current = rawFrameAt(
                tagFrames, _layerFrameCursor);
            _layerCurTime = static_cast<double>(
                detail::motionPropGetInt(current, TJS_W("time")));
            const tTJSVariant next = rawFrameAt(
                tagFrames, _layerFrameCursor + 1);
            _layerNextTime = static_cast<double>(
                detail::motionPropGetInt(next, TJS_W("time")));

            // The gate is keyed on the cursor frame and fires only when its
            // truncated time equals the live target and type is one.
            if(_layerCurTime == _clampedEvalTime &&
               rawFrameType(current) == 1) {
                const tTJSVariant content = rawFrameContent(current);
                if(_syncActive) {
                    // The align gate deliberately re-tests equality.
                    if(_layerCurTime == _clampedEvalTime &&
                       detail::motionPropGetBool(
                           content, TJS_W("align"))) {
                        _motionCompleted = true;
                        _clampedEvalTime = _layerCurTime;
                        _frameTickCount = _layerCurTime;
                    }
                    // The sync gate deliberately re-tests _syncActive.
                    if(_syncActive && detail::motionPropGetBool(
                           content, TJS_W("sync"))) {
                        _syncWaiting = true;
                        _clampedEvalTime = _layerCurTime;
                        _frameTickCount = _layerCurTime;
                        enqueueSyncEvent_guess();
                    }
                }
                // content["action"] is ungated and becomes
                // onAction(void, name).
                const ttstr action = detail::motionPropGetString(
                    content, TJS_W("action"));
                if(!action.IsEmpty()) {
                    enqueueActionEvent_guess(tTJSVariant(), action);
                }
            }
        }

        // ---- STEP 2: root-content scan over the persistent priority owner ----
        // Construction order is tag owner, priority owner, current root frame,
        // next root frame. All four live through the tail and unwind in reverse.
        const tTJSVariant priorityFrames = _priorityFrameSourceVariant;
        const int priorityCount = rawFrameCount(priorityFrames);

        int index = 0;
        if(priorityCount != 0) {
            if(priorityCount >= 1) {
                // Unlike the tag scan, this loop advances one index per
                // iteration. Equality stops on that frame; the first frame
                // greater than the target backs up by one.
                for(index = 0; index < priorityCount; ++index) {
                    const tTJSVariant frame = rawFrameAt(
                        priorityFrames, index);
                    const double frameTime = rawFrameTime(frame);
                    const double evaluationTime = _clampedEvalTime;
                    if(evaluationTime == frameTime) {
                        break;
                    }
                    if(frameTime <= evaluationTime) {
                        continue;
                    }
                    --index;
                    break;
                }
            }
            // This write is outside the count>=1 branch in every target. A
            // negative dynamic count therefore commits zero before the clamp.
            _rootFrameCursor = index;
        } else {
            index = _rootFrameCursor;
        }
        _rootFrameCursor = std::min(
            index, subtractTwoWrapping32_guess(priorityCount));

        // Commit order is observable on getter failure: cursor, content,
        // current time, then next time.
        const tTJSVariant rootCurrent = rawFrameAt(
            priorityFrames, _rootFrameCursor);
        _rootContentVariant = rawFrameContent(rootCurrent);
        _rootCurTime = rawFrameTime(rootCurrent);
        const tTJSVariant rootNext = rawFrameAt(
            priorityFrames, _rootFrameCursor + 1);
        _rootNextTime = rawFrameTime(rootNext);

        // ---- STEP 3: variable-track absolute reseed ----
        reseedVariableTracks_guess();

        // ---- STEP 4: absolute node-slot reseed ----
        // Reposition every non-root node independently of its previous cursor.
        // This phase is complete: frameProgress does not append a separate
        // incremental node pass after any of its absolute-reseek call sites.
        reseedNodeTimelineSlots_guess();

        // The full-reseek tail always restores/prunes join snapshots first.
        restoreAndPruneJoinSnapshots_guess();

        // Then every HM1 map node is visited. Native libstdc++ and libc++ expose
        // different internal walks, but each rebuild reads only the Player node
        // deque and that entry, so map iteration order does not affect results.
        for(auto &kv : _evalCascadeMap) {
            rebuildEvalCascadeEntry_guess(kv.second);
        }
    }

    void Player::restoreAndPruneJoinSnapshots_guess() {
        // The full-reseek STEP5 tail. All four references run two gated loops
        // and then clear both join-snapshot maps unconditionally.
        //
        // HM4 restore is skipped entirely when the map is empty. For each live
        // active slot, a cascade-key hit overwrites only slot.value. Misses and
        // type-zero slots are untouched.
        if (!_variableSnapshotMap.empty()) {
            for (auto &item : _variableLabelScopes) {
                const int cursor = item.activeSlotCursor & 1;
                if (item.slot[cursor].typeZeroFlag) {
                    continue;
                }
                if (const auto it = _variableSnapshotMap.find(item.cascadeKey);
                    it != _variableSnapshotMap.end()) {
                    item.slot[cursor].value = it->second;
                }
            }
        }

        // HM3 restore/prune follows the HM4 pass. For every non-root node it
        // looks up the hierarchical path. A matching snapshot is consumed only
        // when joinTarget and nodeType agree; restore covers the common block and
        // the type-3/type-4 child or particle payload, optionally refreshes a
        // type-0 source, then erases the entry. The retained active-slot source
        // string extends snapshot lifetime but is deliberately not written back.
        // The terminal clear pre-invalidates child objects retained by unmatched
        // snapshots and then destroys both maps.
        if(!_perNodeLayerStateMap.empty()) {
            for(size_t k = 1; k < _nodes.size(); ++k) {
                detail::MotionNode &node = _nodes[k];
                const ttstr key = detail::buildNodePathKey_guess(
                    _nodes, static_cast<int>(k));
                const auto it = _perNodeLayerStateMap.find(key);
                if(it == _perNodeLayerStateMap.end()) {
                    continue;
                }
                detail::PerNodeLayerState &v = it->second;
                if(!node.joinTarget || v.nodeType != node.nodeType) {
                    continue;
                }
                // Restore V into the active ClipSlot.
                node.restoreJoinSnapshot_guess(v);
                if(node.nodeType == 0 && !node.activeSlot().done) {
                    findSourceForNode_guess(node);
                }
                _perNodeLayerStateMap.erase(it);
            }
        }
        clearJoinSnapshotMaps_guess();
    }

    void Player::interpolateVarTrackValues_guess() {
        // This member reads _clampedEvalTime directly and writes each track's
        // current value before binding it. The active slot is the lower frame;
        // the other slot is the upper frame. A type-zero active slot is skipped,
        // otherwise the path is HOLD or LERP.
        //
        // Easing is {x:[...], y:[...]}. X values select a stride-three segment,
        // but the original t is fed directly into the cubic polynomial; x does
        // not re-parameterize it. X/Y property hints persist across all calls.
        for (auto &item : _variableLabelScopes) {
            const int cursor = item.activeSlotCursor & 1;
            detail::VarTrackSlot &active = item.slot[cursor];     // prev
            detail::VarTrackSlot &other = item.slot[cursor ^ 1];  // next
            if (active.typeZeroFlag) {
                continue;
            }
            double v;
            if (active.interpFlag == 0 || other.typeZeroFlag) {
                v = active.value;
            } else {
                double d = _clampedEvalTime - active.time;
                if (active.interval != 0) {
                    // All four references convert the quotient to uint64_t with
                    // truncation toward zero, multiply in unsigned integer
                    // space, then convert back to double. This is not floor() for
                    // invalid negative/out-of-range cursor state.
                    const std::uint64_t interval = active.interval;
                    d = static_cast<double>(
                        static_cast<std::uint64_t>(d / interval) * interval);
                }
                const double Vp = active.value, Vo = other.value;
                if (Vo == Vp) {
                    v = Vp;
                } else {
                    double t = d / (other.time - active.time);
                    if (active.easing.Type() != tvtVoid) {
                        t = internal::evaluateVariableTrackEasing_guess(
                            active.easing, t);
                    }
                    v = Vo * t + Vp * (1.0 - t);
                }
            }
            item.value = v;
            // The var-track caller enters the shared binder with mode 0 and
            // populates HM1 (_evalCascadeMap, scope keys) + HM2
            // (_evalResultValues) so getVariable's HM1-join branch resolves.
            bindParameterValue_guess(item.cascadeKey, 0, v);
        }
    }

    void Player::evaluateTimelinesForJoinSnapshot_guess() {
        // Android armv7 and both iOS references retain this aggregate helper;
        // Android arm64 inlines the same sequence into resetMotionState. The
        // binary starts at index one, so the root node is deliberately excluded.
        interpolateVarTrackValues_guess();
        for(std::size_t i = 1; i < _nodes.size(); ++i) {
            auto &node = _nodes[i];
            node.flags = 1;
            (void)evaluateTimeline_guess(
                node, _clampedEvalTime, true);
        }
    }

    void Player::resetMotionState_guess() {
        // Four-target parity: playImpl calls this for PlayFlagJoin before loading
        // the new motion. The complete body is gated on !_queuing.
        if (_queuing) {
            return;
        }
        clearJoinSnapshotMaps_guess();
        // Compute live variable values and evaluate every non-root timeline
        // before either snapshot is rebuilt.
        evaluateTimelinesForJoinSnapshot_guess();
        // Snapshot each live variable-track value into HM4, gated on the active
        // slot's type-zero marker and keyed by cascadeKey.
        for (const auto &item : _variableLabelScopes) {
            const int cursor = item.activeSlotCursor & 1;
            if (!item.slot[cursor].typeZeroFlag) {
                _variableSnapshotMap[item.cascadeKey] = item.value;
            }
        }
        // Rebuild HM3 per-node-path layer state from the freshly evaluated node.
        // Gate order matches all four references: joinTarget first, then the
        // exact node-type mask {0, 2, 3, 4, 7, 8}.
        // Only joinTarget nodes are snapshotted into HM3. The path-key space is
        // distinct from the raw-label node-index map. The map
        // is read on the maintenance side by restoreAndPruneJoinSnapshots_guess
        // (reseek STEP5), whose per-node restore payload is ported.
        for(size_t k = 1; k < _nodes.size(); ++k) {
            auto &node = _nodes[k];
            if(!node.joinTarget) {
                continue;
            }
            const int t = node.nodeType;
            if(t >= 0 && t <= 8 && ((1 << t) & 0x19D) != 0) {
                const ttstr key = detail::buildNodePathKey_guess(
                    _nodes, static_cast<int>(k));
                node.initJoinSnapshot_guess(_perNodeLayerStateMap[key]);
            }
        }
    }

    void detail::MotionNode::initJoinSnapshot_guess(
        detail::PerNodeLayerState &v) {
        // The HM3 value embeds a complete, initially-zero ClipSlot. Only the
        // members below are populated; its other string/Variant/vector owners
        // remain empty but participate in the native reverse destruction chain.
        detail::MotionNode &mnode = *this;
        v.nodeType = mnode.nodeType;
        const auto &c = mnode.accumulated;
        auto &slot = mnode.activeSlot();

        // The mesh copy precedes both specialized ownership transfers.
        if(mnode.meshType == 1) {
            v.meshControlPoints = mnode.meshControlPoints;
        }

        if(mnode.nodeType == 3) {
            v.childPlayerSnapshot = mnode.childPlayerVar;
            mnode.childPlayerVar.Clear();
        }

        // The type-4 Array is transferred before the done gate. A live particle
        // slot additionally snapshots the nine evaluated interpolation values.
        if(mnode.nodeType == 4) {
            v.particleArraySnapshot = mnode.particleArrayVar;
            mnode.particleArrayVar.Clear();
            if(slot.done) {
                v.clipSlot.done = true;
                return;
            }
            for(int i = 0; i < 9; ++i) {
                v.particleInterp[i] = mnode.particleInterp[i];
            }
            v.clipSlot.done = false;
        } else {
            v.clipSlot.done = slot.done;
            if(v.clipSlot.done) {
                return;
            }
        }

        // srcValue is deliberately retained only for lifetime extension. The
        // restore path does not copy it back into the active slot.
        v.clipSlot.srcValue = slot.srcValue;
        v.clipSlot.contentMask = slot.contentMask;
        v.clipSlot.blendMode = slot.blendMode;
        v.clipSlot.ox = slot.ox;
        v.clipSlot.oy = slot.oy;
        std::memcpy(v.clipSlot.packedColors.data(), mnode.colorBytes,
                    sizeof(mnode.colorBytes));
        v.clipSlot.opacity = c.opacity;
        v.clipSlot.x = c.posX;
        v.clipSlot.y = c.posY;
        v.clipSlot.z = c.posZ;
        v.clipSlot.flipX = c.flipX;
        v.clipSlot.flipY = c.flipY;
        v.clipSlot.angle = c.angle;
        v.clipSlot.scaleX = c.scaleX;
        v.clipSlot.scaleY = c.scaleY;
        v.clipSlot.slantX = c.slantX;
        v.clipSlot.slantY = c.slantY;
    }

    void detail::MotionNode::restoreJoinSnapshot_guess(
        detail::PerNodeLayerState &v) {
        detail::MotionNode &node = *this;
        detail::MotionNode::ClipSlot &slot = node.activeSlot();

        // Restore order is observable: mesh, specialized Variant ownership,
        // particle interpolation, then the common scalar block.
        if(node.meshType == 1) {
            slot.meshControlPoints = v.meshControlPoints;
        }

        if(v.nodeType == 3) {
            node.childPlayerVar = v.childPlayerSnapshot;
            v.childPlayerSnapshot.Clear();
        }

        if(v.nodeType == 4) {
            node.particleArrayVar = v.particleArraySnapshot;
            v.particleArraySnapshot.Clear();
            if(!v.clipSlot.done) {
                slot.prtFmin  = v.particleInterp[0];
                slot.prtF     = v.particleInterp[1];
                slot.prtVmin  = v.particleInterp[2];
                slot.prtV     = v.particleInterp[3];
                slot.prtAmin  = v.particleInterp[4];
                slot.prtA     = v.particleInterp[5];
                slot.prtZmin  = v.particleInterp[6];
                slot.prtZ     = v.particleInterp[7];
                slot.prtRange = v.particleInterp[8];
            }
        }

        // Neither a currently-done slot nor a snapshot of a done slot accepts
        // the common scalar block. srcValue is intentionally absent below.
        if(slot.done || v.clipSlot.done) {
            return;
        }
        slot.contentMask = v.clipSlot.contentMask;
        slot.blendMode = v.clipSlot.blendMode;
        slot.ox = v.clipSlot.ox;
        slot.oy = v.clipSlot.oy;
        slot.packedColors = v.clipSlot.packedColors;
        slot.opacity = v.clipSlot.opacity;
        slot.x = v.clipSlot.x;
        slot.y = v.clipSlot.y;
        slot.z = v.clipSlot.z;
        slot.flipX = v.clipSlot.flipX;
        slot.flipY = v.clipSlot.flipY;
        slot.angle = v.clipSlot.angle;
        slot.scaleX = v.clipSlot.scaleX;
        slot.scaleY = v.clipSlot.scaleY;
        slot.slantX = v.clipSlot.slantX;
        slot.slantY = v.clipSlot.slantY;
    }

    // Native forward incremental boundary takes only `this`. The extracted
    // phases read the current Player evaluation cursor at their own boundary;
    // the variable phase additionally reloads it on every loop comparison.
    void Player::advanceTimelineStreams_guess() {
        const tTJSVariant tagFrameSourceOwner = _tagFrameSourceVariant;
        advanceLayerEventStreamPhase_guess(tagFrameSourceOwner);
        const tTJSVariant priorityFrameSourceOwner =
            _priorityFrameSourceVariant;
        advanceRootContentStreamPhase_guess(priorityFrameSourceOwner);
        advanceVariableTracksPhase_guess();
        seekNodeTimelineSlotsIncrementalPhase_guess();
    }

    // Native reverse incremental boundary with the same four ordered phases.
    void Player::rewindTimelineStreams_guess() {
        const tTJSVariant tagFrameSourceOwner = _tagFrameSourceVariant;
        rewindLayerEventStreamPhase_guess(tagFrameSourceOwner);
        const tTJSVariant priorityFrameSourceOwner =
            _priorityFrameSourceVariant;
        rewindRootContentStreamPhase_guess(priorityFrameSourceOwner);
        rewindVariableTracksPhase_guess();
        seekNodeTimelineSlotsIncrementalPhase_guess(/*forward=*/false);
    }

    void Player::frameProgress(double dt) {
        // This is a progress-pass work counter, not a lifetime statistic.  The
        // four references clear it before every other observable progress
        // side effect, including paths that return early after a reseek.
        _processedMeshVerticesNum = 0;

        // All four progress implementations perform their entry side effects
        // unconditionally; syncActive gates only later align/sync/action work,
        // not the complete progress pass.
        const double actualDelta = dt;
        // The incoming frame delta has no separate retained owner. It is used
        // directly in speed*delta below; frameLastTime remains motion metadata.

        // motionCompleted is likewise cleared before any branch/return.  A
        // loop wrap can set it again later in this same pass; there is no
        // cross-frame dependency on its previous value.
        _motionCompleted = false;

        // Store this pass's scaled delta before first-frame/reseek consumers,
        // the anchor zero-delta gate, and damping calculations can observe it.
        _deltaTime = _speedMul * actualDelta;
        if(_directEdit) {
            initEmoteMotion_guess(2u);
        }

        // HM2 is persistent across progress calls. The four current cores clear
        // only the mesh-work counter and motion-completed byte at entry; none
        // writes any of Player's four hash maps. Bind results therefore remain
        // readable until a motion reset/load clears them.

        // Modified-node refresh is the first out-of-line phase, before all
        // first-frame and cursor routing.
        refreshModifiedNodeTimelines_guess();

        // A Player-level parameter selected while loading the motion drives the
        // cursor directly; ordinary dt progression is not entered while this
        // alias into _parameterEntries is present.
        if(_selectedParameterEntry != nullptr) {
            const double parameterTime = _selectedParameterEntry->value;
            if(_firstFrame) {
                _frameTickCount = parameterTime;
                _firstFrame = false;
                _clampedEvalTime = parameterTime;
                reseekTimelineCursors();
                return;
            }
            if(parameterTime > _clampedEvalTime) {
                _frameTickCount = parameterTime;
                _clampedEvalTime = parameterTime;
                advanceTimelineStreams_guess();
                return;
            }
            if(parameterTime < _clampedEvalTime) {
                _frameTickCount = parameterTime;
                _clampedEvalTime = parameterTime;
                rewindTimelineStreams_guess();
                return;
            }
            // An equal cursor refreshes only nodes whose parameter-table
            // pointer is present.
            refreshParameterizedNodeTimelines_guess();
            return;
        }

        // Without a selected Player-level parameter, an idle non-first-frame
        // player never enters the timed cursor/wrap machine. All four references
        // compare parameterEntries.begin/end here: an empty parameter table
        // returns, while a nonempty table takes only the parameterized-node
        // refresh route.
        if(!_firstFrame && !_allplaying) {
            if(_parameterEntries.empty()) {
                return;
            }
            // 非空时只刷新 parameterEntry 非空的节点；普通时间节点不参与这条
            // early-return 路径。共享 stepper 自己读取参数值并执行 source tail。
            refreshParameterizedNodeTimelines_guess();
            return;
        }

        // Reaching the timed state machine (firstFrame || playing) always
        // rechecks the two cooperative-stop bytes after the direct-edit and
        // modified-node entry work above.
        if(_syncWaiting) {
            return;
        }
        if(_motionCompleted) {
            // Entry clears this byte, but direct-edit initialization and the
            // modified-node refresh run afterward and may publish it again.
            return;
        }
        // First-frame handling is a one-shot absolute seed followed by an
        // optional direction-aware corrective traversal. Every successful
        // path falls through into the common cursor/wrap state machine below;
        // it is not an unconditional first-frame return. Normally play leaves
        // the queue gate set, making that fall-through numerically inert, but
        // the public queue setter can clear the gate before this call.
        if(_firstFrame) {
            // 四端的 progress inner 都不写 syncActive；构造器和脚本 setter
            // 是仅有写入者，advance/rewind/reseek 只把它读作 gate。原先此处
            // `_syncActive = _syncWaiting && _allplaying` 是杜撰，已删。
            const double deltaTime = _deltaTime;
            _firstFrame = false;

            // A first reverse step from raw frame zero starts at the end.
            if(deltaTime < 0.0 && _frameTickCount == 0.0) {
                _clampedEvalTime = _cachedTotalFrames;
                _frameTickCount  = _cachedTotalFrames;
            }

            if(_reverseSeekFlag) {
                _reverseSeekFlag = false;
                if(deltaTime >= 0.0) {
                    const double saved = _clampedEvalTime;
                    _clampedEvalTime = 0.0;
                    reseekTimelineCursors();
                    if(_syncWaiting)     return;
                    if(_motionCompleted) return;
                    _clampedEvalTime = saved;
                    advanceTimelineStreams_guess();
                    if(_syncWaiting)     return;
                } else {
                    const double lastTime = _cachedTotalFrames;
                    if(lastTime > _frameTickCount) {
                        const double saved = _clampedEvalTime;
                        _clampedEvalTime = lastTime;
                        reseekTimelineCursors();
                        if(_syncWaiting)     return;
                        if(_motionCompleted) return;
                        _clampedEvalTime = saved;
                        rewindTimelineStreams_guess();
                        if(_syncWaiting)     return;
                    }
                }
            } else {
                reseekTimelineCursors();
                if(_syncWaiting) return;
            }
            if(_motionCompleted) return;
        }

        // The player owns one raw-frame cursor. Its normal advance is gated by
        // the queue byte: construction and explicit cursor setters publish a
        // queued state, while updateLayers releases that gate after applying a
        // nonempty node tree. No second loop-time accumulator participates here.
        if(!_queuing) {
            _frameTickCount += _deltaTime;
        }

        // Four-reference topology: this generic Player core consumes frameDt
        // exactly once. It does not run the EmoteEngine timeline/controller
        // slice loop, variable-value bind loop, or clamp controls; those are
        // owned by EmoteEngine::progress before it enters the native-shaped Player
        // bridge. A plain Motion.Player script call enters that bridge directly.
        //
        // _evalResultValues is the persistent Player HM2 mirror. The core does
        // not clear it at frame entry; fixed-controller evaluation overwrites
        // only the labels it produces. A former port-only 1.1-frame substep loop
        // was removed because none of the four current cores contains it. Large,
        // negative, infinite and NaN script deltas are likewise not clamped by
        // the native wrapper before reaching this one-shot core.
        //
        // Camera velocity/friction is owned by the update-layers pre-loop.

        // Shared progress-core cursor update after gated tick accumulation. The
        // native core branches on delta sign and the end/start boundary into
        // forward-normal, forward-to-end, reverse-rewind and loop-wrap cases.
        // Every terminal branch dispatches one of two complete four-stream
        // functions: layer events, root content, variable tracks, then nodes.
        // Their non-parameterized node loops are direction-specific; parameterized
        // nodes instead call the shared forward-plus-corrective-backward helper.
        // The local extracted node phase preserves that discriminator.
        //
        // Snapshot the queue gate and per-pass delta once for the terminal
        // direction/wrap dispatch. When the gate is clear, the raw cursor was
        // advanced above; cap only its evaluation copy at totalFrames.
        const bool gate = _queuing;
        const double deltaTime = _deltaTime;
        if(!gate) {
            double clampedCursor = _frameTickCount;
            if(clampedCursor > _cachedTotalFrames) {
                clampedCursor = _cachedTotalFrames;
            }
            _clampedEvalTime = clampedCursor;
        }

        bool dispatchDirectionStreams = false;

        if(deltaTime >= 0.0) {
            const double totalFrames = _cachedTotalFrames;
            if(totalFrames <= _frameTickCount) {
                // loopTime is the fixed motion-table wrap target; progress never
                // accumulates dt into it.
                const double loopTime = _loopTime;
                _clampedEvalTime = totalFrames;
                if(loopTime >= 0.0) {
                    // Run the complete forward four-stream function at the clamped
                    // end. Its layer phase precedes its node phase at the same
                    // evaluation time, so any synchronization snap performed by
                    // the layer stream reaches the following node walk.
                    advanceTimelineStreams_guess();
                    if(!_syncWaiting && !_motionCompleted) {
                        _clampedEvalTime = _loopTime;
                        // Full absolute reseek includes layer, root, variable,
                        // node, join-snapshot and HM1-rebuild phases.
                        reseekTimelineCursors();
                        if(!_syncWaiting && !_motionCompleted) {
                            double wrappedTick = _frameTickCount;
                            if(_cachedTotalFrames > wrappedTick) {
                                _clampedEvalTime = wrappedTick;
                            } else {
                                do {
                                    wrappedTick = wrappedTick + _loopTime -
                                                  _cachedTotalFrames;
                                } while(_cachedTotalFrames <= wrappedTick);
                                _frameTickCount = wrappedTick;
                                _clampedEvalTime = wrappedTick;
                            }
                            // The second traversal advances from the absolute
                            // loop-target reseek to the wrapped raw cursor.
                            advanceTimelineStreams_guess();
                        }
                    }
                } else {
                    _allplaying = false;
                    if(!gate) {
                        dispatchDirectionStreams = true;
                    }
                }
            } else if(!gate) {
                dispatchDirectionStreams = true;
            }
            // A set queue gate with a cursor before the end dispatches nothing.
        } else {
            const double frameTickCount = _frameTickCount;
            const double loopTime = _loopTime;
            if(frameTickCount >= 0.0 && loopTime <= frameTickCount) {
                if(!gate) {
                    dispatchDirectionStreams = true;
                }
            } else if(loopTime < 0.0) {
                _clampedEvalTime = 0.0;
                _allplaying = false;
                _frameTickCount = 0.0;
                if(!gate) {
                    dispatchDirectionStreams = true;
                }
            } else {
                _clampedEvalTime = loopTime;
                rewindTimelineStreams_guess();
                if(!_syncWaiting && !_motionCompleted) {
                    _clampedEvalTime = _cachedTotalFrames;
                    // Reverse wrap uses the same complete absolute reseek as
                    // forward wrap, now targeted at the final frame.
                    reseekTimelineCursors();
                    if(!_syncWaiting && !_motionCompleted) {
                        double wrappedTick = _frameTickCount;
                        if(_loopTime <= wrappedTick) {
                            _clampedEvalTime = wrappedTick;
                        } else {
                            do {
                                wrappedTick = wrappedTick - _loopTime +
                                              _cachedTotalFrames;
                            } while(_loopTime > wrappedTick);
                            _frameTickCount = wrappedTick;
                            _clampedEvalTime = wrappedTick;
                        }
                        // The second traversal rewinds from the absolute
                        // end-frame reseek to the reverse-wrapped raw cursor.
                        rewindTimelineStreams_guess();
                    }
                }
            }
        }

        // Normal forward, normal reverse and non-loop terminal cases dispatch the
        // complete direction-specific four-stream function here. Its node phase
        // fills both parsed-frame slots for every non-root node; the separate
        // updateLayers pass only interpolates those positioned slots. Loop-wrap
        // branches above already performed their own four-stream dispatches.
        if(dispatchDirectionStreams) {
            // Dispatch with the same delta sign that selected the terminal branch.
            // Each direction owns its layer, root, variable and node loops; the
            // empty-node-deque guard lives inside the extracted node phase.
            if(deltaTime >= 0.0) {
                advanceTimelineStreams_guess();
            } else {
                rewindTimelineStreams_guess();
            }
        }

        // The motion["tag"] event stream is driven inside each forward or
        // reverse root traversal. Each node-slot traversal is preceded by the
        // matching directional layer loop at the same evaluation time, so the
        // layer stream, root stream, variable tracks, and node walk form one
        // unit. This fixes the three defects of the old single end-of-frame call:
        //   (1) loop-wrap segments (totalFrames then loopTime) each now scan the
        //       layer stream, so align/sync/action inside a wrapped segment fire;
        //   (2) align/sync time snaps now propagate to the same advance's
        //       node walk (was a 1-frame lag);
        //   (3) the gate-set not-at-end / firstFrame-queuing paths correctly do
        //       NOT scan the layer stream (binary returns without advanceRoot).
        // Full reseek carries its own exact-frame layer/tag scan. That scan is
        // intentionally distinct from the incremental four-stream functions.
        // (Per-node frame actions — node mask 0x40000 from the node seek — remain
        // 洞2, already handled inside progressSeekNodeSlotsLike's _pendingEvents.)

        // The core does not derive `_allplaying` from a timeline-label vector;
        // only terminal non-loop branches clear it. Image-side players can thus
        // remain playing without a locally started timeline label.
        // Removed fabricated `_syncActive = _syncWaiting && _allplaying`:
        // progress_inner never writes syncActive; construction and the public
        // setter are its only write sites in all four references.
    }

    void Player::enqueueSyncEvent_guess() {
        const detail::MotionEvent event{1, tTJSVariant(), tTJSVariant()};
        _pendingEvents.push_back(event);
    }

    void Player::enqueueActionEvent_guess(const tTJSVariant &param1,
                                          const ttstr &action) {
        const detail::MotionEvent event{0, param1, tTJSVariant(action)};
        _pendingEvents.push_back(event);
    }

    void Player::dispatchPendingEvents_guess(iTJSDispatch2 *dispatch) {
        if(_pendingEvents.empty()) {
            return;
        }

        // The four reference helpers retain one raw dispatch for the entire
        // traversal, reuse one callback-result Variant, and deliberately do
        // not consume the event vector.  AddRef is unconditional once the
        // vector is non-empty: a null dispatch is therefore a native crash
        // boundary, not a silent-drop path.
        dispatch->AddRef();
        struct DispatchReleaseGuard {
            iTJSDispatch2 *dispatch;
            ~DispatchReleaseGuard() {
                if(dispatch) dispatch->Release();
            }
        } retainedDispatch{dispatch};
        {
            // Keep the result owner in an inner scope: every reference helper
            // destroys it before releasing the retained dispatch.
            tTJSVariant callbackResult;
            // The native loop advances a raw element pointer but reloads the
            // vector's live end on every condition. Appends that fit existing
            // capacity are therefore visited in this same pass; a reallocation
            // invalidates the raw cursor just as it does in the references.
            const detail::MotionEvent *event = _pendingEvents.data();
            while(event != _pendingEvents.data() + _pendingEvents.size()) {
                if(event->type == 0) {
                    tTJSVariant param1(event->param1);
                    tTJSVariant param2(event->param2);
                    tTJSVariant *args[] = {&param1, &param2};
                    dispatch->FuncCall(0, TJS_W("onAction"),
                                       &detail::onActionMemberHint_guess,
                                       &callbackResult, 2, args, dispatch);
                } else if(event->type == 1) {
                    dispatch->FuncCall(0, TJS_W("onSync"),
                                       &detail::onSyncMemberHint_guess,
                                       &callbackResult, 0, nullptr, dispatch);
                }
                ++event;
            }
        }
    }

    // The four current bridge bodies take (Player, raw currentDispatch,
    // frameDt).  Engine progress and metadata initialization pass nullptr;
    // script progress passes objthis.  Neither the bridge nor its dispatch
    // helper clears the event vector.  NO ms->frame conversion occurs here.
    // This is the step-7 Player progress run AFTER the G2-C bind-loop so the
    // bound HM1/HM2 values are in place when the frame seek/eval reads them.
    // _evalResultValues/HM2 is persistent across frames in both the four
    // references and this implementation; frameProgress does not clear it.
    void Player::progressFrames_guess(iTJSDispatch2 *currentDispatch,
                                      double frameDt) {
        _currentDispatch = currentDispatch;
        frameProgress(frameDt);
        updateLayers();                         // unconditional in all four targets
        calcBounds();
        dispatchPendingEvents_guess(_currentDispatch);
        _currentDispatch = nullptr;
    }

    tjs_error Player::progressCompatMethod(tTJSVariant *result, tjs_int numparams,
                                           tTJSVariant **param,
                                           iTJSDispatch2 *objthis) {
#if defined(KRKR2_WASMTIME_HEADLESS)
        detail::MotionTraceProgressScope traceScope(nullptr, objthis);
#endif
        // The legacy method object clears a non-null result before entering
        // this raw callback; the callback itself never writes it.
        (void)result;

        auto *self =
            ncbInstanceAdaptor<Player>::GetNativeInstance(objthis, false);
        if(!self) {
            return TJS_E_NATIVECLASSCRASH;
        }

        // Native-instance resolution precedes the lower-bound arity gate.
        if(numparams < 1) {
            return TJS_E_BADPARAMCOUNT;
        }

        // Conversion happens before the bridge installs objthis. Therefore a
        // conversion exception preserves the previous raw dispatch slot, while
        // an exception in any downstream phase leaves objthis installed.
        const double delta = param[0]->AsReal();
#if defined(KRKR2_WASMTIME_HEADLESS)
        traceScope.setDeltaMs(delta);
#endif
        self->progressFrames_guess(
            objthis, (delta * 60.0) / 1000.0);
        return TJS_S_OK;
    }

} // namespace motion
