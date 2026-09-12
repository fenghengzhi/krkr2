// EmoteEngine — local typed reconstruction of the native emote engine.
//
// The four reference ABIs share this ownership chain despite different object
// sizes and offsets:
//   D3DEmotePlayer -> EmoteObject -> EmoteEngine -> Player
//
// Four-reference ownership rules recovered here:
//   - the seven direct-controller fields are single-pointer std::unique_ptr
//     owners; their library-specific destructor/reset ordering explains the
//     ctor-unwind and normal-dtor owner-slot helpers in all four targets
//   - the Player field is the same single-pointer std::unique_ptr owner
//   - ten typed deques retain their distinct entry shapes, roles and lifetimes
//   - the progress dirty byte is Engine state, not a Player member
//   - the Engine owns an internal metadata/controller scale pair which is
//     independent from Player::meshDivisionRatio
//   - the seven inline unordered containers are typed fields: three
//     unordered_set<ttstr> instances and four maps. Their physical offsets are
//     ABI-specific and belong in the four-reference recovery notes, not in
//     portable source-level field names
//   - the four adjacent vector<ttstr> fields are likewise logical members whose
//     physical offsets vary by ABI
// Exact object sizes, container-header layouts and member offsets for all four
// targets are recorded in analysis/; this header states the common source-level
// structure and lets the target standard library provide its native ABI.

#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tjs.h"
#include "EmoteSpring.h"
#include "EmoteVarController.h"
#include "EmoteAngleController.h"
#include "EmoteBlinkController.h"
#include "EmoteEyebrowController.h"
#include "EmoteMouthController.h"
#include "EmoteSelectorController.h"
#include "EmoteLoopController.h"
#include "EmoteWindEmitter.h"
#include "internal/player_containers.h"
#include "internal/ttstr_hash.h"

namespace motion {

    class Player;
    class ResourceManager;

    namespace internal {

        // The controller-slice cap keeps the live remaining interval as the
        // first operand. This matters for the dirty-forced unordered case:
        // all four references propagate a NaN remaining value into the
        // controller step instead of replacing it with the 1.1 cap.
        [[nodiscard]] double controllerSliceTime_guess(
            double remaining) noexcept;

    } // namespace internal

    // ========================================================================
    // EmoteEngine inline-container value typedefs reconstructed from the four
    // current Android/iOS reference binaries.
    //
    // All seven unordered containers share the KiriKiri ttstr Hint cache and
    // the exact 1025/6/9/32769/11 mix in detail::ttstr_hash_utf16. A null ttstr
    // hashes to zero; a non-null empty string receives the non-zero sentinel.
    //
    // The controller-reference and variable-value maps use the same platform
    // unordered-map header family but occupy ABI-specific offsets. Android
    // old-libstdc++ uses 56/28-byte headers; iOS libc++ uses 40/20-byte headers.
    // Android's old libstdc++ default construction requests ten buckets and
    // selects the eleven-bucket prime; libc++ starts with no bucket allocation.
    // Those are library implementation details of the same source-level
    // default constructor and must not be forced into the portable port.
    //
    // VALUE TYPES (evidence status):
    //   variable values = <ttstr,double>. get-or-insert returns the
    //     mapped double, initially +0.0, and every writer overwrites it. The
    //     destructor releases only the key ttstr before deleting each node.
    //   controller refs = <ttstr,EmoteVarRef>. Metadata builders write
    //     {type,index}; setVariable reads that pair and selects the
    //     corresponding controller deque. The value is an unowned 8B POD.
    //   variable ranges: every node owns both its key ttstr and a second
    //     same-label ttstr in the mapped value, followed by four doubles. The
    //     first pair is constructed as DBL_MAX/-DBL_MAX but has no reader; the
    //     builder reads and updates only the last pair, which is source-level
    //     indeterminate until the first frame update. Clear releases both
    //     string references before deleting the node.
    //   The timeline-state map owns the raw timeline element, two single-pointer
    //     owners, scalar playback state and the frame-cursor vector. Its mapped
    //     value is 112/88/112/84 bytes across the four ABIs; hash-node prefix
    //     and tail layout follow the platform STL implementation.
    //
    // PLATFORM_BOUNDARY: libc++ unordered_map header (~32-40B) != libstdc++ 56B
    // and libc++ vector (24B, matches). sizeof(EmoteEngine) on the Web build can
    // therefore no longer equal 1496B exactly. User-accepted trade-off (same
    // posture as player_containers.h): we align typed K/V semantics + shared
    // hash + lifetime, not byte-level 1496B. Offset comments are for trace only.
    // ========================================================================
    namespace detail {

        // The mirror-match positive/negative caches use the same
        // unordered_set<ttstr> specialization as instant-variable labels; no
        // mapped-value slot. Native node layout is an STL ABI boundary:
        // Android old-libstdc++ stores {next,key,hash}, while iOS libc++ stores
        // {next,hash,key}; pointer width makes the nodes 24/12 bytes. Keep
        // distinct aliases for the source roles.
        using EmoteMirrorMatchSet =
            std::unordered_set<ttstr, ttstr_hash, ttstr_equal>;
        using EmoteMirrorMissSet =
            std::unordered_set<ttstr, ttstr_hash, ttstr_equal>;

