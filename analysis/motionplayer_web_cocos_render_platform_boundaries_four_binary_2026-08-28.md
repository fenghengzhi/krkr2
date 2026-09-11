# MotionPlayer Web/Cocos 与参考渲染栈的平台边界四参考联合审计

日期：2026-08-28  
原始任务：`MP-G23`

2026-09-12 更正：下文 WebGL2 是能力分析的前提，不是实际 context 检测结果。
Cocos SDL 当前请求 GLES 2.0，NEKOPARA 4 实测运行于 WebGL 1；原来的 stencil
格式/双 attachment 设置在该 context 上非法。详见
[四文件取证与 WebGL 修复](nekopara4_webgl_stencil_four_binary_2026-09-12.md)。

## 1. 结论

四个参考二进制要求的MotionPlayer核心渲染语义，在当前Web/Cocos架构中都可以表达：

- named private OpenGL manager与默认software manager彼此独立；
- RGBA target texture、FBO bind、target-as-source snapshot；
- affine、Bezier和composite mesh的显式triangle/source-coordinate提交；
- Add/Sub/Mul/Screen/Alpha blend和alpha-test discard；
- 8-bit stencil reference、depth-stencil attachment和逐item stencil state；
- software pixel loop、Canvas/TJS Layer调用、GPU alpha-mask路径；
- target-local framebuffer到Cocos texture/sprite的交接。

所以不能把“Web不是原生Android/iOS”“oracle暂时看不到GPU内部状态”或“尚未跑Web构建”标成
`PLATFORM_BOUNDARY`。当前没有一个参考per-vertex状态、blend模式、mask操作或stencil动作因为WebGL2
API缺失而只能删掉。

真正不可由motionplayer源码消除的边界只有三组：

1. **跨GPU/driver的像素实现差异**：GLSL浮点精度、texture filtering/rasterization、边缘覆盖和最终
   UNORM舍入并不保证在不同GPU、浏览器和四个原生设备间bit-exact；四个参考端自身也没有建立跨
   GPU bit-exact契约。CPU几何、draw-call序列和API状态仍必须一致。
2. **WebGL资源与context模型**：最大texture/renderbuffer尺寸由浏览器和GPU决定；WebGL context可
   异步丢失，客户端vertex-array由Emscripten `FULL_ES2`模拟，validation/error/failure时点无法等同
   native GLES driver。正常可用context上的目标draw结果仍可表达，不能以此放宽正常路径。
3. **Cocos/浏览器最终呈现**：MotionPlayer产出的target-local texture交给Cocos sprite以后，CSS/
   device-pixel scaling、浏览器compositor、显示色彩管理、RAF/tab throttling和physical-screen present
   已经超出motionplayer.dll契约。可以比较交接前framebuffer，不能宣称不同显示栈的physical pixels和
   present timestamp必然一比一。

可表达部分可以保留在共享源码/私有OpenGL backend中。原审计没有实际检查 context 版本，
因此“无需 production 修改”的结论过强；2026-09-12 已确认需要 WebGL 1 stencil API 适配。

## 2. 本轮 fresh 四端证据总量

本轮使用原生`mcp__idalib__*`对28个独立函数范围重新执行decompile、完整disassembly和
`xrefs_to`审计。所有disassembly均为`truncated=false`，所有decompile均无error。

| 平台 | 独立范围 | 完整指令 | `xrefs_to` | IDB 更新 |
|---|---:|---:|---:|---|
| Android arm64 | 7 | 6,865 | 44 | 7条任务注释、1个书签 |
| Android armv7 | 7 | 5,395 | 41 | 7条任务注释、1个书签 |
| iOS arm64 | 7 | 4,591 | 43 | 7条任务注释、1个书签 |
| iOS armv7 | 7 | 6,561 | 43 | 7条任务注释、1个书签 |
| 合计 | 28 | 23,412 | 171 | 28条注释、4个书签；四库原位保存 |

七类fresh根是private manager getter、D3DAdaptor envelope、shared deep renderer、mesh submit、
Private Motion GLL `Draw_GPU`、alpha-mask operation和Canvas renderer。它们覆盖reference从Layer/
software边到实际GL triangles/stencil的完整API需求，而不是只抽查一个顶层router。

## 3. 四端根映射

