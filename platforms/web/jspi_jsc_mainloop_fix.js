// WebKit/JSC JSPI 兼容补丁：覆盖 emscripten 内置 emscripten_set_main_loop_arg。
//
// 背景：glue 的 getWasmTableEntry 依赖「wasmTable.get(ptr) === instance.exports.f」
// 的函数对象身份比较（Asyncify.isAsyncExport）识别 JSPI_EXPORTS 导出，命中才包
// WebAssembly.promising。V8 满足该身份等价（Wasm JS-API 规范要求同一 wasm 函数
// 对应同一 JS 对象），但 JSC（实测 WebKit 26.4 / Safari 27 beta，2026-06）的
// table.get 返回与 export 不同一的函数对象，识别落空 → krkr2_main_loop_tick
// 未包 promising → 主循环 tick 内首次 JSPI 挂起读抛
// "SuspendError: Suspending() wrapper called outside of a promising() context"。
//
// 本项目 emscripten_set_main_loop_arg 的唯一注册者是 krkr2_main_loop_tick
//（项目显式配置的 JSPI_EXPORTS 唯一成员；SDK 另行包装 main 与 pthread 入口，
// 见 CMakeLists.txt 与 vcpkg cocos2dx patch
// CCApplication-emscripten.cpp），故此处无条件包 promising——与 Chrome 下
// getWasmTableEntry 命中身份比较后的行为完全一致，对 V8 无行为变化。
addToLibrary({
  emscripten_set_main_loop_arg__deps: [
    '$setMainLoop', '$MainLoop', '$handleException'
  ],
  emscripten_set_main_loop_arg: (func, arg, fps, simulateInfiniteLoop) => {
    var wrappedTick = WebAssembly.promising(wasmTable.get(func));
    // Emscripten's stock MainLoop.runIter intentionally discards the callback
    // return value and schedules the next RAF immediately. A JSPI export,
    // however, returns a Promise whenever its Wasm stack can suspend. Keep the
    // Promise beside the scheduler so the next frame is requested only after
    // the original tick has resumed and fully returned.
    var promiseState = MainLoop.__krkr2PromiseAwareSchedulerState;
    if (!promiseState) {
      promiseState = {
        pending: null,
        waiting: null,
        target: null,
        wrapper: null,
      };
      MainLoop.__krkr2PromiseAwareSchedulerState = promiseState;
      globalThis.__krkr2MainLoopPromiseState = promiseState;
    }
    // Browser-only frame-pump policy. Keep RAF as the sole scheduler, but use
    // its display-synchronised timestamp to limit how often the WASM main loop
    // runs. The target defaults to 15 FPS and can be overridden with
    // `?fps=<positive number>`; invalid values deliberately fall back to 15.
    var targetFps = 15;
    try {
      var requestedFps = Number(
          new URLSearchParams(globalThis.location?.search || '').get('fps'));
      if (Number.isFinite(requestedFps) && requestedFps > 0) {
        targetFps = requestedFps;
      }
    } catch (e) {}
    var frameInterval = 1000 / targetFps;
    var lastRafTimestamp = -1;
    var accumulatedTime = 0;

    // TVPWebFrameTickUpdate consumes the same timestamp for its main-thread
    // clock phase lock. Install the wrapper before setMainLoop schedules its
    // first RAF so both the limiter and the engine observe the actual RAF
    // timestamp starting with the first callback.
    if (!globalThis.__tvpRafWrapped) {
      globalThis.__tvpRafWrapped = 1;
      globalThis.__tvpRafT = -1;
      var requestRaf = globalThis.requestAnimationFrame.bind(globalThis);
      globalThis.requestAnimationFrame = (callback) => requestRaf((timestamp) => {
        globalThis.__tvpRafT = timestamp;
        callback(timestamp);
      });
    }

    var runTick = () => {
      var promise = wrappedTick(arg);
      if (promise && typeof promise.then === 'function') {
        promiseState.pending = promise;
      }
      return promise;
    };

    var iterFunc = () => {
      var timestamp = globalThis.__tvpRafT;
      if (!(timestamp >= 0)) timestamp = performance.now();

      if (lastRafTimestamp < 0) {
        lastRafTimestamp = timestamp;
        return runTick();
      }

      var elapsed = timestamp - lastRafTimestamp;
      lastRafTimestamp = timestamp;
      if (!(elapsed >= 0) || elapsed > 1000) {
        // Do not replay frames accumulated while the tab was suspended.
        accumulatedTime = 0;
        return runTick();
      }

      accumulatedTime += elapsed;
      if (accumulatedTime + 0.001 < frameInterval) return;

      // Consume one due frame, preserve its fractional remainder for stable
      // non-divisor rates such as 45 FPS on a 60 Hz display, and discard any
      // additional whole frames accumulated during a stall.
      accumulatedTime = Math.max(0, accumulatedTime - frameInterval);
      accumulatedTime %= frameInterval;
      return runTick();
    };
    setMainLoop(iterFunc, fps, simulateInfiniteLoop, arg);

    if (!promiseState.wrapper) {
      promiseState.target = MainLoop.scheduler;
      promiseState.wrapper = () => {
        var pending = promiseState.pending;
        if (!pending) {
          var target = promiseState.target;
          if (target) target();
          return;
        }
        if (promiseState.waiting === pending) return;

        promiseState.waiting = pending;
        Promise.resolve(pending).then(() => {
          if (promiseState.pending === pending) promiseState.pending = null;
          if (promiseState.waiting === pending) promiseState.waiting = null;
          // Use the current scheduler target: set_main_loop_timing, pause and
          // resume are allowed to replace it while the tick is suspended.
          if (MainLoop.func && MainLoop.scheduler) MainLoop.scheduler();
        }, (error) => {
          if (promiseState.pending === pending) promiseState.pending = null;
          if (promiseState.waiting === pending) promiseState.waiting = null;
          handleException(error);
        });
      };

      // Preserve Emscripten's public scheduler slot while interposing on all
      // future assignments. In particular, emscripten_set_main_loop_timing()
      // can switch the underlying RAF/timeout function without bypassing the
      // Promise-aware gate.
      Object.defineProperty(MainLoop, 'scheduler', {
        configurable: true,
        enumerable: true,
        get: () => promiseState.target ? promiseState.wrapper : null,
        set: (scheduler) => {
          promiseState.target = scheduler;
        },
      });
    }
  },

  // Browser-only policy boundary.  Android's OpenAL/Oboe device starts
  // synchronously, while a browser is allowed to create its AudioContext only
  // in the suspended state. Emscripten's autoResumeAudioContext uses one-shot
  // listeners tied to the context that existed when it was called. Keep the
  // Android sound object/data flow unchanged, but retain a capture listener so
  // later contexts and a rejected first attempt can use the next real gesture.
  krkr2_install_web_audio_resume__deps: ['$AL'],
  krkr2_install_web_audio_resume: () => {
    if (globalThis.__krkr2WebAudioResumeInstalled) return;
    globalThis.__krkr2WebAudioResumeInstalled = true;

    var resumeCurrentOpenALContext = (event) => {
      if (!event.isTrusted) return;
      var audioContext = AL.currentCtx?.audioCtx;
      if (!audioContext || audioContext.state !== 'suspended') return;
      var promise = audioContext.resume();
      if (promise) promise.catch(() => {});
    };

    for (var event of
         ['pointerdown', 'mousedown', 'touchstart', 'keydown', 'click']) {
      document.addEventListener(event, resumeCurrentOpenALContext, {
        capture: true,
        passive: true,
      });
    }
  },
});
