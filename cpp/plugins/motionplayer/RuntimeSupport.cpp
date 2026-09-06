//
// Internal helpers for motionplayer/emoteplayer runtime state.
//

#include "RuntimeSupport.h"
#include "MotionDispatch.h"
#include "Player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <memory>
#include <mutex>

#include <spdlog/spdlog.h>

#include "common/LogoTrace.h"

#include "tjsArray.h"
#include "tjsDictionary.h"
#include "ScriptMgnIntf.h"

#define LOGGER spdlog::get("plugin")

namespace motion::detail {

    // All four current references destroy the persistent render item first,
    // delete it, clear the node slot, and only then destroy the node suffix.
    MotionNode::~MotionNode() {
        delete preparedRenderItem;
        preparedRenderItem = nullptr;
    }

    tjs_uint32 widthMemberHint_guess = 0;
    tjs_uint32 heightMemberHint_guess = 0;
    tjs_uint32 originXMemberHint_guess = 0;
    tjs_uint32 originYMemberHint_guess = 0;
    tjs_uint32 blankMemberHint_guess = 0;
    tjs_uint32 clipMemberHint_guess = 0;
    tjs_uint32 leftMemberHint_guess = 0;
    tjs_uint32 topMemberHint_guess = 0;
    tjs_uint32 rightMemberHint_guess = 0;
    tjs_uint32 bottomMemberHint_guess = 0;
    tjs_uint32 xMemberHint_guess = 0;
    tjs_uint32 yMemberHint_guess = 0;
    tjs_uint32 positionControlTMemberHint_guess = 0;
    tjs_uint32 positionControlSMemberHint_guess = 0;
    tjs_uint32 controllerPhaseHint_guess = 0;
    tjs_uint32 controllerFrameHint_guess = 0;
    tjs_uint32 controllerVHint_guess = 0;
    tjs_uint32 controllerTargetHint_guess = 0;
    tjs_uint32 controllerLengthHint_guess = 0;
    tjs_uint32 controllerLengthDoneHint_guess = 0;
    tjs_uint32 controllerExponentHint_guess = 0;
    tjs_uint32 controllerSpeedHint_guess = 0;
    tjs_uint32 controllerRequestQueueHint_guess = 0;
    tjs_uint32 controllerP0Hint_guess = 0;
    tjs_uint32 controllerP1Hint_guess = 0;
    tjs_uint32 controllerMouthHint_guess = 0;
    tjs_uint32 controllerPrevHint_guess = 0;
    tjs_uint32 controllerTickHint_guess = 0;
    tjs_uint32 emoteControllerBeginFrameHint_guess = 0;
    tjs_uint32 emoteControllerEndFrameHint_guess = 0;
    tjs_uint32 emoteControllerBlinkIntervalMinHint_guess = 0;
    tjs_uint32 emoteControllerBlinkIntervalMaxHint_guess = 0;
    tjs_uint32 emoteControllerBlinkFrameCountHint_guess = 0;
    tjs_uint32 emoteControllerBlinkEnabledHint_guess = 0;
    tjs_uint32 emoteControllerEdgeHint_guess = 0;
    tjs_uint32 emoteControllerNodeHint_guess = 0;
    tjs_uint32 emoteVariableRangeMinHint_guess = 0;
    tjs_uint32 emoteVariableRangeMaxHint_guess = 0;
    tjs_uint32 emoteSpringGravityHint_guess = 0;
    tjs_uint32 emoteSpringCoefficientHint_guess = 0;
    tjs_uint32 emoteSpringFrictionHint_guess = 0;
    tjs_uint32 emoteSpringScaleXHint_guess = 0;
    tjs_uint32 emoteSpringScaleYHint_guess = 0;
    tjs_uint32 emoteBustChainFrictionXHint_guess = 0;
    tjs_uint32 emoteBustChainFrictionYHint_guess = 0;
    tjs_uint32 emoteBustChainBRateHint_guess = 0;
    tjs_uint32 emoteBustChainVBoundHint_guess = 0;
    tjs_uint32 emoteBustChainUdEftHint_guess = 0;
    tjs_uint32 emoteBustChainBendSpdHint_guess = 0;
    tjs_uint32 emoteBustChainBendVolHint_guess = 0;
    // One native mutable word backs the descriptor-key reader and every
    // render/serialization publisher; keep its address identity process-wide.
    // The reference BSS places an ABI-aligned OwnerFilter std::function after
    // this word rather than the next declaration below, so do not treat this
    // portable consolidation order as recovered native adjacency.
    tjs_uint32 commandKeyMemberHint_guess = 0;
    tjs_uint32 mtxMemberHint_guess = 0;
    tjs_uint32 triPriorityMemberHint_guess = 0;
    tjs_uint32 clipRectMemberHint_guess = 0;
    tjs_uint32 meshTransformMemberHint_guess = 0;
    tjs_uint32 compositeMeshMemberHint_guess = 0;
    tjs_uint32 stencilChainMemberHint_guess = 0;
    tjs_uint32 vtxMemberHint_guess = 0;
    tjs_uint32 divxMemberHint_guess = 0;
    tjs_uint32 divyMemberHint_guess = 0;
    tjs_uint32 timeMemberHint_guess = 0;
    tjs_uint32 typeMemberHint_guess = 0;
    tjs_uint32 contentMemberHint_guess = 0;
    tjs_uint32 maskMemberHint_guess = 0;
    tjs_uint32 actMemberHint_guess = 0;
    tjs_uint32 srcMemberHint_guess = 0;
    tjs_uint32 nodeFrameOxMemberHint_guess = 0;
    tjs_uint32 nodeFrameOyMemberHint_guess = 0;
    tjs_uint32 coordMemberHint_guess = 0;
    tjs_uint32 nodeFrameBmMemberHint_guess = 0;
    tjs_uint32 colorMemberHint_guess = 0;
    tjs_uint32 nodeFrameOpaMemberHint_guess = 0;
    tjs_uint32 nodeFrameFxMemberHint_guess = 0;
    tjs_uint32 nodeFrameFyMemberHint_guess = 0;
    tjs_uint32 angleMemberHint_guess = 0;
    tjs_uint32 nodeFrameZxMemberHint_guess = 0;
    tjs_uint32 nodeFrameZyMemberHint_guess = 0;
    tjs_uint32 nodeFrameSxMemberHint_guess = 0;
    tjs_uint32 nodeFrameSyMemberHint_guess = 0;
    tjs_uint32 nodeFrameTiMemberHint_guess = 0;
    tjs_uint32 nodeFrameCccMemberHint_guess = 0;
    tjs_uint32 nodeFrameOccMemberHint_guess = 0;
    tjs_uint32 nodeFrameAccMemberHint_guess = 0;
    tjs_uint32 nodeFrameZccMemberHint_guess = 0;
    tjs_uint32 nodeFrameSccMemberHint_guess = 0;
    tjs_uint32 nodeFrameCpMemberHint_guess = 0;
    tjs_uint32 meshMemberHint_guess = 0;
    tjs_uint32 nodeFrameObjMemberHint_guess = 0;
    tjs_uint32 nodeFrameCcMemberHint_guess = 0;
    tjs_uint32 nodeFrameMccMemberHint_guess = 0;
    tjs_uint32 nodeFrameBpMemberHint_guess = 0;
    tjs_uint32 bezierPatchMemberHint_guess = 0;
    tjs_uint32 motionMemberHint_guess = 0;
    tjs_uint32 nodeFrameFlagsMemberHint_guess = 0;
    tjs_uint32 nodeFrameDtMemberHint_guess = 0;
    tjs_uint32 nodeFrameDocmplMemberHint_guess = 0;
    tjs_uint32 nodeFrameDofstMemberHint_guess = 0;
    tjs_uint32 nodeFrameDtgtMemberHint_guess = 0;
    tjs_uint32 nodeFrameTimeOffsetMemberHint_guess = 0;
    tjs_uint32 nodeFrameModelMemberHint_guess = 0;
    tjs_uint32 nodeFrameLoopMemberHint_guess = 0;
    tjs_uint32 nodeFramePrtMemberHint_guess = 0;
    tjs_uint32 nodeFrameTriggerMemberHint_guess = 0;
    tjs_uint32 nodeFrameFminMemberHint_guess = 0;
    tjs_uint32 nodeFrameFmaxMemberHint_guess = 0;
    tjs_uint32 nodeFrameVminMemberHint_guess = 0;
    tjs_uint32 nodeFrameVmaxMemberHint_guess = 0;
    tjs_uint32 nodeFrameAminMemberHint_guess = 0;
    tjs_uint32 nodeFrameAmaxMemberHint_guess = 0;
    tjs_uint32 nodeFrameZminMemberHint_guess = 0;
    tjs_uint32 nodeFrameZmaxMemberHint_guess = 0;
    tjs_uint32 nodeFrameRangeMemberHint_guess = 0;
    tjs_uint32 nodeFrameCameraMemberHint_guess = 0;
    tjs_uint32 nodeFrameFovMemberHint_guess = 0;
    tjs_uint32 nodeFrameTargetMemberHint_guess = 0;
    tjs_uint32 nodeFrameAnchorMemberHint_guess = 0;
    tjs_uint32 nodeFrameFeedbackMemberHint_guess = 0;
    tjs_uint32 nodeFrameTimespanMemberHint_guess = 0;
    tjs_uint32 requestCharaMemberHint_guess = 0;
    tjs_uint32 onFindMotionMemberHint_guess = 0;
    tjs_uint32 findMotionMemberHint_guess = 0;
    tjs_uint32 commandIdMemberHint_guess = 0;
    tjs_uint32 playerParameterDiscretizationHint_guess = 0;
    tjs_uint32 playerParameterRangeBeginHint_guess = 0;
    tjs_uint32 playerParameterRangeEndHint_guess = 0;
    tjs_uint32 divisionMemberHint_guess = 0;
    tjs_uint32 motionListMemberHint_guess = 0;
    tjs_uint32 nodeEmoteEditMemberHint_guess = 0;
    tjs_uint32 nodeLabelMemberHint_guess = 0;
    tjs_uint32 parameterizeMemberHint_guess = 0;
    tjs_uint32 coordinateMemberHint_guess = 0;
    tjs_uint32 nodeJoinTargetMemberHint_guess = 0;
    tjs_uint32 nodeGroundCorrectionMemberHint_guess = 0;
    tjs_uint32 nodeFrameListMemberHint_guess = 0;
    tjs_uint32 nodeInheritMaskMemberHint_guess = 0;
    tjs_uint32 nodeTransformOrderMemberHint_guess = 0;
    tjs_uint32 nodeRequireLayerIdMemberHint_guess = 0;
    tjs_uint32 emoteEditModifiedHint_guess = 0;
    tjs_uint32 onGroundCorrectionMemberHint_guess = 0;
    tjs_uint32 emoteEditPriorDrawMemberHint_guess = 0;
    tjs_uint32 emoteEditFlipXMemberHint_guess = 0;
    tjs_uint32 emoteEditFlipYMemberHint_guess = 0;
    tjs_uint32 emoteEditZoomXMemberHint_guess = 0;
    tjs_uint32 emoteEditZoomYMemberHint_guess = 0;
    tjs_uint32 emoteEditSlantXMemberHint_guess = 0;
    tjs_uint32 particleArrayAddMemberHint_guess = 0;
    tjs_uint32 particleArrayEraseMemberHint_guess = 0;
    tjs_uint32 loadSourceMemberHint_guess = 0;
    tjs_uint32 blendModeMemberHint_guess = 0;
    tjs_uint32 assignImagesMemberHint_guess = 0;
    tjs_uint32 onSyncMemberHint_guess = 0;
    tjs_uint32 onActionMemberHint_guess = 0;
    tjs_uint32 meshCopyMemberHint_guess = 0;
    tjs_uint32 bezierPatchCopyMemberHint_guess = 0;
    tjs_uint32 affineCopyMemberHint_guess = 0;
    tjs_uint32 setClipMemberHint_guess = 0;
    tjs_uint32 bufLayerMemberHint_guess = 0;
    tjs_uint32 operateMeshMemberHint_guess = 0;
    tjs_uint32 operateBezierPatchMemberHint_guess = 0;
    tjs_uint32 operateAffineMemberHint_guess = 0;
    tjs_uint32 drawMeshFrameMemberHint_guess = 0;
    tjs_uint32 drawBezierPatchMeshFrameMemberHint_guess = 0;
    tjs_uint32 drawBezierPatchFrameMemberHint_guess = 0;
    tjs_uint32 drawLineMemberHint_guess = 0;
    tjs_uint32 visibleMemberHint_guess = 0;
    tjs_uint32 setPosMemberHint_guess = 0;
    tjs_uint32 opacityMemberHint_guess = 0;
    tjs_uint32 isValidMemberHint_guess = 0;
    tjs_uint32 parameterMemberHint_guess = 0;
    tjs_uint32 releaseLayerIdMemberHint_guess = 0;
    tjs_uint32 windowMemberHint_guess = 0;
    tjs_uint32 piledCopyMemberHint_guess = 0;
    tjs_uint32 randomMemberHint_guess = 0;
    tjs_uint32 calcMbpMemberHint_guess = 0;
    tjs_uint32 calcInvOffsetMemberHint_guess = 0;
    tjs_uint32 calcInvMatrixMemberHint_guess = 0;
    tjs_uint32 patchMemberHint_guess = 0;
    tjs_uint32 calcCmeshMemberHint_guess = 0;
    tjs_uint32 calcMatrixMemberHint_guess = 0;
    tjs_uint32 drawLayerMemberHint_guess = 0;
    tjs_uint32 setSizeMemberHint_guess = 0;
    tjs_uint32 copyRectMemberHint_guess = 0;
    tjs_uint32 operateRectMemberHint_guess = 0;
    tjs_uint32 adjustGammaMemberHint_guess = 0;
    tjs_uint32 primaryLayerMemberHint_guess = 0;
    tjs_uint32 fillRectMemberHint_guess = 0;
    tjs_uint32 neutralColorMemberHint_guess = 0;
    tjs_uint32 updateMemberHint_guess = 0;
    tjs_uint32 positionControlPMemberHint_guess = 0;
    tjs_uint32 layerClassMemberHint_guess = 0;
    tjs_uint32 absoluteMemberHint_guess = 0;
    tjs_uint32 hitThresholdMemberHint_guess = 0;