        // Controller-reference value {int32 type; int32 index}, verified by the
        // four-reference eye builder (writes type=4 and index=loopIndex) and the
        // other category builders (type 5/6/7/8). The setVariable reader
        // consumes type@+0/index@+4 and dispatches into
        // the matching controller deque by index. Corrects the header's prior
        // old `double` placeholder (the dtor releases only the ttstr key, so
        // the value is a non-owned POD — an 8B {type,index} pair fits exactly).
        struct EmoteVarRef {
            int32_t type  = 0; // +0 — controller category tag (4 = eye)
            int32_t index = 0; // +4 — loop index into the category deque
        };
        static_assert(sizeof(EmoteVarRef) == 8,
                      "controller references are two adjacent int32s");
        using EmoteVarRefMap =
            std::unordered_map<ttstr, EmoteVarRef, ttstr_hash, ttstr_equal>;

        // Variable-range mapped value. On every insertion miss the node receives
        // two CopyRefs of the same label: one becomes the map key and one this
        // value's labelCopy_guess. The constructor writes only the first double
        // pair. frameMin/frameMax are deliberately not initialized: all four
        // builders read and update those indeterminate fields directly. Do not
        // add a defensive zero/limit seed; the references do not contain one.
        struct EmoteVariableRange {
            ttstr  labelCopy_guess;
            double unusedMinSeed_guess;
            double unusedMaxSeed_guess;
            double frameMin;
            double frameMax;

            explicit EmoteVariableRange(const ttstr &label)
                : labelCopy_guess(label),
                  unusedMinSeed_guess(std::numeric_limits<double>::max()),
                  unusedMaxSeed_guess(-std::numeric_limits<double>::max()) {}
        };
        using EmoteVariableRangeMap =
            std::unordered_map<ttstr, EmoteVariableRange,
                               ttstr_hash, ttstr_equal>;

        // The four builders create each 24B frame as {double time,
        // bool typeZero, float value, double easingWeight}. The final frame is
        // a sentinel: the four current timeline seek/window
        // implementations scan through size()-1 and never dispatch that tail.
        struct EmoteTimelineFrame24B {
            double time = 0.0;
            bool typeZero = false;
            float value = 0.0f;
            double easingWeight = 0.0;
        };

        // Source-level deque element. Its natural ABI size is 56 bytes on both
        // 64-bit references and 28 bytes on both 32-bit references.
        struct EmoteTimelineTrack {
            ttstr label;
            bool instantVariable = false;
            std::vector<EmoteTimelineFrame24B> frameList;
            std::unique_ptr<EmoteVarController> controller;
            float output = 0.0f;

            EmoteTimelineTrack() = default;
        };

        // The object is exactly the platform std::deque header: 0x50/0x28 on
        // Android arm64/armv7 and 0x30/0x18 on iOS arm64/armv7.
        struct EmoteTimelineData {
            std::deque<EmoteTimelineTrack> variableList;
        };

        // Timeline-state mapped value, recovered jointly from the four-reference insert,
        // builder, timeline consumers, contribution accumulator and destructor.
        // The source-level fields intentionally do not hard-code one platform's
        // node offsets or standard-library layout.
        struct EmoteTimelineState {
            std::unique_ptr<EmoteTimelineData> timelineData;
            std::unique_ptr<EmoteVarController> blendController;
            tjs_uint32 flags = 0;
            tTJSVariant rawElement;
            double loopBegin = 0.0;
            double loopEnd = 0.0;
            double lastTime = 0.0;
            double currentTime = 0.0;
            float blendWeight = 1.0f;
            double autoStop = 0.0;
            std::vector<int32_t> frameCursors;

            EmoteTimelineState() = default;
        };
        using EmoteTimelineStateMap =
            std::unordered_map<ttstr, EmoteTimelineState,
                               ttstr_hash, ttstr_equal>;

        // Instant-variable labels form an unordered_set, not a map. Native table
        // headers and owned-key nodes follow the two platform STL ABIs. Exact
        // header/node order, offsets, node sizes and rehash boundaries are
        // recorded in the variable-container-tail analysis note.
        using EmoteInstantVariableSet =
            std::unordered_set<ttstr, ttstr_hash, ttstr_equal>;

    } // namespace detail

    // ============================================================================
    // 10 deque element POD types (per binary spec).
    // Each type is distinct (CLAUDE.md hard rule: no uniform abstraction).
    // The controller and physics records below now use named fields. Reference
    // strides remain provenance for container analysis, not wasm size contracts.
    // ============================================================================

    // Deque #1 — owning simple-spring nodes populated from `bustControl`.
    // Each node owns one 72-byte
    // spring state and carries the init flag, shape label, two value-map keys, and the
    // previous resolved anchor. The reference ABI stride is 48B on arm64 and
    // 28B on armv7; the per-file offset/block table lives in analysis/. Normal
    // C++ field layout is intentional here.
    struct EmoteHairPartsNode48B {
        EmoteHairPartsNode48B() = default;
        explicit EmoteHairPartsNode48B(EmoteSpringState* rawSpring)
            : spring(rawSpring), initFlag(1), anchorX(0.0f), anchorY(0.0f) {}

        std::unique_ptr<EmoteSpringState> spring;
        uint8_t           initFlag;
        ttstr             shapeLabel;
        ttstr             keyX;
        ttstr             keyY;
        float             anchorX;
        float             anchorY;
    };
    static_assert(sizeof(EmoteHairPartsNode48B) ==
                      (sizeof(void*) == 8 ? 48u : 28u),
                  "simple-spring deque entry must keep the reference ABI stride");

    // Deque #2 — first chain family. Each entry owns one chain spring and four
    // ttstr values, retains one intentionally unwritten init byte, and caches
    // the last resolved anchor. keyA/keyB/keyC receive segment1-X, segment0-X,
    // and the selected Y output respectively. Per-ABI layouts live in analysis/.
    struct EmoteBustChain1Node56B {
        EmoteBustChain1Node56B() = default;
        explicit EmoteBustChain1Node56B(EmoteBustChainSpring* rawSpring)
            : spring(rawSpring), anchorX(0.0f), anchorY(0.0f) {}

