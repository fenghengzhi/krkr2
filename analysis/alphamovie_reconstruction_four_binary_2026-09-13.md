# AlphaMovie 四文件复原实施记录

日期：2026-09-13。目标保持完整六维复原；本文件记录已经取证的实现步骤与剩余缺口。
四个文件简称及 IDB 对应关系沿用 alphamovie_scope_estimate_four_binary_2026-09-13.md。
本轮重新核对四个 binary/.i64 可读并调用 server_health；未使用外部上游源码。
原生 decompile 原始输出保存在 out/alphamovie-reconstruction/2026-09-13/{A64,A32,I64,I32}.json。
该区域现收集 A64 73、A32 90、I64 98、I32 98 个函数，区域收集不等于调用闭包证明。

## 位流与 Huffman 实施前证据

地址均只属于列头指定的二进制；未保留原名的角色标识使用 _guess。
下列所有显式入口在本轮均有原生 decompile 记录，A64 内联另通过对应调用体证明。

| 源码角色 | Kirikiroid2_1.3.9_Android_arm64-v8a.so | Kirikiroid2_1.3.9_Android_armabi-v7a.so | Kirikiroid2_1.3.9_iOS_arm64 | Kirikiroid2_1.3.9_iOS_armv7 |
|---|---|---|---|---|
| Huffman 构造 | sub_524830@0x524830 | sub_48FA5C@0x48fa5c | sub_1001D0FD4@0x1001d0fd4 | sub_1CEFEC@0x1cefec |
| build_codes | 内联于上一格 | 内联于上一格 | sub_1001D11B4@0x1001d11b4 | sub_1CF16C@0x1cf16c |
| PeekBits_guess | 内联于 sub_5282E4/sub_528578 | sub_491634@0x491634 | sub_1001D33BC@0x1001d33bc | sub_1D1358@0x1d1358 |
| ReadBits_guess | 内联于 sub_5282E4/sub_52886C | sub_4916F4@0x4916f4 | sub_1001D34E4@0x1001d34e4 | sub_1D1436@0x1d1436 |
| SkipBits_guess | 内联于 sub_5282E4/sub_528578 | 内联于 sub_491474/sub_491528 | sub_1001D359C@0x1001d359c | sub_1D14D2@0x1d14d2 |
| Decode1 | sub_52886C@0x52886c | sub_4916A0@0x4916a0 | sub_1001D3438@0x1001d3438 | sub_1D13B4@0x1d13b4 |
| DecodeFirstValue | sub_5282E4@0x5282e4 | sub_491474@0x491474 | sub_1001D3130@0x1001d3130 | sub_1D1190@0x1d1190 |
| DecodeAC | sub_528578@0x528578 | sub_491528@0x491528 | sub_1001D3244@0x1001d3244 | sub_1D124E@0x1d124e |

build_codes / Decode1 / DecodeAC / DecodeFirstValue 精确名字来自 iOS 两库断言函数名；
类名、字段名和 Peek/Read/Skip 的原名未证明，保留 _guess 标识。普通 C++ 类表达字段、
继承与尾部 DC 累积值，不用 padding 或 ABI sizeof 断言。

### 共同伪代码与边界

BitReader:
- 字段：当前 32 位字指针、原始字节结束指针、当前剩余位数、bswap 后的当前字。
- 构造：输入地址向下对齐至 4 字节；end=input+size；bits=8*(4-(input&3))；
  不检查 size 就读取对齐后的首字。构造在四库两条帧解码路径内联。
- Peek(n)：n<=0 返回 0；复制四个字段到局部。仅在 cur<end 时继续；
  bits>=n 时组合 mask[n] & (word>>(bits-n)) 返回，否则将剩余低位左移组合，
  n-=bits，读取 ++cur 的字、bits=32，直到返回。完全不写回对象。
- Read(n)：同样条件与组合，直接修改对象字段。恰好用完当前字时，
  bits=32，++cur，立即读取下一字，不在预读前额外检查 end。
- Skip(n)：cur+=n>>5；bits-=n&31；bits<=0 时 cur++，bits+=32，预读下一字；
  不走 Peek/Read 的 end 检查，不能替换为 Read 并丢弃结果。

Huffman:
- 内含 uint8[512] 消费位数、uint8[512] 原符号、int16[512] 展开值；
  符号表借用指针、int32 maxcode[18]、int32 valoffset[17]。
- build_codes：长度 1..16，nb=bits[length]，断言 nb+k<=256，
  按序写 uint16 codes[k++]=code++，每层结束 code*=2。
