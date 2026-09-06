// Web platform boundary: acquire a fully loaded Worker before pthread_create
// returns, without moving the application main thread off the browser thread.
// Uses the Emscripten 6.0.9 pthread library and its PROXY_SYNC_ASYNC support.
#if !PTHREADS || ASYNCIFY != 2
#error "pthread_jspi.js requires -pthread and -sJSPI=1"
#endif
#if OFFSCREENCANVAS_SUPPORT
#error "pthread_jspi.js does not support the one-way pthread canvas-transfer path"
#endif

// Preserve the SDK's attribute checks, pthread registration and run protocol.
// These aliases are captured at link time, not by editing the installed SDK.
addToLibrary({
  $krkr2PthreadCreateOriginal: LibraryManager.library.__pthread_create_js,
  $krkr2PthreadCreateOriginal__deps:
      LibraryManager.library.__pthread_create_js__deps,
  $krkr2SpawnThreadOriginal: LibraryManager.library.$spawnThread,

  $krkr2PthreadJspi__deps: ['$PThread', '$Asyncify',
                           '$krkr2SpawnThreadOriginal', '$terminateWorker'],
  $krkr2PthreadJspi__postset: 'krkr2PthreadJspi.install();',
  $krkr2PthreadJspi: {
    pending: new Map(),
    stopped: false,

    install() {
      if (ENVIRONMENT_IS_PTHREAD) {
#if STACK_OVERFLOW_CHECK
        // pthread_exit clears pthread_self before publishing DT_EXITED. A
        // joiner can then free the stack before invokeEntryPoint's JSPI
        // trampoline (or handleException) checks its cookie. An idle Worker
        // no longer owns that stack; keep all checks on live threads intact.
        const originalCheck = checkStackCookie;
        checkStackCookie = () => {
          if (_pthread_self()) originalCheck();
        };
#endif
        return;
      }
      const originalLoad = PThread.loadWasmModuleToWorker;
      PThread.loadWasmModuleToWorker = (worker) => {
        if (worker.krkr2LoadPromise) return worker.krkr2LoadPromise;
        worker.krkr2LoadPromise = new Promise((resolve, reject) => {
          let loading;
          try {
            loading = originalLoad.call(PThread, worker);
          } catch (error) {
            reject(error);
            return;
          }
          const originalError = worker.onerror;
          const originalMessageError = worker.onmessageerror;
          const restore = () => {
            worker.onerror = originalError;
            worker.onmessageerror = originalMessageError;
          };
          const fail = (error) => {
            // Bootstrap failure belongs to pthread_create. Errors after load
            // retain the SDK's normal fatal thread-error handling.
            error.preventDefault?.();
            restore();
            reject(error);
          };
          worker.onerror = fail;
          worker.onmessageerror = fail;
          loading.then(() => {
            restore();
            worker.krkr2Ready = true;
            resolve(worker);
          }, fail);
        });
        return worker.krkr2LoadPromise;
      };

      const originalTerminate = PThread.terminateAllThreads;
      PThread.terminateAllThreads = () => {
        krkr2PthreadJspi.stopped = true;
        // Reserved workers are intentionally absent from both SDK pools.
        for (const [worker, cancel] of krkr2PthreadJspi.pending) {
          terminateWorker(worker);
          cancel();
        }
        krkr2PthreadJspi.pending.clear();
        originalTerminate.call(PThread);
      };
    },

    discard(worker, pthread) {
      if (PThread.pthreads[pthread] === worker) {
        delete PThread.pthreads[pthread];
      }
      const index = PThread.unusedWorkers.indexOf(worker);
      if (index !== -1) PThread.unusedWorkers.splice(index, 1);
      worker.pthread_ptr = 0;
      terminateWorker(worker);
    },

    start(worker, params) {
      // No await or Wasm callback between publishing this ready worker and
      // the SDK's getNewWorker(). The same JS turn consumes the reservation.
      PThread.unusedWorkers.push(worker);
      try {
        return krkr2SpawnThreadOriginal(params);
      } catch (error) {
        krkr2PthreadJspi.discard(worker, params.pthread_ptr);
        return {{{ cDefs.EAGAIN }}};
      }
    },

    spawn(params) {
      if (krkr2PthreadJspi.stopped) return {{{ cDefs.EAGAIN }}};
      let worker = PThread.unusedWorkers.pop();
      if (!worker) {
        try {
          worker = PThread.allocateUnusedWorker();
          // allocateUnusedWorker publishes immediately, before loading. Take
          // it back before yielding so another request cannot steal it.
          PThread.unusedWorkers.pop();
        } catch (error) {
          return {{{ cDefs.EAGAIN }}};
        }
      }
      if (worker.krkr2Ready) return krkr2PthreadJspi.start(worker, params);

      // Only the cold path returns a Promise. The SDK also awaits this Promise
      // when called through pthreadCreateProxied, blocking only that pthread.
      return Asyncify.handleAsync(async () => {
        let cancelled = false;
        const cancellation = new Promise((resolve) => {
          krkr2PthreadJspi.pending.set(worker, () => {
            cancelled = true;
            resolve();
          });
        });
        try {
          await Promise.race([
            PThread.loadWasmModuleToWorker(worker), cancellation
          ]);
          if (cancelled) return {{{ cDefs.EAGAIN }}};
          return krkr2PthreadJspi.start(worker, params);
        } catch (error) {
          if (!cancelled) krkr2PthreadJspi.discard(worker, params.pthread_ptr);
          return {{{ cDefs.EAGAIN }}};
        } finally {
          krkr2PthreadJspi.pending.delete(worker);
        }
      });
    },
  },

  $spawnThread__deps: ['$krkr2PthreadJspi'],
  $spawnThread: (params) => krkr2PthreadJspi.spawn(params),

  // In 6.0.9 these two decorators select PROXY_SYNC_ASYNC: the calling pthread
  // blocks until an integer or Promise result on the main thread completes.
  // __async is only supported on C-facing library symbols, not $JS helpers.
  $pthreadCreateProxied__proxy: 'none',
  $pthreadCreateProxied__deps: ['krkr2_pthread_create_proxied'],
  $pthreadCreateProxied: (pthread, attr, entry, arg) =>
      _krkr2_pthread_create_proxied(pthread, attr, entry, arg),
  krkr2_pthread_create_proxied__sig: 'ipppp',
  krkr2_pthread_create_proxied__proxy: 'sync',
  krkr2_pthread_create_proxied__async: true,
  krkr2_pthread_create_proxied__deps: ['$krkr2PthreadCreateOriginal'],
  // PROXY_SYNC_ASYNC's receiver calls .then(), including on the warm path.
  krkr2_pthread_create_proxied: (pthread, attr, entry, arg) =>
      Promise.resolve(krkr2PthreadCreateOriginal(pthread, attr, entry, arg)),

  __pthread_create_js__async: true,
  __pthread_create_js__deps: ['$krkr2PthreadCreateOriginal',
                             'krkr2_pthread_create_state',
                             'krkr2_pthread_create_failed'],
  __pthread_create_js: (pthread, attr, entry, arg) => {
    // Capture the caller's C frame before suspending. Promise completions must
    // not write to a shared TLS slot which another creation could overwrite.
    const state = _krkr2_pthread_create_state();
    const finish = (result) => {
      // This runs on the caller's thread, after the proxy has returned. Do
      // not free here: libc still has to unlink its partially created thread.
      if (result) _krkr2_pthread_create_failed(state, pthread);
      return result;
    };
    const result = krkr2PthreadCreateOriginal(pthread, attr, entry, arg);
    return result instanceof Promise ? result.then(finish) : finish(result);
  },
});
