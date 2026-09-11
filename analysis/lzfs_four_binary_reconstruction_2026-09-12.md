# lzfs 四文件源码复原

## 输入与范围

2026-09-12 本轮逐个检查四个二进制及配套 `.i64` 可读，使用原生
`mcp__idalib__idb_open/server_health/survey_binary` 核对会话。下文列名缩写
只用于表格，地址始终属于所在列，不能跨文件使用。

| 缩写 | 二进制 | database | 架构 / 基址 |
|---|---|---|---|
| A64 | Kirikiroid2_1.3.9_Android_arm64-v8a.so | 5cf47d21 | AArch64 / 0 |
| A32 | Kirikiroid2_1.3.9_Android_armabi-v7a.so | 5ecebaec | ARM / 0 |
| I64 | Kirikiroid2_1.3.9_iOS_arm64 | e0a14ea7 | arm64 slice / 0x100000000 |
| I32 | Kirikiroid2_1.3.9_iOS_armv7 | 2ae416ed | armv7 slice / 0x4000 |

I32 IDB 的 module 是 `Kirikiroid2_1.3.9_iOS_armv7.thin-armv7`，为已有
恢复数据库；I64/I32 来自同一 fat Mach-O 的不同 slice。历史 input path
保留 Windows 路径，与本机配套 IDB 的路径不同，不能据此混用两个 slice。

模块范围以 `lzfs.dll` registrar 和该 registrar 创建对象的完整虚表为根。
四个对象虚表 RTTI 槽均为零，未保留媒体类、context holder、初始化回调或
源码文件的精确名称；本轮新造的这些标识符使用 `_guess`，不冒充原名。
标准库模板、LZ4 导出 API 和 iTVPStorageMedia 的虚接口保留可确认的名称。

原生工具返回的完整函数和 Open 反汇编保存在
`out/diagnostics/lzfs-reconstruction/<二进制文件名>.json`。这些是本轮实测
结果的本地归档，不是另一路反编译输入。

## 四文件映射（全部已 fresh decompile）

以下 `sub_`/`InitFunc_` 是本轮 IDA 名称；虚方法名称由完整虚表槽序、
每个槽的 fresh 反编译以及 core 接口调用链共同确认。

| 角色 | A64 | A32 | I64 | I32 |
|---|---|---|---|---|
| static registrar | sub_42DF40 @ 0x42df40 | sub_3004C0 @ 0x3004c0 | InitFunc_162 @ 0x10035fddc | InitFunc_162 @ 0x36346c |
| init callback / 内联媒体构造 | sub_5F25A8 @ 0x5f25a8 | sub_515060 @ 0x515060 | sub_10035FCF4 @ 0x10035fcf4 | sub_363364 @ 0x363364 |
| 媒体 vtable | 0x1a121a8 | 0x10b6ae0 | 0x1019b4f60 | 0x177b788 |
| 空析构 | nullsub_292 @ 0x5f2660 | nullsub_193 @ 0x5150d4 | nullsub_467 @ 0x10035fd24 | nullsub_465 @ 0x363388 |
| deleting destructor | 0x5f25d8 | 0x515084 | 0x10035fd28 | 0x36338a |
| AddRef | sub_5F25DC @ 0x5f25dc | sub_515088 @ 0x515088 | sub_10035FD2C @ 0x10035fd2c | sub_36338E @ 0x36338e |
| Release | sub_5F25EC @ 0x5f25ec | sub_515090 @ 0x515090 | sub_10035FD3C @ 0x10035fd3c | sub_363396 @ 0x363396 |
| GetName | sub_5F260C @ 0x5f260c | sub_5150A4 @ 0x5150a4 | sub_10035FD5C @ 0x10035fd5c | sub_3633A8 @ 0x3633a8 |
| NormalizeDomainName | nullsub_289 @ 0x5f2620 | nullsub_190 @ 0x5150b4 | nullsub_468 @ 0x10035fd70 | nullsub_466 @ 0x3633ba |
| NormalizePathName | nullsub_290 @ 0x5f2624 | nullsub_191 @ 0x5150b6 | nullsub_469 @ 0x10035fd74 | nullsub_467 @ 0x3633bc |
| CheckExistentStorage | sub_5F2628 @ 0x5f2628 | sub_5150B8 @ 0x5150b8 | sub_10035FD78 @ 0x10035fd78 | sub_3633BE @ 0x3633be |
| Open | sub_5F210C @ 0x5f210c | sub_514D68 @ 0x514d68 | sub_10035F920 @ 0x10035f920 | sub_362F64 @ 0x362f64 |
| GetListAt | nullsub_291 @ 0x5f2630 | nullsub_192 @ 0x5150be | nullsub_470 @ 0x10035fd80 | nullsub_468 @ 0x3633c4 |
| GetLocallyAccessibleName | sub_5F2634 @ 0x5f2634 | sub_5150C0 @ 0x5150c0 | sub_10035FD84 @ 0x10035fd84 | sub_3633C6 @ 0x3633c6 |
| context holder 析构 | Open 正常/异常出口内联 free | Open 正常出口内联 free | sub_10035FDB0 @ 0x10035fdb0 | sub_3633DC @ 0x3633dc |

