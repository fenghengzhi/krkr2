# NEKOPARA 0 人物眼睛高光黑框：accurate SLA 遮罩操作参数

日期：2026-09-07。

## 复现与像素证据

完整游戏输入为 `/Users/fenghengzhi/Downloads/【KRKR】【官中】NEKOPARA 0.zip`，
入口 `启动游戏.xp3`，Web Debug，标题菜单 GAME START 后第一句
「早上 8 點！起床！」的巧克力、香草。四只眼睛高光周围均有不透明黑框。

通过 playwright-cli 的 `krkr2` 会话，在当前 Wasm 的间接函数表上临时安装
只转发原调用的观测包装器，定位到 `renderAccurateSeparateLayerAdaptor_guess`。
调试过程中一个属性观测包装器起初没有考虑 LLVM 删除 unused this 参数，发生
function signature mismatch；重新加载游戏并按实际 Wasm signature 修正，以下证据
来自重新加载后的正常调用。包装器未写游戏数据，源码修改前已恢复函数表。

独立捕获的坐标/像素链：

1. 现有 `tests/test_files/emote/e-mote3.0バニラパジャマa.psb` 经 mtndump 导出
   `src/face_eye_hi_l/icon1` 为 18×13，包含 80 个 alpha=0 像素，原始透明度正常。
2. 游戏运行中同尺寸源纹理也有 80 个 alpha=0 像素。源矩形
   `[0,0,18,13]` 经人物变形与 clip-local 偏移，提交顶点例如
   `(0.515,6.505),(16.279,0.723),(4.642,17.884),(20.411,12.101)`，
   写入 32×32 的底层存储；网格 copy 后仍有 887 个透明像素。
3. 跟踪小纹理的 GetScanLineForWrite→GetScanLineForRead，遮罩调用前后某帧
   alpha=0 从 883 降到 51，alpha=255 从 42 升到 891。
   最后一次写入口栈为 `applyMotionAlphaMaskOwnedVariants_guess`。
4. Layer→BltImage→software OperateRect 已收到被改坏的 alpha；对应屏幕高光区域
   如 `[352,273,376,292]`。再经 LayerManager/DrawDevice 的最终纹理和 Cocos
   显示，出现黑框。源解码和网格几何不是这次偏差的起点。

最初检查过 GPU 加算混合，但当前 GL program 只有 Cocos 呈现程序，实际人物走
软件 accurate SLA。源高光本来就是透明图像，不需要把普通 alpha 改为加算混合。

## 本轮四文件函数映射（均已 fresh decompile）

四个目标与配套 IDB 均已检查可读，并通过 server_health 校验 module/input。
iOS armv7 IDB 为同一 fat Mach-O 的 thin-armv7 slice；另外两库保留 Windows input path。

| 二进制 | database | accurate SLA / 遮罩调用点 | Canvas / 遮罩调用点 | alpha-mask helper |
|---|---|---|---|---|
| Kirikiroid2_1.3.9_Android_arm64-v8a.so | ee382dbd | 0x6C7088 / 0x6C8144 | 0x6C4820 / 0x6C5770 | 0x6AC4E4 |
| Kirikiroid2_1.3.9_Android_armabi-v7a.so | 2d9c5ae6 | 0x590468 / 0x591260 | 0x58E2CC / 0x58EFD2 | 0x57E1E8 |
| Kirikiroid2_1.3.9_iOS_arm64 | a104b54d | 0x10011A9E8 / 0x10011B7F0 | 0x1001186E0 / 0x100119338 | 0x100104E68 |
| Kirikiroid2_1.3.9_iOS_armv7 | a8b2af01 | 0x118D70 / 0x119D1A | 0x11653C / 0x117924 | 0x10243C |

所有目标已定位，无缺失/内联代替取证的情况。过长输出从原生 decompile 返回的
output URL 取回完整 JSON 后检查，而非依据截断摘要。

## 四者共同伪代码与差异

```cpp
for (ancestor = item.parent; ancestor; ancestor = ancestor.parent) {
    if (ancestor.drawable && !ancestor.blank) {
        mask = (ancestor.stencilComposite & 4)
            ? ancestor.composedLayer : ancestor.leafLayer;
        // 坐标差和尺寸先 float 运算，再向零转 int；source x/y 均为 0。
        alphaMask(finalLayer, ancestor.clip.xy - item.clip.xy,
                  mask, 0, 0, ancestor.clip.size,
                  64, player.maskMode, ancestor.stencilComposite & 3);
    } else if ((ancestor.stencilComposite & 3) == 1) {
        finalLayer.fillRect(0, 0, width, height); // 原生 argc=4，忽略失败
        break;
    }
}
```