        std::unique_ptr<EmoteBustChainSpring> spring;
        uint8_t               initFlag;
        ttstr                 shapeLabel;
        ttstr                 keyA; // variable-value key (segment 1 X)
        ttstr                 keyB; // variable-value key (segment 0 X)
        ttstr                 keyC; // variable-value key (selected Y)
        float                 anchorX;
        float                 anchorY;
    };
    static_assert(sizeof(EmoteBustChain1Node56B) ==
                      (sizeof(void*) == 8 ? 56u : 32u),
                  "chain-spring deque entry must keep the reference ABI stride");

    // Deque #3 — Bust chain #2 spring nodes (same entry type as #2).
    using EmoteBustChain2Node56B = EmoteBustChain1Node56B;

    // Deque #4-#10 — distinct variable/state-machine record types. Their builder
    // and step paths are live; do not collapse them into one uniform abstraction.
    // Deque #4 (eye, TYPE 4) element — verified by all four current eye
    // builders, range destructors and progress loops. The first field is a
    // single-pointer owner, followed by the label; reverse member destruction
    // therefore releases label before deleting the controller. The raw-pointer
    // constructor matches the native emplace path: ownership begins only when
    // the destination deque element is constructed.
    struct EmoteEyeControlEntry_Deque4 {
        explicit EmoteEyeControlEntry_Deque4(
            EmoteBlinkController *controller = nullptr)
            : ctl(controller) {}

        EmoteEyeControlEntry_Deque4(
            EmoteBlinkController *controller, const ttstr &outputLabel)
            : ctl(controller), label(outputLabel) {}

        std::unique_ptr<EmoteBlinkController> ctl; // single-pointer owner
        ttstr label;                               // PSB "label" / value key
    };
    // Deque #5 (eyebrow, TYPE 5) independently repeats deque #4's owner shape:
    // raw-pointer destination construction, then label assignment; destruction
    // releases label before deleting the slim controller through its one-pointer
    // owner. Its two source members are both pointer-width representations.
    struct EmoteEyebrowControlEntry_Deque5 {
        explicit EmoteEyebrowControlEntry_Deque5(
            EmoteEyebrowController *controller = nullptr)
            : ctl(controller) {}

        EmoteEyebrowControlEntry_Deque5(
            EmoteEyebrowController *controller, const ttstr &outputLabel)
            : ctl(controller), label(outputLabel) {}

        std::unique_ptr<EmoteEyebrowController> ctl; // one-pointer owner
        ttstr label;                                  // PSB "label" / value key
    };
    // Deque #6 (mouth, TYPE 6) independently uses the same one-pointer owner
    // construction as #4/#5, followed by two published ttstr keys. Reverse
    // member destruction therefore releases talkLabel, then label, then the
    // controller. The raw-pointer constructor matches the native emplace path.
    struct EmoteMouthControlEntry_Deque6 {
        explicit EmoteMouthControlEntry_Deque6(
            EmoteMouthController *controller = nullptr)
            : ctl(controller) {}

        EmoteMouthControlEntry_Deque6(
            EmoteMouthController *controller,
            const ttstr &outputLabel,
            const ttstr &animatedOutputLabel)
            : ctl(controller), label(outputLabel),
              talkLabel(animatedOutputLabel) {}

        std::unique_ptr<EmoteMouthController> ctl; // one-pointer owner
        ttstr label;       // beginFrame / first value key
        ttstr talkLabel;   // currentValue / second value key
    };
    static_assert(sizeof(EmoteMouthControlEntry_Deque6) ==
                      3 * sizeof(void *),
                  "mouth deque entry must remain three pointer-width fields");
    // Deque #7 (clampControl) entry. Each target ABI naturally aligns the two
    // doubles within this common source field sequence. Only metadata elements
    // whose `enabled` property is true are appended. Runtime
    // evaluation reads two Engine variable values, adds active timeline-track
    // contributions, normalizes them over [min,max], performs the selected 2D
    // remap and binds both values into the embedded Player. The LR result alone
    // is subject to mirror negation.
    struct EmoteClampControlEntry_Deque7 {
        int    type     = 0;   // disk-remap mode (0 = squircle, 1 = clamp-circle)
        double minValue = 0.0;
        double maxValue = 0.0;
        ttstr  varLr;          // X-axis key (`var_lr`)
        ttstr  varUd;          // Y-axis key (`var_ud`)
    };
    // Transition (TYPE 7) entry. ctl is a single-pointer owner; reverse member
    // destruction releases label before the controller.
    // flag starts at one, gates direct writes, and is cleared when a selector
    // borrows this controller; per-frame stepping does not inspect the flag.
    struct EmoteTransitionControlEntry_Deque8 {
        EmoteTransitionControlEntry_Deque8() = default;
        explicit EmoteTransitionControlEntry_Deque8(
            EmoteVarController *controller) : ctl(controller) {}
        EmoteTransitionControlEntry_Deque8(
            EmoteVarController *controller, const ttstr &outputLabel,
            uint8_t directWriteFlag)
            : ctl(controller), label(outputLabel), flag(directWriteFlag) {}

