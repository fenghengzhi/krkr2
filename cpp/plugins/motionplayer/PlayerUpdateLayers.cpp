// PlayerUpdateLayers.cpp — updateLayers dispatcher and native phase order
// Split from Player.cpp for maintainability.
//
#include "PlayerInternal.h"
#include "MotionTraceWeb.h"

using namespace motion::internal;

namespace motion {
    // --- updateLayers: four-reference 3-phase pipeline ---
    // Operates on persistent MotionNode deque instead of re-walking PSB tree.
    void Player::updateLayers() {
        // Every reference clears the producer flag before any phase can set it
        // again. Post-draw only snapshots it; post-draw never clears it.
        _needsInternalAssignImages = false;
        auto &nodes = _nodes;
        // Player construction establishes the root deque entry. All four
        // references dereference it directly; an empty deque is not a
        // recoverable native state.
        const double currentTime = _clampedEvalTime;

        updateLayersPhase1_PreLoop(currentTime);
        updateLayersPhase2_MainLoop(currentTime);

        // The native root publishes this pass before camera constraints and
        // every phase-3 helper. It includes the synthetic root. Queuing writes
        // three zero words; otherwise phase-1's saved position is subtracted
        // from the freshly evaluated accumulated position.
        for (auto &node : nodes) {
            if (_queuing) {
                node.deltaPosX = 0.0;
                node.deltaPosY = 0.0;
                node.deltaPosZ = 0.0;
            } else {
                node.deltaPosX = node.accumulated.posX - node.prevPosX;
                node.deltaPosY = node.accumulated.posY - node.prevPosY;
                node.deltaPosZ = node.accumulated.posZ - node.prevPosZ;
            }
        }

        // The camera-constraint dirty state has now been consumed by every
        // non-root node. Clear it before the constraint pass publishes the
        // next frame's state.
        _cameraConstraintDirty_guess = false;

        // === PHASE 3: Post-loop processing ===
        // The order is shared by all four current reference binaries.
        updateLayersPhase3_CameraConstraint();
        updateLayersPhase3_VertexComputation();
        updateLayersPhase3_Visibility();
        updateLayersPhase3_CameraNode();
        updateLayersPhase3_ShapeAABB();
        updateLayersPhase3_ShapeGeometry();
        updateLayersPhase3_MotionSubNode();
        updateLayersPhase3_ParticleEmitter();
        updateLayersPhase3_ParticleSystem();
        updateLayersPhase3_AnchorNode();
#if defined(KRKR2_WASMTIME_HEADLESS)
        // Freeze values at the same helper-return boundary as the oracle.
        detail::motionTraceSnapshotPhase3(this);
#endif

        // === Post-loop cleanup ===
        // All four references clear the same player and per-node state here.

        // Clear the complete flags byte and accumulated dirty state for every
        // non-root node.
        for (size_t ci = 1; ci < nodes.size(); ++ci) {
            nodes[ci].flags = 0;
            nodes[ci].accumulated.dirty = false;
        }

        // Parameter mode is a one-update trigger consumed by the type-3 child
        // pass above. The value and every other parameter field remain live.
        for (auto &parameter : _parameterEntries) {
            parameter.mode = 0;
        }

        // Clear the first-update and parent-queue state only after the two
        // record ranges have been consumed and reset.
        _noUpdateYet = false;
        _queuing = false;
    }

} // namespace motion