    tTJSVariant createLayerVariant_guess(
        const tTJSVariant &owner, const tTJSVariant &parent) {
        // The references keep both factory outputs as raw locals. CreateNew's
        // status is ignored, and no null check or scope guard is present: a
        // normal failure that leaves created null reaches created->Release(),
        // while an exception from CreateNew leaks the acquired global ref.
        iTJSDispatch2 *global = TVPGetScriptDispatch();
        iTJSDispatch2 *created = nullptr;
        tTJSVariant *args[] = {
            const_cast<tTJSVariant *>(&owner),
            const_cast<tTJSVariant *>(&parent)
        };
        (void)global->CreateNew(
            0, TJS_W("Layer"), &layerClassMemberHint_guess,
            &created, 2, args, global);
        tTJSVariant result(created, created);
        created->Release();
        global->Release();
        return result;
    }

    namespace {

        struct LogoChainTraceSession {
            std::uint64_t sequence = 0;
            std::string motionPath;
            std::string motionName;
            std::string firstBadStage;
            std::string firstBadExpected;
            std::string firstBadActual;
            std::string upstreamLastGoodStage;
            std::string likelyRootCause;
            bool summaryEmitted = false;
        };

        std::mutex &logoTraceMutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::unordered_map<std::string, LogoChainTraceSession>
        &logoTraceSessions() {
            static std::unordered_map<std::string, LogoChainTraceSession> sessions;
            return sessions;
        }