- 构造：仅把消费位数数组的 512 字节清零，其余数组不整体清零；
  有 code 的长度设置 valoffset 与 maxcode，无 code 仅设置 maxcode=-1，
  最后 maxcode[17]=0xfffff。
- 长度 1..8 建立 9 位 lookup 展开，符号低 4 位作为数值位数，
  相应填入消费总位数、原符号与已经符号扩展的 int16 值。
- Decode1：先 Read(9)，超过该长度 maxcode 时逐位扩展；
  最后断言长度<=16，按 symbols[valoffset[length]+code] 返回。
- DC：先 Peek(9)，命中且消费<=9 则 Skip(消费)；
  消费>9 则 Skip(9) 后把余位加到预计算值；不命中走 Decode1，
  断言 s>0，再 Read(s) 并符号扩展。结果累加到专属 DC 前值。
- AC：相同 lookup/fallback，原符号高 4 位是 run；非零系数写
  jpeg_natural_order[index+run]；零且 run!=15 返回当前 index，
  否则跳过 16 个位置；循环到 index>=64。不增加越界保护或把备用自然顺序表砍成 64 项。

### 四文件差异

- Android 把部分 helper 内联，A64 还对建表循环向量化；iOS 留独立 helper。
- iOS 保留上述四条 assert；Android 此区域没有对应 assert（构建配置差异），
  普通源码使用 assert，由 Debug/Release 的 NDEBUG 决定是否生效。
- 指针宽度、Huffman 基类尾部 padding 与 DC 派生字段偏移不同；
  共享结构用普通 C++ 字段/继承表达，不手工对齐到任何参考 ABI。
- 当前已核对的 mask[33]、四组 code-length 表及三个 value 表、jpeg_natural_order[80]
  在四库逐字节相同。首组八张表的 native get_bytes 输出保存在 *-tables.json。
- 本地从零增加上述类与方法，逐方法与本节同名伪代码对应。后续 frame/线程/绑定
  按各自四文件证据追加；未追加部分不视为已实现。

## 实施阶段说明

本文按先取证、再实施的阶段追加。下述文件/帧/生命周期、native绑定和平台wrapper
现已实装；最后一节记录构建、运行检查以及仍不能证明的边界。源码行数和区域函数
收集数量均不等于源码同一性证明。

## 文件/帧/生命周期实施前证据

本轮四库区域中的下面入口均 fresh decompile；表格列顺序固定为 A64 / A32 / I64 / I32，
其文件名对应首节的完整文件表。A64 工作线程被 IDA 合并进 play，额外 fresh disasm
在 loc_525E6C 显示独立 SUB SP/STP 序言至 0x526014 前返回；此地址未跨库复用。

| 角色 | A64 | A32 | I64 | I32 |
|---|---|---|---|---|
| 文件 Open | sub_524F3C@0x524f3c | sub_48FE8C@0x48fe8c | sub_1001D1650@0x1001d1650 | sub_1CF6BC@0x1cf6bc |
| 帧选择/循环 | sub_526418@0x526418 | sub_4908F8@0x4908f8 | sub_1001D21D0@0x1001d21d0 | sub_1D0380@0x1d0380 |
| 独立 zlib Alpha 帧 | sub_526568@0x526568 | sub_490AAC@0x490aac | sub_1001D2304@0x1001d2304 | sub_1D04E0@0x1d04e0 |
| 块编码 Alpha 帧 | sub_527228@0x527228 | sub_490E6C@0x490e6c | sub_1001D2878@0x1001d2878 | sub_1D09BC@0x1d09bc |
| 帧尺寸/分配 | 内联于上述两入口 | sub_491274@0x491274 | sub_1001D2E08@0x1001d2e08 | sub_1D0EE8@0x1d0ee8 |
| chroma 块 | 内联于上述两入口 | sub_4912F0@0x4912f0 | sub_1001D2EB0@0x1001d2eb0 | sub_1D0F54@0x1d0f54 |
| luma/alpha 块 | 内联于上述两入口 | sub_4913B4@0x4913b4 | sub_1001D2FDC@0x1001d2fdc | sub_1D1028@0x1d1028 |
| SeekFrame | sub_528F74@0x528f74 | sub_491AD8@0x491ad8 | sub_1001D3844@0x1001d3844 | sub_1D1864@0x1d1864 |
| native 析构 | sub_5246A8@0x5246a8 | sub_48F940@0x48f940 | sub_1001D1238@0x1001d1238 | sub_1CF1DC@0x1cf1dc |
| clear | sub_524C34@0x524c34 | sub_48FBFC@0x48fbfc | sub_1001D1360@0x1001d1360 | sub_1CF364@0x1cf364 |
| stop | sub_524D5C@0x524d5c | sub_48FCA8@0x48fca8 | sub_1001D1414@0x1001d1414 | sub_1CF3D2@0x1cf3d2 |
| decoder 析构 | sub_524E20@0x524e20 | sub_48FD40@0x48fd40 | sub_1001D156C@0x1001d156c | sub_1CF51C@0x1cf51c |
| native set frame | sub_528E58@0x528e58 | sub_4919F0@0x4919f0 | sub_1001D3750@0x1001d3750 | sub_1D1730@0x1d1730 |
| worker | loc_525E6C@0x525e6c (disasm) | sub_49069C@0x49069c | sub_1001D1FBC@0x1001d1fbc | sub_1D012C@0x1d012c |
| 条件变量 Wait | sub_526348@0x526348 | sub_490894@0x490894 | sub_1001D217C@0x1001d217c | sub_1D0350@0x1d0350 |