        std::unique_ptr<EmoteVarController> ctl;
        ttstr                               label;
        uint8_t                             flag = 1;
    };
    static_assert(sizeof(EmoteTransitionControlEntry_Deque8) ==
                      3 * sizeof(void *),
                  "transition entry must remain three pointer-width words");
    // Selector (TYPE 8) entry. It owns ctl; ctl's options in turn borrow
    // transition controllers. targets is a separate dormant, non-owning
    // relationship used by selector sync/target APIs. All four builders create
    // it empty and the plugin contains no writer, so the three target APIs cannot
    // match anything during a normal native object lifetime.
    struct EmoteSelectorControlEntry_Deque9 {
        EmoteSelectorControlEntry_Deque9() = default;
        explicit EmoteSelectorControlEntry_Deque9(
            EmoteSelectorController *controller) : ctl(controller) {}
        EmoteSelectorControlEntry_Deque9(
            EmoteSelectorController *controller, const ttstr &outputLabel)
            : ctl(controller), label(outputLabel) {}

        std::unique_ptr<EmoteSelectorController> ctl;
        ttstr                                    label;
        // The native metadata builder leaves this direct-enqueue gate
        // uninitialised. Keep the boundary explicit; it is not inherited from a
        // transition entry and targets must not be inferred from option labels.
        uint8_t                                   flag;
        std::vector<EmoteTransitionControlEntry_Deque8*> targets;
    };
    static_assert(sizeof(EmoteSelectorControlEntry_Deque9) ==
                      6 * sizeof(void *),
                  "selector entry must remain six pointer-width words");
    // Owning loopControl entry. The builder constructs the destination from a
    // raw pointer and assigns label afterwards. Reverse member destruction
    // releases label before the controller's keyframe backing and object.
    struct EmoteLoopControlEntry_Deque10 {
        EmoteLoopControlEntry_Deque10() = default;
        explicit EmoteLoopControlEntry_Deque10(
            EmoteLoopController *controller) : ctl(controller) {}
        EmoteLoopControlEntry_Deque10(
            EmoteLoopController *controller, const ttstr &outputLabel)
            : ctl(controller), label(outputLabel) {}

        std::unique_ptr<EmoteLoopController> ctl;
        ttstr label;
    };

    // ============================================================================
    // EmoteEngine — non-polymorphic source declaration shared by all four ABIs.
    // Its sole destructor is an ordinary, non-virtual destructor: the four
    // references contain no Engine deleting-destructor entry. Allocation owners
    // (EmoteObject, the NCB adaptor and constructor-failure cleanup) each perform
    // the equivalent of this destructor followed by scalar operator delete.
    // Physical object-size and member-offset tables live in analysis/.
    // ============================================================================
    class EmoteEngine {
    public:
        // The four current constructors receive the RM dispatch wrapper and
        // forward it to the single-input Player constructor. The engine does
        // not own the native RM; it just passes the dispatch down.
        explicit EmoteEngine(const tTJSVariant &rmDispatch);
        ~EmoteEngine();

        EmoteEngine(const EmoteEngine&) = delete;
        EmoteEngine& operator=(const EmoteEngine&) = delete;

        Player& player();
        const Player& player() const;

        // Emote facade variable reader. Scope-owned labels bypass the join
        // snapshot; every other label reads the Player join snapshot before
        // Player's direct HM1/HM2 bound-value getter.
        [[nodiscard]] double getVariable(ttstr label);

        // Consumes a by-value metadata Variant and rebuilds every metadata-owned
        // controller, deque, hashmap and TJS container in the common order of
        // the four current references. The Primary initPhysics descriptor binds
        // this member directly.
        void applyMetadata_guess(tTJSVariant metadata);

        // Four-reference metadata-reset chain. Clears all metadata-owned
        // controller/container state, recreates the three TJS variable
        // containers, and clears instant labels, ranges and controller refs.
        void resetMetadataState();

        // Builds the label Array, per-label frame Arrays/Dictionary and range
        // records directly from raw TJS dispatch values.
        void buildVariableList_guess(const tTJSVariant &variableList);

        // Retains the current label Array through the script Array's remove
        // method call for one variable label.
        void removeVariableLabel_guess(const ttstr &label);

        // Publishes a fresh public variable-key Array, copies the current
        // label deque into it, synchronizes every selector gate/controller and
        // its non-owning transition-target vector, then marks the engine dirty.
        void syncSelectorControls_guess();

        // Registered EmotePlayer selector-target surface. These functions scan
        // selector-entry.targets, not a decoded motion registry. activate and
        // deactivate are separate native functions which differ at the selector
        // entry's direct-control gate write.
        [[nodiscard]] bool isSelectorTarget(ttstr label);
        void activateSelectorTarget(ttstr label);
        void deactivateSelectorTarget(ttstr label);

        // Commits pending timeline/controller values, clears their queues and
        // re-arms every spring node in the native family order.
        void resetControllers_guess();

        // Reports Engine-owned animation activity. Active timeline variable
        // labels suppress matching standalone controller activity; this is
        // intentionally independent of Player's motion-node playback flag.
        [[nodiscard]] bool getAnimating_guess() const;

        // Stores the external request, compares it with the metadata base,
        // forwards the XOR to the canonical Player flip-X setter, then performs
        // the complete controller reset above.
        void setMirror_guess(bool mirror);

        // Routes the exact, case-sensitive outer-force labels "bust", "hair"
        // and "parts" to the three Engine-owned two-channel controllers.
        // Unknown labels are a silent no-op. `power` is already normalized by
        // the caller when the script-facing ease convention is used.
        void setOuterForceTarget_guess(const ttstr &label, double x, double y,
                                       double duration, double power);

        // Shared five-float wind entry point used by both D3D startWind and the
        // all-zero stopWind wrapper. The stop predicate differs by pointer
        // width; see the four-reference audit linked from the implementation.
        void setWind_guess(float minAngle, float maxAngle, float amplitude,
                           float freqX, float freqY);

