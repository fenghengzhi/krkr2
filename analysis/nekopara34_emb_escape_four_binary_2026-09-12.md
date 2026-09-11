# NEKOPARA 3 / 4 启动黑屏：emb 的 escape 参数

## 运行时证据

完整输入为用户提供的 `【KRKR】【官中】NEKOPARA 4.zip`（入口
`运行游戏.xp3`）和 `【KRKR】【官中】NEKOPARA Vol.3／猫娘乐园 Vol.3／巧克力与香子兰3.zip`
（入口 `data.xp3`）。Web Debug 的两个游戏均完成 startup，但停在
`title.ks:35 [s]`，`SystemHook.isDefined("title.loop")` 返回 0。

通过 playwright-cli 在本地 Wasm 的 Scripts.exec 入口调用诊断脚本，读取状态，
并在重走实际 first.ks 时记录 conductor.getNextTag 的返回值：
`first.ks:26` 中 `[emb escape=false exp="createCallConfigFile('custom.ks')"]`
产生的 `[call storage=custom.ks]` 被返回为逐字符 `ch`，没有执行 call。
macro.ks 的动态 call 同样被转为文字。标题 hook 因而没有注册，UI 图层没有创建。
诊断脚本、日志和截图位于 `out/diagnostics/nekopara34-20260912/`。

## 本轮四文件映射

四个二进制及配套 i64 均可读，idb_open / server_health 已核对。
iOS armv7 数据库使用同一 fat Mach-O 的 thin-armv7 slice；两个数据库保留旧 Windows input path。
以下地址均已通过本轮原生 find_bytes、xrefs_to 和 decompile 核对；
没有用旧注释中的无归属地址作函数映射。

| 二进制文件 | database | getNextTag 对应函数 | escape 字符串 / 缓存 key | 读取与转义分支 |
|---|---|---|---|---|
| Kirikiroid2_1.3.9_Android_arm64-v8a.so | 264077ea | sub_550E54 @ 0x550E54 | 0x14BFDF2 / 0x1AAF8E8 | 0x554994–0x554C24 |
| Kirikiroid2_1.3.9_Android_armabi-v7a.so | ffe5c45d | sub_4AE2B4 @ 0x4AE2B4 | 0xD777E2 / 0x110E348 | 0x4AED9A–0x4AEEC0 |
| Kirikiroid2_1.3.9_iOS_arm64 | db84fccc | sub_10009F1EC @ 0x10009F1EC | 0x101958D5C / 0x101B67030 | 0x10009F8F4–0x10009FA98 |
| Kirikiroid2_1.3.9_iOS_armv7 | d4dca3b4 | sub_9DD9C @ 0x9DD9C | 0x174B0C0 / 0x187BE20 | 0x9EAB2–0x9EC22 |

get_bytes 确认四份 key 都是 UTF-16LE `escape\0`。Android 两库原来只显示
`"e"`，本轮已修正该 key 和 getNextTag 字面量的 unsigned-short 数组类型。
四个完整反编译结果由原生工具的 output URL 下载保留到诊断目录。

## 共同伪代码（修改前记录）

```cpp
// 仅在 condition && ExcludeLevel == -1 的 emb 分支，宏分支保持自己的路径。
if (ldelim != 0) ++CurPos;
variant val;
string exp = DicObj.PropGet("exp");
if (exp.empty()) throw syntaxError;
executeExpression(exp, Owner, &val);
exp = string(val);
DicObj.PropGet(0, "escape", keyHint, &val, DicObj);
bool escape = val.type == Void ? true : bool(val);
count = 0;
for (ch : exp) count += 1 + (escape && ch == '[');
tailLength = strlen(CurLineStr + CurPos);
length = tagstartpos + count + tailLength;
if (ldelim == 0 && !IgnoreCR) ++length;
string newbuf = allocBuffer(length + 1);
copyPrefix(newbuf, CurLineStr, tagstartpos);
for (ch : exp) {
    if (escape && ch == '[') append(newbuf, "[[");
    else append(newbuf, ch);
}
append(newbuf, CurLineStr + CurPos);
if (ldelim == 0 && !IgnoreCR) append(newbuf, '\\');
newbuf.FixLen();
LineBuffer = newbuf;
CurLineStr = LineBuffer.c_str();
CurPos = tagstartpos;
LineBufferUsing = true;
// 从嵌入位置重新解析。
```

逐文件差异：Android arm64 把 PropGet wrapper 和 variant bool 转换内联，
其余三份分别调用 PropGet helper 和 bool helper。Android armv7 helper 为
`sub_4AD5D8 @ 0x4AD5D8`，iOS arm64 为 `sub_10009E268 @ 0x10009E268`，
iOS armv7 为 `sub_9CCDC @ 0x9CCDC`，均已 fresh decompile：flags=0，
结果写入同一 val，objthis 为字典本身。ABI 的 hint 偏移、引用计数宽度、
异常清理及循环指令不同；本分支条件、默认 true、两次转义门控和字符串拼接顺序一致。
Android arm64 的类型 switch：Void 保留 true，Object/Octet/Integer 根据值，
String 调字符串 bool，Real 与零比较；其余三份通过既有 variant bool helper 完成。

## 本地逐项对照与修复范围

`cpp/core/base/KAGParser.cpp::GetNextTag` 原有 exp 求值、空表达式检查、
前缀和尾部复制、行式反斜杠、FixLen、LineBuffer 与 CurPos 更新均已有对应步骤。
偏差仅在 exp 转换后缺少 escape 的字典读取，计数循环和复制循环均无条件转义 `[`。

1. 在现有静态字符串 key 列表增加 interned `escape`，与四份静态初始化一致。
2. 在 `exp = val` 后对同一 val 调用 flags=0 / key hint / DicObj 的 PropGet。
3. 用 `val.Type() == tvtVoid ? true : val.operator bool()` 保留原有默认值与类型转换。
4. 计数循环及复制循环分别加入 `escape &&`，保留分配、拼接和生命周期顺序。

## 验证

- Emscripten 6.0.9 Web Debug / Web Release 均配置和构建通过，
  `git diff --check` 通过。最终本地构建版本分别为 `20260912015834` /
  `20260912015904`，更新了资源版本参数，避免沿用旧 HTTP / PWA 缓存。
  下述端到端游戏验证使用 Web Debug。
- 两个原始完整 ZIP 的标题菜单均恢复，截图 `vol3-after.png` / `vol4-after.png`。
- Vol.4 的 `title.loop` hook 从 0 恢复为 1；点击 GAME START 后进入
  `start.ks:29 [sceneplay]`，显示咖啡店背景与开场选项（`vol4-game-start.png`）。
- Vol.3 点击 GAME START 后显示咖啡店背景和巧克力的「欢迎光临 Soleil～♪」
  开场对白（`vol3-game-start.png`）；Vol.4 选择开场选项后显示对白
  （`vol4-opening.png`）。
- 鼠标 capture 计数确认 pointerdown/up、mousedown/up、click 各一次。
- 首次自动化使用无持久化浏览器时遇到独立的 OPFS 配额限制，改用本任务专用
  `/tmp/krkr2-nekopara34-profile` 后完整资源正常读取。增量构建初次复测又命中
  旧 HTTP 缓存，禁用缓存后核对实际 Wasm decodedBodySize=78186815；以上成功
  结果来自新 Wasm。未修改原游戏或其补丁来规避解析问题。
- 本轮复用用户提供的实际完整游戏验证，没有编造新剧情或 fixture。