### 共同顺序与分支

- Decoder 构造只初始化 Stream=null、四个 Huffman 表与两个 DC 累积值、
  SwsContext=null、Loop=true、NextLoop=true、额外字节标志=false、NextFile 默认 ttstr。
  量化表、帧数/帧率/尺寸、文件位置不在构造时补零。
  最后调用共享 TVPInitLibAVCodec。此 helper 四文件为
  A64 sub_953470@0x953470 / A32 sub_6F1E5C@0x6f1e5c /
  I64 sub_100056294@0x100056294 / I32 sub_55090@0x55090，
  共同一次性 lockmgr/register_all/network_init，与本地 krffmpeg.cpp 对照。
- Open：先 delete 旧 Stream，再直接赋新 TVPCreateStream；不能插入 stop/clear，
  创建抛异常时不能提前把 Stream 清空。失败日志按原拼写，失败汇合处 delete并置null。
  40 字节头：magic=1297107521，revision=0，headerSize 只能168或232，
  frameCount/FPSScale/FPSRate/width/height必须非零；按原顺序逐字段发布。
  属性 bit0 优先：bit0=1 时 mode=false 且必须232；否则bit1=1 时mode=true且必须168；
  两位均无则失败。读 headerSize-40 字节量化表，保存当前位置两次，Frame=0；
  三张64项byte→int16表顺序填充，只有128字节时第三张清零。
- FrameBuffer 构造仅 pixel/x/y/width/height 为0；pitch/frame不补零。
  Resize：pitch=(width+15)&~15；pitch或height变更则释放pixel并置null；
  发布height/pitch/x/y/width；pixel为空才分配 4*width*(height+1)，不是4*pitch*height。
- DecodeNext：Frame >= NumFrames-1（无符号比较）时，Loop则位置复位、Frame=0；
  否则 NextFile 非空且 Open 成功才继续并 Loop=NextLoop，否则返回2。
  按 Position（32位无符号值）seek，失败日志并返回1；成功按mode选两条帧路径。
- zlib Alpha 路径：读24字节帧头；Resize并发布Frame；依次分配Y、第一chroma、
  第二chroma、Alpha；内作用域分配alpha压缩数据、完整读取、uncompress
  （容量 width*height，忽略返回码），释放压缩数据；再分配/读余下块数据。
  完整读取后才 Position+=frameSize+8、Frame=header.frame、构造bitreader、重置DC。
  每16x16块先第一chroma后第二chroma，再四个Y；行列上界均向下整除16。
- 块 Alpha 路径：读20字节头，先分配并完整读取frameSize-12，再发布Position/Frame、
  初始化bitreader/重置DC，然后Resize并发布sample.frame；width或height=0时跳过
  所有plane分配与转换，仍返回成功。否则每块2个chroma、4个Y、4个Alpha；
  Alpha使用与Y相同的DC/AC对象，只换第三张量化表。
- 两条路径均经 sws_getCachedContext、getColorspaceDetails，拷贝两张4项系数表，
  再 setColorspaceDetails，把输入输出 range都设1，其余亮度/对比度/饱和度沿用。
  sws_scale的平面顺序为Y、第二chroma、第一chroma、Alpha，最后仍调用
  TVPBindMaskToMain(pixel,alpha,pitch*height)，不能把它看成冗余而删除。
- 每个块先清零144字节局部buffer，将coeff指针向上16对齐。四文件保留该手动缓冲
  对齐结构（不是对象ABI padding）。DC与量化值均有符号16位；
  I64反编译一度把__b显示为unsigned，fresh disasm 0x1001d2fdc 两个LDRSH确认
  signed语义。仅DC时 value=DC*q/8+128，夹到0..255，按pitch写8行各8字节；
  否则经IDCT函数指针调用。