        // dt-sliced physics + animation main loop. The controller and spring
        // step paths called by this loop are implemented; see their individual
        // four-target audits before changing edge behavior.
        void progress(double dt);

        // EmoteEngine_progress calls this once with (force=false, original dt)
        // before entering its std::min(dt, 1.1) controller-slice loop.
        void preProgress_guess(bool force, double dt);

        // Motion.EmotePlayer state persistence. Player has no parallel save
        // model; the NCB members bind to these engine receivers on all four
        // current reference ABIs.
        [[nodiscard]] tTJSVariant serializeState_guess();
        void unserializeState_guess(tTJSVariant data);
        [[nodiscard]] tTJSVariant serializeTimelineState_guess();
        // Each collection serializer returns a fresh native Array in the
        // corresponding controller-deque order. Eye/Eyebrow/Mouth/Selector
        // publish accessor-owned fresh Dictionaries directly as Object
        // closures; Transition instead transfers the Var-state Variant through
        // a retained accessor, destroys that Variant, and appends label last.
        // Property-set status is ignored and completed items are not exposed if
        // a later exception prevents the Array Variant from being returned.
        [[nodiscard]] tTJSVariant serializeEyeState_guess() const;
        [[nodiscard]] tTJSVariant serializeEyebrowState_guess() const;
        [[nodiscard]] tTJSVariant serializeMouthState_guess() const;
        [[nodiscard]] tTJSVariant serializeTransitionState_guess() const;
        [[nodiscard]] tTJSVariant serializeSelectorState_guess() const;
        [[nodiscard]] tTJSVariant serializeBaseState_guess() const;
        [[nodiscard]] tTJSVariant serializeOuterForceState_guess() const;
        void restoreTimelineState_guess(tTJSVariant value);
        void restoreEyeState_guess(const tTJSVariant &value);
        void restoreEyebrowState_guess(const tTJSVariant &value);
        void restoreMouthState_guess(const tTJSVariant &value);
        void restoreTransitionState_guess(const tTJSVariant &value);
        void restoreSelectorState_guess(const tTJSVariant &value);
        void restoreBaseState_guess(const tTJSVariant &value);
        void restoreOuterForceState_guess(const tTJSVariant &value);

        // Engine-owned timeline machine. The state table and active-label
        // vector are independent: an empty query means "is anything active",
        // while an empty stop clears every active label but retains capacity.
        void playTimeline_guess(const ttstr &label, tjs_uint32 flags);
        void stopTimeline_guess(const ttstr &label);
        [[nodiscard]] bool isTimelinePlaying_guess(const ttstr &label) const;
        void setTimelineBlendController_guess(
            const ttstr &label, float value, float transition,
            float easingWeight, bool autoStop);
        // Flushes the remaining non-loop timeline keyframes. Parallel timelines
        // enter a 20-frame auto-stop fade and stay active; ordinary timelines
        // are removed after their remaining frames are enqueued.
        void passTimelines_guess();
        void fadeInTimeline_guess(
            const ttstr &label, float duration, float easingWeight);
        [[nodiscard]] double getTimelineBlendRatio_guess(ttstr label) const;
        [[nodiscard]] tjs_int countMainTimelines_guess() const;
        [[nodiscard]] ttstr getMainTimelineLabelAt_guess(
            tjs_uint32 index) const;
        [[nodiscard]] tjs_int countDiffTimelines_guess() const;
        [[nodiscard]] ttstr getDiffTimelineLabelAt_guess(
            tjs_uint32 index) const;
        [[nodiscard]] tjs_int countPlayingTimelines_guess() const;
        [[nodiscard]] ttstr getPlayingTimelineLabelAt_guess(
            tjs_uint32 index) const;
        [[nodiscard]] tjs_int getPlayingTimelineFlagsAt_guess(
            tjs_uint32 index) const;
        [[nodiscard]] bool getLoopTimeline_guess(ttstr label) const;
        [[nodiscard]] double getTimelineTotalFrameCount_guess(
            ttstr label) const;
        [[nodiscard]] tTJSVariant getMainTimelineLabelList_guess() const;
        [[nodiscard]] tTJSVariant getDiffTimelineLabelList_guess() const;
        [[nodiscard]] tTJSVariant getPlayingTimelineInfoList_guess() const;
        void accumulateTimelineContribution_guess(
            const ttstr &label, double &value);

        // Steps the 4 direct controllers (pos/scale/color/angle) and applies
        // their outputs to the embedded Player through the live coordinate,
        // slant, color and angle setters.
        void stepRootControllers_guess(float dt);

        // Resolves one raw layer label to the point geometry exposed by its
        // LayerGetter and writes the mesh-division-adjusted crossed outputs.
        // Callers pass their embedded label by const reference; every failure
        // path leaves both outputs untouched.
        bool resolveShapeAnchor_guess(const ttstr &label,
                                      float *outX, float *outY);

        // Iterates deque #1 (_hairPartsNodes), resolves each shape anchor, then
        // drives the 72-byte EmoteSpringState and writes both angles into values.
        // The two scratch outputs intentionally remain uninitialized when a
        // non-first node receives dt <= 0.0001, matching all four references.
        void stepHairParts(float dt);

        // Steps one chain family using its script-visible scale value.
        //   stepBust(ctlTarget, chainNodes, scale, dt): iterates a bust
        //   chain deque (#2 or #3); per node resolves the "shape" anchor then
        //   drives EmoteBustChainSpring via its two-stage solver/post-bend flow,
        //   writing three angle outputs into the variable-value map. Scratch remains
        //   uninitialized on the same boundary paths as all four references.
        void stepBust(EmoteVarController* ctlTarget,
                      std::deque<EmoteBustChain1Node56B>& chainNodes,
                      double scale, float dt);

