# KrKr2 WebAssembly 移植

## 交流语言
- 始终用**简体中文**回复用户（代码、标识符、命令、二进制符号名保持原文）。

## 项目目标

本项目的最高目标不是功能等价的 WebAssembly 移植，而是尽可能 100% 一比一复原 `reference/binaries/` 中四个参考二进制背后的源代码结构、数据流、调用链、对象生命周期、内部容器实现和边界行为。

`reference/binaries/` 中四个文件的联合反编译结果是权威来源，禁止只分析其中一个文件就代表整个原始实现。本地代码、变量名、现有抽象、Web/Cocos/Emscripten 适配层都不能反向推导原始行为。除明确标注且不可避免的平台边界外，`cpp/` 实现必须优先复刻四个参考二进制共同证明的架构和中间步骤，并明确保留二进制之间的平台、编译器或版本差异，而不是追求表面行为一致。

如果在修复某个具体问题的过程中，发现当前代码修改对该问题本身没有直接帮助，但它推进了“尽可能 100% 一比一复原四个参考二进制背后的源代码结构、数据流、调用链、对象生命周期、内部容器实现和边界行为”这个方向，则该修改不应因为当前问题未被解决而自动撤销。只要该修改有四文件反编译证据支撑、没有引入已知回归，并且让本地实现更接近参考二进制共同证明的原始源码，可以保留它作为架构复原进展。

## 构建
- 调试版：`cmake --preset "Web Debug Config"` → `cmake --build out/web/debug`
- 发布版：`cmake --preset "Web Release Config"` → `cmake --build out/web/release`
- 依赖：Emscripten 6.0.9（与 CI 一致）、emsdk 已 source、VCPKG_ROOT 已设置、ninja、cmake 3.31.1+、bison 3.8.2+；切换 SDK 后使用新的构建目录并重编译所有依赖，禁止复用旧 SDK 的静态库
- 固定输出：`out/web/{debug,release}/` → index.html, index.js, index.wasm, vlfs.js, assets.zip（UI 资源 stored-zip；--preload-file/index.data 已移除，游戏与 UI 文件经 VirtualLazyFS 懒加载，见 `cpp/core/environ/web/VirtualLazyFS.h`）。`index.worker.js`、`.symbols` 等 sidecar 是否出现取决于当前 Emscripten 版本和构建选项，不得当作固定产物
- 机器特定的 EMSDK/VCPKG_ROOT 路径可写在可选且不入库的 `.claude.local.md`；该文件不存在时使用当前 shell 环境或实际安装路径，禁止假定它必然存在

### 构建陷阱
- Windows/vcpkg 构建应显式导出 EMSDK_PYTHON，避免 vcpkg 误选不兼容的旧 Python 或重复下载嵌入式 Python；要求是所选解释器支持项目脚本所需语法，不得笼统断言“系统 Python 缺少 `match`”
- 改 CMakeLists.txt（增删改文件）后必须重跑 `cmake --preset` 再构建
- macOS/Homebrew 上若 bison 报错 "require 3.8.2 but have 2.3"，将 `BISON_EXECUTABLE` 指向本机 Homebrew bison；其它平台使用本机实际路径，禁止照抄 `/opt/homebrew/...`
- 构建前必须关闭 coi-server — 否则提供旧 wasm

## 项目结构
- `cpp/plugins/` — NCB 插件实现；一个虚拟 `.dll` 模块可以由多个 translation unit 共同组成，模块边界以 NCB 注册/`NCB_MODULE_NAME` 和 CMake 目标为准，不能按“一文件一模块”推断
  - `PackinOne.cpp` — 批量加载器，`Plugins.link("PackinOne.dll")` 时加载 8 个子插件
  - `DrawDeviceD3D.cpp` — iTVPDrawDevice 封装（Web 构建的 D3D 桩实现）
