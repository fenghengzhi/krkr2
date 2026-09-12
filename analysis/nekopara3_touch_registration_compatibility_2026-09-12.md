# NEKOPARA Vol.3 触摸注册兼容修正（2026-09-12）

## 修正前证据与结论

用户授权修复安卓 Kirikiroid2 同样存在的问题。本次是游戏脚本/场景数据组合的兼容修正，
不是 Emote 命中算法的源码复原，也不把新增兼容逻辑归因于参考二进制。

完整 ZIP、独立持久化浏览器 profile、Debug HEAD `baeebec8`，读取 No.01：
`03_01a.txt*start`，`curState=26`，巧克力「主人～！」。
页面 capture 计数确认 pointer/mouse down/up/click 成对到达。

- `initTouchmode()` 从 `world_object.env.layerList` 注册 head/body/bust。
- 进入触摸模式后该列表 count=0，`touchInfos` 为 void，`touchLock=1`。
- `world_object.refreshEnv()` 正常返回；其恢复记录 `env` 只有 `name:env`。
- 分别直接打开 `vol3.xp3>03_01a.txt.scn` 与 `krpatch.xp3>03_01a.txt.scn`，
  `scenes[0].texts[25][5].env` 都只有 name，没有 objectList；复制前后相同。
  因此不是 Scripts.clone 丢数据，也不能将问题仅归咎于 krpatch。
- 当前画面 `world_object.envlayerList` 则有可见 Emote 巧克力。
- 坐标链：1280×720 浏览器画布 → 1920×1080 primary；头部点击 (640,200)
  → 中心坐标 (0,-240) → AffineLayer 基点 (960,540) → revmtx
  `[1,0,0,1,-960,-705]` → 模型 (0,-405)。
  `im.checkTouch(0,-240,targetLayer)` 返回 head，targetLayer 返回 ショコラ:head。
  `contains("hit_head",0,y)` 在 y=-500/-450/-400 返回真。
  两参数直接调用 im.checkTouch 是诊断调用错误（缺少第三个 owner 参数），不能作为游戏异常证据。
- 只补注册当前可见人物的三个动作后，真实点击立即进入
  `おさわり汎用Ａhead_0_2`，然后跳回 touchloop。这证明丢失的注册是此症状的断点。

## 四文件核验、函数映射与 fresh 反编译

四个目标及各自 .i64 均已检查可读，并核对 IDA module/base/Hex-Rays。
下表每列地址仅属于对应文件。四文件均已 fresh decompile。

| 角色 | Android arm64-v8a.so | Android armabi-v7a.so | iOS_arm64 | iOS_armv7 |
|---|---|---|---|---|
| Scripts class 注册 | 0x8E75DC | 0x6B6054 | 0x100189370 | 0x187224 |
| Scripts.execStorage callback | 0x8E7948 | 0x6B639C | 0x1001896AC | 0x187670 |
| TVPExecuteStorage（语义映射） | 0x8E4714 | 0x6B4AD0 | 0x10018768C | 0x184F98 |
| Motion.EmotePlayer.contains | 0x67EEEC | 0x497BFE | 0x1001B5E84 | 0x1B5B74 |

文件全名分别是 `Kirikiroid2_1.3.9_Android_arm64-v8a.so`、
`Kirikiroid2_1.3.9_Android_armabi-v7a.so`、`Kirikiroid2_1.3.9_iOS_arm64`、
`Kirikiroid2_1.3.9_iOS_armv7`。从各自 UTF-16 execStorage 字符串及注册 callback
追踪到 loader，并保留原始工具输出于本地诊断目录。遇到的单字符截断字面量已读原始字节、
修正完整 unsigned short[N] 类型；Hex-Rays 部分缓存伪代码仍显示旧字面量，不以该旧显示判定字符串。

共同伪代码（修改前）：

```text
Scripts.execStorage(args):
    require argc >= 1 (else -1004)
    name = string(args[0]); mode = args[1] if non-void else empty
    context = object(args[2]) if non-void else null
    ExecuteStorage(name, context, result, false, mode)
ExecuteStorage(name, context, result, isExpression, mode):
    require scriptEngine
    place = SearchPlacedPath(name); shortName = ExtractStorageName(place)
    stream = CreateBinaryStreamForRead(place, mode)
    if stream and LoadByteCode(stream,result,context,shortName): release; return
    release binary stream and path/string temporaries
    resolve place and shortName again
    stream = CreateTextStreamForRead(place,mode)
    text = stream.Read(all); release stream
    if scriptEngine:
        if isExpression: EvalExpression(text,result,context,shortName)
        else: ExecScript(text,result,context,shortName)
contains(label,x,y):
    node = primaryPlayer.findNodeByRawLabel(label,recursive=true)
    return node ? GeometryShape.contains(node.shape,x,y) : false
```

逐文件差异：Android arm64 用原子 64 位引用计数、布尔 bit0；Android armv7 用 32 位
引用计数及 bool==1；iOS arm64 使用 libc++ 清理包装和非零 bool；iOS armv7 32 位 ABI
并保存较多浮点寄存器。内部错误路径保留 Android 相对源码路径、iOS 绝对构建路径。
参数默认值、字节码优先/文本回退分支、表达式选择和 contains 行为一致。

本地对照：`ScriptMgnIntf.cpp::TVPExecuteStorage` 的 binary scope 保持同一优先顺序，
文本 scope 执行 Read(all)，然后按 isexpression 选择 ExecScript/EvalExpression，参数原样传递。
本地已有追踪日志及 AfterInit 包装属于先存差异，与这次触摸注册无关。
`EmotePlayer.cpp` → `PlayerLayerQuery.cpp` 已按共同 contains 流程执行，无需修改。