### 字符串

四库 `find(type=string)` 对 lzfs/LZFS/Lzfs 均未命中；对这三种大小写分别
进行 UTF-8、UTF-16LE、UTF-32LE `find_bytes`，只有小写 UTF-16LE 命中，
全部搜索页 `cursor.done=true`。`get_bytes` 确认完整内容及终止符，
`xrefs_to` 将字符串连到上述根函数。已修正五组宽字符串的数组类型。

| 字面量 | A64 | A32 | I64 | I32 |
|---|---|---|---|---|
| Cannot open lz4 file: %1 | 0x14cb44c | 0xd7dfc4 | 0x101979bf4 | 0x176bfa6 |
| Fail to get lz4 header info: （末尾空格） | 0x14cb47e | 0x515014 | 0x101979c26 | 0x176bfd8 |
| lzfs: Decompress fail, data may be damaged. | 0x14cb4ba | 0xd7dff6 | 0x101979c62 | 0x176c014 |
| lzfs.dll | 0x14cb512 | 0xd7e04e | 0x101979cba | 0x176c06c |
| lzfs | 0x14cb524 | 0xd7e060 | 0x101979ccc | 0x176c07e |

## 修改前的共同伪代码

```cpp
static preRegistrar("lzfs.dll", init, nullptr); // intrusive PreRegist head
init() { TVPRegisterStorageMedia(new Media); } // ref=1; no Release after register
AddRef() { ++ref; }
Release() { if (ref == 1) delete this; else --ref; }
GetName(out) { out = "lzfs"; }
NormalizeDomainName/NormalizePathName/GetListAt(...) { }
CheckExistentStorage(name) { return TVPIsExistentStorage(name); }
GetLocallyAccessibleName(name) { name.Clear(); }

Open(name, flags) {
    stream = TVPCreateStream(name, flags);
    if (!stream) throw "Cannot open lz4 file: %1", name;
    magic = stream->ReadI32LE(); // ReadBuffer(4), short read throws
    stream->SetPosition(0); // checked Seek SET, before magic test
    if (magic != 0x184d2204) return stream;
    fileSize = stream->GetSize();
    vector<unsigned char> input;
    input.resize(fileSize); // value-initializes bytes
    stream->ReadBuffer(input.data(), fileSize); // tjs_uint conversion
    contextHolder context; // context=null, create(&context,100), ignore status
    LZ4F_frameInfo_t info; // only contentSize explicitly set to zero
    info.contentSize = 0;
    remaining = input.size();
    consumed = remaining;
    result = LZ4F_getFrameInfo(context, &info, input.data(), &consumed);
    if (isError(result) || consumed == 0) {
        AddLog(ttstr("Fail to get lz4 header info: ") + name);
        stream->SetPosition(0);
        return stream; // holder/vector destruct, input stream stays alive
    }
    delete stream;
    remaining -= consumed;
    pointer = input.data() + consumed;
    consumed = remaining;
    output = new tTVPMemoryStream;
    if (info.contentSize != 0) {
        output->SetSize(info.contentSize); // unsigned 32-bit conversion
        produced = info.contentSize; // size_t conversion
        result = decompress(context, output->GetInternalBuffer(), &produced,
                            pointer, &consumed, nullptr);
        if (isError(result) || consumed != remaining || produced != info.contentSize) {
            delete output;
            throw decompressionFailure;
            return nullptr;
        }
    } else {
        vector<unsigned char> chunk;
        chunk.resize(128 * 1024);
        while (result != 0) {
            produced = chunk.size();
            result = decompress(context, chunk.data(), &produced,
                                pointer, &consumed, nullptr);
            if (isError(result)) { delete output; throw decompressionFailure; return nullptr; }
            remaining -= consumed;
            pointer += consumed;
            consumed = remaining;
            output->WriteBuffer(chunk.data(), produced);
        }
        if (isError(result) || consumed != remaining || output->GetSize() != info.contentSize) {
            delete output;
            throw decompressionFailure;
            return nullptr;
        }
    }
    return output; // context destroyed before input vector
}
```