- `cpp/plugins/motionplayer/` — EmotePlayer + Player (MotionPlayer)，带 NCB TJS2 绑定，详见各文件头注释
- `cpp/core/tjs2/` — TJS2 脚本引擎核心
- `cpp/core/visual/WindowIntf.cpp` — Window 类：drawDevice setter 要求 `interface` 属性返回 iTVPDrawDevice*
- `cpp/core/plugin/PluginImpl.cpp` — TVPLoadPlugin（由 Plugins.link 调用）、TVPLoadInternalPlugins（启动时）
- `cpp/core/base/StorageIntf.cpp` — 自动路径表、TVPAddAutoPath、TVPGetPlacedPath
- `cpp/core/environ/web/Platform.cpp` — Web 平台启动逻辑
- `cpp/core/environ/web/VirtualLazyFS.{h,cpp}` + `platforms/web/vlfs.js` — VirtualLazyFS：游戏/UI 文件懒加载（JSPI 挂起读 + pthread 代理 + OPFS spill + 写 overlay），仅 Chromium 137+
- `tests/unit-tests/plugins/motionplayer-dll.cpp` — MotionPlayer/EmotePlayer 单元测试

## 代码模式
- TJS2 属性绑定：`NCB_PROPERTY(name, getter, setter)`、`NCB_PROPERTY_RO(name, getter)`
- TJS2 方法绑定：`NCB_METHOD(name)`、`NCB_METHOD_RAW_CALLBACK(name, &Class::func, flags)`
- 桩模式：`#define STUB_WARN(name) LOGGER->warn("ClassName::" #name "() stub called")`
- 字符串转换：`detail::narrow(ttstr)` → std::string、`detail::widen(std::string)` → ttstr

## 调试工具
- XP3 解包：使用 `krkr2-xp3-tool` skill 的 resolver 获取当前主机二进制；不要写死 `tools/bin/mac/rel/xp3`
- TJS2 字节码反汇编：使用 `tjs2-disasm` skill，并按 `tools/bin/<mac|linux|win>/<rel|dbg>/` 选择当前主机产物
- 原生工具只在非 Web / Android / iOS 构建中启用。当前仓库只提供 macOS 原生 CMake preset；Linux/Windows 没有现成 native preset 时必须明确报告或先补有效配置，禁止编造 `Linux Release Config` / `Windows Release Config`

## 调试注意事项
- 不要用单独的 XP3 文件测试 — 不完整的 XP3 集合会导致初始化失败，掩盖真正的 bug
- 浏览器自动化：使用 `playwright-cli` skill，并遵循 `.codex/skills/krkr2-debug/SKILL.md`。测试游戏输入优先使用 `playwright-cli click` 或在单次 `run-code` 中完成鼠标点击；不要跨调用拆分触摸 down/up。判断输入是否进入引擎前，先在页面安装 capture listener 核对浏览器事件计数
- C++ 日志（`spdlog`/`printf`/`fprintf(stderr)`）均输出到浏览器控制台
- WASM 引擎每秒可能产生数百条日志，浏览器的近期 console 视图可能遗漏初始化日志，不能单独作为诊断依据。按 `krkr2-debug` skill 在导航前注入捕获脚本，再分批读取 `window._filteredLogs`
- URL 参数：`?xp3=file.xp3` 加载单个 XP3，`?game=file.zip` 加载 ZIP 包。注意不要混用

## Codex 原生 IDA MCP 逆向工程

### 参考二进制清单

`reference/binaries/` 中的四个逆向目标是：

| 平台 | 架构 | 二进制文件 | IDA 数据库 |
|------|------|------------|------------|
| Android | arm64-v8a | `Kirikiroid2_1.3.9_Android_arm64-v8a.so` | `Kirikiroid2_1.3.9_Android_arm64-v8a.so.i64` |
| Android | armv7 | `Kirikiroid2_1.3.9_Android_armabi-v7a.so` | `Kirikiroid2_1.3.9_Android_armabi-v7a.so.i64` |
| iOS | arm64 | `Kirikiroid2_1.3.9_iOS_arm64` | `Kirikiroid2_1.3.9_iOS_arm64.i64` |
| iOS | armv7 | `Kirikiroid2_1.3.9_iOS_armv7` | `Kirikiroid2_1.3.9_iOS_armv7.i64` |

