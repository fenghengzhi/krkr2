#pragma once

// Wasmtime sampling interface. Implementations live in the differential guest;
// browser builds do not collect or publish motion-frame JSON.

#include <cstdint>

#if defined(KRKR2_WASMTIME_HEADLESS)
#include <string>
#include <vector>
#endif

namespace motion {
    class Player;
}

namespace motion::detail {

    struct PreparedRenderItem;

    class MotionTraceProgressScope {
    public:
        MotionTraceProgressScope(Player *player, void *objthis);
        ~MotionTraceProgressScope();
#if defined(KRKR2_WASMTIME_HEADLESS)
        void setDeltaMs(double deltaMs);
#endif

        MotionTraceProgressScope(const MotionTraceProgressScope &) = delete;
        MotionTraceProgressScope &operator=(const MotionTraceProgressScope &) = delete;

    private:
        Player *_player = nullptr;
#if defined(KRKR2_WASMTIME_HEADLESS)
        void *_capture = nullptr;
#endif
    };

    void motionTraceRecordUpdatePlayer(Player *player);

#if defined(KRKR2_WASMTIME_HEADLESS)
    void motionTraceSnapshotPhase3(Player *player);
    class MotionTraceRenderDrawScope {
    public:
        MotionTraceRenderDrawScope(Player *player, void *argVariant,
                                   void *targetObject);
        ~MotionTraceRenderDrawScope();

        MotionTraceRenderDrawScope(const MotionTraceRenderDrawScope &) = delete;
        MotionTraceRenderDrawScope &operator=(const MotionTraceRenderDrawScope &) = delete;

        void setRoute(const char *route);
        void recordTargetCheckD3D(bool hit);
        void recordTargetCheckSLA(bool hit);
        void recordPrepareResult(bool ok);
        void recordBranchAfterPrepare(bool d3dDrawMode);
        void recordApplyPreparedProjection();
        void recordRenderToCanvas(bool ok);
        void recordUpdateLayerAfterDraw(bool internalAssignRequested, bool ok);

    private:
        void emitStep(const char *drawStep, const char *outcome,
                      const char *route = nullptr,
                      const char *extraPayload = nullptr);

        Player *_player = nullptr;
        void *_argVariant = nullptr;
        void *_targetObject = nullptr;
        const char *_route = nullptr;
        int _drawId = -1;
        int _stepIndex = 0;
        std::vector<std::string> _steps;
        bool _prepareCalled = false;
        bool _prepareOk = false;
        bool _prepareOkKnown = false;
        bool _d3dDrawModeAfterPrepare = false;
        bool _d3dDrawModeAfterPrepareKnown = false;
        bool _renderToCanvasCalled = false;
        bool _updateLayerAfterDrawCalled = false;
        bool _internalAssignRequested = false;
        bool _internalAssignRequestedKnown = false;
    };

    class MotionTraceRenderExecuteScope {
    public:
        MotionTraceRenderExecuteScope(Player *player, void *renderLayerObject,
                                      bool skipUpdate,
                                      const std::vector<PreparedRenderItem *> &mainList,
                                      const char *boundary = "Player.renderToCanvas");
        ~MotionTraceRenderExecuteScope();

        MotionTraceRenderExecuteScope(const MotionTraceRenderExecuteScope &) = delete;
        MotionTraceRenderExecuteScope &operator=(const MotionTraceRenderExecuteScope &) = delete;

        void setResult(bool ok);

    private:
        Player *_player = nullptr;
        void *_renderLayerObject = nullptr;
        bool _skipUpdate = false;
        bool _ok = false;
        const char *_boundary = nullptr;
        const std::vector<PreparedRenderItem *> *_mainList = nullptr;
    };

    void motionTraceRenderPrepareEnter(Player *player);
    void motionTraceRenderPrepareLeave(
        Player *player, bool ok,
        const std::vector<PreparedRenderItem *> &mainList,
        const std::vector<PreparedRenderItem *> &auxList);
    void motionTraceRenderApplyProjectionEnter(Player *player);
    void motionTraceRenderApplyProjectionLeave(
        Player *player,
        const std::vector<PreparedRenderItem *> &mainList);
    void motionTraceRenderBuildItemsEnter(Player *player,
                                          std::uint32_t inheritedColor,
                                          bool inheritedDrawFlag19,
                                          bool inheritedFlag18);
    void motionTraceRenderBuildItemsLeave(
        Player *player,
        const std::vector<PreparedRenderItem *> &mainList,
        const std::vector<PreparedRenderItem *> &auxList);
    void motionTraceRenderBuildCommandsEnter(Player *player,
                                             int canvasWidth,
                                             int canvasHeight,
                                             const std::vector<PreparedRenderItem *> &mainList,
                                             const std::vector<PreparedRenderItem *> &auxList);
    void motionTraceRenderBuildCommandsLeave(Player *player,
                                             int canvasWidth,
                                             int canvasHeight,
                                             const std::vector<PreparedRenderItem *> &mainList,
                                             const std::vector<PreparedRenderItem *> &auxList);
    void motionTraceRenderImageCheckpoint(Player *player,
                                          void *renderLayerObject,
                                          const char *phase,
                                          const char *samplePoint);
    void motionTraceBeginAccurateSlaRender(Player *player,
                                           void *renderTargetObject);
    void motionTraceEndAccurateSlaRender(Player *player,
                                         void *renderTargetObject);
    bool motionTraceIsAccurateSlaRenderActive();
    void motionTraceRecordPostDrawLayerCandidate(Player *player,
                                                 void *layerObject,
                                                 const char *samplePoint);
    void motionTraceRenderDirectExecuteProbe(Player *player,
                                             const char *samplePoint,
                                             const char *payload);
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
                                         bool visibleCheck);
    void motionTraceLayerRawProbe(Player *player, void *renderLayerObject,
                                  const char *samplePoint);
    void motionTraceLayerRawProbeNative(Player *player, const void *nativeLayer,
                                        const char *samplePoint);
#endif

} // namespace motion::detail