`pointer += consumed` 与 WriteBuffer 的机器指令排布略有移动，但四端均先
扣减/重置 consumed，再 WriteBuffer，再进入下轮；源级实现保留独立指针、
remaining、consumed、produced、result，不将状态合并成简化解压调用。

## 生命周期、容器与边界

- 媒体仅 vptr + 32-bit ref；没有缓存、文件表、挂载容器或全局媒体指针。
  依据是工厂完整构造、11 槽完整虚表和全部方法；并非一次负搜索结论。
- registrar 是进程生命周期静态对象，五个指针字段（vptr/name/next/init/term），
  term=null。init 每次分配新媒体，交给 core 的媒体注册哈希表；不补静态 once guard，
  不补 Release。正常注册后创建方的一份 ref 保留，注册表另有一份 ref。
- 现有 NCB 加载器在成功后记录模块键，重复 LoadModule 返回 false 且不重跑
  callback；Plugins.link 不依赖该 bool。重复加载的测试保留这个返回值。
- input/chunk 都是 `std::vector<unsigned char>`。A32 保留了
  `_M_default_append` 模板符号；I64/I32 是 libc++ vector resize helper；
  A64 把 resize 的 new/memset/end 三指针操作内联。不能把 A64 伪代码里的
  `operator new` 误还原为源码裸数组。
- context 指针 holder 构造传 100 且忽略错误码；析构忽略 free 的返回码。
  I32 SJLJ cleanup 表和 A64 landing pads 确认异常也释放 context、input、chunk。
- stream/output 是裸指针。ReadI32LE、SetPosition、resize、ReadBuffer、SetSize、
  WriteBuffer 等抛出时没有通用流析构 guard；不擅自修复原版泄漏路径。
  已识别的解压错误先 delete output 再抛错；构造失败仍由 C++ new 表达式回收。
- 少于 4 字节的普通文件也先读 4 字节并抛出，不在 magic 前作大小判断。
- 非 LZ4 magic（包括 skippable frame magic）原样返回定位为零的原流。
- LZ4 头失败只 AddLog 并返回原流；不能改成抛异常。
- 已知 contentSize 路径只要求非 error、完整消费输入、输出长度符合声明；
  **不要求 result==0**，也不作循环补读。
- 未知 contentSize 路径保持 128 KiB 分块和最终 `GetSize()==0` 判断；
  非空成功解压仍会抛错；最终 consumed==remaining 是重赋值后的冗余判断，
  不是“必须没有剩余输入”。原版没有无进展退出条件。
- 已知大小直接写内部缓冲区，游标保持零；分块路径 WriteBuffer 推进游标。
- flags 全量转发，不添加只读门控；存在性只调用 core 的路径解析。
- 清空可本地访问名称，不对 name 进行 scheme 重写；两个 normalize 方法均为空。

## 逐文件差异

- A64/I64 指针与 size_t 为 64 位，A32/I32 为 32 位；所有 tTVPMemoryStream
  Size/AllocSize/CurrentPos 和 ReadBuffer/WriteBuffer 参数仍为 32 位。
  大文件 vector 分配与实际读取的截断，以及 contentSize 转 SetSize/size_t 的
  截断均保留自然 C++ 转换，不硬凑任一平台对象尺寸。
- A64/A32 libstdc++ 与 I64/I32 libc++ 的 vector resize、字符串临时值和
  异常清理展开不同。I32 的 SJLJ 造成 Hex-Rays 虚构大量栈参数；Open 的
  汇编确认入口只消费 this/name/flags，不能把伪参数写入源码。
- A32 的 header log 字面量位于代码段 literal island，其余三个在常量段。
- 四个插件控制流没有尚未解释的业务分支差异。
- LZ4F context 自身分配 A64/I64=200、A32=160、I32=156 字节；是指针宽度、
  64-bit 成员对齐造成的 ABI 差异，不能以 pack/padding 模拟。