`.i64` 是配套 IDA 数据库，不是额外的逆向目标。文件名中的 `armabi-v7a` 是参考文件的实际拼写，引用时不得擅自改名。

### 核心原则
- 无原始项目源代码；逆向输入是 `reference/binaries/` 中的四个参考二进制。所有逆向使用 IDA MCP
- **禁止寻找不存在的上游源码** — 本项目没有上游原始仓库。进行源码还原时，不得搜索、猜测、克隆或引用任何所谓的“原始仓库”或“原版源码”，也不得把找到上游代码当作工作路径；唯一有效的还原依据是 `reference/binaries/` 中四个参考二进制的联合反编译证据
- 每轮逆向开始时核对上述四个目标二进制及四个配套 `.i64`；任一目标或 IDB 缺失、不可读或对应关系错误时必须先报告并停止取证，禁止静默退化为单文件分析。统计目标时排除 `.i64`
- 四个参考二进制与本地代码都并非一一对应。必须为目标函数建立逐文件映射，记录每个文件中的函数名/地址及状态（已定位、内联、缺失或尚未定位）；地址只能在所属二进制内使用，禁止跨文件复用
- 始终以四文件联合反编译结果为权威来源，本地代码可能有误或不完整。共同控制流用于证明共享源码结构；发现差异时必须分别记录并判断是平台、ABI、编译器、版本还是尚未解释的差异，禁止静默选择其中一个实现
- **完全对齐架构，不接受功能等价** — 必须复刻四个参考二进制共同证明的代码架构和内部实现（如 TJS dispatch 包装、TJS Array 管理），不能用 C++ 简化替代（如 shared_ptr、std::vector）即使行为结果相同

### 已有分析成果
- `analysis/` 只保存当前仍有效、可追溯到四个参考二进制的新证据；旧单文件结论已删除。分析前应检查现存文件，但不得把目录存在或一次空搜索理解为已有完整覆盖

### IDA 工具注意事项
- 当前唯一受支持的工具命名空间是 Codex 原生 `mcp__idalib__*`；插件展示名仍可为 “IDA Pro MCP”，但展示名不是工具前缀。不要根据插件名拼接工具名
- `mcp__idalib__idb_open` 打开每个配套 `.i64` 并返回独立 `session_id`；所有后续 IDB 工具调用都必须把该值作为 `database` 显式传入，不存在可跨调用依赖的“当前 IDB”
- `mcp__idalib__idb_list` 枚举已经打开或可接管的会话；使用返回的 `session_id`，不要自行猜测会话名
- `mcp__idalib__server_health` 用于核对当前 `database` 对应的 `module`、`input_path`、架构基址和 Hex-Rays 状态；确认对应关系后才能执行地址查询
- `mcp__idalib__decompile` 配合当前二进制内的函数地址和 `database` 获取伪代码
- `mcp__idalib__find` 配合 `type: "string"` 定位字符串引用，但仅匹配 ASCII/UTF-8；UTF-16/UTF-32 使用 `/ida-search-string` 技能中的 `mcp__idalib__find_bytes` 工作流
- IDA MCP 必须通过 Codex 原生暴露的 `mcp__idalib__*` 工具调用；若当前工具清单中不存在这些工具，停止取证并报告，禁止用 Python MCP 客户端、手工 JSON-RPC、IDA CLI 或其它旁路伪造原生工具调用记录
- IDA 可能只显示 UTF-16 字符串首字符（如 "f" 代表 "fstat.dll"）— 使用 `mcp__idalib__get_bytes(database=..., regions=[{addr, size}])` 读取原始字节确认，当前接口没有 `get_operand_value` 工具
- IDA 经常将 UTF-16LE 字符串误标为 ASCII（如 `"z"` 实际是 `"zx"`）。原因：UTF-16LE 的 `7A 00 78 00` 被 IDA 在 `7A 00` 处截断为 ASCII `"z"`。参考二进制中传给 `iTJSDispatch2::PropGet` 的 key 使用 `tjs_char*`=UTF-16LE，因此**反编译中出现的单字符字符串常量都应怀疑是截断的 UTF-16LE**。遇到时用 `mcp__idalib__get_bytes(database=..., regions=[{addr, size: 16}])` 确认真实内容，再用 `mcp__idalib__set_type(database=..., edits=[...])` 逐个修复 IDA 标注
- IDA 有时合并独立函数 — 检查 `loc_` 地址处是否有 `SUB SP` 函数序言
- NCB 类注册函数：在每个参考二进制中分别定位 `ncb_addMember` 和 `ncb_addConstant`；固定地址只属于产生该地址的二进制，不得当作四文件公共锚点
- IDA 操作细则见 `.claude/skills/ida-decompile/SKILL.md`
- NCB 模块加载（`LoadModule`）不区分大小写（加载前转小写）