        std::string lowercase(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
            return value;
        }

        std::string basename(const std::string &value) {
            const auto slash = value.find_last_of("/\\");
            return slash == std::string::npos ? value : value.substr(slash + 1);
        }

        bool isTargetLogoMotionPath(const std::string &motionPath) {
            const auto lowered = lowercase(motionPath);
            return lowered.find("yuzulogo.mtn") != std::string::npos ||
                lowered.find("m2logo.mtn") != std::string::npos;
        }

        bool logoTraceQueryEnabled() {
            return TVPLogoTraceEnabled();
        }

        bool logoSnapshotQueryEnabled() {
            return false;
        }

        LogoChainTraceSession &ensureLogoTraceSessionLocked(
            const std::string &motionPath) {
            auto &session = logoTraceSessions()[lowercase(motionPath)];
            if(session.motionPath != motionPath) {
                session = {};
                session.motionPath = motionPath;
                session.motionName = basename(motionPath);
            }
            if(session.motionName.empty()) {
                session.motionName = basename(motionPath);
            }
            return session;
        }

        std::string frameLabel(double frameTime) {
            return std::isfinite(frameTime)
                ? fmt::format("{:.3f}", frameTime)
                : "n/a";
        }

    } // namespace

    void ensureRootNode_guess(Player &player) {
        if(!player._nodes.empty()) {
            return;
        }
        player._nodes.emplace_back();
        player._nodes.back().parentIndex = 0;
    }

