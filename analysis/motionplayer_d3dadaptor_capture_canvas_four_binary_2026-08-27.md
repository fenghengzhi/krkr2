# D3DAdaptor captureCanvas 软件行复制与 GPU 纹理交换四参考二进制联合恢复

日期：2026-08-27

## 1. 结论

本 slice 闭合 D3DAdaptor 注册面最后一个 pending callback：`captureCanvas`。四端具有同一
17-block、cyclomatic complexity 8 的控制流：先把唯一 Variant 转换为 native Layer，再按
当前 renderer 分为 software row-copy 与 GPU texture-swap 两条完全不同的数据路径。

本地总体结构正确，但联合证据发现并修复两处边界偏差：

1. GPU 路径原版只执行 `GetMainImage()` 内联的 `ApplyFont`，不会先执行
   `IndependMainImage()`；多余的 independence 会在共享 bitmap 时改变被选为复用候选的
   texture 身份和引用拓扑；
2. software 非等 pitch 路径的单行长度是 `size_t(width) * sizeof(uint32_t)`。因此 LP64
   在乘 4 前把 width 扩为 64 位，ILP32 自然保持 32 位 `size_t` 模运算；原本
   `size_t(width * 4)` 会在 LP64 仍先发生 int32 乘法。

## 2. 四端函数等价类

| 平台 | callback | size | blocks | 完整指令数 |
|---|---:|---:|---:|---:|
| Android arm64 | `0x6AAD0C` | `0x1BC` | 17 | 111 |
| Android armv7 | `0x57CF94` | `0x108` | 17 | 107 |
| iOS arm64 | `0x100103DBC` | `0x1CC` | 17 | 115 |
| iOS armv7 | `0x10116E` | `0x10E` | 17 | 109 |

四端 callback 都只有公开 registrar/xref 入口，没有函数体内部 catch 或恢复 landing pad。
任一 Layer/texture/render-manager 调用抛出时，已经发生的引用或对象状态变更不会由
`captureCanvas` 自己回滚。

## 3. software 路径数据流

共同伪代码：

```text
layer = Layer::FromVariant(argument)

if isSoftwareRenderManager():
    width  = targetTexture.width
    height = targetTexture.height
    layer.SetSize(width, height)

    src = targetTexture.GetScanLineForRead(0)
    dst = layer.GetMainImagePixelBufferForWrite()
    srcPitch = targetTexture.GetPitch()
    dstPitch = layer.GetMainImagePixelBufferPitch()

    if srcPitch == dstPitch:
        memcpy(dst, src, size_t(int32(srcPitch * height)))
    else if height >= 1:
        rowBytes = size_t(width) * sizeof(uint32_t)
        repeat exactly height times:
            memcpy(dst, src, rowBytes)
            src += srcPitch
            dst += dstPitch
    return
```

关键边界：

- Variant 到 Layer 的转换发生在 renderer 分支之前；非法/空 Layer 不会被静默忽略；
- target texture、scanline 0、Layer 写缓冲和 pitch 都没有本地 null/范围检查；
- `SetSize` 先发生，因此其部分状态即使后续取缓冲或复制失败也不会回滚；
- 等 pitch 路径没有 `height >= 1` gate。`srcPitch * height` 明确在 signed int32 中乘法，
  再符号扩展/转换为 `size_t`；height 为 0 时仍进入零长度 `memcpy`；
- 非等 pitch 路径仅在 signed height 大于等于 1 时循环；width 没有非负检查；
- LP64 的 per-row length 由 signed width 扩宽后左移 2，ILP32 在 32 位左移。共同 C++
  形状正是 `size_t(width) * sizeof(uint32_t)`；
- Layer 写缓冲 helper 自身会令 main image 可写并设置图像修改状态，这不是纯指针 getter。

本地 `copyTargetTextureRows_guess` 是同一 row-copy 规则的内部测试入口，但公开
`captureCanvas` 还包括 Variant 转换和 `SetSize` 先行副作用，不能把两者视为完全相同
的脚本边界。

## 4. GPU 路径：旧 Layer texture 与 adaptor target 的交换

共同伪代码：

```text
// GetMainImage first applies pending font state; no IndependMainImage call.
candidate = layer.GetMainImage().GetTexture()
replacement = null

if !candidate.IsStatic()
   and candidate.width  == targetTexture.width
   and candidate.height == targetTexture.height:
    candidate.AddRef()
    replacement = candidate

layer.AssignTexture(targetTexture)
targetTexture.Release()
this.targetTexture = replacement

if this.targetTexture == null:
    this.targetTexture = privateOpenGLRenderManager.CreateTexture2D(
        pixels=null, pitch=0,
        width=this.width, height=this.height,
        format=RGBA, flags=0)
```

### 4.1 引用转移

当旧 Layer texture 非静态且尺寸与 adaptor target 相同：

1. callback 先对候选旧 Layer texture 增加一个引用；
2. `Layer::AssignTexture` 把 adaptor target 交给 Layer bitmap：不同指针时释放 Layer 原旧
   texture、保存 target 并 `AddRef` target；即使 texture identity 相同，该 helper 后续的
   image-size、dirty、clip 和 update 副作用也仍会执行；