## 工作流 — 代码修改前置条件（BLOCKING）

任何会改变 `cpp/` 中参考实现运行行为、源码结构、数据流、调用链、对象生命周期、内部容器或边界行为的 C/C++ 修改，**必须**满足以下全部条件，缺一不可。不满足条件的修改视为无效，必须回退。

纯构建系统修改（如 CMake target/source discovery）、纯注释或文档修改、格式化，以及经检查确认不改变运行语义的机械性 include/path/编译兼容修正不适用四文件函数取证前置条件。豁免只针对“没有可对应参考函数的非语义修改”；只要改动可能影响上述任一运行语义，或无法确定是否影响，就必须按完整四文件流程执行。

### 前置检查清单
1. **四文件函数映射** — 列出 `reference/binaries/` 的四个文件，以及目标函数在每个文件中的函数名/地址和定位状态；禁止只给一个地址
2. **四文件反编译证据** — 本次对话中必须有对四个目标函数的原生 `mcp__idalib__decompile` 调用记录（每次显式传入对应文件的 `database`）；若某文件中函数被内联、缺失或无法定位，必须提供本轮原生 `mcp__idalib__find` / `find_bytes`、`xrefs_to`、`disasm` 或调用链搜索记录说明原因
3. **关键逻辑摘要** — 先写四者共同控制流的伪代码，再逐项列出每个二进制的差异，包括所有条件分支和默认值；禁止把不一致强行合并成单一结论
4. **本地实现对照** — 逐行说明本地代码如何复刻上述伪代码

### 硬性禁止（违反任何一条 = 立即停止并反编译）
- **禁止从 PSB 键名推导行为** — 必须反编译确认读取条件（如 mask 位掩码门控）、默认值、数据类型
- **禁止从变量名推导语义** — 必须逐文件反编译确认参考二进制实际使用的字符串常量
- **禁止"先改代码再验证"** — 必须"先反编译 → 写伪代码 → 再改本地代码"
- **禁止把多个推测链接成结论** — 每一步都必须有独立的反编译/运行时日志证据
- **禁止从本地代码推断参考二进制行为** — 本地代码可能是错的
- **禁止用单个参考二进制代替四文件取证** — 即使某个文件的伪代码最清晰，也必须完成其余三个文件的映射和核对；差异未解释前不得宣布共享源码结论
- **禁止在架构不一致的基础上打补丁** — 当修复需要 workaround 架构差异时（如本地代码缺少参考二进制中存在的计算步骤、或存在参考二进制中不存在的计算），必须先重构代码使数据流和计算步骤与四文件反编译证据一一对应（同样的输入→同样的中间变量→同样的计算顺序→同样的输出），再进行修复。打补丁只会引入新 bug