## 本地对照与实现计划（在 cpp 修改前记录）

修改前以文件名、`lzfs` 大小写文本、`iTVPStorageMedia` 派生类、插件 CMake
source list、NCB 模块注册点交叉检查：本地没有 lzfs 模块。现有 UtilStreams
已具备上述 MemoryStream 字段与 SetSize，tjs.cpp 已具备 ReadI32LE /
ReadBuffer / WriteBuffer / SetPosition；复用这些 core 方法，不重造流包装。

新增 `cpp/plugins/lzfs.cpp`：静态 registrar、只有 ref 的媒体类、十个虚接口
（析构占两个 ABI 槽）、独立 Open、一个 context holder。Open 按上述伪代码
逐段落地，保留 vector、两个解压分支和裸指针生命周期；仅添加 CMake source
及已存在的 LZ4 依赖链接。精确原文件名与私有标识符无法从二进制恢复。

实现后的逐段对照（`cpp/plugins/lzfs.cpp`）：

| 本地行 | 对应证据 / 作用 |
|---|---|
| 3–4、143–147 | lzfs.dll 的 PreRegist callback，new media 后直接 Register |
| 14–15 | 本地构建约束：检测实际使用的头文件版本，非参考运行行为 |
| 19–27 | context=null，create(...,100)，析构 free，两个返回码均忽略 |
| 29–43 | 只有 ref 的媒体类、空析构、非原子 AddRef、先比较一再删除/递减 |
| 45–56 | 完整媒体虚接口，保留原来的空 normalize/list 及 clear-local-name |
| 59–66 | 原 flags/name 转发、null 抛错、ReadI32LE 与 checked rewind |
| 67–79 | magic 门控、vector resize/read、contentSize 单字段初始化、getFrameInfo |
| 80–88 | header 失败日志/回退；成功释放原流并构造内存流 |
| 89–104 | 已知大小 SetSize、直接写内部 buffer、三个失败条件和先删除再抛错 |
| 105–124 | 未知大小 vector、128 KiB 分块、各 size_t 状态重赋值和 WriteBuffer |
| 125–135 | 零 contentSize 最终比较、冗余 consumed 比较、输出流交还 |
| 137–140 | 原流或内存流返回；context/vector 栈展开，原流不补 RAII |

## LZ4 依赖核对与版本固定

Android 两库 `LZ4_versionNumber`（A64 0x62f670、A32 0x5364fa）均返回
10704；I64/I32 的五个实际 API 被 strip，但由 Open 调用链定位并 fresh
decompile，控制流与 Android 对应 API 一致。原本本地 vcpkg 的 LZ4 为
1.10.0；现已通过 manifest override 和 `vcpkg/ports/lz4` overlay 全局固定
官方 1.7.4。不是仅修改版本显示，也不是只给 lzfs 包装一层新版 API。

