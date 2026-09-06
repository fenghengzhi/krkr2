# Player accurate SeparateLayer renderer（四参考二进制，2026-08-27）

2026-09-07 纠正：第 6 节的“低 2 位 composite mode”取证正确，但本地实参曾漏掉
`& 3`，此前“实现已匹配”的结论不成立。这会使 NEKOPARA 0 高光遮罩从裁剪变成
alpha 填充，出现黑框。本轮重新反编译四端 accurate SLA、Canvas 和 alpha-mask
helper 后补回该操作；Canvas 原版传完整标志，保持不变。详见
`nekopara0_emote_highlight_stencil_operation_four_binary_2026-09-07.md`。

## 1. 入口与完整取证

| 端 | renderer | body instructions | iOS armv7 SjLj cleanup |
|---|---:|---:|---:|
| Android arm64 | `0x6C7088` | 2051 | — |
| Android armv7 | `0x590468` | 1676 | — |
| iOS arm64 | `0x10011A9E8` | 1328 | — |
| iOS armv7 | `0x118D70` | 1955 | `0x11A4F8`, 695 instructions |

四端 7010 条 renderer body 已通过四个独立 IDB session 按 240 条分页全部读取。A32
Hex-Rays 给出完整 1126 行伪代码；另外三端的 decompiler文本触及输出上限，因此结论
以 fresh full disassembly、调用/字符串索引和 A32完整伪代码联合建立，不拿截断文本
补猜。iOS armv7 的 110-case SjLj cleanup 另有 695 条指令，也已完整读取。

renderer入口及 cleanup 已命名/注释/bookmark，四个 IDB均保存。四端体积差异主要是
Variant参数数组、ARMv7 SjLj、AArch64内联 map swap 和 vector/Array growth，不是不同
算法。

## 2. target owner、Layer accessor 与 pass 开始顺序

共同 prelude：

1. 从 SLA `_targetLayer` 按值 copy 一个临时 Variant；
2. 对临时值 `AsObject()`，取得一个 Object-only AddRef；
3. 立即销毁临时 Variant的 Object/ObjThis owner，只让 raw Object owner跨函数；
4. 构造全局 `Layer` name accessor；
5. 通过 Layer class receiver、target raw Object作 objthis，依次读 width、height；
6. 以 `[0,0,float(width),float(height)]` 更新 Player particle outside rect；
7. SLA active/retired ordered maps交换，pass sequence清零；
8. 调 common `buildRenderCommands(main, aux, targetClip)`；
9. 遍历 main pointer-vector。

width/height没有 positive gate，PropGet HRESULT忽略，Variant-to-Integer异常传播。正常尾
先 clear retired Layers，再析构 Layer accessor，最后 Release target raw Object。异常
不会执行 end-pass，retired map故意留给下一次 begin swap。

本地 `ncbPropAccessor targetLayerOwner{sla->getTargetLayer()}` 正好产生上述临时-copy →
AsObject AddRef → 临时销毁形状；本轮删除的 trace不再插入额外 target resolve或 owner。

## 3. item admission 与 accurate clip

item 只以三个 native字段入场：skipFlag0 false、rawFlag16 false、opacity非零。blend 6
仍可进入 accurate renderer，只在稍后的 mask-buffer gate中有特殊含义。

clip算法：paint left/top 与 0做 fmax-number；paint right/bottom以 `< canvas`显式选择，
unordered选择 canvas。随后仅根据 viewport四个 float判断 `right >= left && bottom >= top`；
成立时 floor left/top、ceil right/bottom并与 paint/canvas结果相交。native item没有
`hasViewport` byte，缺失 viewport只由 inverted数值矩形表达。最终只拒绝 strictly
reversed边；相等和 unordered边界保留。

本地原来额外要求 Web sidecar `item.hasViewport`，会让“sidecar false但四数值有效”的
item跳过 native viewport裁剪。本轮已删除该 gate，并新增 production helper直连测试：
`hasViewport=false`、viewport `[1.2,2.2,6.2,18.2]` 仍产生 `[1,2,7,19]`。

clipWidth/height在 float相减后提升为 tjs_real；offset为 `-0.5f-clip.left/top`，保留
float运算次序。

## 4. payload、ordered map 与 Layer复用

每个 admitted item 构造 call-local `SeparateLayerPayload`，按顺序发布：completionType、
outline/meshline任一非Void、commandSrc、blendMode、四 packed colors、paintBox+viewport
八 float、条件 mesh vector和corners。layerId1转 uint32作为 ordered-map key，resolver
从 retired map复用或新建 Layer，并输出 createdOrChanged。

mesh vector联合证据：

- meshType 2复制 `commandCompositeMeshPoints`；
- meshType 1复制实际渲染用的 `meshPoints`（四端 offset与随后Bezier copy读取相同）；
- 其他类型两 vector为空。