    bool visitNodeOwnedPlayerVariants_guess(
        const std::deque<MotionNode> &nodes,
        const std::function<bool(const tTJSVariant &)> &visitor) {
        struct DispatchRelease {
            void operator()(iTJSDispatch2 *dispatch) const {
                if(dispatch) {
                    dispatch->Release();
                }
            }
        };

        for(const MotionNode &node : nodes) {
            if(node.nodeType == 4) {
                tTJSVariant arrayVariant(node.particleArrayVar);
                std::unique_ptr<iTJSDispatch2, DispatchRelease> array(
                    arrayVariant.AsObject());
                arrayVariant.Clear();

                tTJSVariant countValue;
                (void)array->PropGet(
                    0, TJS_W("count"), nullptr,
                    &countValue, array.get());
                const int count =
                    static_cast<int>(countValue.AsInteger());
                for(int index = 0; index < count; ++index) {
                    tTJSVariant child;
                    // This constant zero is intentional. All four references
                    // increment the loop counter but repeatedly fetch element 0.
                    (void)array->PropGetByNum(
                        0, 0, &child, array.get());
                    if(!visitor(child)) {
                        return false;
                    }
                }
            } else if(node.nodeType == 3) {
                if(!visitor(node.childPlayerVar)) {
                    return false;
                }
            }
        }
        return true;
    }