- 默认IDCT适配：8行指针；函数局部static生成1408字节range表，含零区、0..255、
  255区、零区和128字节拷贝，保留终生；jpeg对象只写sample_range_limit，
  component只写dct_table，其它字段不初始化，调用jpeg_idct_islow。
  Android另有NEON入口；iOS同名角色是到标量入口的thunk，不是空函数。
- SeekFrame：目标>=NumFrames日志失败；int32局部扫描偏移从FirstPosition起，
  每次读20字节头，magic=1296126534；相等才发布Frame和Position，超目标失败，
  小于目标则偏移+=frameSize+8并再次seek。每个错误分支保留独立日志。
- native：3把TJS critical section，Decoder按值内嵌，raw std::thread*，
  两个各自含condition_variable+mutex的等待对象，两个std::list<Frame*>，
  当前显示帧号/位置/preload=5，三个bool初始false。第二把TJS锁与第一个bool
  在现已收集调用中仍保留，不能因为看似少用而删除。
- clear先锁队列，逐一delete待显示frame再clear其节点；再delete复用frame并clear。
  不stop、不关Stream、不改播放标志。析构体先clear再stop，成员按逆序析构。
- stop在第三把TJS锁内将Stop和Exit都置true，解锁，notify复用等待，
  thread非空才join/delete/置null；不能用detach或joinable保护改变异常行为。
- play先stop，再锁队列，把待显示frame指针逐个push到复用list后clear原节点，
  按preload收缩/扩充Frame对象，Stop/Exit=false，new std::thread绑定成员worker。
- worker外层while(!Exit)：锁队列取是否为空，解锁；为空则单独读取Stop决定return，
  否则等待独立条件变量（无predicate）；非空也单独读取Stop；
  再锁队列pop一个复用frame，保持该锁完成DecodeNext及push待显示/notify。
  Decode返回2直接return，其他非0抛Decode error；已经pop的raw frame不会回收。
  只有走完外层Exit循环才TVPOnThreadExited，提前return路径不调用它。
  退出回调helper四文件已fresh反编译：A64 sub_A345D4@0xa345d4、
  A32 sub_777C0C@0x777c0c、I64 sub_10002EA9C@0x10002ea9c、
  I32 sub_2D118@0x2d118；与本地ThreadImpl.cpp回调vector遍历对应。
- set frame锁队列，把待显示指针逐个复制到复用list并清原节点，
  SeekFrame成功才置通知标志并notify；失败不回滚上述队列迁移。
- showNextImage空队列返回旧显示帧号；有帧先pop/解锁，发布显示帧号，
  查询Layer native，成功则SetSize、正尺寸才GetMainImage()->Update、
  SetPosition(frame.x+left,frame.y+top)、Update；最后锁队列归还frame并notify。
  Layer native查询返回码被忽略，局部结果指针不补初始化。

### 本地实现对应与差异约束

本地新增的 FileHeader/FrameHeader 是普通固定宽度 POD；对象类保持普通C++字段。
Buffer RAII只复刻已有单指针AlignedAlloc/Dealloc，不引入shared_ptr或异常回滚。
Decoder的Open/Decode/Seek/Block方法逐项按以上伪代码顺序实现，颜色转换阶段保留
两条原始帧路径中的调用位置；不得合并成重新排序的通用视频后端。
STL列表的链接方向、size字段、thread启动控制块、同步对象内存大小属于库/ABI
差异；共享源码用std::list/std::thread/std::condition_variable表达，
不手抄某一平台STL展开。JPEG/NEON与共享helper的后续取证、实现边界见后文。

### 旧共享 helper 误命名纠正

2026-09-13 补追 thunk 的四个目标体：A64 sub_804CD0@0x804cd0、
A32 sub_62EF98@0x62ef98、I64 sub_10007816C@0x10007816c、
I32 sub_7555C@0x7555c。它们更新 Layer Rect 宽高，调用 ImageLayerSizeChanged
并通知主层尺寸变化，与普通 SetSize/InternalSetSize 相符；不对应 SetImageSize。
三个 IDB 中旧 TJSNI_BaseLayer_SetImageSize_guess 已改为
TJSNI_BaseLayer_SetSize_guess，I32 原名正确。相关 captureCanvas 笔记同步纠正。
该命名仍为行为交叉核对后的 _guess，不冒充二进制保留的精确源码名字。

## TJS 绑定实施前证据

以下回调均在本轮四库区域原生 decompile 集合中；每个属性顺序为 getter / setter。

