# NEKOPARA 4 读档立绘缺失：WebGL 模板缓冲适配

## 现象与根因（修改前）

提交 `1f60c5e5` 已能通过 lzfs 读取巧克力、香草冬制服 PSB，两个 root 均为
`id=motion, spec=krkr, version≈3.03`。完整 ZIP 读取 No.01 后背景/对白正常，立绘缺失。
浏览器首先报告 `renderbufferStorage: invalid internalformat`，然后报告
`clear/drawArrays: conflicting DEPTH/STENCIL/DEPTH_STENCIL attachments`。
引擎对应 BeginStencil 和 OperateTriangles；修改前证据在
`out/diagnostics/nekopara4-lzfs-verify-20260912/`。

实际 context 为 WebGL 1.0。Cocos 的 Emscripten GLView 请求 GLES 2.0，
`MAX_WEBGL_VERSION=2` 仅允许 WebGL 2，不会把 SDL 的 GLES 2 请求升级。
Emscripten 6.0.9 的 glRenderbufferStorage 直接转发参数，不转换格式。

[WebGL 1 规范 §6.5 Framebuffer Object Attachments](https://registry.khronos.org/webgl/specs/latest/1.0/#6.5)
要求 packed depth/stencil renderbuffer 使用 `DEPTH_STENCIL`，并通过
`DEPTH_STENCIL_ATTACHMENT` 挂载。原生的 `DEPTH24_STENCIL8` 加两个独立
depth/stencil attachment 在这个 context 上非法，导致整个 FBO 的 clear/draw 被拒绝。
这属于 GL API 的平台适配，不能通过取消 Emote 模板遮罩来处理。

## 本轮四文件映射与 fresh 取证

四个二进制和四个配套 IDB 已逐个打开读取并核对。I32 是同一 fat Mach-O 的
armv7 slice，恢复 IDB 的 module 为 `Kirikiroid2_1.3.9_iOS_armv7.thin-armv7`。
历史 Windows input path 与本机路径不同；已核对模块、架构基址和配套关系。

| 二进制 | database | BeginStencil 对应函数 | EndStencil 对应函数 | 深层调用者 |
|---|---|---|---|---|
| Kirikiroid2_1.3.9_Android_arm64-v8a.so | 177e664e | sub_A5B2C8 @ 0xa5b2c8 | sub_A5B3D4 @ 0xa5b3d4 | 0x6ab39c |
| Kirikiroid2_1.3.9_Android_armabi-v7a.so | c3c0088c | sub_789160 @ 0x789160 | sub_789238 @ 0x789238 | 0x57d3dc |
| Kirikiroid2_1.3.9_iOS_arm64 | 9d56587c | sub_1002E9950 @ 0x1002e9950 | sub_1002E9A5C @ 0x1002e9a5c | 0x100104450 |
| Kirikiroid2_1.3.9_iOS_armv7 | 64e6be69 | sub_2EA498 @ 0x2ea498 | sub_2EA55E @ 0x2ea55e | 0x101850 |

上述 12 个范围均在本轮调用原生 `mcp__idalib__decompile`。
取证后四端 Begin/End 统一标注为 `TVPRenderManager_OpenGL_BeginStencil_guess` /
`TVPRenderManager_OpenGL_EndStencil_guess`；表格保留取证前名称便于追溯。四库已保存。
以各文件 glRenderbufferStorage 的 code xref 定位 Begin，再由保存旧 renderbuffer
binding 的 global xref 定位 End；独立方法名搜索无结果，不据此判为函数缺失。
Begin 的虚表引用分别为 A64 0x1a2f7c8、A32 0x10c561c（Thumb 指针低位为 1）、
I64 0x101afd770、I32 0x1840800。原生结果保存在
`out/diagnostics/nekopara4-stencil-fix-20260912/<二进制名>.json`。

### 四者共同伪代码（先于 C++ 修改记录）

```cpp
BeginStencil(reftex) {
    if (currentFBOValid && currentRenderTarget != 0) {
        glGetIntegerv(RENDERBUFFER_BINDING, &previousRenderbuffer);
        glBindRenderbuffer(RENDERBUFFER, sharedStencilRenderbuffer);
        bool recreate = false;
        unsigned w = reftex->GetInternalWidth();
        unsigned h = reftex->GetInternalHeight();
        if (cachedWidth != w) { cachedWidth = w; recreate = true; }
        if (cachedHeight != h) { cachedHeight = h; recreate = true; }
        if (recreate)
            glRenderbufferStorage(RENDERBUFFER, DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT,
                                  RENDERBUFFER, sharedStencilRenderbuffer);
        glFramebufferRenderbuffer(FRAMEBUFFER, STENCIL_ATTACHMENT,
                                  RENDERBUFFER, sharedStencilRenderbuffer);
    }
}
EndStencil() {
    glDisable(STENCIL_TEST);
    glFramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, 0);
    glFramebufferRenderbuffer(FRAMEBUFFER, STENCIL_ATTACHMENT, RENDERBUFFER, 0);
    glBindRenderbuffer(RENDERBUFFER, previousRenderbuffer);
}
```

深层调用者均在 stencil count>0 时调用 manager 的 Begin，随后 disable depth test、
stencil mask=255、clearStencil=0、clear depth|stencil、op=REPLACE/KEEP/KEEP、
depthMask=false、disable stencil test、清 shared enabled cache。逐 batch 使用模板状态，
正常尾部 flush 后调用 End。不增加异常回滚或改变缓冲生命周期。

### 逐文件差异

- A64：尺寸接口在 texture 虚表 +24/+32；Begin 分支合并为同一 return，End 为 GL 尾调用。
- A32：尺寸接口 +12/+16；Begin 的返回寄存器被反编译为 int，但调用者不消费。
  glRenderbufferStorage 的旧 IDB call type 漏掉第 4 参数；已修正 thunk/import 类型，
  当前缓存伪代码仍显示三参数。本轮 disasm 独立确认 R3 保存 height、R2=width、
  R1=0x88f0、R0=0x8d41 后 BLX，属于反编译类型残留，不是三参数调用。
- I64：尺寸接口 +24/+32，GL API 类型完整，尾调用形式与 A64 相同。
- I32：尺寸接口 +12/+16，GL thunk 尾调用；FBO gate 的 global 被 IDB 合并为大数组偏移。

所有平台的 gate、宽高更新顺序、重分配条件、格式 0x88f0、depth 后 stencil
的挂载/卸载顺序和恢复 binding 一致。四端均无 WebGL 分支。

## 本地逐段对照与拟修改

修改前 `cpp/core/visual/ogl/RenderManager_ogl.cpp`：

| 原行 | 对照 |
|---|---|
| 5014 | 与四库相同的 FBO/target 双 gate |
| 5015–5016 | 保存旧 binding，绑定共享 stencil buffer |
| 5017–5027 | 两次尺寸虚调用，宽/高独立更新，尺寸变化才分配 |
| 5028–5031 | 原生 DEPTH24_STENCIL8 调用与四库一致；缺 WebGL 1 格式适配 |
| 5032–5035 | 原生双 attachment 与四库一致；缺 WebGL combined attachment 适配 |
| 5041–5046 | disable、卸载、恢复与四库一致；Web 必须对称卸载 combined attachment |

改动限定在 `__EMSCRIPTEN__` API 边界：重分配时查询实际 context majorVersion，
WebGL 1 使用 DEPTH_STENCIL，WebGL 2 保持 DEPTH24_STENCIL8；Web 两种版本均使用
DEPTH_STENCIL_ATTACHMENT 挂载/卸载同一个 packed buffer。非 Web 参数与顺序保持四库行为。
不改 PSB、坐标、batch、stencil mask/ref/op、颜色/纹理计算或 source/target 对象。

第一次 Debug 回归在版本检测处遇到 Emscripten 6.0.9 的 SDK 问题：
`emscripten_webgl_get_context_attributes` 把 `GL.currentContext.attributes.enableExtensionsByDefault`
直接写入整型堆；SDL 创建的 context 未设置该字段，其值为 undefined，触发 SAFE_HEAP 断言。
日志调用栈和实际属性记录在 `context-attributes-sdk-failure.json`。
改用 EM_ASM_INT 只读取 `GL.currentContext.version`，与 SDK 查询该字段的来源一致，
避免复制与版本判断无关的可选 context 属性；不修改 SDK 或全局默认属性。

已有 MP-G23 文档把“WebGL2 支持该调用”当成当前运行状态，缺少实际 context 验证；
本次同步纠正该假设及 coverage 表对应结论。能力可表达不等于当前参数合法。

## 验证

Emscripten 6.0.9 下 Web Debug 和 Web Release 构建均通过。只保留已有压缩纹理
enum case、TJS 和 JS library 等编译警告，`git diff --check` 通过。

完整游戏 `/Users/fenghengzhi/Downloads/【KRKR】【官中】NEKOPARA 4.zip`，
`entry=运行游戏.xp3&loadMode=lazy`，独立 Chromium 153 profile，禁用 HTTP 缓存。
读取 ZIP 内 No.01（2026/09/12 02:31），全程使用鼠标输入并核对五类 capture 事件。

Debug 回归：

- 读档后香草立绘出现；连续推进两句对白后巧克力、香草同时正确显示。
  `debug-loaded.png` / `debug-third.png`；从游戏内再次读取 No.01 得到相同稳定画面
  (`debug-reloaded.png`)。
- 实际 context 仍为 WebGL 1.0。记录到一个 2048×2048 renderbuffer 分配，
  format=34041 (`DEPTH_STENCIL`)，attachment=33306 (`DEPTH_STENCIL_ATTACHMENT`)；
  同一对象持续成对挂载/卸载，复用期间没有重复分配。
- 初次采样：1745 次 draw，其中 456 次启用 stencil；完整 Debug 会话累计 31064 次
  draw、13278 次 stencil draw，`checkFramebufferStatus` 失败次数均为 0。
  stencil viewport 为 1920×1080，最终截图为 1280×720。
- 原先 invalid internalformat / conflicting attachments / OpenGL 0x0506 不再出现。
  读取同一实际资源验证，未创建替代游戏或人造 motion fixture。

用户随后明确确认：原版同一存档中的巧克力也会短暂出现后消失，原版本身存在此问题，
要求当前项目对齐该表现。因此不把“巧克力在该对白持续显示”列为修复目标，
不修改 Emote 动作恢复/结束逻辑。稳定读档画面与后续双人对白均已记录。

最终构建：

- Debug wasm 78201124 bytes，SHA-256
  `85eba013412a72fd704a641007616640ab238f01265a4cd7ea16ae9585eb6fce`。
- Release wasm 22635239 bytes，SHA-256
  `cce5c0b5d0c6536c832d62426f09cbf316ebaf17006967f47d93a60b00e5af39`。

Release 的 info 级游戏脚本日志未输出；依赖 Debug 的 updatebgm 日志等待条件会超时，
截图确认已正常到达标题菜单后改用实际 UI 完成回归。

Release 使用同一完整 ZIP 和 No.01，未安装 GL 拦截或 TJS 诊断调用器：

- `release-loaded.png`：读档后香草立绘正常。
- `release-third.png`：推进两句，巧克力、香草同时显示，表情随对白更新。
- `release-runtime.json`：下载的 wasm 为上述 22635239 bytes，五类输入事件均为 4；
  无 Wasm abort、pageerror、unhandled rejection 或资源读取异常。
- 浏览器原生 console 的 WebGL warning 数为 0。

WebGL 2 格式保留原始 sized 格式，但本次
游戏默认创建 WebGL 1，不能把它报告成 WebGL 2 端到端验证。