### 证据是阻塞项，验证是尽力项（澄清）
- 上面"禁止先改代码再验证"约束的是**顺序**（反编译证据在前），**不是**"没有运行时验证手段就不许实现"。
- **不得仅因"无法验证"而阻塞或 defer 实现**：当目标函数已满足前置检查清单（四文件映射和取证 + 写出共同伪代码与差异 + 本地对照）、但缺运行时验证手段（无 fixture / oracle / 差分覆盖）时，应照常忠实复刻 → 构建通过 → 用**现有**手段尽力补验证（复用现有 fixture / oracle、加诊断日志、必要时给工具补能力如解密 seed / dump 探针读现有资产）。**没有现成物料（fixture / 测试数据）就不实现对应测试，且不要尝试构建物料**——现有 fixture / oracle 覆盖不到该路径时，直接放弃新增测试（绝不从零捏造 fixture / 物料），在注释或 analysis/ 标注验证缺口即可。
- 仍然 BLOCKING 的只有**证据缺失**（未反编译就改、从键名/变量名/本地代码推断、把推测链接成结论、在架构不一致上打补丁）——**不是**验证缺失。
- **oracle-inert（改动对现有差分/运行时不可观察）不是拒绝、defer、降优先级、或"建议改做别的 P0"的理由。** 复原的价值标的是六维架构本身（源码结构 / 数据流 / 调用链 / 对象生命周期 / 内部容器实现 / 边界行为），**没有一维要求"能被现有 logo 差分观察到"**。一段忠实复刻即便对所有现有 fixture 全程 inert（容器拓扑 STL→KiriKiri 替换、变量级联、HM3 等 dead-data 快照、root/var-track 流……），只要有反编译证据且构建不回归，就照常实现并保留；此时 logo 0-mismatch 是**非回归守护**，不是它存在的理由。**禁止把"oracle 看不到"包装成 ROI / 可验证性论据去排挤架构复原工作**，也禁止据此把一个 open P0 判为"低价值"。
- 唯一**合法可停**的两种情况，且都不得用 inert 冒充：(a) **证据缺失**（见上）；(b) **明确标注且不可避免的平台边界**（如本地渲染栈无 per-vertex 顶点色 → 4-corner 颜色必须 bake，纹理只能按 (name,color) 缓存）——必须在代码注释 / analysis 写明边界的**具体技术原因**，"改动 oracle 观察不到"**不算**平台边界。
- **「不存在 / 缺失 / 死字段 / 架构前置缺失」是强断言，下结论前必须独立交叉核实，禁止单凭一次 negative grep 判定。** 宣布"本地缺少某机制 / 某字段从未被使用 / 前置数据流缺失 / 应判平台边界"之前，必须换搜索词、换工具、读调用链、或派独立 agent 复核确认它**确实**不存在。**`grep` 返回空 ≠ 不存在**——尤其注意 shell cwd 漂移（`cd` 失败后 cwd 不变）会让 `*.cpp` 之类相对 glob 静默落空。据一次空搜索就打"architecture-blocked"标签、defer、或写进注释/memory，会把错误固化并误导后续 session。反面教训：本仓库曾因一次漂移的 grep 误判 `_internalRenderLayer`/`updateLayerAfterDraw` 为"缺失"，把本可直接实装的 anchor w/h + 612 gate 错标为"架构前置缺失、不可移植"（commit 5018087，后由 eb347f5 纠正）。
- **memory / 代码注释 / analysis 笔记一旦被后续证据证伪，必须就地立即纠正，不得放任。** 反编译结论、agent-memory、`MEMORY.md` 索引、代码注释中只要发现方向搞反 / 前提错误 / 字段误判 / "缺失"误判，必须当场更新或删除那条记录，并在 commit 说明里点明"纠正了 X 及其原因"。**错误的 memory 会传染**——本仓库已多次被错误 memory 误导（M5 path-key 把 Player+24 误判为 path-keyed、M7 把已存在的渲染前置误判为缺失）。宁可多花一步纠正源头，也不要让错误结论在 memory/注释里留存继续误导。

### 标准工作流程
1. 发现问题 → 加诊断日志确认现象
2. 枚举 `reference/binaries/` 四个文件 → 在每个二进制中定位并反编译对应函数 → 建立四文件函数映射
3. 提炼共同伪代码并记录逐文件差异 → 对比本地代码 → 找到精确偏差
4. 修改本地代码精确复刻联合证据 → 编译源码中的注释只记录必要的语义依据，不写反编译地址；逐文件函数名、地址、偏移与映射统一写入 `analysis/*.md`，并在每条记录中标明所属二进制
5. 构建验证 → 运行时诊断确认修复