四端 accurate SLA 均明确传 `& 3`。bit 2 只选择组合遮罩来源，不能进入最终裁剪 op。
关键 callee 行为四端一致：

| maskMode | op 1 | op 2 | op 5/6 | 其他 op |
|---|---|---|---|---|
| 0 | overlap 外清零；srcA<64 时清零，否则保留 dstA | srcA>=64 时清零，否则保留 | srcA>=64 时写 255，否则保留 | 不做像素运算 |
| 1 | overlap 外清零；dstA×srcA/255 | dstA×(255-srcA)/255 | srcA+(255-srcA)×dstA/255 | 不做像素运算 |

空 overlap 只有 op 1 清空完整 clip，其他 op 返回。其他 maskMode 不执行上述像素
分支。目标/source 的类型转换、持有、clip 和 update 顺序均沿用现有 helper。

需要保留的差异：

- **两条 renderer 的源码行为差异**：四端 Canvas 的对应调用均传完整
  `stencilComposite`，不是 `& 3`。组合遮罩构造也需要 op 5/6。因此不能在公共
  alpha-mask helper 里统一截断，也不能顺手修改 Canvas 调用。
- **ABI/反编译形态**：iOS arm64 accurate 调用把 threshold/maskMode 显示为一个
  64-bit `v109`，低/高 32 位分别是 64/maskMode；最终 op 仍独立 `& 3`。
  其他三端显示 11 个参数。ARMv7/arm64 的成员偏移和 Variant cleanup 不同，
  目标逻辑和位运算相同。

## 修改前本地逐项对照

`cpp/plugins/motionplayer/PlayerRenderTargets.cpp` 的 accurate SLA ancestor 循环：

- `rawFlag21 && !rawFlag16`：匹配 drawable/blank gate。
- `(stencilComposite & 4) ? composedLayer : leafLayer`：匹配 source 选择。
- 两个 clip 坐标相减、width/height 相减、向零转换：匹配。
- `64, _maskMode`：匹配固定阈值与 live maskMode。
- 最后实参原为完整 `ancestor->stencilComposite`：唯一确认的偏差，应改为 `& 3`。
- else 分支的低两位判断、argc=4 fillRect、break：匹配。

`PlayerRenderExecute.cpp` 的 Canvas 调用传完整值与四端一致，保留。
`PlayerRenderInternal.cpp` 的 op 5/6 扩张 alpha 与四端一致，保留。

既有 `motionplayer_accurate_sla_renderer_four_binary_2026-08-27.md` 第 6 节已记录
“低 2 位 composite mode”，但当时“实现已匹配”的结论漏检了本地实参。本次纠正实现
和该报告的验证结论，不改变原本正确的四文件语义记录。

## 验证

- Emscripten 6.0.9：`cmake --build out/web/debug` 成功。构建前停止 coi-server，
  构建后重启并从新浏览器会话加载完整 ZIP；页面脚本版本为 `20260907023952`。
- 第一段同一台词的巧克力、香草近景：四只眼睛的白色高光与透明边缘正常，
  修复前的不透明黑框消失。继续推进到巧克力、时雨中景，检查了眨眼、表情与
  人物缩放，未再观察到高光黑框。
- 运行控制台无引擎错误；唯一 ERROR 为 `favicon.ico` 404。临时函数表观测包装器
  已恢复，未加入生产源码。
- 对照文件位于 `out/diagnostics/nekopara0-highlight-20260907/`：
  `before.png`、`after.png` 为同一台词前后对照，`after-dialogue.png` 为后续台词。
  `mask-alpha-before.json` 保存修复前的写入链，`mask-alpha-after.json` 保存修复后
  后续表情中小纹理的 alpha 统计；后者不是同帧配对数据，不用于逐像素差分。

现有 alpha-mask 单元 fixture 只覆盖空 overlap，不能证明实际高光透明度；本次
复用完整游戏和现有 PSB，未新增资产 fixture。未进行原版设备的逐像素 oracle
对比，也未构建 Release；本次运行回归范围为上述 Debug 场景。