| 语义根 | Android arm64 | Android armv7 | iOS arm64 | iOS armv7 |
|---|---:|---:|---:|---:|
| private OpenGL manager | `0x6930E4`，51/28 | `0x570EA0`，46/28 | `0x1000F3D90`，28/28 | `0xF0834`，68/28 |
| D3DAdaptor envelope | `0x6AB204`，100/2 | `0x57D2CC`，79/2 | `0x100104284`，87/2 | `0x101680`，140/2 |
| shared deep renderer | `0x6AB39C`，606/2 | `0x57D3DC`，655/2 | `0x100104450`，545/2 | `0x101850`，888/2 |
| mesh/cell submit | `0x69AFE4`，1,829/5 | `0x575800`，871/5 | `0x1000F974C`，787/5 | `0xF685C`，1,035/5 |
| Private GLL Draw_GPU | `0x6DA94C`，407/1 | `0x59BFB4`，420/0* | `0x10012A9B4`，416/1 | `0x129724`，621/1 |
| alpha-mask operation | `0x6AC4E4`，1,509/5 | `0x57E1E8`，1,433/3 | `0x100104E68`，1,197/4 | `0x10243C`，1,654/4 |
| Canvas renderer | `0x6C4820`，2,363/1 | `0x58E2CC`，1,891/1 | `0x1001186E0`，1,531/1 | `0x11653C`，2,155/1 |

表格单元为“完整指令/`xrefs_to`”。Android armv7 `Draw_GPU`由derived Layer vtable data pointer
真实可达；IDA对Thumb function-pointer没有生成普通code xref，不能把0误判为dead code。

## 4. reference渲染能力清单

四端共同调用链要求以下数据和操作：

```text
Player CPU state
  → PreparedRenderItem vectors
  → source/target texture callbacks
  → private named OpenGL manager
  → render-method selection + uniform color/threshold
  → optional target snapshot for destination-reading blend
  → explicit source/destination triangle arrays
  → target clip viewport
  → optional depth/stencil attachment and GL stencil state
  → texture/FBO result

或：

PreparedRenderItem vectors
  → TJS Layer clip/copy/piledCopy/fillRect calls
  → engine Layer bitmap/texture
```

reference不要求geometry shader、compute shader、integer vertex attribute、per-vertex color、
multisample resolve、native window handle或不可移植的D3D command buffer。D3D命名是plugin API历史，
四端实际deep path统一落在named `opengl` manager。

## 5. 当前Web/Cocos后端能够逐项表达的语义

### 5.1 context与private manager

Web link明确启用`MAX_WEBGL_VERSION=2`和`FULL_ES2=1`。Motion通过
`TVPGetRenderManager(TJS_W("opengl"))`取得独立manager，不把process default software manager误当
GPU manager。Web启动时从live GL context读取`GL_MAX_TEXTURE_SIZE`并写`TVPMaxTextureSize`，使atlas/
packer使用实际客户端限制。

对应本地：

- `CMakeLists.txt:116`：WebGL2/FULL_ES2配置；
- `cpp/core/environ/cocos2d/AppDelegate.cpp:58`：live max-texture-size bridge；
- `cpp/plugins/motionplayer/MotionRenderBackend.cpp:21`：private manager getter；
- `cpp/core/visual/ogl/RenderManager_ogl.cpp:5050`：target texture转FBO target。

### 5.2 triangle和texture coordinates

CPU-side motion代码已经完成Bezier tessellation、cell admission和固定六顶点展开；backend收到的是
显式destination points和每张source texture对应的source points。Web OpenGL manager把destination
转换为clip-space float vertex array，把source坐标交给texture adapter，最后执行
`glDrawArrays(GL_TRIANGLES)`。Emscripten client-array模拟改变上传机制，不改变正常draw-call的数组内容。

没有reference per-vertex state被丢弃。`TL,TR,BL / TR,BL,BR`、`-0.5/+0.5`和target clip仍在共享
motion源码中决定。

### 5.3 destination-reading blend

Add/Sub/Mul等render method需要读取当前destination。Web backend有两条可表达路径：

- framebuffer-fetch扩展可用时直接读`gl_LastFragColor`；
- 不可用时把target区域复制到temporary texture，作为第二texture输入shader。