        // Common four-reference eye-control builder semantics.
        //   For each enabled element in the metadata-base "eyeControl" PSB array:
        //   allocate an EmoteBlinkController, run its ctor over the element,
        //   push the raw owner with an empty label onto deque#4, assign label,
        //   then register VarRef {type=4,index=metadataIndex}.
        //   `eyeControl` is the PSB list (= the binary's L"eyeControl" value
        //   passed by metadata application; iterated by index, count via
        //   Motion_propGetCount).
        void buildEyeControl_guess(const tTJSVariant& eyeControl);

        // Common four-reference eyebrow builder. It has the same outer shape
        // as buildEyeControl, but allocates the slim Eyebrow controller, pushes
        // a raw owner with an empty label onto deque#5, assigns label, and only
        // then registers {type=5,index=metadataLoopIndex}.
        void buildEyebrowControl_guess(const tTJSVariant& eyebrowControl);

        // Common four-reference mouth builder. For every enabled metadata
        // element it constructs the controller, appends {owner,empty,empty} to
        // deque#6, assigns label and then talkLabel, and publishes those two
        // map keys in the same order as {type=6,index=metadataIndex}. Disabled
        // elements leave holes in the stored index. Equal, duplicate and empty
        // keys are accepted; the later map write wins.
        void buildMouthControl_guess(const tTJSVariant& mouthControl);

        // Four-reference selector builder. It reads the selector label before
        // enabled, removing disabled labels from the raw variable list. Every
        // option performs an independent first-match scan of transition deque#8,
        // borrows (never owns) the matched controller, clears that entry's
        // direct-write flag, and removes the option label. The selector ctor
        // immediately applies index 0 in option order. deque#9 raw-emplace leaves
        // its separate gate byte indeterminate; map refs retain metadata indices.
        // The setter later indexes deque#9 directly with that sparse value, so
        // disabled holes preserve the native unchecked-index boundary.
        void buildSelectorControl_guess(const tTJSVariant &selectorControl);

        // Four-reference transition builder. Enabled metadata creates one
        // owning scalar controller, raw-appends {owner,empty label,flag=1}, then
        // assigns label and publishes {type=7,index=metadataIndex}. Disabled
        // entries leave sparse indices; duplicate and empty labels are accepted.
        // The setter directly uses that index against compacted deque#8, retaining
        // the native unchecked disabled-hole boundary. Must precede selector build.
        void buildTransitionControl_guess(const tTJSVariant &transitionControl);

        // Builds enabled loopControl entries from transitionList float triples,
        // then registers `var_loop` with controller type 3 and the original metadata
        // index. It appends without a builder-local clear.
        void buildLoopControl_guess(const tTJSVariant &loopControl);

        // Snapshots Count once. Disabled raw elements leave no placeholder or
        // retained sparse index. For each enabled element, first append a
        // zero-valued entry, then fill type, var_lr, var_ud, min and max in
        // source order. The builder owns the two ttstr slots through deque#7;
        // it neither clears the deque nor allocates/registers a controller.
        // Failure during a post-emplace read preserves the partial entry.
        void buildClampControl_guess(const tTJSVariant &clampControl);

        // Reads mirrorControl.variableMatchList through its dedicated global
        // hint and appends every element as a ttstr to the Engine vector,
        // preserving order, duplicates and empty strings. Reset owns
        // clearing/releasing the vector; this builder does not clear it itself.
        void buildMirrorControl_guess(const tTJSVariant &mirrorControl);

        // Inserts each raw array element, converted directly to ttstr, into the
        // instant-variable set. The outer metadata function owns the optional
        // property gate; this builder performs no filtering or local clear.
        void buildInstantVariableList_guess(
            const tTJSVariant &instantVariableList);

        // Clears/fills the main and diff label vectors with the original
        // two-read diff-property path, then retains each complete raw element
        // in timelineStates[label]. Duplicate labels remain in the vectors
        // while the last raw element wins; active labels and old map-only keys stay.
        void buildTimelineControl_guess(const tTJSVariant &timelineControl);
        void appendTimelineControlEntries(const tTJSVariant &timelineControl);

        void initializeTimelineState_guess(
            detail::EmoteTimelineState &state);
        void initializeTimelineControllers_guess(
            detail::EmoteTimelineState &state, tjs_uint32 flags);
        void seekTimeline_guess(
            detail::EmoteTimelineState &state, double time);
        void applyTimelineWindow_guess(
            detail::EmoteTimelineState &state, bool inclusive,
            double targetTime);

        // Uses mirrorChanged, the raw variableMatchList and two Engine-owned
        // ttstr sets as positive/negative caches. Match requires the first
        // IndexOf(pattern, 0) result to be >= 1; a prefix match wins over and
        // therefore masks any later occurrence.
        bool shouldMirrorLabel_guess(const ttstr &label);
        void applyClampControls_guess();

        // Despite the PSB key name "bustControl", this populates deque #1 with
        // the simple spring consumed by stepHairParts, not either chain deque.
        // Count is snapshotted once. Disabled elements are omitted from the
        // deque but retain their original sparse metadata indices in refs. For
        // each enabled element, construct the 72-byte state from the outer
        // element, then overwrite the spring's vec3 fields from nested `param`:
        //   "op"/"p"/"pv" (each a dict x/y/z -> storedXYZ/posXYZ/velXYZ) and
        //   "ofs" -> biasY; node.initFlag = 1; node labels = baseLayer(shape),
        //   var_lr (X key), var_ud (Y key); register two refs {type=0,
        //   index=originalMetadataIndex} keyed by var_lr and then var_ud. Empty,
        // equal and duplicate keys are accepted; later publication overwrites
        // the map value without removing an earlier deque owner.
        void buildBustControl_guess(const tTJSVariant& bustControl);