本地错误地把 type-1 payload指向 `commandBezierPatchPoints`，使复用比较缓存的几何与
实际 copy几何不同。本轮已改为 `item.meshPoints`。当前 shipped comparator最终总返回
refresh，但 caller-side createdOrChanged gate和payload布局仍需精确保留。

resolver返回 owning base Layer Variant。随后再用“临时 Variant copy → AsObject AddRef →
临时销毁”取得 base raw Object owner；它跨越本 item的 source copy、mask、debug和发布，
不携带 closure ObjThis owner。

## 5. created/changed source copy

只有 createdOrChanged true执行图像刷新：

1. 覆写 Player persistent source descriptor/color并解析 source Layer Variant；
2. 再构造 source `ncbPropAccessor`，增加一个 Object-only owner；
3. 用 source Object自己作 receiver/objthis依次读 width、height；
4. base Layer `setSize(Real clipWidth, Real clipHeight)`；
5. sourceRect固定 `[0,0,width,height]`；
6. 按 meshType调用：
   - 0：affineCopy argc14，三点来自 corners TL/TR/BL加 offset，completionType，clear=true；
   - 1：Bezier point Array来自 `meshPoints`加 offset，uint32 cell division，
     bezierPatchCopy，clear=true；
   - 2：mesh point Array来自 commandCompositeMeshPoints，meshDivX/Y，meshCopy，
     clear=true；
   - 其他：只setSize，不发copy primitive。

本地原来只借用 source Variant raw Object，少了贯穿 width/height/copy的 Object-only
accessor owner。本轮补回 `ncbPropAccessor sourceLayerOwner`，恢复回调期间引用拓扑和
逆序释放。

## 6. optional mask Layer与 ancestor alpha chain

finalLayer初始copy base Variant。仅 low blend不是6且 item.parent非空时创建/复用
layerId2 ordinal：

- base.visible=false；
- finalLayer替换为mask Layer；
- 从临时 final Variant取得独立 masked raw Object owner；
- masked.assignImages(baseLayerVariant)，argc=1；
- masked.setSize(clipWidth,clipHeight)；
- 从 immediate parent向根遍历。

ancestor `rawFlag21 && !rawFlag16` 时选择 stencilComposite bit2 ? composedLayer :
leafLayer，坐标/尺寸由 clip差值 float-to-int toward-zero/saturating转换，调用 alpha-mask
operation，固定 opacity 64，传 Player maskMode和低2位 composite mode。否则若
`(stencilComposite & 3)==1`，故意用 argc4 调 Layer.fillRect；Layer拒绝该参数数目，
caller忽略结果并立即停止 ancestor walk。masked raw owner在 debug phase之前释放。

## 7. debug frame、publication 与 owner stack

outline/meshline任一非Void，且 `createdOrChanged || parent != nullptr` 时，renderer从
临时 final Variant取得一份独立 debug raw Object owner，按 meshType发 drawLine/
drawBezierPatchFrame/mesh frame，随后释放。

publication再次独立执行临时 copy → AsObject AddRef → 临时销毁，然后固定顺序：

1. `setPos(Real clip.left, Real clip.top)`；
2. `type = mapped layer type`，MEMBERENSURE；
3. `visible = 1`；
4. `opacity = item.opacity`（不使用 priorDraw折半）。

layer type映射：low1→14 additive，2/5→15 subtractive，3→16 multiplicative，4→17
screen，其余→2 alpha。

正常 item尾 owner逆序已由 iOS armv7指令注释和 110-case cleanup联合确认：publication
raw Object → final Variant → base raw Object → base resolver Variant；source accessor/Variant、
masked/debug owners在各自更内层scope提前死亡。任何中途异常只清理已live owner，不撤销
ordered-map节点、已发TJS调用或前一item publication。

## 8. 本地修改与验证状态

本轮修改：

- 删除 accurate renderer 的 motion-path/logo/headless trace、诊断 target resolve、
  renderedItems计数和每item附加owner；
- clip不再读取 Web-only hasViewport；
- type-1 payload改存实际 meshPoints；
- source刷新scope补回 Object-only accessor owner；
- 新增 accurate clip test-only直连入口和数值gate测试。

其余主控制流、Layer primitive参数、mask ancestor chain、debug/publication顺序和
begin/end pass与四端一致。本项标记 `IMPLEMENTED`；common buildRenderCommands、SLA
payload resolver/ordered map、alpha-mask primitive和各copy helper是已分离或后续独立
callee覆盖项，不用本项代替。

`git diff --check` 与 coverage 12列校验通过。正式 CMake/unit/Web build仍因本机无
CMake/Ninja/Emscripten且无既有 build/out未运行。
