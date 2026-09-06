// Browser integration test for the Web pthread adapter. Build with the same
// pthread_jspi.c/js and --wrap flags as the application; see README.md.
#include <atomic>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <malloc.h>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include <emscripten.h>
#include <emscripten/threading.h>

EM_JS(int, worker_count, (), {
    return PThread.unusedWorkers.length + Object.keys(PThread.pthreads).length;
});

EM_JS(void, fail_next_worker, (int kind), {
    // Force the cold path without stopping any live pthread.
    for (const worker of PThread.unusedWorkers.splice(0)) worker.terminate();
    if (kind === 0) {
        const original = PThread.allocateUnusedWorker;
        PThread.allocateUnusedWorker = () => {
            PThread.allocateUnusedWorker = original;
            throw new Error('expected test Worker constructor failure');
        };
    } else {
        const original = PThread.loadWasmModuleToWorker;
        PThread.loadWasmModuleToWorker = (worker) => {
            PThread.loadWasmModuleToWorker = original;
            return Promise.reject(new Error('expected test Worker load failure'));
        };
    }
});

static void *nothing(void *) { return nullptr; }
static void *exit_explicitly(void *) { pthread_exit(nullptr); }

int main() {
    assert(emscripten_is_main_browser_thread());
    assert(emscripten_is_main_runtime_thread());
#ifndef WARM_POOL
    assert(worker_count() == 0);
#endif
    // Real std::thread constructors must suspend inside libc/libc++, while
    // all 32 children remain live. No Worker may be reused prematurely.
    std::mutex mutex;
    std::condition_variable wake;
    bool go = false;
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 32; ++i) {
        threads.emplace_back([&] {
            assert(!emscripten_is_main_browser_thread());
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&] { return go; });
            ++completed;
        });
    }
    assert(worker_count() == 32);
    {
        std::lock_guard<std::mutex> lock(mutex);
        go = true;
    }
    wake.notify_all();
    for (auto &thread : threads) thread.join();
    assert(completed == 32);

    const int warmed = worker_count();
    for (int i = 0; i < 64; ++i) {
        std::thread thread([&] { ++completed; });
        thread.join();
    }
    assert(completed == 96);
    assert(worker_count() == warmed);
    puts("PASS main-thread std::thread, 32 live workers, 64 warm reuses");

    // Explicit pthread_exit publishes completion before the surrounding JSPI
    // trampoline finishes. A cookie check must not read the freed stack.
    for (int i = 0; i < 128; ++i) {
        pthread_t thread = 0;
        assert(pthread_create(&thread, nullptr, exit_explicitly, nullptr) == 0);
        assert(pthread_join(thread, nullptr) == 0);
    }
    puts("PASS 128 explicit pthread_exit / join / reuse cycles");

    // A parent may block joining its child, but the browser main thread must
    // service asynchronous startup. Creation JSPI does not make join async.
    std::atomic<bool> parentGo{false}, parentDone{false};
    std::thread parent([&] {
        while (!parentGo) std::this_thread::yield();
        std::thread child([&] { ++completed; });
        child.join();
        parentDone = true;
    });
    EM_ASM({
        for (const worker of PThread.unusedWorkers.splice(0)) worker.terminate();
    });
    parentGo = true;
    while (!parentDone) emscripten_sleep(1);
    parent.join();
    assert(completed == 97);
    puts("PASS pthread creates and joins a cold child");

    // Exercise the SDK rollback and the C wrapper with real allocated stacks.
    const auto before = mallinfo().uordblks;
    for (int i = 0; i < 16; ++i) {
        fail_next_worker(i % 2);
        pthread_t thread = 0;
        int rc = pthread_create(&thread, nullptr, nothing, nullptr);
        assert(rc == EAGAIN);
        assert(thread == 0);
    }
    const auto after = mallinfo().uordblks;
    assert(after <= before + 65536);
    fail_next_worker(1);
    bool threw = false;
    try {
        std::thread thread([] {});
        thread.join();
    } catch (const std::system_error &) {
        threw = true;
    }
    assert(threw);
    puts("PASS main-thread creation failures roll back and free stack/TLS");

    parentGo = false;
    parentDone = false;
    std::atomic<int> childError{0};
    std::thread failingParent([&] {
        while (!parentGo) std::this_thread::yield();
        pthread_t thread = 0;
        childError = pthread_create(&thread, nullptr, nothing, nullptr);
        assert(thread == 0);
        parentDone = true;
    });
    fail_next_worker(1);
    parentGo = true;
    while (!parentDone) emscripten_sleep(1);
    failingParent.join();
    assert(childError == EAGAIN);
    // Failure on a worker must not poison main-thread TLS or the next create.
    std::thread recovery([&] { ++completed; });
    recovery.join();
    assert(completed == 98);
    assert(emscripten_is_main_browser_thread());
    puts("PASS proxied creation failure and recovery");
    EM_ASM({ document.title = 'pthread JSPI PASS'; });
    return 0;
}