3. callback 释放 adaptor 原有 target 引用；Layer 已经持有它；
4. adaptor slot 接收候选旧 Layer texture 的预留引用。

结果是两边交换所持 texture，而不是复制像素。候选为 static 或尺寸不匹配时不预留引用；
Layer 接收旧 target 后，adaptor slot 先变 null，再通过 Motion 私有 OpenGL manager 创建一个
`RGBA` 新 target。

### 4.2 顺序和异常边界

- 对 candidate 的 `IsStatic`、width、height 调用都早于 Layer 赋值，没有 candidate null
  检查；
- 合格 candidate 的 `AddRef` 早于 `AssignTexture`。若后者抛出，callback 没有清理该额外
  引用；这是四端共同的尖锐边界；
- `AssignTexture` 完成后才释放 adaptor 原 target，避免 Layer 在接管前失去最后引用；
- 原 target `Release` 后立即发布 replacement/null。若随后 `CreateTexture2D` 抛出，adaptor
  target slot 保持 null，没有 rollback；
- 新 texture 尺寸取 adaptor 自身 width/height 字段，不取刚才的 Layer/candidate 尺寸；
- 创建调用固定 `pixels=null`、`pitch=0`、`RGBA`、flags 0，没有失败 null 检查。

## 5. 共享 Layer helper 的 fresh disposition

为避免把 callsite 名称猜测当成事实，本轮还 fresh 读取了四端同一组 helper：

| 角色 | Android arm64 | Android armv7 | iOS arm64 | iOS armv7 |
|---|---:|---:|---:|---:|
| `SetSize` thunk | `0x805724` | `0x62F6A4` | `0x100078A98` | `0x75C94` |
| write-buffer getter | `0x807C00` | `0x630F28` | `0x10007A980` | `0x77AC2` |
| Layer buffer pitch | `0x807C20` | `0x630F40` | `0x10007A9A0` | `0x77ADA` |
| `ApplyFont` | `0x80C848` | `0x634004` | `0x10007E854` | `0x7BD38` |
| `AssignTexture` | `0x8071A0` | `0x6308A8` | `0x10007A164` | `0x772FC` |

`ApplyFont` helper只在 font-dirty 且 MainImage 存在时清 dirty 并把 font 写入 bitmap；它不
执行 bitmap independence。四端 GPU 分支在读 MainImage texture 前只有这一个 Layer helper
call，因此可以排除 `IndependMainImage`。

`AssignTexture` helper 则继续执行 native bitmap texture assignment、image-size 同步、
ImageModified/clip reset 和 Layer update；它不是一个只换 raw pointer 的叶函数。

## 6. 本地改动与验证

- `cpp/plugins/motionplayer/D3DAdaptor.cpp:82` 保留 software/GPU 两分支和原始状态顺序；
- 删除 GPU 分支中参考实现不存在的 `IndependMainImage()`；
- 把 per-row size 改为先转换 width 到 `size_t`，再乘 `sizeof(uint32_t)`；
- `tests/unit-tests/plugins/motionplayer-dll.cpp:11236` 现有测试覆盖 target 两行、不同目标
  pitch 和逐行落点；D3D texture cache/holder 生命周期由紧随其后的测试覆盖；
- 四端 callback、五组共享 Layer helper已命名/核实，software/GPU 分支和引用转移点已
  添加注释与书签，四个 IDB 已原位保存。

本轮之后 D3DAdaptor 的 15 个非 Factory NCB callback 已全部逐体闭合，NCB 总账中的
`captureCanvas` 可提升为 `IMPLEMENTED`。随后
`MP-L14-D3DADAPTOR-LIFECYCLE` / `MP-D14-D3DADAPTOR-CLEAR` 已闭合完整构造/析构异常
生命周期、进程 shared adaptor 与 target clear/render cache；随后
`MP-R14-MOTION-PRIVATE-OPENGL-ENVELOPE` 已闭合私有 manager root 与 target bind envelope，
software source map 后来由 `MP-R14-D3D-SOURCE-GETTER-MAP-INSERT` 闭合；shared deep
renderer 的外层、batch、method 与 stencil 后来由 `MP-R14-D3D-DEEP-BATCH-STENCIL` 闭合；
公共 mesh helper 又由 `MP-R14-D3D-MESH-SUBMIT-CELLS` 独立闭合。

当前环境缺少 CMake、Ninja 和 Emscripten，且单头文件语法检查被缺失的
`boost/locale.hpp` 阻塞，因此本 slice 不宣称完成正式 native/Web 构建。

2026-09-13 纠正：四个上述尺寸 thunk 深入目标体后确认修改 Layer Rect，
对应 SetSize/InternalSetSize，旧 SetImageSize 命名错误。四文件 fresh 证据见
`alphamovie_reconstruction_four_binary_2026-09-13.md` 的共享 helper 纠正段。
当前 D3DAdaptor.cpp 的 software capture 仍调用 SetImageSize；本笔记历史
IMPLEMENTED 结论不能覆盖这项新发现的偏差。其调用站点应另行四文件复核，
本次 AlphaMovie 工作没有借此修改其它插件的 C++ 行为。
