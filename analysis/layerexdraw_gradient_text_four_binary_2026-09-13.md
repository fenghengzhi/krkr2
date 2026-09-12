# LayerExDraw 渐变正文颜色：四文件取证

## 复现与独立运行时证据

输入为用户提供的完整游戏 ZIP。保留 data.xp3、adult.xp3 和同目录文件，浏览器选择
data.xp3，点击开始游戏并接受默认名字，到第一段浴室背景对话。正文大面积白色，
仅底部残留深色。自动化采用 playwright-cli 的 krkr2 session；鼠标五种事件均抵达页面。

游戏原有 main/default.tjs 设置普通正文为黑/灰渐变（0x000000、0x707070），已读
正文为粉色渐变（0xf3759c、0xfc9ec8）。原有 sysscn/msghack.tjs 的
FontGradTemp.updateSize 将 `[0,0,1,26]` 数组作为当前字号的渐变画笔 rect；
DrawTextWithGradationColor 先生成渐变图层，再以 dfMain/holdAlpha 的 copyRect
复制到字形 RGB，最后 operateRect 合成。它们是诊断输入，未改动原 ZIP 或脚本逻辑。

在当前 Web Debug 加只读诊断后记录：

```
gradient ff000000 ff707070
rect-array=1 values=0,0,1,26 rawNative=0x58fb58 classNative=0
brush rect=4.0797e-41,4.0799e-41,0,8.172165e-39
fill status=2 graphics=0x4c938d0 brush=0x0
draw 52x32 pitch=208 rect=0,0,52,26 pixel=00ffffff/00ffffff/00ffffff
```

输入数组本身正确，但 `RectFConvertor<RectF>` 查询未注册 C POD 类型的 class ID
（默认 0），把 Array 原生实例当作 ncb adaptor 读取；错误的非空指针使数组分支被
跳过。读出的宽度为 0，GDI+ 渐变画笔构造失败，FillPath 返回 InvalidParameter，
渐变图层保留透明白色。后面的 RGB 复制保留字形 alpha，因此显示为白字。
同样的未注册 POD 类型错误也存在于 getPoint。

## 四文件映射（均已 fresh decompile）

| 二进制 | PointF 转换 | RectF 转换 | createBrush | GdiPlus 注册 |
|---|---|---|---|---|
| Kirikiroid2_1.3.9_Android_arm64-v8a.so | sub_575488 @ 0x575488 | sub_576214 @ 0x576214 | sub_56CDFC @ 0x56cdfc | sub_579304 @ 0x579304 |
| Kirikiroid2_1.3.9_Android_armabi-v7a.so | sub_4C3C68 @ 0x4c3c68 | sub_4C4318 @ 0x4c4318 | sub_4BEDB8 @ 0x4bedb8 | sub_4C59A4 @ 0x4c59a4 |
| Kirikiroid2_1.3.9_iOS_arm64 | sub_1002FABA4 @ 0x1002faba4 | sub_1002FB288 @ 0x1002fb288 | sub_1002F4F48 @ 0x1002f4f48 | sub_1002FCBD0 @ 0x1002fcbd0 |
| Kirikiroid2_1.3.9_iOS_armv7 | sub_2FB850 @ 0x2fb850 | sub_2FBF64 @ 0x2fbf64 | sub_2F53A4 @ 0x2f53a4 | sub_2FDB18 @ 0x2fdb18 |

四个二进制及配套 IDB 均已核对存在且可读；本轮 session 分别为 168efc86、0d6ddd99、
0dddd36f、d9952ecf。server_health 核对 module、架构和基址。
iOS armv7 IDB 输入名为 thin-armv7；通过 get_bytes 读取 0x4000 的 Mach-O 头，
与原 fat 文件中 CPU=12/subtype=9、offset=16384 的 armv7 slice 的 32 字节完全一致。

注册字面量 RectF 的 UTF-16 地址按表顺序为 0x14c1cba、0x4c679c、
0x10197694e、0x1768cfa；均有 fresh find_bytes/get_bytes/xrefs_to。
PointF/RectF 的转换必须使用各自注册的类型 ID：

| 二进制 | PointF ID 槽 | RectF ID 槽 |
|---|---|---|
| Kirikiroid2_1.3.9_Android_arm64-v8a.so | 0x1ab1d88 | 0x1ab1db0 |
| Kirikiroid2_1.3.9_Android_armabi-v7a.so | 0x110f7d8 | 0x110f7ec |
| Kirikiroid2_1.3.9_iOS_arm64 | 0x101afe120 | 0x101afe148 |
| Kirikiroid2_1.3.9_iOS_armv7 | 0x1840ce4 | 0x1840cf8 |

## 改动前共同伪代码与逐文件差异

