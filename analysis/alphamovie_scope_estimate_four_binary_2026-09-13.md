# AlphaMovie 复原代码量初步估算

日期：2026-09-13。用途：工程规模估算，不是完成度审计或实施许可。

## 结论与计数口径

复用仓库已有 TJS、存储、Layer、线程基础设施及第三方库，AlphaMovie 专属
C++ 实现暂按 **3,000～5,000 行、中心预算 4,000 行**考虑。
计入 .cpp/.h、专属类型、数据表、绑定、解码、队列与生命周期；不计空行、
纯注释、测试、取证文档或整套第三方库。这个范围是基于关键链抽查的工程判断，
不是从机器码机械换算得到的精确源码行数，完整逐函数审计后可能调整。

复用公共组件不代表已经证明它们满足六维复原目标。若审计要求修改这些组件，
其改动量须额外计算。本估算不声称涵盖整条依赖链的全部复原工作。
四份二进制用于交叉取证，不表示写四套源码或把估算乘以四。

## 输入核对

四个目标与四个配套 .i64 均存在且可读；原生 idb_open/server_health/survey_binary
已调用。会话如下：

| 简称 | 二进制文件 | database | 位数 / 基址 |
|---|---|---|---|
| A64 | Kirikiroid2_1.3.9_Android_arm64-v8a.so | 31cd7e6e | 64 / 0 |
| A32 | Kirikiroid2_1.3.9_Android_armabi-v7a.so | 644e315e | 32 / 0 |
| I64 | Kirikiroid2_1.3.9_iOS_arm64 | 123463b0 | 64 / 0x100000000 |
| I32 | Kirikiroid2_1.3.9_iOS_armv7 | 0622bd5e | 32 / 0x4000 |

I32 数据库输入名为 thin-armv7。get_bytes 读取其 0x4000 的 64 字节 Mach-O
头，与参考 fat 文件 CPU=12、subtype=9、offset=0x4000 分片的 64 字节一致。
其余数据库保留历史输入路径的情况已与模块名、架构、对应 IDB 路径核对。

## 本轮 fresh decompile 映射

下表每列地址只属于上表指定二进制。角色名是分析标签，不声称是原始源码名；
实际函数名为相应 sub_<地址>，除 I64 模块初始化的 InitFunc_64 未纳入此表。

| 角色 | A64 | A32 | I64 | I32 |
|---|---|---|---|---|
| AlphaMovie 类注册 | 0x5228f0 | 0x48e42c | 0x1001cf98c | 0x1cdb10 |
| native instance 工厂 | 0x522f88 | 0x48e9bc | 0x1001cff44 | 0x1ce130 |
| native 构造 | 0x52447c | 0x48f79c | 0x1001d0d64 | 0x1cec7c |
| 内嵌解码对象构造 | 0x5245b0 | 0x48f87c | 0x1001d0efc | 0x1ceeb0 |
| open dispatch | 0x52309c | 0x48ea78 | 0x1001d0014 | 0x1ce230 |
| open native | 0x524e88 | 0x48fe14 | 0x1001d15d0 | 0x1cf5e0 |
| 文件头/量化表读取 | 0x524f3c | 0x48fe8c | 0x1001d1650 | 0x1cf6bc |
| showNextImage dispatch | 0x523264 | 0x48ebc8 | 0x1001d0188 | 0x1ce3b2 |
| showNextImage native | 0x525a24 | 0x490390 | 0x1001d1c48 | 0x1cfd0c |
| play native | 0x525c08 | 0x4904ec | 0x1001d1df8 | 0x1cff10 |
| setNextMovieFile dispatch | 0x5236b0 | 0x48ee80 | 0x1001d0470 | 0x1ce5a4 |
| 默认 IDCT 适配入口 | 0x5291f0 | 0x491c6c | 0x1001d39e8 | 0x1d1a78 |

默认 IDCT 入口由各库自己的函数指针槽 get_bytes 确认，32 位 Thumb 地址去最低位。
Android 两份入口直接调用 jpeg_idct_islow；iOS 两份调用对应独立 helper，
本轮只核对适配层共同结构，未宣称完成底层 JPEG 函数的四文件验证。