这两条都是既有OpenGL manager的通用能力。扩展缺失不是Motion blend不可表达，只影响实现路径、
性能及跨GPU最终舍入。method名字、uniform color、alpha threshold和blend tuple与reference调用一致。

### 5.4 alpha test、stencil和alpha mask

WebGL没有legacy `glAlphaFunc`时，backend用fragment shader的`discard`与uniform threshold表达同一
alpha-test gate。WebGL2支持`DEPTH24_STENCIL8` renderbuffer；实际 WebGL 1 context 则要求
`DEPTH_STENCIL` 和 `DEPTH_STENCIL_ATTACHMENT`，不能直接复用原生双 attachment 调用。
BeginStencil/EndStencil 必须按实际 API 配对挂载/卸载同一个 renderbuffer，Motion 继续执行
reference 的 mask、func 和 replace/keep 序列。

alpha-mask software分支直接改BGRA alpha；GPU分支继续用private manager method与blend tuple。
`GL_MAX`等本任务实际用到的blend equation在WebGL2能力范围内。不存在“Web只能忽略stencil composite”
或“只能把mask烘焙成普通alpha”的降级。

### 5.5 Canvas与Cocos handoff

Canvas route调用的是引擎自己的TJS Layer API，clip/copy/fillRect/piledCopy/assignImages仍在相同对象图
上执行。Layer最终texture通过`GetAdapterTexture`交给Cocos `DrawSprite`；同尺寸时复用Cocos texture
identity，否则设置texture rect、禁用额外sprite blend并重置sprite。

到这个handoff为止仍可以检查framebuffer内容和logical尺寸。之后Cocos scene graph、browser canvas
和display compositor才进入真正的平台边界。

## 6. 不成立的平台边界理由

以下项目均不得用于降低复原要求：

| 借口 | 判定 |
|---|---|
| “原版叫D3D，Web没有Direct3D” | 不成立；四端实际使用named OpenGL manager，Web同样注册该manager |
| “Web默认renderer是software” | 不成立；reference也区分default renderer与Motion private OpenGL manager |
| “没有framebuffer fetch扩展” | 不成立；target snapshot/second-texture fallback可表达同一destination read |
| “Web没有legacy alpha func” | 不成立；shader discard+threshold表达相同gate |
| “Cocos没有motion mesh对象” | 不成立；Motion先在CPU展开triangle，Cocos/OpenGL只消费显式数组 |
| “oracle看不到GPU state” | 不成立；这是验证限制，不是API能力限制 |
| “尚未正式Web build” | 不成立；属于`MP-V06..V08`未完成验证，不是技术不可能性 |
| “STL对象大小不同” | 不成立于render语义；portable C++容器表达同一源级owner/order，ABI padding不外泄 |

## 7. 真正的平台边界一：GPU像素实现

reference固定CPU数据流、render method字符串、shader/GL状态和triangle顺序，但不固定某一物理GPU的：

- fragment/texture interpolation内部精度；
- texture filter和边缘sample的最低位舍入；
- triangle shared-edge coverage与subpixel quantization；
- shader中`pow`、除法、NaN/Inf等implementation-dependent结果；
- normalized RGBA写回及blend accumulator的最低位；
- driver是否使用framebuffer-fetch或target-copy fallback带来的中间rounding差异。

这不是允许任意视觉偏差。可控制的输入状态、shader算法、filter/wrap、target format、viewport、
triangle序列和blend/stencil状态仍必须一致；differential应先比较这些产品，再为最终像素使用有依据的
tolerance。只有末位级、driver-defined部分是平台边界。

## 8. 真正的平台边界二：WebGL resource/context/failure模型

Web客户端的`MAX_TEXTURE_SIZE`、`MAX_RENDERBUFFER_SIZE`、texture units和内存预算取决于浏览器/GPU。
超过硬件上限的参考target无法由motionplayer源码强制创建；现有代码读取live limit是正确边界桥接，
不应伪造参考设备常数。

WebGL context loss是浏览器异步事件；原生reference的raw GL state泄漏、异常时未EndStencil、method
pointer publication和texture lifetime无法在context已经被浏览器撤销后继续保持相同物理object。
context恢复需要平台层重建资源。这个边界仅适用于loss/resource-failure路径，不能改变正常context的
reference cleanup/partial-commit语义。