```
convert<T>(src):                          // T 是已注册的 PointF 或 RectF
    if src.type != object:
        dst = T()                        // 所有坐标为 0
    else:
        adaptor = src.NativeInstanceSupport(GETINSTANCE, T.classID)
        if query succeeds and adaptor != null and adaptor.instance != null:
            dst = *adaptor.instance
        else:
            accessor = ncbPropAccessor(src) // 持有/释放脚本对象
            if src.IsInstanceOf("Array") == TJS_S_TRUE:
                dst = float(readReal(0, 0)), float(readReal(1, 0)), ...
            else:
                dst = float(readReal("x", 0)), float(readReal("y", 0)), ...
                // RectF 继续读 width / height；PointF 到 y 结束
    output = dst

createBrush, type == LinearGradient:
    color1 = reverseRB(readInteger("color1", default=0))
    color2 = reverseRB(readInteger("color2", default=0))
    if point1 exists:
        p1 = convert<PointF>(point1)
        read point2 into same variant; p2 = convert<PointF>(point2)
        brush = new LinearGradientBrush(p1, p2, color1, color2)
    else if rect exists:
        r = convert<RectF>(rect)
        if angle exists:
            brush = new LinearGradientBrush(r, color1, color2,
                        readReal(angle,0), bool(readInteger(isAngleScalable,0)))
        else:
            brush = new LinearGradientBrush(r, color1, color2, readInteger(mode,0))
    else:
        throw "must set point1,2 or rect"
    apply common brush parameters
    if wrapMode exists: brush.SetWrapMode(integer(wrapMode))
    release temporary variants / accessor
    return brush
```

`reverseRB(c) = (c & 0xff00ff00) | ((c & 0xff) << 16) | ((c >> 16) & 0xff)`，
保留 alpha 和 green。四文件均在 createBrush 内、进入 GDI+ 构造器前执行，
不是在最终屏幕输出处补偿。这里只修改被本游戏使用的线性渐变两个端点。

- Android arm64：标量 float 转换与 8/16 字节拷贝；native instance 位于 adaptor+8，
  class ID 查询用虚表字节槽 200；颜色交换以移位/按位或表达。
- Android armv7：指针为 4 字节，adaptor+4、虚表字节槽 100；double 返回由整数寄存器对
  传递；颜色交换中的 UXTB16 是 ARM 编译器展开。
- iOS arm64：与 Android arm64 共享上述语义，局部变量保存和返回寄存器形态不同。
- iOS armv7：与 Android armv7 共享上述语义；Hex-Rays 将浮点寄存器保存显示为额外
  未初始化局部变量，属于 ABI 保存序列。
- 这些目标函数的分支、默认值和颜色转换没有观察到平台行为差异。libgdiplus 的
  实际底层后端不同，不将其实现差异据此抹平；本次不修改底层 GDI+。

## 本地逐项对应

- `general/main.cpp::getPoint`：将 `PointFConvertor<PointF>` / POD 返回临时改为
  `PointFConvertor<PointFClass>` / `PointFClass`，与 `NCB_SUBCLASS(PointF, PointFClass)`
  的注册类型一致。仍经同一个 converter 的 native→Array→Dictionary 分支，保留默认值。
- `general/main.cpp::getRect`：同理使用 `RectFClass`，与
  `NCB_SUBCLASS(RectF, RectFClass)` 一致；恢复正确的 class ID 和数组 fallback。
- `general/LayerExDraw.cpp::createBrush`：线性渐变颜色读取后立即使用
  TVP_REVRGB，再构造 Color；其后的 point/rect/angle/mode 和 common 参数路径保持原结构。
- 不改变 ncbind 全局类型注册、不添加 class ID 特判、不绕过原有 converter，
  不改游戏脚本的颜色，不引入绘制失败后的替代颜色。

## 验证

修复后的 Web Debug 在同一个原始游戏流程中记录：

```
gradient ff000000 ff707070
brush rect=0,0,1,26
fill status=0, brush 非空
draw 52x32 pitch=208 rect=0,0,52,26 pixel=ff010101/ff242424/00ffffff
```

第一句正文恢复完整、可读的灰黑渐变。进入游戏设置→文本设置，原有已读文本示例
显示为粉色，记录的渐变端点为 `ff9c75f3/ffc89efc`，绘制成功；RGBA 缓冲区
首像素为 `ff9c75f3`（字节为 f3/75/9c/ff），与脚本的 RGB `f3759c` 一致。
说明两项修复分别恢复了渐变画笔生成和已读颜色通道顺序。

临时日志已移除；最终 `cmake --build out/web/debug` 与
`cmake --build out/web/release` 均退出 0，`git diff --check` 通过。
保留原有 deprecated literal operator 和 Emscripten JS library 警告，没有新增构建错误。
现有游戏资产用于验证，
本轮不从零制造额外游戏 fixture。PointF 原生/字典分支由四文件证据与构建核对，
此游戏的直接复现覆盖 RectF 数组分支。

修正并保存四个 IDB 中这轮遇到的 UTF-16LE 类型标注和相关函数注释。