        // Four-reference "hairControl" / "partsControl" builder; tag=1 feeds
        //   deque#2 and tag=2 feeds deque#3. Count is snapshotted once; disabled
        //   rows do not append but retain their sparse metadata indices in refs.
        //   Each enabled element allocates the spring, runs its argument
        //   constructor on the outer element, then overwrites from nested param:
        //   op/ofs/bendR/bendS and the p/pv/bp 2-segment arrays.
        //   Node labels are assigned in
        //   baseLayer(shape), var_lr (keyA), var_lrm (keyB), var_ud (keyC);
        //   then refs are published lr/lrm/ud as
        //   {type=tag,index=originalMetadataIndex}. Empty/equal/duplicate keys
        //   are accepted and later writes win without removing earlier owners.
        //   The entry constructor intentionally leaves its init byte and ABI
        //   padding unwritten, while default-constructing labels and zeroing
        //   both anchors.
        void buildChainControl_guess(
            std::deque<EmoteBustChain1Node56B>& chainNodes,
            const tTJSVariant& chainControl, int typeTag);

        // Four-reference EmoteEngine variable router. Current addresses and
        // per-ABI layouts live in the set-variable analysis note. A HM6 hit
        // marks dirty before dispatch; actual controller calls lazily narrow
        // the three doubles after category gates. A ref miss, or type 0/1/2
        // while directEdit is set, falls through to the variable-value map.
        void setVariable(const ttstr& key, double value, double easing,
                         double durationFrames);

    public:
        // ====== Native declaration order ======

        // deque #1 — Hair/Parts spring nodes
        std::deque<EmoteHairPartsNode48B>     _hairPartsNodes;
        // deque #2 — Bust chain #1 spring nodes
        std::deque<EmoteBustChain1Node56B>    _bustChain1Nodes;
        // deque #3 — Bust chain #2 spring nodes
        std::deque<EmoteBustChain2Node56B>    _bustChain2Nodes;
        // deque #4 — Eye blink controllers (TYPE 4). Element =
        //   {unique_ptr<EmoteBlinkController> ctl; ttstr label}. Populated by
        //   EmoteEngine::buildEyeControl; stepped each frame by
        //   EmoteBlinkController_step, writing the scalar
        //   result into variable values keyed by elem.label.
        std::deque<EmoteEyeControlEntry_Deque4> _stateMachineDeque4;
        // deque #5 — Eyebrow controllers (TYPE 5). Element =
        //   {unique_ptr<EmoteEyebrowController> ctl; ttstr label}. Populated by
        //   EmoteEngine::buildEyebrowControl; stepped each frame by
        //   EmoteEyebrowController_step, writing the scalar result into values
        //   keyed by elem.label.
        std::deque<EmoteEyebrowControlEntry_Deque5> _stateMachineDeque5;
        // Mouth controllers (TYPE 6). Each step publishes beginFrame under
        // label and currentValue under talkLabel.
        std::deque<EmoteMouthControlEntry_Deque6> _compositeVarDeque6;
        // Clamp-control pool. It has no per-slice controller step; it is consumed
        // once after the variable-value bind loop and before Player progress.
        std::deque<EmoteClampControlEntry_Deque7> _clampControlDeque7;
        // Transition controllers (TYPE 7). The deque owns each controller;
        // progress writes its scalar output into values under entry.label.
        std::deque<EmoteTransitionControlEntry_Deque8> _auxVarDeque8;
        // Selector controllers (TYPE 8). The deque owns each selector; progress
        // writes selectedIndex as a float into values under entry.label.
        std::deque<EmoteSelectorControlEntry_Deque9> _vectorVarDeque9;
        // Loop controllers (controller-ref type 3). They run last among metadata
        // controller deques and publish their float curve blend, widened to
        // double, into variable values under entry.label.
        std::deque<EmoteLoopControlEntry_Deque10> _lookupCurvesDeque10;

        // Populated by buildMirrorControl_guess from variableMatchList. Each
        // push copies the ttstr ref; clear/dtor releases every element and the
        // vector buffer.
        std::vector<ttstr> _mirrorVariablePatterns;

        // Positive result cache for the mirror-label predicate. Clearing it
        // retains the unordered_set bucket allocation and policy state.
        detail::EmoteMirrorMatchSet _mirrorMatchCache;

        // Negative result cache for the mirror-label predicate. It is queried
        // after the positive cache and destroyed before it in reverse order.
        detail::EmoteMirrorMissSet _mirrorMissCache;

        // Timeline state table keyed by label. Its node value owns the raw
        // metadata reference, decoded tracks, blend controller and play flags.
        detail::EmoteTimelineStateMap _timelineStates;

        // Three independent contiguous ttstr vectors: declared main labels,
        // declared diff labels and currently playing labels. Rebuilding timeline
        // metadata clears only the first two; the playing vector is preserved.
        std::vector<ttstr> _timelineLabels;
        std::vector<ttstr> _timelineDiffLabels;
        std::vector<ttstr> _activeTimelineLabels;

        // One-pointer unique owner of an independently allocated Player. Each
        // ABI allocates its native Player size; construction failure destroys
        // the initialized Engine prefix.
        std::unique_ptr<Player> _player;