### 渲染/定位问题专项
- 修复前必须 trace 完整坐标链（PSB → ownerLayer → primaryLayer → paintBox → screen），每层有独立 transform
- 反编译完整渲染链（Layer→DrawDevice→Texture→Cocos2D），不要只看局部

### IDA 反编译质量改善（手动逐个修正）
反编译后如果发现以下问题，**当场修正**，不要留到以后。每次分析函数顺手修几个，IDB 质量持续提升。

#### UTF-16LE 字符串修正
发现截断的单字符字符串时：
1. `mcp__idalib__get_bytes(database=..., regions=[{addr, size: 32}])` 确认真实 UTF-16LE 内容
2. `mcp__idalib__set_type(database=..., edits=[{addr, ty: "unsigned short[N]"}])` 修正完整字面量范围的类型标注；`N` 按 UTF-16 code unit 数和终止符确定
3. 重新调用 `mcp__idalib__decompile(database=..., addr=...)` 确认反编译输出已更新

#### 类型信息丰富
- `mcp__idalib__declare_type(database=..., decls=[...])` — 把本地代码中的 C++ struct/class 定义导入当前 IDB（如 EmotePlayer、tTVPRect、iTJSDispatch2）
- `mcp__idalib__set_type(database=..., edits=[{addr, signature: ...}])` — 给函数签名设正确的参数和返回类型（如 `void __fastcall fn(EmotePlayer *this, int index)`）
- `mcp__idalib__infer_types(database=..., addrs=[...])` — 修正关键函数类型后调用，让 IDA 在指定地址推断并应用类型
- 导入类型的优先级：高频基础类（iTJSDispatch2、tTJSVariant）> 当前分析的目标类 > 其余

#### 函数/变量重命名
- `mcp__idalib__rename(database=..., batch={...})` — 批量重命名，`batch` 下分 4 类对象：`func`（函数，按 `addr`+`name`）、`data`（全局/数据变量，`old`→`new`）、`local`（Hex-Rays 伪代码局部变量，按 `func_addr`+`old`→`new`）、`stack`（栈变量，同 local 形参）。支持 `dry_run`（只校验不改）、`allow_overwrite`、`stop_on_error`
- `sub_XXXX` 重命名为 `ClassName_MethodName`（命名规范见下方"IDA 符号管理"）走 `func`
- **局部变量可持久重命名**：`rename(batch={local:{func_addr, old, new}})` 等价于 IDA UI 右键 Rename，重命名后整个反编译体的所有引用都会更新。`set_comments` 仅在需要额外标注语义时补充使用，不是因为 rename 改不动局部变量

#### 修正后保存
- 一轮分析结束后调用 `mcp__idalib__idb_save(database=...)` 持久化当前 IDB 的所有修正；四个数据库分别保存

### IDA 符号管理
- **命名权威 = 四个参考二进制自身的名字证据，不是本地项目。** 优先级：
  1. **二进制里字面存在的名字** —— NCB 注册的成员字符串（`ncb_addMember` 的 key）、字符串常量、RTTI/typeinfo、导出符号。这是 ground truth，**读取它 ≠ 推断**；与本地冲突时**以二进制为准**。本地代码是“待验证 / 可能错”的一方，**绝不能反过来当命名权威**。（反面教训：本仓库 angle 访问器 getAngleDeg/getAngleRad 本地一度接反，正确映射应来自四个参考二进制各自注册函数中字面的 "angleDeg"/"angleRad" 绑定；若“以本地为据”就会把错误命名灌进 IDB。）
  2. **本地代码仅作交叉参照** —— 用于确认类名 + `ClassName_MethodName` 写法约定，且**仅在二进制无任何名字信号（纯 `sub_XXXX`）时**使用。