    void eraseNonRootNodesAndClearLabelMap_guess(Player &player) {
        if(player._nodes.size() > 1) {
            player._nodes.erase(std::next(player._nodes.begin()),
                                player._nodes.end());
        }
        player._nodeLabelMap.clear();
    }

    ttstr buildNodePathKey_guess(
        const std::deque<motion::detail::MotionNode> &nodes, int nodeIndex) {
        ttstr accumulated;
        while(nodeIndex) {
            const motion::detail::MotionNode &node =
                nodes[static_cast<size_t>(nodeIndex)];
            const ttstr segment = ttstr(TJS_W("/")) + node.layerName;
            accumulated = segment + accumulated;
            nodeIndex = node.parentIndex;
        }
        return accumulated;
    }

    std::string narrow(const ttstr &value) { return value.AsStdString(); }

    ttstr widen(const std::string &value) { return ttstr{ value }; }

    TJSArrayWithItems_guess createTJSArrayWithItems_guess() {
        // All four current references return an owning Array closure together
        // with a non-owning pointer to tTJSArrayNI::Items.  Object and ObjThis
        // are the same dispatch, so the Variant constructor performs two
        // AddRefs; releasing the factory reference leaves those closure refs
        // as the owner.  The native instance is not retained separately.
        iTJSDispatch2 *dispatch = TJSCreateArrayObject();
        const tTJSVariant value(dispatch, dispatch);
        dispatch->Release();

        // The native output slot is intentionally not pre-zeroed. References
        // publish the tTJSArrayNI::Items subobject only on the exact zero
        // result; every nonzero status publishes nullptr without inspecting
        // the returned native instance.
        tTJSArrayNI *native;
        const tjs_error status = dispatch->NativeInstanceSupport(
            TJS_NIS_GETINSTANCE, TJSGetArrayClassID(),
            reinterpret_cast<iTJSNativeInstance **>(&native));
        return { value,
                 status == TJS_S_OK ? &native->Items : nullptr };
    }

    tTJSVariant makeArray(const std::vector<tTJSVariant> &items) {
        iTJSDispatch2 *array = TJSCreateArrayObject();
        static tjs_uint addHint = 0;
        for(const auto &item : items) {
            tTJSVariant value = item;
            tTJSVariant *args[] = { &value };
            array->FuncCall(0, TJS_W("add"), &addHint, nullptr, 1, args, array);
        }
        tTJSVariant result(array, array);
        array->Release();
        return result;
    }