依赖来自 [LZ4 官方 v1.7.4 release](https://github.com/lz4/lz4/releases/tag/v1.7.4)，
只用于公共压缩库；lzfs 插件本身的唯一还原依据仍是上述四个二进制。
源码归档 SHA-512：
`723c4489f17e3aa574a278ba8882d8de999c52dca09ec460e52019c7387d14d0877baf6e5e09aa7e2056ce52c22029ed5038889bfac1629a1491572012418973`。
现有 builtin registry 最早记录为 1.7.4.2，因此使用 overlay 精确构建 v1.7.4，
而不把 1.7.4.2 当成相同版本。

| API | A64 | A32 | I64 | I32 |
|---|---|---|---|---|
| getFrameInfo | 0x63ace0 | 0x53c52c | 0x100268054 | 0x268a4e |
| decompress | 0x63ae38 | 0x53c634 | 0x100268190 | 0x268b2c |
| createDecompressionContext | 0x63ac50 | 0x53c4e8 | 0x100267fc4 | 0x268a0a |
| freeDecompressionContext | 0x63ac98 | 0x53c508 | 0x10026800c | 0x268a2a |
| isError | 0x639db8 | 0x53bc50 | 0x100267fb8 | 0x2689fc |
| decodeHeader_guess | 0x63b8b0 | 0x53cc4c | 0x1002689ec | 0x269212 |
| updateDict_guess | decompress 内联 | 0x53ce30 | 0x100268c8c | 0x269410 |

五个 API 已修正 IDB 函数原型，避免 Hex-Rays 把返回 size_t 错认成 long double、
把 64-bit 指针参数截成 int。

### 与旧 codec 的四文件对照

最终使用官方依赖原封不动的 `lz4.c/lz4hc.c/lz4frame.c/xxhash.c`。
本轮曾新增的私有 Frame/Block/XXHash 复原代码已移除。下表和伪代码保留为
反编译取证，与实际使用的官方 1.7.4 逐项核对，可用于审计依赖是否偏离。

| 下层函数 | A64 | A32 | I64 | I32 |
|---|---|---|---|---|
| LZ4_decompress_safe | 0x632ee8 | 0x539178 | 0x1002a3bec | 0x2a890a |
| safe 函数指针适配 thunk | 0x63bb48 | 0x53cf08 | 0x100268dec | 0x2694e4 |
| LZ4_decompress_safe_usingDict | 0x635948 | 0x53a078 | 0x1002a4008 | 0x2a8c6e |
| XXH32 | 0x63e8a8 | 0x53e474 | 0x1003410a8 | 0x34392e |
| XXH32_reset | 0x63ea44 | 0x53e6f0 | 0x100341300 | 0x343bac |
| XXH32_update | 0x63eac8 | 0x53e768 | 0x100341358 | 0x343c28 |
| XXH32_digest | 0x63ec68 | 0x53e89e | 0x1003414e4 | 0x343d34 |
| small-offset inc 表 | 0x14d0c80 | 0xd824e0 | 0x1015d8f10 | 0x1468660 |
| small-offset dec 表 | 0x14d0ca0 | 0xd82500 | 0x1015d8f30 | 0x1468680 |
| block-size 表 | 0x14d0f20 | 0xd82720 | 0x1015d8518 | 0x1467ce4 |

上述所有函数四端均 fresh decompile，常量表均 get_bytes。A64 对 generic
block/XXH 循环向量化，I32 对 hash update 用 NEON；另三端交叉证明这是
同一轮转/乘法或 8-byte wild-copy 的编译器展开。A64 的 updateDict 完整
内联在 0x63ae38；其余三端有独立函数，不能据 A64 无符号而省掉该步骤。

共同 codec 伪代码（由四端取证归纳）：

```text
create: calloc context, version=caller value, fail=-1, success=0
free: null=>0; snapshot stage; free tmpIn,tmpOut,context; return stage
isError: unsigned(size_t)code > size_t(-19)
getFrameInfo:
  stage>=2 => copy frameInfo; decompress(empty input/output) 返回 next hint
  available<5 => consumed=0, -12
  magic skippable=>header=8; normal=>header=7+(FLG&8); otherwise -13
  available<header=>consumed=0,-12
  consumed=header; decompress(header,outputSize=0)
  stage<2 => -12; otherwise copy info, return decompress result
decodeHeader:
  need>=7; clear frameInfo; skippable enters get/store skip size
  normal: length=7+(FLG&8); incomplete=>buffer header, stage=storeHeader
  require version bits=01; reject block checksum; reject reserved bits;
  block ID 4..7; require low BD bits zero; XXH32(header)>>8 checksum
  publish frame fields, optional contentSize/frameRemainingSize
  reset checksum if enabled; buffer capacity=blockSize+(linked?128KiB:0)
  grow by free/calloc tmpIn then free/calloc tmpOut; errors=-9
  reset input/dictionary/output counters; stage=getBlockHeader
decompress:
  reset caller consumed/produced; retain src/dst cursors and optional stableDst
  stages 0/1 get/store header; 2/3 get/store block header; 4 copy raw block;
  5/6 get/store compressed input; 7 choose direct vs temporary output;
  8 direct decode; 9 temporary decode; 10 flush temporary;
  11/12 get/store suffix; 13/14 get/store skip size; 15 skip content
  direct/temporary choose safe-via-thunk vs safe_usingDict by blockMode
  update content checksum, remainingSize and linked dictionary after output
  suffix requires remainingSize==0; optional checksum; stage=0 on finish
  yield preserves dictionary for !stableDst and stage 1..10, then publishes sizes
  early errors return before publishing sizes; no-progress returns a hint
block safe generic (four usingDict variants inline):
  zero capacity accepts only compressedSize=1 && token=0
  decode literal length, extension bytes, pointer overflow, last literals
  wild-copy literals by 8 bytes; read offset; range-check; write offset at output
  decode match length, extension bound inputEnd-5; add MINMATCH=4
  external dictionary match may copy only dictionary or span dictionary/output
  normal matches use inc={0,1,2,1,4,4,4,4}, dec={0,0,0,-1,0,1,2,3}
  first 8 bytes, near-end bound outputEnd-5 then bounded tail; else 8-byte wildcopy
  errors=-(inputConsumed)-1; success=bytesProduced
usingDict: size=0 noDict; contiguous size>=65535 prefix64KiB;
           contiguous smaller prefix; otherwise external dictionary
XXH32: total_len_32/large_len/v1..v4/mem32[4]/memsize/reserved
  reset a local state, zero all except final reserved, set four seed lanes,
  memcpy whole state (reserved intentionally uninitialized, never consumed)
  update blocks of 16, retain partial tail; digest never frees state
  single-shot has independent four-lane loop, then 4-byte/byte tails/avalanche
```

官方 1.7.4 对应以上 16 阶段、context 字段、buffer/dictionary 管理、header
条件、错误码、block 泛型展开、XXH32 state 和保留字行为。overlay 不修改
这些 C 源码，只提供现代 CMake config target、静态/动态安装和 pkg-config。
现有软件纹理模块使用的 `LZ4_compress_default/LZ4_decompress_fast` 均在
1.7.4 API 中，随全局依赖一起回退。

唯一额外的链接处理是沿用官方 Makefile 的 `XXH_NAMESPACE=LZ4_`：本地
Cocos 导出的旧 XXH32 streaming ABI 与 LZ4 自带的 XXH32 不兼容（尤其
旧 digest 会释放 state）；此前直接使用全局符号会串库。符号前缀保证
LZ4F 调用其随库编译的正确 XXH32，不改算法、对象状态或调用步骤。
`cpp/plugins/lzfs.cpp` 的版本 static_assert 防止以后静默换回新库。

## 验证状态

使用官方 LZ4 1.7.4 的完整 Web Debug 构建已通过（Emscripten 6.0.9）。
按扩展名搜索未见 lz4/lzfs 文件，但进一步读取 tests/test_files 与已有
NEKOPARA 解包资产的文件头，在 6341 个文件中确认 **37 个已有 .psb 实际
是 LZ4 帧**，全部位于 `out/diagnostics/nekopara34-20260912/vol4/extracted/运行游戏/`。
不能把扩展名负搜索当作没有 fixture。这 37 个文件可复用作真实压缩数据
验证，不生成新压缩帧。损坏/巨型帧等实际资产未覆盖的路径保留静态证据验证。

官方 1.7.4 原生动态库对全部 37 文件流式解压成功，合计 1,055,881,673
输出字节，每个输出的 SHA-256 已记录在
`out/diagnostics/lzfs-reconstruction/official-1.7.4-assets.json`。
这些输入 FLG 均为 0x6c：independent blocks、声明 contentSize、content checksum。
这证明当前已有资产能由固定版本正确解码；它不是四参考运行时差分 oracle。
`tests/unit-tests/plugins/lzfs-dll.cpp` 用于经完整存储媒体链验证普通文件回退和
这些资产，期望内容由同版库的 8191-byte 输入 / 4096-byte 输出流式调用取得，
可与插件的全输入/直接输出路径对比；真实资产路径经 `KRKR2_LZFS_FIXTURE_DIR`
提供，默认不生成或下载测试物料。

MacOS Debug 的 `lzfs-dll` 测试目标已构建通过；37 个真实资产已全部通过
存储媒体路径的逐字节比对和零游标断言。普通文件路径、模块大小写/重复加载、
存在性、空目录枚举、本地名称清空也已通过（独立运行 11 个断言）。
最终组合测试 **全部通过：2 个 test cases、553,945 个 assertions**，
43.064 秒，结果记录为 `out/diagnostics/lzfs-reconstruction/test-native-final.log`。

复跑命令（fixture 路径指向已有解包目录）：

```sh
cmake --build out/macos/debug --target lzfs-dll
KRKR2_LZFS_FIXTURE_DIR=/path/to/existing/extracted/assets \
  out/macos/debug/tests/unit-tests/plugins/lzfs-dll '[lzfs]' --reporter compact
```

源码复原覆盖完整媒体虚表、PreRegist 根、Open 全部分支、流/向量/context
生命周期及实际调用的 LZ4 1.7.4 依赖。未声明大小、linked blocks、损坏帧、
大于 4 GiB 和分配失败路径没有对应现成物料，结论仅有静态四文件证据。
私有标识符/原始文件划分及编译器消去的源码细节不可从现有符号唯一反推，
因此不宣称逐字 100% 的原始源码恢复，也不将本地资产测试称为四端运行时差分。

浏览器冒烟使用同一完整 NEKOPARA 4 ZIP。Wasm 及 assets.zip 成功加载，但
该新浏览器 session 的完整 ZIP 缓存触发 QuotaExceededError 并进入 session
storage 回退，尚未到游戏启动/脚本入口；已关闭测试浏览器，没有把等待或
缓存失败误报成插件运行成功。开发服务器已按原来的 8080/NEKOPARA 4 配置恢复。

### 后续 IDB 状态

初始四端取证完成后，旧 worker 会话失联。重开时，2026-09-12 03:09（本机时间）
发现 `Kirikiroid2_1.3.9_iOS_armv7.i64` 已不在目录中（原二进制仍在），
`idb_open` 返回 Input file not found，文件枚举交叉确认。已停止新增逆向，
没有降级到只用三个文件取证；上述所有四端结论来自缺失发生前的 fresh
原生调用与已归档结果。

2026-09-12 按用户要求从 Google Drive Trash 恢复了最新一份
`Kirikiroid2_1.3.9_iOS_armv7.i64`：Drive 显示大小 381.3 MB，原位置为
`binaries`，活动记录为当天 02:03 上传、03:04 移入回收站。恢复操作返回
成功提示，且已在原 `binaries` 文件夹中确认文件存在。当时本地 Mountain Duck
挂载目录尚未同步显示该文件；此状态已被下面的恢复后实测解除。

### 恢复后的四端补充取证

2026-09-12 03:48–03:54（本机时间），四个原二进制和四个原 `.i64` 均可读。
按用户要求直接打开 `reference/binaries/` 中的 IDB；本轮短暂创建的本地
工作副本及其校验清单已经删除，后续取证和保存均针对原路径。
四端 `idb_open` / `server_health` 返回正常，Hex-Rays 就绪；I32 仍是
`thin-armv7` module、基址 `0x4000`，对应关系与原记录一致。

| 平台 | 本轮 database | 原 IDB 大小（打开前，字节） |
|---|---|---:|
| A64 | d1570652 | 376563504 |
| A32 | 361626f4 | 352497631 |
| I64 | 4292c488 | 341011948 |
| I32 | 1fe7caee | 399811836 |

本轮再次 fresh decompile 四端 Open，并从其内存流构造函数写入的虚表读取
方法地址，补充以下映射。列名沿用本文首表，各地址仅属于对应列的二进制；
除明确列出 thunk 外均为函数入口，未为这些 core 函数冒造原始符号名。

| 角色 | A64 | A32 | I64 | I32 |
|---|---|---|---|---|
| media manager Register | sub_8E8BFC @ 0x8e8bfc | sub_6B6D64 @ 0x6b6d64 | sub_100191894 @ 0x100191894 | sub_190FD0 @ 0x190fd0 |
| media record 构造 | sub_8E9188 @ 0x8e9188 | sub_6B6F28 @ 0x6b6f28 | sub_100195110 @ 0x100195110 | sub_194F6C @ 0x194f6c |
| media hash AddWithHash | sub_8EFCB4 @ 0x8efcb4 | sub_6BA3DC @ 0x6ba3dc | sub_10019609C @ 0x10019609c | sub_195C88 @ 0x195c88 |
| MemoryStream SetSize | sub_8F8464 @ 0x8f8464 | sub_6BEEE8 @ 0x6beee8 | sub_10026581C @ 0x10026581c | sub_266030 @ 0x266030 |
| MemoryStream Write | sub_8F8274 @ 0x8f8274 | sub_6BEDAC @ 0x6bedac | sub_100265678 @ 0x100265678 | sub_265F14 @ 0x265f14 |
| MemoryStream GetSize | sub_8F9014 @ 0x8f9014 | sub_6BF59C @ 0x6bf59c | sub_100265EA4 @ 0x100265ea4 | sub_266668 @ 0x266668 |
| MemoryStream 析构体 | sub_8F80E4 @ 0x8f80e4 | sub_6BECA8 @ 0x6beca8 | sub_100265528 @ 0x100265528，虚表 thunk 0x100265590 | sub_265D8C @ 0x265d8c，虚表 thunk 0x265e5c |

共同逻辑与本地对照：

```text
Register(media):
  GetName(name); 若已经存在则抛错
  record = { domain=".", path="/", holder(media), nameLength=GetName().length }
  HashTable.Add(name, record)
  销毁临时 record 和 name
  ref: 创建时 1 -> 临时 holder 2 -> 表内副本 3 -> 临时析构 2

AddWithHash(name, hash, record):
  bucket = hash & 15
  先搜索 bucket.Next 冲突链，再检查内嵌的一级 bucket
  已有键：移到附加顺序链首部，然后赋值 record
  空一级 bucket：placement-construct key/value，标记 USING
  否则：new element，插到 bucket 后的双向冲突链
  新元素同时进入独立 NPrev/NNext 顺序链，Count++

SetSize(size):
  Reference 时抛写入错误
  Size/AllocSize = size; Block = Realloc(Block, size)
  缩小时将超出范围的 CurrentPos 截回 Size
  size != 0 且 Block == null 时抛分配错误

Write(data, count):
  Reference 时抛写入错误
  newpos = uint32(CurrentPos + count)
  若 newpos >= AllocSize：
    增量由原 AllocSize 选择：<64KiB 为 4KiB；<512KiB 为 16KiB；
    <4MiB 为 256KiB；否则为 2024*1024（并非 2048*1024）
    增加 AllocSize；若仍不足则设为 newpos；直接 Realloc 并写回 Block
    AllocSize != 0 且 Block == null 时抛分配错误
  memcpy(Block+CurrentPos, data, count)
  CurrentPos = newpos; Size = max(Size, CurrentPos); return count

GetSize(): 将 uint32 Size 扩展为 uint64 返回
~MemoryStream(): 仅 Block != null 且 !Reference 时释放 Block
```

`StorageIntf.cpp` 的 `tMediaRecord`、16 桶 `tHashTable`、Register 和
`tjsHashSearch.h` 的 AddWithHash/附加顺序链完整匹配这条注册路径。
上述 ref=2 结论现在有临时构造、表内拷贝和临时析构三处独立证据，不能
在 lzfs 初始化后自行补一次 Release。
`UtilStreams.cpp` 的 SetSize、Write、GetSize（头文件内联）和析构匹配上述
扩容、零长度、无溢出保护及失败后状态；无需为 lzfs 新造内存流或修改 core。

逐端差异：A64/I64 的媒体哈希 element 是 80 字节，A32/I32 是 44 字节，
但均为 16 个内嵌一级桶、独立分配的冲突节点和第二条顺序链。
A64 内联键比较与顺序链调整，其余端保留 helper；I32 的 SJLJ 展开及
iOS 析构 thunk 不改变共同源码结构。MemoryStream 的字段偏移随指针宽度
变化，Size/AllocSize/CurrentPos 四端仍为 32 位；扩容阈值与 2024KiB
常量没有平台差异。上述偏移仅用于取证，没有写入本地 C++。

恢复的 IDB 中仍有截断宽字符串和错误的 LZ4 参数类型。本轮重新读取五组
UTF-16LE 原始字节，重建数组边界、添加描述性 `_guess` 名称与字面量注释；
修正四端五个 LZ4F API、Android 调用 thunk、Open 和 SetSize 的原型。
通过 `force_recompile` 清除旧伪代码缓存，再 fresh decompile Open，确认
六参数 decompress、完整 source-size 指针及明确的字面量符号引用。
I64 CreateStream 的 flags 参数也改回 32 位，消除了此前的局部变量分配警告。
四次原生 `idb_save` 均成功，保存后四份 `.i64` 在本地目录中仍存在。
这只确认本地保存，不代替 Mountain Duck 云端上传完成状态。

完整原生结果归档为 `out/diagnostics/lzfs-resume-20260912/{A64,A32,I64,I32}.json`。
本轮未发现需要修正的 lzfs/C++ 行为偏差；新增的是四端证据、IDB 标注和
本分析记录。此前 Web/原生构建和 37 个真实资产测试结果继续有效，本轮
没有因纯取证重复运行它们；前述未知大小、linked、损坏帧、大文件和浏览器
游戏入口的验证缺口仍然保留。原 IDB 缺失这一暂停条件已经解除。