`FULL_ES2`对客户端vertex arrays做WebGL buffer上传/validation；因此allocation/validation错误出现
在何时、GL error如何暴露不可能与四个native driver完全一致。成功提交的vertex值和draw order仍须
一致。

## 9. 真正的平台边界三：Cocos到physical screen

`TVPWindowLayer::UpdateDrawBuffer`之后，target texture成为Cocos sprite texture。后续包含：

- Cocos logical content size、viewport和design-resolution scaling；
- browser canvas backing-store与CSS pixel/devicePixelRatio换算；
- browser/GPU compositor的缩放、色彩空间、premultiplication与display conversion；
- `requestAnimationFrame` scheduling、后台tab throttling、丢帧和present时间；
- 显示设备自己的transfer function和subpixel布局。

这些都不是motionplayer.dll读取的ownerLayer/primaryLayer/paintBox数值链。正确验收分界是：

```text
严格/结构级比较：CPU state → draw products → engine target framebuffer
平台呈现比较：    Cocos texture → browser canvas → physical display
```

项目已有RAF锁相逻辑用于逼近Android主线程tick的可观察节奏，但浏览器不给应用native Choreographer/
display queue的相同控制权，所以physical present timestamp仍是明确边界。

## 10. ABI/ISA差异不是本任务的渲染降级许可

已有两个独立coverage slice记录AArch64 `FMAX`与ARMv7 ordered compare在NaN payload/signed-zero上的
machine级差异。WebAssembly采用自己的确定性FP profile；若要求bit-exact模拟某一ISA，需要显式
per-ISA compatibility layer。该问题属于编译器/ISA边界，不代表WebGL不能表达Motion render stack，
也不能用来忽略普通finite坐标或颜色差异。

同理，libstdc++/libc++/Web libc++的deque/map/vector header、allocation timing和异常ABI不同；共享
源码只复原可观察元素顺序、owner、publication和boundary behavior，不伪造native object padding。

## 11. 本地对照位置

- `cpp/plugins/motionplayer/MotionRenderBackend.cpp:21`：private manager；
- `cpp/plugins/motionplayer/MotionRenderBackend.cpp:278`：normal/alpha-test method selector；
- `cpp/plugins/motionplayer/MotionRenderBackend.cpp:452`：stencil begin/state/end；
- `cpp/plugins/motionplayer/MotionRenderBackend.cpp:507`：mesh/cell submit；
- `cpp/plugins/motionplayer/PlayerRenderTargets.cpp:948`：D3DAdaptor envelope；
- `cpp/plugins/motionplayer/PlayerRenderTargets.cpp:987`：deep renderer；
- `cpp/plugins/motionplayer/PrivateMotionGLL.cpp:250`：legacy private Layer GPU consumer；
- `cpp/plugins/motionplayer/PlayerRenderInternal.cpp:910`：software/GPU alpha-mask；
- `cpp/plugins/motionplayer/PlayerRenderTargets.cpp:1081`：Canvas route；
- `cpp/core/visual/ogl/RenderManager_ogl.cpp:2576`：framebuffer-fetch/target-copy blend选择；
- `cpp/core/visual/ogl/RenderManager_ogl.cpp:4740`：Web/Cocos OpenGL triangle consumer；
- `cpp/core/visual/ogl/RenderManager_ogl.cpp:5013`：depth-stencil FBO；
- `cpp/core/environ/cocos2d/MainScene.cpp:1113`：texture到Cocos sprite handoff；
- `cpp/core/environ/web/Platform.cpp:263`：browser RAF time bridge；
- `CMakeLists.txt:116`：WebGL2、FULL_ES2和Web runtime配置。

## 12. 验证状态

本轮完成23,412条完整指令、171个`xrefs_to`、28条任务注释、4个书签和四库保存。源码对照确认
reference需要的正常render API均有Web实现，不新增production近似或测试专用分支。

coverage与163-ticket映射随后重生成并执行严格列数、重复ID和`git diff --check`检查。正式Web Debug
build、浏览器framebuffer capture、GL state/draw-call differential、context-loss和多浏览器pixel tolerance
验证继续由`MP-V06..V16`追踪；它们未执行不改变本报告的平台能力判定。

`MP-G23`没有剩余task-local静态差异。coverage状态使用`PLATFORM_BOUNDARY`是为了明确上述三类不可控
边界，不是把可实现的render语义豁免为近似。
