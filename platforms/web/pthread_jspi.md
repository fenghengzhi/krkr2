# 主线程 JSPI / pthread 按需创建

此适配面向项目使用的 Emscripten 6.0.9。应用 `main()` 和游戏主循环仍在浏览器
主线程执行，`std::thread` / `pthread_create()` 的调用点和线程函数保持不变。

## 创建路径

`pthread_jspi.js` 在链接时保存 SDK 的 `__pthread_create_js` 和 `spawnThread`
实现，保留其属性检查、线程映射和启动消息协议，仅替换 Worker 获取过程：

1. 取出一个已就绪的空闲 Worker 时，直接调用 SDK 启动线程，返回整数，不挂起。
2. 没有空闲 Worker 时创建一个；初始化中的 Worker 从公共池移除，独占预留给
   当前请求，避免其它创建请求在 JSPI 挂起期间取走它。
3. 复用 SDK `loadWasmModuleToWorker()` 的完成 Promise，收到模块加载完成通知后
   才派发 `run`。冷路径通过 `Asyncify.handleAsync()` 维护 runtime keepalive。
4. 主线程的 Wasm 导入 `__pthread_create_js` 标记为可挂起；后台 pthread 的调用
   经 `krkr2_pthread_create_proxied` 的 `__proxy: 'sync'` 和 `__async: true` 组合
   进入 SDK `PROXY_SYNC_ASYNC`。后者使用 `emscripten_proxy_sync_with_ctx`，等待
   主线程返回的 Promise 完成。SDK 接收器无条件调用 `.then()`，因此代理入口
   的热路径也返回 Promise；主线程直接创建的热路径仍返回整数。
5. 正常退出和复用继续走 SDK 的原有生命周期。

Debug 栈检查还需避开一个 SDK JSPI 退出竞态：`pthread_exit()` 在发布可 join 状态
前将 `pthread_self()` 清零，joiner 随即可以释放栈，但 SDK `invokeEntryPoint`
仍会在获取 JSPI Promise 后检查栈 cookie。Worker 已无活动 pthread 时不能再读取
旧栈，因此适配在该状态下跳过 cookie 检查；活动线程及浏览器主线程仍保留检查。

JS library 的 `$` helper 不允许使用 `__async`，因此后台代理必须通过独立的
C-facing library symbol，不能直接给 `$pthreadCreateProxied` 加这个标记。

## 失败清理

Worker 构造、模块加载或 `run` 消息发送失败时，撤销 JS 线程映射、终止该 Worker，
并返回 `EAGAIN`。模块加载完成后的运行错误继续由 SDK 原有错误处理器处理。
关闭 runtime 时也会终止尚未完成初始化的预留 Worker，释放其等待者。

Emscripten 6.0.9 `system/lib/pthread/pthread_create.c` 的失败分支会重置输出句柄、
撤销线程链表和计数，但没有释放新线程块。`pthread_jspi.c` 用链接器 `--wrap`
覆盖 `pthread_create`、`__pthread_create` 和 `emscripten_builtin_pthread_create`
三个入口，在 SDK 完成回滚并返回后调用 `_emscripten_thread_free_data()`。
这也覆盖 libc++、C11 和 ASan 使用的不同别名。

失败信息写入调用者的 C 栈帧；JS 在挂起前捕获该帧地址，不会在 Promise 完成时
把失败写入一个可被其它调用覆盖的公共 TLS 槽。后台代理完成后，在原调用 pthread
上记录失败，不会把后台线程的失败写进浏览器主线程。

安装的 SDK 和 `cpp/` 下的引擎实现不作修改。

## 构建和边界

项目默认 `KRKR2_WEB_PTHREAD_POOL_SIZE=0`。已有 CMake cache 可能还保存旧值，
首次迁移时显式配置：

```sh
cmake --preset "Web Debug Config" -DKRKR2_WEB_PTHREAD_POOL_SIZE=0
cmake --build out/web/debug
```

非零值仍可用于预热，零值也会复用退出的 Worker。池会保留达到过的并发容量，
没有自动缩容策略。

所有可能挂起的调用链必须从 `WebAssembly.promising` 入口进入。当前 SDK 自动
包装 `main` 和 pthread 入口，项目包装 `krkr2_main_loop_tick`，并在 tick 完成前
暂停下一帧调度。新增浏览器回调或全局构造中的线程创建需要单独审查，不能假定
普通 JS → Wasm 回调允许挂起。