- 四个文件的名字证据冲突时，逐文件保留并记录冲突，禁止挑选最像本地代码的名字作为公共名称；只有在调用链和注册证据解释差异后才能统一命名
- **禁止从二进制行为推断 / 猜测命名**（如把 `StartProcess` 猜成 `Process`）。“读二进制里字面存在的名字”属第 1 项（允许）；“看行为猜名字”才是被禁止的。
- 二进制名字证据与本地标识符都找不到对应名时，加 `_guess` 后缀（如 `Layer_Update_guess`）。
- **IDB 里发现的误命名 = 被证伪的产物，必须就地修复**（与 memory / 注释 / analysis 的「证伪即纠正」同规则，见工作流 BLOCKING 节）：`rename` 到正确名 + 函数头 `set_comments` 记录纠正依据（注册站点 / 字符串地址）+ `idb_save`；并同步更新代码里按旧符号写的注释。

## 字节布局复刻工作法（重要方法论）
- **忠实复刻 ≠ 写"更安全"的代码。** 二进制里的死值运算（算了不消费的指针/偏移）、未初始化局部、refcount no-op（AddRef+即刻 Release）都是**源码 token**，必须复刻，不得因"更安全 / oracle 不可观察"省略或补 0 初始化。死值→`T* x=&...; (void)x;`（`(void)` 仅压跨编译器 unused 警告）；no-op→即刻析构的拷贝（如 `ttstr x=src;`）。
- 目标是复刻生成四个参考二进制的**共享源代码**（.cpp/.h），不是复刻其中任何一个**编译产物**。四个文件之间以及它们与 wasm 之间可能使用不同平台、ABI、编译器和指针宽度，字节偏移不一致是正常现象；必须从跨二进制共同控制流反推源码结构，不能硬选一个文件的布局
- 复刻“源代码结构”= 写普通 C++ 类（带继承/字段名/方法语义）让编译器自由算偏移。**禁止**用 `#pragma pack` / `_padN` 填充 / `static_assert(offsetof/sizeof==N)` 去硬凑任一参考二进制的字节布局——作者源码写的是字段名（`double time;`），从不写 `_pad4`；硬凑既非源码，也无法同时匹配 arm64、armv7 和 wasm32
- 要对齐的六维全是语义层：源码结构 / 数据流 / 调用链 / 对象生命周期 / 内部容器选型（用 deque/hashmap 而非 std 替代，指**实现选型**对齐，非字节布局）/ 边界行为。**没有一维要求字节偏移一致**
- 字节偏移 + 证据地址的正确归属是 `analysis/*.md`（反编译对照笔记），且每条都必须标明所属二进制文件，**不是任何被编译的 C++ 文件**。偏移是反编译时确认字段语义/无遗漏的工具，不进代码
- 例外：当二进制按 `*(float*)(elem+4*i)` 真的按字节读某容器**元素的内部数据格式**时，该元素 POD 的内部字段布局是与平台无关的数据契约，可保留（如曲线关键帧 20B 元素）；但“**对象在内存的 ABI 偏移**”永不需对齐
- **反编译里的容器尺寸表达式（循环上界 / `dequeSize-1` / `+1`）可能是 STL `size()`/迭代器的内联展开产物，不是源码 token。** 判定「容器拓扑 / 源码结构是否已还原」必须反编译容器的**构造点**（push/erase 计数），不能只看消费循环的上界。反例（本仓库曾在 Android arm64-v8a 参考二进制中实测）：libstdc++ `std::deque::size()` 对 >512B 元素（1-elem/block，如 node 2632B）展开成 `(start.last-start.cur)/T == size()+1`，故反编译 `idx < dequeSize-1` **就是**源码 `idx < size()`，那个 `-1` 是编译器产物而非源码 `size()-1`，更不是 sentinel；其它三个目标仍须独立核对各自 STL/编译器展开。曾因只看上界连错两次（先判 off-by-one，再判 trailing sentinel）。数值等价 ≠ 源码偏离：先确认反编译表达式是不是 STL 展开，再下「源码被简化」结论