| 绑定字符串 | A64 | A32 | I64 | I32 |
|---|---|---|---|---|
| finalize | sub_522FF0 | sub_48E9FC | sub_1001CFF88 | sub_1CE1D8 |
| AlphaMovie | sub_522FF8 | sub_48EA00 | sub_1001CFF90 | sub_1CE1DC |
| open | sub_52309C | sub_48EA78 | sub_1001D0014 | sub_1CE230 |
| clear | sub_5231D4 | sub_48EB54 | sub_1001D0118 | sub_1CE36C |
| showNextImage | sub_523264 | sub_48EBC8 | sub_1001D0188 | sub_1CE3B2 |
| isPlaying | sub_523340 | sub_48EC64 | sub_1001D0244 | sub_1CE42A |
| play | sub_523400 | sub_48ECFC | sub_1001D02E8 | sub_1CE49E |
| stop | sub_523490 | sub_48ED70 | sub_1001D0358 | sub_1CE4E4 |
| setPosition | sub_523520 | sub_48EDE4 | sub_1001D03C8 | sub_1CE52A |
| setNextMovieFile | sub_5236B0 | sub_48EE80 | sub_1001D0470 | sub_1CE5A4 |
| numOfFrame | sub_52381C / sub_5238A8 | sub_48EF8C / sub_48F000 | sub_1001D05A8 / sub_1001D0614 | sub_1CE718 / sub_1CE75E |
| frame | sub_5238B0 / sub_52393C | sub_48F00C / sub_48F080 | sub_1001D061C / sub_1001D0688 | sub_1CE768 / sub_1CE7AE |
| loop | sub_523A34 / sub_523AC4 | sub_48F0F4 / sub_48F164 | sub_1001D06FC / sub_1001D076C | sub_1CE7F4 / sub_1CE838 |
| nextLoop | sub_523BC4 / sub_523C54 | sub_48F1DC / sub_48F24C | sub_1001D07E8 / sub_1001D0858 | sub_1CE882 / sub_1CE8C6 |
| preloadSamples | sub_523D54 / sub_523DE0 | sub_48F2C4 / sub_48F334 | sub_1001D08D4 / sub_1001D0940 | sub_1CE910 / sub_1CE954 |
| left | sub_523EE8 / sub_523F74 | sub_48F3AC / sub_48F41C | sub_1001D09C4 / sub_1001D0A30 | sub_1CE99E / sub_1CE9E2 |
| top | sub_524068 / sub_5240F4 | sub_48F48C / sub_48F4FC | sub_1001D0AA0 / sub_1001D0B0C | sub_1CEA26 / sub_1CEA6A |
| screenWidth | sub_5241E8 / sub_524274 | sub_48F56C / sub_48F5E0 | sub_1001D0B7C / sub_1001D0BE8 | sub_1CEAAE / sub_1CEAF4 |
| screenHeight | sub_52427C / sub_524308 | sub_48F5EC / sub_48F660 | sub_1001D0BF0 / sub_1001D0C5C | sub_1CEAFE / sub_1CEB44 |
| FPSScale | sub_524310 / sub_52439C | sub_48F66C / sub_48F6E0 | sub_1001D0C64 / sub_1001D0CD0 | sub_1CEB4E / sub_1CEB94 |
| FPSRate | sub_5243A4 / sub_524430 | sub_48F6EC / sub_48F760 | sub_1001D0CD8 / sub_1001D0D44 | sub_1CEB9E / sub_1CEBE4 |

共同伪代码：先 TJS_GET_NATIVE_INSTANCE，失败返回 -1008；有参数要求的四个方法
再检查数量（open/showNextImage/setNextMovieFile >=1，setPosition >=2），不足返回 -1004。
open/setNextMovieFile 使用 ttstr 拷贝；showNextImage 使用 AsObjectNoAddRef。
void 方法若 result 非空则 Clear；isPlaying 在第三把停止状态锁内读取 Stop，
解锁后返回 !Stop，因此刚构造时为 true。
只读属性 setter 无条件返回 -1007；普通 getter/setter 不补锁。
frame getter 读 Decoder.Frame，setter 走带锁队列迁移/SeekFrame；
preloadSamples 只接受 1..30，其余静默忽略；bool 从 int!=0 转换。
构造/析构用传统 tTJSNativeInstance + TJS 原生成员宏，finalize 空方法。
class publication：TVPGetScriptDispatch；global非空则创建class、variant AddRef、
class Release、PropSet(MEMBERENSURE)、variant析构、global Release。
之后 CPU mask 检查与 IDCT 选择位于 global 分支之外。
NCB PRE callback 挂 AlphaMovie.dll，unregister 为 null；不改成 NCB class adaptor。
本地逐个回调保持上述转换、检查、赋值和调用，native 类保持既定成员构造顺序与队列操作。