本适配只等待 Worker 可承载线程，不保证线程函数已经执行，也不把 `join`、锁或
条件变量自动变成异步等待。主线程持锁挂起时仍需避免回调重入或相互等待。
若主线程同步 join 一个还要动态创建子线程、读取异步文件的后台线程，仍可能
阻塞后续操作所需的事件循环。

当前不支持 `OFFSCREENCANVAS_SUPPORT` 的单向 transferable 创建路径，编译时
明确拒绝该组合。当前项目的 WebGL 上下文留在主线程，不需要这个选项。
构建锁定到 6.0.9：升级 SDK 时需核对 Worker 池协议、代理接收器和 C 失败清理
分支，再更新版本检查并重新运行测试，尤其避免 SDK 修复泄漏后重复释放线程块。

## 验证

纯 JS 的并发预留、反序完成、复用、预热、启动失败和关闭测试：

```sh
node tests/web/pthread_jspi.test.mjs
```

真实浏览器专项测试（已 source emsdk，必须将 `.c` 按 C 编译）：

```sh
mkdir -p out/pthread-jspi-validation
emcc -c platforms/web/pthread_jspi.c -pthread -fwasm-exceptions -O1 \
  -o out/pthread-jspi-validation/pthread_jspi.o
em++ tests/web/pthread_jspi_smoke.cpp \
  out/pthread-jspi-validation/pthread_jspi.o \
  --js-library platforms/web/pthread_jspi.js \
  -pthread -fwasm-exceptions -sJSPI=1 -sPTHREAD_POOL_SIZE=0 \
  -sEXIT_RUNTIME=1 -sASSERTIONS=2 -sINITIAL_MEMORY=67108864 \
  -sDEFAULT_PTHREAD_STACK_SIZE=1048576 \
  -Wl,--wrap=pthread_create -Wl,--wrap=__pthread_create \
  -Wl,--wrap=emscripten_builtin_pthread_create \
  -O1 -o out/pthread-jspi-validation/smoke.html
python3 coi-server.py out 18080 18443
```

用项目 `playwright-cli` 工作流打开
`http://localhost:18080/pthread-jspi-validation/smoke.html`，页面标题应变为
`pthread JSPI PASS`。测试覆盖 32 条同时存活的 std::thread、64 次复用、128 次显式
`pthread_exit` / join / 复用、后台创建
冷 Worker、失败回滚和内存释放、C++ 异常、后台失败后的恢复；并断言应用 main
始终在浏览器主线程。测试故意使用部分同步 join，会触发 SDK 的主线程阻塞提示。

预热测试增加 `-DWARM_POOL` 并改成 `-sPTHREAD_POOL_SIZE=2`。ASan 测试须对 C
桥和 C++ 测试都添加 `-fsanitize=address`，并将初始内存改为 268435456，启用
`-sALLOW_MEMORY_GROWTH=1`。

引擎回归使用已有完整游戏 ZIP，检查标题到剧情切换、浏览器输入计数、Worker
数量和错误日志。其它 Web 回归测试见 `tests/web/*.test.mjs`。

### 2026-09-07 验证记录

- Emscripten 6.0.9：完整引擎 Debug / Release 构建通过，预创建数量均为 0。
- Chrome 152：零预热、预热 2 个 Worker、ASan 三组上述专项测试全部通过。
- SDK `test/pthread/test_pthread_create.c`：零预热下完成 1008 次创建，没有
  `pthread_exit` 后的栈检查错误。
- 完整 DRACU-RIOT!：Debug / Release 均由标题进入剧情，按需使用 41 个 Worker，
  输入事件正常，无新增 Wasm/线程异常。原有 Fontconfig 默认配置和 AlphaMovie
  缺失提示仍在；这不是完整引擎的 ASan 游戏验证。
- Worker 预留/复用/失败/退出、主循环调度、VLFS ZIP 缓存、Release 资源配套的
  Node 测试全部通过。

本次本机截图、浏览器结果、构建日志和源文件/产物 SHA-256 保存在
`out/pthread-jspi-validation/`，汇总为 `validation.json`；该目录不入库。