## 有意兼容差异

只在 loader 的非表达式、文本脚本分支处理 `nw_touchmode.tjs`。校验已知完整脚本的
归一化 UTF-16LE FNV-1a64 指纹 `a1642438f5f2ba95` 与 9019 code units（忽略 CR）；
归一化 UTF-8 SHA-256 为 `81a70b1af0751db98c28116de80308e32dd9d8cf936b631760bda66986c8fa81`。
Vol.3 与此前 Vol.4 的该脚本一致。脚本名称/内容不符均不修改，字节码与表达式也不修改。
这不是通用场景反编译器：只修正该已知脚本在环境列表为空时的触摸状态。

游戏原 ZIP 及存档不改写。未新增伪造 PSB/游戏 fixture；使用现有完整 ZIP 做验证。

### 连续触摸验证后的修正（最终实现前）

仅补注册的初版不足：首次动作会创建默认 KAGEnvImage，`classFlag=772` 但
`isShow()=0`；人物缩放/位置也使用了不完整状态，因此第二次触摸失效。该初版不作为最终修复。
直接使用早先编译记录的 `redraw.posName` 也不正确：No.01 记录为“出右長”，而当前已在画面
中央；这种重放会把人物移出屏幕。必须快照当前可见状态，不重放旧的进场状态。

最终改在原 `initTouchmode` 取 layerList 之前：仅在环境列表为空时，给当前可见的 Emote
人物创建游戏自己的环境对象，通过其现有 `onRestore` 恢复当前 imageFile/imageOptions、
位置/缩放/旋转/透明度/层级及 Emote 变量。变量以 actionList/cpropsact 的游戏存档结构写入。
再执行原来的 FLAG_STAND/isShow 判定及三个 addTouch；不另写注册、命中或动作调度算法。
不持有跨恢复的 live/player 对象。环境列表非空时完全走原逻辑。

脚本依据：`KAGEnvBase.onRestore` 清 cprops/actionList，复制传入 actionList，对静态值调用
storePropValue，再恢复 cpropsact；`KAGEnvImage.onRestore` 读取 disp、imageFile、imageOptions/
imageOptionsAll、调用 KAGEnvTrans.onRestore。`world.tjs::EnvLayerObject` 保存当前 file/options/
targetLayer；`AffineLayer` 提供被快照的变换属性。游戏字节码与运行日志在诊断目录保存。

运行试验 `try-liveenv.tjs` 已证明：onRestore 后 isShow=1，文件仍是制服a；两次真实头部
点击均触发 `MakeTouchActionTags` 和 head_0_2，第二次前 touchLock=0，headHit 正确。
图像 `live-seeded-first.png` 显示巧克力保持原位置、原制服并产生闭眼微笑反应。

独立原始 PSB 检查（复用仓库 PackedArray/name decoder）：krpatch 解压后的
`03_01a.txt.scn` 中 env 位于 0x5fc79，tag=0x21，成员数为 1，唯一 key=name。
这与运行时直接打开两个归档、克隆前/后的枚举结果相互印证。

## 验证

- 使用实际 nw_touchmode.tjs 的 native C++ harness：原文件与 LF 换行版本匹配；其他文件名、
  内容改动、二次应用均被拒绝。无虚构游戏 fixture。
- Debug、Release 均构建通过；有既存编译警告（TJS null-reference、JS library 等）。
- 最终 Debug 从完整 ZIP 重新启动、No.01、进入触摸模式：envcount=1、touchLock=0，
  head/body/bust 三个键齐全。两次头部点击分别触发 head_0_2 与 head_2_2，伴随
  choko_cs_touch0003 / choko_cs_touch0016。随后 body 与 bust 分别触发对应绑定和动作。
- Debug 退出触摸模式：touchenv 恢复 void、touchLock=1、envcount=0，原台词「主人～！」、
  制服及人物原位置恢复；待机循环继续。该回退没有改变退出时清环境的原游戏约定。
- capture 8 次 pointer/mouse down/up/click 全部成对。触摸阶段没有新异常；启动期间仍有
  游戏原有 windowMaxSize getter 的已捕获类型转换日志，不作为本修正的回归或修复范围。
- Debug wasm: 78,218,974 bytes，SHA-256
  `d5060689cdc0517e4176eb260ddd5712e06b96eb98124406ac23a943955589d4`。
- Release wasm: 22,641,927 bytes，SHA-256
  `78a372e5a1bf8ceb4056333a61f78b3b78b4baed9a9fb01d0e40ebb6ec972abd`。

所有诊断脚本、fresh 四文件反编译、构建日志、截图和检查结果在
`out/diagnostics/nekopara3-touch-fix-20260912/`（不提交游戏素材）。

Release 使用同一完整 ZIP 再次读取 No.01，通过两次头部反应、身体反应及退出恢复。
`final-release-head-first.png` / `final-release-head-second.png` 都可见闭眼微笑和姿态变化；
`final-release-exit.png` 恢复原台词、人物和 UI。capture 的 7 组输入完整配对。
Release 日志裁剪不能用于证明具体动作标签；标签证据来自 Debug，Release 用可见反应核验。

Vol.4 完整 ZIP、Debug No.01 回归：进入触摸后 envcount=4，故不执行环境重建分支。
巧克力与香草六个触摸键齐全；两人的头部点击分别触发 head_0_2 / head_2_2 和对应语音。
操作后 touchLock=0，两人均可见、待機ループ00/01 继续，未回归此前立绘消失问题。