Layer SetPosition 的本轮四文件入口为 A64 sub_8053C8@0x8053c8 /
A32 sub_62F464@0x62f464 / I64 sub_1000787B4@0x1000787b4 /
I32 sub_75A68@0x75a68。四者共同先尺寸位置相等早退、检查主层不可位移、
更新旧区域、平移 Rect、通知父层区域缓存失效、再更新新区域；与本地
LayerIntf.cpp::SetPosition 调用边界对照，I64 的向量平移是编译器展开。

IDCT 平台边界：Wasm 指令集不能直接执行 Android ARM NEON 汇编内核；本地 Web
优化入口保留到标量适配函数的转发结构，与两份 iOS 参考的 thunk 相同。
CPU gate 的掩码/比较和函数指针发布位置保持原样；不把 Android 的内核说成空实现。
Android 内核入口分别 sub_13A6218@0x13a6218、sub_CC4748@0xcc4748，属于
共享 JPEG SIMD 内核，已 fresh decompile 但手写汇编的 Hex-Rays 输出不可作为完整 C++。
后续保存 disasm 并核对 wrapper 的量化表/系数/八行输出指针/列偏移0数据契约。

## JPEG 8x8 标量内核实施前证据

本轮另行 fresh decompile 四个实际内核：A64 jpeg_idct_islow@0x13bd888 /
A32 jpeg_idct_islow@0xcd3f08 / I64 sub_100497988@0x100497988 /
I32 sub_462848@0x462848；原始输出存于 *-jpeg-scalar.json。
共同 16 位有符号量化表，int workspace[64]，列/行两遍、各自全 AC 为零捷径。
列：zero-AC => coeff[0]*quant[0]*4 填八项；否则乘量化后偶数项与奇数项
蝶形，常数4433/-15137/6270/2446/16819/25172/12299/-7373/-20995/
-16069/-3196/9633，八输出按0/7、1/6、2/5、3/4发布，+1024右移11。
行：相同蝶形，无再量化；zero-AC => (workspace[0]+16)>>5；普通路径
+131072右移18；两者均 &1023 后索引 sample_range_limit+128，按八列写出。

差异必须保留：I64 第一次量化乘法用 W 寄存器，之后符号扩展到 X 寄存器进行
蝶形（fresh disasm 中 MUL W / MUL X 已核对），workspace仍存32位；其余三份
使用32位蝶形。这是 JPEG 内部 long/32-bit typedef 的平台构建差异。
I64 第二遍最终mask只消费低28位，编译器可窄化为W运算，不证明源码换了类型。
本地用平台条件的内部标量类型表示：Apple用long，其余int32_t；Wasm为32位。
不把I64的极端溢出结果声称成四份共同结果。

当前依赖 libjpeg-turbo 的 Wasm jconfig.h 禁用 WITH_SIMD，使 MULTIPLIER=int，
直接把原插件 short[64] 交给库内核会错读。故在插件翻译单元内复原上述8x8内核，
保留 jpeg_decompress_struct/component/dct_table/rows 的适配调用层与16位输入契约；
不增加每块32位量化表转换，也不修改全局JPEG库的配置。局部名字jpeg_idct_islow
对应Android保留的精确导出名，内部普通临时变量名只为表达相同蝶形。

还发现Android参考默认适配写sample_range_limit的位置与其链接JPEG内核读取位置
不一致：A64适配偏移424/内核392，A32适配324/内核288；iOS分别424/424和324/324。
属于参考自身的JPEG头/库ABI差异，不能硬造padding去复制；本地使用本机头的字段访问，
明确保留此边界记录。Android通常可走NEON wrapper，该分支不访问上述jpeg对象。
这里不能宣称Android默认标量路径已由四份证明运行无误。

四个 uint32 属性 getter（numOfFrame/frame/FPSScale/FPSRate）再次 fresh decompile：
沿前述绑定表全部16个地址。A64/I64将32位字段零扩展，A32/I32传入64位值高半部0。
故本地向tTJSVariant赋值显式转tjs_int64，保留0..4294967295；不能转int32。

### 优化 wrapper 的源码平台分支