    tTJSVariant makeDictionary(
        const std::vector<std::pair<std::string, tTJSVariant>> &entries) {
        iTJSDispatch2 *dic = TJSCreateDictionaryObject();
        for(const auto &[key, value] : entries) {
            tTJSVariant tmp = value;
            dic->PropSet(TJS_MEMBERENSURE, widen(key).c_str(), nullptr, &tmp,
                         dic);
        }
        tTJSVariant result(dic, dic);
        dic->Release();
        return result;
    }

    bool logoChainTraceEnabled() {
        static const bool enabled = logoTraceQueryEnabled();
        return enabled;
    }

    bool logoSnapshotMarkEnabled() {
        static const bool enabled = logoSnapshotQueryEnabled();
        return enabled;
    }

    bool logoChainTraceEnabledForPath(const std::string &motionPath) {
        return logoChainTraceEnabled() && isTargetLogoMotionPath(motionPath);
    }

    bool logoSnapshotMarkEnabledForPath(const std::string &motionPath) {
        return logoSnapshotMarkEnabled() && isTargetLogoMotionPath(motionPath);
    }

    void logoChainTraceLog(const std::string &motionPath,
                           const char *stage,
                           const char *func,
                           const double frameTime,
                           const std::string &message) {
        if(!logoChainTraceEnabledForPath(motionPath) || !LOGGER) {
            return;
        }
        std::lock_guard lock(logoTraceMutex());
        auto &session = ensureLogoTraceSessionLocked(motionPath);
        ++session.sequence;
        LOGGER->warn(
            "CHAIN SEQ={} stage={} func={} motion={} frame={} {}",
            session.sequence, stage, func, session.motionName,
            frameLabel(frameTime), message);
    }

    void logoChainTraceCheck(const std::string &motionPath,
                             const char *stage,
                             const char *func,
                             const double frameTime,
                             const std::string &expected,
                             const std::string &actual,
                             const bool ok,
                             const std::string &likelyRootCause) {
        if(!logoChainTraceEnabledForPath(motionPath) || !LOGGER) {
            return;
        }

        std::lock_guard lock(logoTraceMutex());
        auto &session = ensureLogoTraceSessionLocked(motionPath);
        ++session.sequence;
        LOGGER->warn(
            "CHAIN SEQ={} stage={} func={} motion={} frame={} exp={} act={} ok={}",
            session.sequence, stage, func, session.motionName,
            frameLabel(frameTime), expected, actual, ok ? 1 : 0);

        if(ok) {
            if(session.firstBadStage.empty()) {
                session.upstreamLastGoodStage = stage;
            }
            return;
        }

        if(session.firstBadStage.empty()) {
            session.firstBadStage = stage;
            session.firstBadExpected = expected;
            session.firstBadActual = actual;
            session.likelyRootCause = likelyRootCause;
        }
    }

    void logoChainTraceSummary(const std::string &motionPath,
                               const char *func,
                               const double frameTime,
                               const std::string &note) {
        if(!logoChainTraceEnabledForPath(motionPath) || !LOGGER) {
            return;
        }

        std::lock_guard lock(logoTraceMutex());
        auto &session = ensureLogoTraceSessionLocked(motionPath);
        if(session.summaryEmitted) {
            return;
        }
        session.summaryEmitted = true;

        const auto firstBadStage = session.firstBadStage.empty()
            ? std::string("none")
            : session.firstBadStage;
        const auto expected = session.firstBadExpected.empty()
            ? std::string("all_logged_stages_ok")
            : session.firstBadExpected;
        const auto actual = session.firstBadActual.empty()
            ? std::string("all_logged_stages_ok")
            : session.firstBadActual;
        const auto upstream = session.upstreamLastGoodStage.empty()
            ? std::string("none")
            : session.upstreamLastGoodStage;
        const auto rootCause = session.likelyRootCause.empty()
            ? std::string("not_detected_in_logged_fields")
            : session.likelyRootCause;

        LOGGER->warn(
            "CHAIN SUMMARY func={} motion={} frame={} first_bad_stage={} expected={} actual={} upstream_last_good_stage={} likely_root_cause={}{}{}",
            func, session.motionName, frameLabel(frameTime), firstBadStage,
            expected, actual, upstream, rootCause,
            note.empty() ? "" : " note=", note);
    }

} // namespace motion::detail
