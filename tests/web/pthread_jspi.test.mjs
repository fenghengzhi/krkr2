import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(new URL('../../platforms/web/pthread_jspi.js',
                                     import.meta.url), 'utf8')
    .replace(/^#.*$/gm, '')
    .replaceAll('{{{ cDefs.EAGAIN }}}', '6');

function setup() {
  let nextId = 0;
  let suspensions = 0;
  let failAllocation = false;
  let callerState = 0;
  const workers = [];
  const failures = [];
  const fatal = () => { throw new Error('runtime thread error'); };
  const pthread = {
    unusedWorkers: [], pthreads: {},
    allocateUnusedWorker() {
      if (failAllocation) throw new Error('Worker constructor failed');
      const worker = {
        id: ++nextId, runs: [], terminations: 0, loads: 0,
        postMessage(message) {
          if (this.failRun) throw new Error('run dispatch failed');
          this.runs.push(message);
        },
      };
      workers.push(worker);
      this.unusedWorkers.push(worker);
      return worker;
    },
    loadWasmModuleToWorker(worker) {
      ++worker.loads;
      worker.onerror = fatal;
      return new Promise((resolve, reject) => {
        worker.loaded = resolve;
        worker.loadRejected = reject;
      });
    },
    terminateAllThreads() {
      for (const worker of [...this.unusedWorkers,
                            ...Object.values(this.pthreads)]) {
        ++worker.terminations;
      }
      this.unusedWorkers = [];
      this.pthreads = {};
    },
  };
  const lib = {
    __pthread_create_js: (ptr, attr, entry, arg) => context.spawnThread({
      pthread_ptr: ptr, startRoutine: entry, arg,
    }),
    __pthread_create_js__deps: [],
    $spawnThread(params) {
      const worker = pthread.unusedWorkers.pop();
      assert.ok(worker.krkr2Ready, 'SDK must only receive a loaded worker');
      pthread.pthreads[params.pthread_ptr] = worker;
      worker.pthread_ptr = params.pthread_ptr;
      worker.postMessage(params);
      return 0;
    },
  };
  const context = vm.createContext({
    LibraryManager: {library: lib},
    addToLibrary: (items) => Object.assign(lib, items),
    PThread: pthread, ENVIRONMENT_IS_PTHREAD: false, Promise, Map,
    Asyncify: {async handleAsync(fn) { ++suspensions; return await fn(); }},
    terminateWorker: (worker) => { ++worker.terminations; },
    _krkr2_pthread_create_state: () => callerState,
    _krkr2_pthread_create_failed: (state, ptr) => {
      assert.equal(state, ptr + 999, 'failure must reach its original C frame');
      failures.push(ptr);
    },
  });
  vm.runInContext(source, context);
  for (const [key, value] of Object.entries(lib)) {
    if (key.startsWith('$') && !key.includes('__')) context[key.slice(1)] = value;
  }
  context.krkr2PthreadJspi.install();
  const create = (ptr) => {
    callerState = ptr + 999;
    return lib.__pthread_create_js(ptr, 0, 123, 456);
  };
  return {context, pthread, lib, workers, failures, create, fatal,
          suspensions: () => suspensions,
          failAllocation: () => { failAllocation = true; }};
}

// Two requests suspend concurrently. Reverse completion must not exchange
// their reserved workers or dispatch a thread on a half-loaded instance.
{
  const s = setup();
  const a = s.create(100);
  const b = s.create(200);
  assert.equal(s.pthread.unusedWorkers.length, 0);
  assert.equal(Object.keys(s.pthread.pthreads).length, 0);
  assert.equal(s.workers.length, 2);
  s.workers[1].loaded();
  assert.equal(await b, 0);
  assert.equal(s.pthread.pthreads[200], s.workers[1]);
  assert.equal(s.pthread.pthreads[100], undefined);
  s.workers[0].loaded();
  assert.equal(await a, 0);
  assert.equal(s.pthread.pthreads[100], s.workers[0]);
  assert.equal(s.workers[0].onerror, s.fatal);

  // Simulate SDK cleanup, then verify warm creation never suspends.
  delete s.pthread.pthreads[100];
  s.workers[0].pthread_ptr = 0;
  s.pthread.unusedWorkers.push(s.workers[0]);
  const count = s.suspensions();
  assert.equal(s.create(300), 0);
  assert.equal(s.suspensions(), count);
  assert.equal(s.workers.length, 2);
  assert.equal(s.workers[0].loads, 1);
}

// Optional prewarming and on-demand acquisition share one load Promise.
{
  const s = setup();
  const worker = s.pthread.allocateUnusedWorker();
  const preload = s.pthread.loadWasmModuleToWorker(worker);
  const creation = s.create(100);
  assert.equal(worker.loads, 1);
  assert.equal(s.pthread.unusedWorkers.length, 0);
  worker.loaded();
  await preload;
  assert.equal(await creation, 0);
}

// Constructor, asynchronous script-load and run-message failures all reach
// the C rollback path, with no failed worker left in either runtime pool.
for (const kind of ['constructor', 'error', 'messageerror', 'rejection', 'run']) {
  const s = setup();
  if (kind === 'constructor') s.failAllocation();
  const creation = s.create(100);
  const worker = s.workers[0];
  if (kind === 'error') worker.onerror(new Error('script load failed'));
  if (kind === 'messageerror') worker.onmessageerror(new Error('bad message'));
  if (kind === 'rejection') worker.loadRejected(new Error('load rejected'));
  if (kind === 'run') { worker.failRun = true; worker.loaded(); }
  assert.equal(await creation, 6, kind);
  assert.deepEqual(s.failures, [100], kind);
  assert.equal(s.pthread.unusedWorkers.length, 0, kind);
  assert.equal(Object.keys(s.pthread.pthreads).length, 0, kind);
  if (worker) assert.equal(worker.terminations, 1, kind);
}

// Failures completing in reverse order keep their captured C frame identity.
{
  const s = setup();
  const a = s.create(100);
  const b = s.create(200);
  s.workers[1].loadRejected(new Error('second failure'));
  s.workers[0].loadRejected(new Error('first failure'));
  assert.deepEqual(await Promise.all([a, b]), [6, 6]);
  assert.deepEqual(s.failures, [200, 100]);
}

// The SDK proxy receiver always requires a Promise, even for a warm Worker.
{
  const s = setup();
  const worker = s.pthread.allocateUnusedWorker();
  worker.krkr2Ready = true;
  const result = s.lib.krkr2_pthread_create_proxied(100, 0, 123, 456);
  assert.ok(result instanceof Promise);
  assert.equal(await result, 0);
  assert.equal(s.suspensions(), 0);
}

// Runtime shutdown also owns reserved workers and releases pending callers.
{
  const s = setup();
  const a = s.create(100);
  const b = s.create(200);
  s.pthread.terminateAllThreads();
  assert.deepEqual(await Promise.all([a, b]), [6, 6]);
  for (const worker of s.workers) {
    assert.equal(worker.terminations, 1);
    assert.equal(worker.runs.length, 0);
    worker.loaded(); // Late bootstrap completion cannot start a pthread.
  }
  await Promise.resolve();
  assert.equal(Object.keys(s.pthread.pthreads).length, 0);
  assert.equal(s.create(300), 6);
}
// An exited pthread has relinquished its stack. Keep checks on live threads,
// but never read cookies after a joiner can have freed/reused the stack.
{
  const s = setup();
  s.context.ENVIRONMENT_IS_PTHREAD = true;
  let thread = 0;
  let checks = 0;
  s.context._pthread_self = () => thread;
  s.context.checkStackCookie = () => { ++checks; };
  s.context.krkr2PthreadJspi.install();
  s.context.checkStackCookie();
  assert.equal(checks, 0);
  thread = 100;
  s.context.checkStackCookie();
  assert.equal(checks, 1);
  thread = 0;
  s.context.checkStackCookie();
  assert.equal(checks, 1);
}
console.log('pthread JSPI worker reservation, reuse, proxy, failure and exit tests passed');