再次 fresh decompile A64 sub_522EF0@0x522ef0、A32 sub_48E940@0x48e940、
I64 sub_1001CFF40@0x1001cff40、I32 sub_1CE12C@0x1ce12c：
Android建立八个行指针后以(quant,coeff,rows,0)调用JPEG NEON内核；iOS直接转发标量。
本地将这个平台分支显式保留：Android ARM且JPEG开启WITH_SIMD时绑定本地依赖提供的
jsimd_idct_islow_neon；其参数声明已经在本地依赖simd/arm/jidctint-neon.c核对。
该外部名字是本地库的绑定名，不冒充已从参考恢复的内部符号名。
Web和Apple保留标量转发；Web不能执行ARM机器码。此处只恢复插件wrapper的调用结构，
不声称libjpeg-turbo 3.1.1的NEON内核源码与旧参考内核完全一致；后者仍作为共享依赖版本边界。


## 实现对应与验收记录

插件实现采用一个 AlphaMovie.cpp 翻译单元；原名未能由名字证据确认的私有类/方法
带_guess。这不表示恢复了原始作者的所有标识符、注释、宏拼写或文件切分。
不增加对象ABI padding。以下表内行号是当前本地实现入口；同名伪代码、四文件函数表
给出逐语句控制条件、赋值顺序和资源操作的依据。

| 本地入口 | 对应证据与语句范围 |
|---|---|
| AlphaMovie.cpp:22 `const tjs_uint32 BitMasks_guess` | 八张固定表，四文件逐字节相同 |
| AlphaMovie.cpp:85 `class AlphaMovieBitReader_guess` | 构造对齐；Peek副本；Read写回；Skip独立预读边界 |
| AlphaMovie.cpp:166 `class AlphaMovieHuffman_guess` | build_codes；MaxCode/ValueOffset；仅512字节清零；9位展开表 |
| AlphaMovie.cpp:243 `class AlphaMovieDCHuffman_guess` | Previous默认0、Reset；Decode1 fallback与DC累加 |
| AlphaMovie.cpp:277 `class AlphaMovieACHuffman_guess` | DecodeAC run/size、自然顺序写回及早退 |
| AlphaMovie.cpp:350 `void jpeg_idct_islow(` | 8x8列/行两遍、16位量化、32位workspace、两级舍入、range表 |
| AlphaMovie.cpp:491 `void AlphaMovieIDCTScalar_guess(` | 八行指针、静态range表生命周期、jpeg对象部分字段赋值 |
| AlphaMovie.cpp:518 `struct AlphaMovieFileHeader_guess` | 40/20/24字节普通POD文件数据契约 |
| AlphaMovie.cpp:546 `class AlphaMovieBuffer_guess` | AlignedAlloc/条件Dealloc RAII；无额外回滚 |
| AlphaMovie.cpp:559 `struct AlphaMovieFrame_guess` | 未初始化pitch/frame；Resize字段发布与width分配表达式 |
| AlphaMovie.cpp:595 `class AlphaMovieDecoder_guess` | 成员构造、Stream/SwsContext销毁、默认循环标志 |
| AlphaMovie.cpp:636 `bool Open_guess(` | 逐字段头验证、属性优先级、量化表、失败日志与Stream清理 |
| AlphaMovie.cpp:739 `int DecodeNext_guess(` | 循环/换文件/Seek/按mode分派 |
| AlphaMovie.cpp:759 `bool SeekFrame_guess(` | 有符号扫描位置、帧头magic、匹配才提交状态 |
| AlphaMovie.cpp:799 `void DecodeChromaBlock_guess(` | 144字节清零后对齐、DC/AC、仅DC填块或调用IDCT |
| AlphaMovie.cpp:825 `void DecodeLumaBlock_guess(` | 同上；Y/Alpha共享的Huffman对象与不同量化表 |
| AlphaMovie.cpp:851 `int DecodeSeparateAlpha_guess(` | 24字节头、zlib压缩Alpha作用域、Y/两chroma、SWS、BindMask |
| AlphaMovie.cpp:936 `int DecodeBlockAlpha_guess(` | 20字节头、先读数据再Resize、零尺寸分支、4Y+4Alpha |
| AlphaMovie.cpp:1038 `class AlphaMovieCondition_guess` | 自有mutex/condition_variable；无predicate等待 |
| AlphaMovie.cpp:1050 `class AlphaMovieNative_guess` | 三把锁、内嵌Decoder、线程指针、两个list与三标志 |
| AlphaMovie.cpp:1088 `~AlphaMovieNative_guess()` | 析构clear→stop→成员逆序；默认native Invalidate空 |
| AlphaMovie.cpp:1099 `void clear()` | 删除帧与清列表节点分开进行；不停止线程 |
| AlphaMovie.cpp:1109 `bool isPlaying()` | 第三把锁内快照Stop，解锁后取反 |
| AlphaMovie.cpp:1118 `void stop()` | 锁内写Stop/Exit；通知；join/delete/null |
| AlphaMovie.cpp:1132 `void play()` | 先stop，队列迁移，新旧节点分开生命周期，按preload收缩扩充 |
| AlphaMovie.cpp:1160 `void SetFrame_guess(` | 持队列锁迁移→SeekFrame→成功才通知 |
| AlphaMovie.cpp:1171 `tjs_int32 showNextImage(` | pop后解锁、Layer更新链、归还复用队列 |
| AlphaMovie.cpp:1203 `void Worker_guess()` | 两级Stop检查、独立wait、持队列锁解码、非正常出口不回收帧 |
| AlphaMovie.cpp:1247 `void AlphaMovieIDCTPlatform_guess(` | Android ARM JPEG SIMD调用；Apple/Wasm标量转发 |
| AlphaMovie.cpp:1274 `iTJSDispatch2 *CreateAlphaMovieClass_guess()` | 全部8方法/11属性、原生TJS宏、参数数目与错误码 |
| AlphaMovie.cpp:1523 `void RegisterAlphaMovie_guess()` | global/class/variant引用顺序，CPU gate，NCB PRE模块登记 |

