# NEKOPARA 4 旧读回缓存与分组时间线修复

## 授权与范围

用户在已知这两处行为来自原版 Kirikiroid2 后，要求修复并推送至
`origin/dev/fixnekopara`。本次是明确授权的原版缺陷修正，下面两项有意偏离
参考实现；不能把它们标成四二进制已有行为的一比一还原。

诊断基线为 `d10ee1dd`。完整 NEKOPARA 4 ZIP、No.01 的运行时证据位于
`out/diagnostics/nekopara4-emote-analysis-20260912/`。巧克力 GPU 绘制完整，但
软件 capture 复制的是旧香草像素；触摸时播放器时间继续推进，画面停顿对应同一
读回缓存不失效循环。场景、资源加载、visible/opacity 与坐标均正常。

## 四文件函数映射与 fresh 取证

四个二进制及对应 `.i64` 已检查可读。2026-09-12 修复前重新打开四份 IDB、核对
module/imagebase，并 fresh decompile AsTarget 与 timeline builder。其余函数在同一
对话的前一轮诊断中已有四端 fresh decompile，完整原生输出已保存在诊断目录。

| 二进制 | AsTarget | SyncPixel | GetScanLineForRead | timeline builder |
|---|---|---|---|---|
| `Kirikiroid2_1.3.9_Android_arm64-v8a.so` | `0xA5F640` | `0xA5F6F0` | `0xA4E02C` | `0x66CBEC` |
| `Kirikiroid2_1.3.9_Android_armabi-v7a.so` | `0x78BB18` | `0x78BB30` | `0x7854A8` | `0x558EB4` |
| `Kirikiroid2_1.3.9_iOS_arm64` | `0x1002ECE58` | `0x1002ECE84` | `0x1002E3B98` | `0x1001ABA30` |
| `Kirikiroid2_1.3.9_iOS_armv7` | `0x2ED700` | `0x2ED718` | `0x2E32B0` | `0x1AB18C` |

上述函数均已定位。Android arm64 内联 framebuffer bind，其余三端调用 helper；
SyncPixel 均为 vtable slot 19。LP64/ILP32 的字段偏移与容器 ABI 不同。
GetScanLineForRead 的 normal-scale gate 在 Android armv7 只检查 scaleW，其余三端
同时检查 scaleW/scaleH；本场景为 (1,1)，该差异不影响故障。timeline builder 的
分支、缺省分类和遍历顺序四端一致，没有递归文件夹的路径。

## 原版共同伪代码与本地对照

```text
AsTarget:
    SyncPixel()
    bind texture

SyncPixel:
    if PixelData is null: return
    if CPU dirty: upload; clear dirty; free PixelData
    else if --counter <= 0: clear dirty; free PixelData

GetScanLineForRead:
    counter = 5
    if no PixelData: allocate and read GPU (or restore scaled texture first)
    return cached row

buildTimelineControl:
    clear main and diff label vectors, retain state map and active list
    for top-level element:
        probe diff; if present and true use diff vector, otherwise main
        read label; append label
        states[label].rawElement = element
```

修复前本地 `RenderManager_ogl.cpp` 的 AsTarget 两行分别对应 SyncPixel 和 bind；
SyncPixel/readback 保留上述缓存规则。实测每个巧克力 capture 周期计数
`5 → 3（清屏）→ 1（绘制）→ 5（读取）`，缓存始终不失效。香草触摸期间同样
保持旧缓存约 4 秒，绘制量增加使计数归零后画面才恢复。

修复前 `EmoteEngine::buildTimelineControl_guess` 逐项对应 clear、count/indexed
read、HasValue(diff)、独立 bool read、label read、vector append、map raw owner
替换。模型实际使用 folder/children，原版因此只注册三个文件夹名，漏掉
`待機ループ00/01`，游戏不再发出对应的 playTimeline。

## 有意修正的行为

1. **写入目标前失效读回缓存**：AsTarget 仍先调用 SyncPixel，确保未上传的 CPU
   修改先提交；随后释放剩余 PixelData、清零计数，再 bind。GetScanLineForRead
   仍能复用同一张未被写入纹理的多行读取缓存，但之后的 GPU 绘制不再沿用旧像素。
   不增加虚函数或成员，不修改 D3DAdaptor 的软件复制/GPU 交换两条路径。
2. **递归注册实际时间线**：builder 仍只在入口清空两个 label vector；新 helper
   按原数组顺序深度优先处理 `type="folder"` 的 children，文件夹本身不注册成
   动作。每个叶子仍沿用原来的 diff/default 分类、重复 label 和 raw owner 更新
   语义，state map 与 active list 仍不清空。folder/type/children 是真实游戏资源
   中已读取的数据，本扩展不声称来自四二进制。

现有 owner/read-order 单测更新为允许先探测可选 type；原有 diff scratch owner
与容器边界断言继续保留。新增行为使用用户提供的真实游戏 ZIP 验证，不另造模型
fixture。Windows 官方 E-mote 插件不在此次取证/验证范围。

## 验证

- Emscripten 6.0.9：Web Debug / Release 构建成功；原生 `motionplayer-dll`
  测试目标构建成功。未改变 CMake 配置。
- 三个针对性用例（timeline builder owner、container boundaries、capture rows）：
  95 assertions 通过；完整 `*timeline*` 过滤集合：34 cases / 848 assertions 通过。
- 完整 NEKOPARA 4 ZIP 的 Debug 与 Release 均成功读取 No.01，在“欢迎光临”
  处持续显示巧克力和香草。
- Debug 实际注册巧克力 67 main / 48 diff、香草 68 main / 48 diff 时间线；
  `待機ループ00` 与 `待機ループ01` 分别处于播放状态。body/head 数值持续变化。
- Debug 摸头后 0.25、1、2、4、6 秒连续截图中，两人均可见，香草姿态/表情
  持续更新；未出现旧帧保持数秒的问题。
- Release 在未注入 TJS/native 调试钩子的页面中重复读取 No.01、摸头和继续
  剧情，人物均正常显示，动作画面持续更新。两种构建均未出现 timeline-not-found、
  Wasm runtime error 或 WebGL framebuffer/INVALID 错误；Fontconfig 缺配置提示
  是已有环境日志，不影响本场景显示。
- 额外用真实游戏对象临时停止待机循环，复现原先巧克力仅 1 batch、香草 8 batch
  的条件。8 次 draw 后只读采样全部为 `PixelData=null, counter=0`，截图中两人
  仍正常显示，确认缓存修复不依赖新增待机循环掩盖问题。诊断只影响临时浏览器。
- 运行时证据保存于 `out/diagnostics/nekopara4-emote-fix-20260912/`，Wasm 的
  decodedBodySize 与下表一致，HTTP 缓存已关闭。

| 构建 | Wasm 字节数 | SHA-256 |
|---|---:|---|
| Debug | 78,201,842 | `1c4e37e9903959848741e9c0d74a03779406ab34566746c03e6463a703a5830d` |
| Release | 22,635,614 | `e0b78326f31d3941f3631dd9fbe35c58ed50afc360875289f8e8c5248d3bea7a` |