        // Direct controller owner, count=2 — Position (x,y)
        std::unique_ptr<EmoteVarController> _ctlPosition;
        // Direct controller owner, count=1 — Scale (uniform)
        std::unique_ptr<EmoteVarController> _ctlScale;
        // Direct controller owner, count=4 — Color RGBA
        std::unique_ptr<EmoteVarController> _ctlColor;
        // Angle/Rotation direct controller owner (shortest-path wrap)
        std::unique_ptr<EmoteAngleController> _ctlAngle;
        // Direct controller owner, count=2 — outer-force label "bust"
        std::unique_ptr<EmoteVarController> _ctlBustOuterForce;
        // Direct controller owner, count=2 — outer-force label "hair"
        std::unique_ptr<EmoteVarController> _ctlHairOuterForce;
        // Direct controller owner, count=2 — outer-force label "parts"
        std::unique_ptr<EmoteVarController> _ctlPartsOuterForce;

        // Deliberately raw, single-owner wind emitter. Rebuild first deletes the
        // old allocation and only overwrites this slot after the replacement is
        // fully initialized. That historical ordering leaves the slot dangling
        // if allocation throws, so unique_ptr would change a native edge case.
        // The pointer is also borrowed by bust/hair springs as collisionCurve.
        EmoteWindEmitter* _windEmitter = nullptr;

        // Wind parameter cache, written by setWind_guess. All five are floats;
        // normalized min/max are read back only while _windEmitter is non-null
        // to decide whether the emitter can be reused or must be rebuilt.
        float _windMin   = 0.f;
        float _windMax   = 0.f;
        float _windAmp   = 0.f;
        float _windFreqX = 0.f;
        float _windFreqY = 0.f;
        // Distinct requested, metadata-base and derived-XOR mirror bytes.
        // Metadata replacement refreshes the base, then reapplies the retained
        // external request through the derived byte.
        bool _mirrorRequested = false;
        bool _mirrorBase = false;
        bool _mirrorChanged = false;

        // Motion.EmotePlayer directEdit backing byte. This is Engine state, not
        // a motion::Player member: true lets scalar types 0..2 fall through to
        // variable values and suppresses the nonzero-dt physics-only pass.
        bool _directEdit = false;

        // Motion.EmotePlayer selectorEnabled backing byte. Construction enables
        // it. The public setter is a one-way trigger which also rebuilds public
        // variable keys and synchronizes every selector control.
        bool _selectorEnabled = true;

        // Motion.EmotePlayer queuing backing byte. Engine controller setters
        // pass it through as their append/replace flag; false replaces queued
        // work and true appends. It is distinct from Player's frame-queue byte.
        bool _queuing = false;

        // Progress main-loop dirty byte. Its source-level initial value is
        // false, then the constructor writes true immediately before each of
        // its four direct-controller seed calls. Android materializes the
        // initial zero beside the other trigger bytes; both current iOS
        // optimizers elide that dead store, so their first concrete write is
        // the true immediately before the position seed. A successfully
        // constructed Engine therefore exposes true on every target.
        bool _dirty = false;

        // Motion.EmotePlayer debugPrint backing byte. All current references
        // construct it false and expose a one-way setter, but contain no runtime
        // reader beyond the property getter.
        bool _debugPrintFlag = false;

        // Engine-internal metadata/controller scale pair. The first value is
        // loaded from metadata "scale" and divides wind coordinates; the
        // second is recomputed as 1/(metadataScale*controllerScale) and maps
        // shape anchors. Public meshDivisionRatio properties do not touch it.
        // Any gap after the preceding trigger bytes is compiler-provided
        // double alignment: it is four bytes on three references and absent on
        // iOS armv7, so there is no explicit source-level padding member.
        double _metadataScale = 1.0;
        double _inverseCombinedScale = 1.0;

        // Three consecutive script-visible scale values. The EmotePlayer and
        // D3DEmotePlayer member tables both expose this exact triplet. During
        // the nonzero, non-direct-edit physics tail, bustScale feeds the simple
        // spring nodes, while hairScale and partsScale feed the two chain
        // passes respectively. All four constructors initialize them to 1.0.
        double _hairScale = 1.0;
        double _partsScale = 1.0;
        double _bustScale = 1.0;

        // Three owning tTJSVariant fields, all initially Void. Metadata reset
        // creates the published base Array, CopyRefs that same dispatch into
        // the current-label field, then creates the frame-list Dictionary.
        // buildVariableList replaces only the current-label Array; selector
        // synchronization publishes a fresh base Array whose Items are copied
        // from current labels. Getter copies can therefore keep superseded
        // published Arrays alive after replacement or Engine destruction.
        tTJSVariant _variableLabelsBase;
        tTJSVariant _variableLabels;
        tTJSVariant _variableFrameLists;

        // Labels whose timeline tracks snapshot instant-update behavior.
        // Metadata reset clears nodes but retains the current bucket allocation.
        detail::EmoteInstantVariableSet _instantVariableLabels;

        // Per-label frame extrema accumulated while building variable metadata.
        // ABI-specific node layouts and constructor/clear paths are recorded in
        // analysis/motionplayer_variable_container_tail_semantic_names_four_binary_2026-08-15.md.
        detail::EmoteVariableRangeMap _variableRanges;

        // Routes a variable label to its controller category and deque index.
        detail::EmoteVarRefMap _variableControllerRefs;

        // Long-lived unordered_map<ttstr,double>. This deliberately uses the map
        // specialization as Player's label-value table, so native builds share
        // the same generated get-or-insert helper. Both standard libraries keep
        // a global forward node chain, but bucket activation, collisions and
        // rehashing determine its order; it is not a source-level insertion-order
        // guarantee. Existing-key assignment changes only the mapped double and
        // does not relink the node.
        detail::LabelValueMap _variableValues;

        // ===== End binary-layout fields =====

    };

} // namespace motion