### 补充数据与工具质量

- 初始ClassID四库分别A64 0x1aa1018、A32 0x1102008、I64 0x101aeb6d0、
  I32 0x18377c8，native get_bytes均确认32位-1。相邻IDCT指针槽分别
  0x1aa1010/0x1102004/0x101aeb6c8/0x18377c4，均指向该库的标量wrapper。
- JPEG自然顺序表与本地已安装依赖jutils.c的80项逐项对应，后16项均为63。
- 编译运行依赖头诊断：AV_PIX_FMT_YUVA420P=35、BGRA=30；Wasm头布局
  sample_range_limit=324/dct_table=80，native64布局424/88。布局只用于取证记录，
  本地代码始终通过普通字段访问。
- BindMask继续走共享TVP函数指针。A64 0xa86040、A32 0x784962、
  I64 0x100272bc4、I32 0x273a48本轮fresh decompile，证明此阶段只覆盖像素alpha字节，
  不是RGB预乘。A64/I64/I32的NEON批处理与标量尾段及A32当前标量入口有平台选择差异；
  对照本地blend_functor_c.h::bind_mask_to_main_functor，不删该调用。
- 四IDB导入Huffman/BitReader普通类型，重新反编译核对字段；A32/I32的0xfffff
  曾被显示成偶然映射地址，set_op_type(hex)+force_recompile后已显示为整数。
- A64 NEON汇编0x13a6828以BLR X30转回wrapper。IDA仍显示noreturn；set_type与
  force_recompile未清除此自动标记。当前原生工具没有IDAPython执行/函数flag修改接口，
  未走外部旁路；已在kernel和wrapper的IDB注释就地记录正常返回依据。
  这只是未清除的IDA展示标记，不进入C++声明或控制流。

### 验证结果与边界

- Web Debug、Web Release 完整配置/编译/链接通过。日志在本轮out取证目录；
  保留既有_tss、pthread+memory growth及Emscripten内部JS依赖警告。
- MacOS Debug alphamovie-dll编译链接通过；两项CTest共64条断言通过。
  通过现有TJS测试框架实测模块登记、native构造/析构、初始isPlaying、clear不stop、
  stop、空帧showNextImage、位置赋值、四方法参数不足、五只读setter拒绝、preload上下界。
  不读取未open时未初始化的帧率/帧数，不构造AMV测试文件。
- 浏览器复用已有dracu.zip（740622267字节，14个XP3）完成启动，正常显示柚子社Logo。
  同步存储/脚本初始化日志及截图已保存；这是非回归检查，未覆盖AMV解码。
  开始时错误使用dracu.zip URL产生404，按服务器实际/game.zip映射修正后运行成功。
- 真实AMV路径reference/xp3/carousel3kag/src/video/komadoria.amv存在，但文件读取
  返回OS error 60；其carousel3kag.xp3也读取超时。现有dracu包内video.xp3仅28字节。
  因现有AMV物料不可读，没有新增解码测试或捏造fixture；两条解码路径、线程播放、
  seek/loop/换片及异常输入尚无真实AMV差分运行结果。
- Android NEON wrapper已保留，但当前验证主机未进行Android交叉编译；绑定的是本地
  JPEG依赖提供的NEON入口，旧内核与新依赖内部优化结构/极端输入差异不在已证明等同范围。
- 源码六维已按本文可定位的四文件证据实装；不宣称恢复原始源码文本，也不把缺少AMV
  差分、JPEG头/库ABI差异或shared-library版本差异抹成“100%已证明”。