## 已确认范围

- 普通字符串列表搜索会遗漏 Android 中的 AlphaMovie。UTF-8/UTF-16LE/UTF-32LE
  find_bytes 均已执行；四份库都找到 UTF-16 的 AlphaMovie 与 AlphaMovie.dll，
  经 get_bytes 和 xrefs_to 定位注册函数，不能从普通 find 的空结果宣称缺失。
- 四份注册均含构造、finalize，以及 8 个业务方法：open、clear、showNextImage、
  isPlaying、play、stop、setPosition、setNextMovieFile。
- 11 个属性为 numOfFrame、frame、loop、nextLoop、preloadSamples、left、top、
  screenWidth、screenHeight、FPSScale、FPSRate。截断为单字符的 key 已读取
  原始 UTF-16 字节确认，并逐项 set_type；重新 decompile 的部分展示仍保留
  单字符，不能声称显示问题已全部解决。计数依据是注册调用与原始字节。
- 四份 open 链均进入内嵌解码对象：读取 40 字节头，检查 magic、revision、
  表大小、帧数、帧率、尺寸和属性，再读取量化表；失败清理流并返回失败，
  外层产生错误。此处仅概括范围，不作为完整分支实现规范。
- 四份 showNextImage 均从待显示链表取出帧，解锁后更新 Layer 图像及位置，
  然后重新加锁，将帧送入复用链表并通知生产侧；空队列返回已有帧号。
- 四份 play 均先停止既有播放、将待显示帧移到复用集合，按缓存规模收缩/扩充
  帧对象，再创建工作线程。列表节点与帧缓冲分别分配，不能省掉生命周期层次。
- Android 列表使用自环节点检查/遍历计数，iOS 展开含显式 size 字段；
  condition variable 的构造形态和对象大小也不同。属于需保留分析的 STL/平台/ABI
  差异，不能把某一库对象字节偏移硬凑到 wasm 类中。

## 解码取样与尚未完成的工作

本轮额外反编译 A64 的 0x526568 / 0x527228、I64 的 0x1001d2304 /
0x1001d2878 两组帧路径，看到块解码、颜色转换、Alpha 处理及临时缓冲清理。
此组尚未完成 A32/I32 对应函数的联合审计，不能据此宣布完整解码行为已证明。

I64 与 I32 的断言字符串保存了源码路径
`/Volumes/E/Projects/kirikiri2_mob/kirikiri2/src/plugins/AlphaMovie.cpp`。
两份本轮反编译均确认 build_codes:306、DecodeAC:397、Decode1:433、
DecodeFirstValue:460。这些是原文件局部行号，**不是原文件总行数**。

估算分配：绑定及专属类型约 400～700 行；格式、位流、解码表约 600～1,000 行；
帧解码和 Alpha/IDCT 适配约 800～1,300 行；播放、同步、双链表、生命周期和
边界处理约 1,000～1,700 行。合计 2,800～4,700，规划时取整为 3,000～5,000。
这些子项均为预算分配，不是已测量的源码统计。

尚未完成：所有虚函数入口/析构/异常与重入路径的闭包、全部 codec helper 的
四文件映射、平台优化入口核验、公共依赖符合度审计及运行时验证。
本轮没有修改 C++ 运行行为，也没有完成 AlphaMovie 实现。


## 后续实施计数（2026-09-13）

激活复原目标后，插件已落地在cpp/plugins/AlphaMovie.cpp：1541物理行，
1415行非空且非纯注释行（包含专属表、传统TJS宏绑定、播放生命周期和为16位量化
契约保留的局部JPEG 8x8内核）。测试与取证文档不计入。最初3000～5000行是
早期工程预算，不是原始源码行数或本次必须达到的代码量；应以当前实装计数与
alphamovie_reconstruction_four_binary_2026-09-13.md的覆盖/边界记录为准。
现有AMV物料读取超时；没有真实AMV差分结果，不据代码行数宣称100%已证明。
