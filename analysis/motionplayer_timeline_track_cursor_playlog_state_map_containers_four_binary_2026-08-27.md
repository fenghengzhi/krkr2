# motionplayer timeline / track / cursor / play-log / state-map 容器总审计（四参考二进制，2026-08-27）

2026-09-12 更新：用户明确要求修复原版 NEKOPARA 兼容缺陷后，本地 metadata
builder 增加了 folder/children 遍历。本文的四二进制取证仍有效，但 builder 的
“本地完全一致”结论仅适用于修复前版本；当前有意差异见
[NEKOPARA 修复记录](nekopara4_readback_and_timeline_folders_fix_2026-09-12.md)。

## 1. 范围、方法与结论

本报告逐要求闭合 `tasks.md` 的 `MP-C03`，覆盖的不是脚本注册面本身，而是
`EmoteEngine` 时间线运行时内部的完整容器闭包：

- metadata 中 `timelineControl` 到 declared main/diff label vector 和 state-map node 的生产；
- state-map mapped value 的 lazy decode、`TimelineData` / `Track` / `Frame24B` 生产与 owner 发布；
- per-state cursor vector、三个 label vector、active play-log 的查找、增长、删除和迭代；
- play / stop / seek / window / pre-progress / pass / animating / list query 的消费链；
- timeline state serialize / restore 的 container access 与 partial-commit 行为；
- metadata reset、ordinary Engine destruction、state node、track、frame vector 和 deque block 的释放；
- empty、duplicate、stale label、0/1-frame、sentinel、compact cursor、NaN、signed zero、
  allocation/dispatch exception、invalid index/owner 等边界。

本轮只使用四份当前参考 IDB 的原生反编译、完整反汇编和 xref；没有把旧报告中的地址表
当作新取证。共 fresh 审计 **89 个不同函数范围、13,472 条完整指令和 304 条 xref**：

| 目标 | 不同函数 | 完整指令 |
|---|---:|---:|
| Android arm64 | 21 | 4,969 |
| Android armv7 | 22 | 2,368 |
| iOS arm64 | 23 | 2,564 |
| iOS armv7 | 23 | 3,571 |

89/89 均成功 fresh decompile；89/89 的 disassembly `cursor.done=true`，没有截断。
Android arm64 把若干 libc++/libstdc++ 风格的小 helper 内联，另外三端保留独立 helper；
本报告按共同源码语义实体计算四端 disposition，而不是为追求地址数量伪造一对一函数。

结论：本地 `EmoteTimelineFrame24B`、`EmoteTimelineTrack`、`EmoteTimelineData`、
`EmoteTimelineState`、`EmoteTimelineStateMap`、三个 declared/active label vector 及其所有
producer/consumer/teardown 路径与四端共同证据一致。本轮没有 semantic C++ edit；
`MP-C03` 可标为 `CLOSED_STATIC`。正式构建和运行验证仍由 `MP-V*` 独立承接。

## 2. fresh 四端函数矩阵

下表的数字是本轮完整反汇编指令数，不是函数字节数。

| 语义实体 | Android arm64 | Android armv7 | iOS arm64 | iOS armv7 |
|---|---:|---:|---:|---:|
| Engine ordinary dtor | `0x67C898` / 304 | `0x5610E8` / 71 | `0x1001B8B4C` / 97 | `0x1B814E` / 99 |
| metadata reset | `0x666D08` / 250 | `0x555AD8` / 32 | `0x1001A67BC` / 34 | `0x1A5F4C` / 32 |
| build timeline control | `0x66CBEC` / 274 | `0x558EB4` / 157 | `0x1001ABA30` / 145 | `0x1AB18C` / 215 |
| lazy state initialize | `0x66D03C` / 760 | `0x5590E8` / 467 | `0x1001ABD5C` / 346 | `0x1AB4B0` / 552 |
| per-track controller initialize | `0x66DC20` / 91 | `0x559848` / 63 | `0x1001AC5DC` / 71 | `0x1ABDA4` / 120 |
| seek / rebuild cursors | `0x66EE30` / 215 | `0x55A0F8` / 160 | `0x1001AD2C0` / 156 | `0x1ACA22` / 182 |
| apply timeline window | `0x6671FC` / 178 | `0x555BC0` / 177 | `0x1001A6BDC` / 143 | `0x1A636C` / 172 |
| pre-progress active scan | `0x66EB44` / 187 | `0x559F78` / 122 | `0x1001AD0DC` / 121 | `0x1AC844` / 152 |
| step timeline controllers | inline + generic step `0x663FD8` / 270 | `0x55A2DC` / 35 | `0x1001AD540` / 62 | `0x1ACC42` / 62 |
| play | `0x670350` / 371 | `0x55AA70` / 87 | `0x1001ADE0C` / 90 | `0x1AD53C` / 149 |
| stop | `0x679680` / 38 | `0x55F6E4` / 28 | `0x1001B341C` / 28 | `0x1B2F70` / 26 |
| pass / flush | `0x67A100` / 183 | `0x55FCC4` / 151 | `0x1001B3FE4` / 153 | `0x1B3BBC` / 165 |
| getAnimating | `0x671378` / 852 | `0x55B18C` / 257 | `0x1001AE5D8` / 457 | `0x1ADE54` / 640 |
| main label Array | `0x672334` / 91 | `0x55B5C8` / 38 | `0x1001AEF14` / 29 | `0x1AE6F4` / 61 |
| playing info Array | `0x6728A4` / 211 | `0x55B788` / 107 | `0x1001AF104` / 87 | `0x1AE9D0` / 146 |
| serialize timeline group | `0x673BC4` / 201 | `0x55C0E4` / 119 | `0x1001AFE68` / 98 | `0x1AF5BC` / 173 |
| restore timeline group | `0x675834` / 231 | `0x55D184` / 159 | `0x1001B1410` / 138 | `0x1B0EB0` / 230 |
| state-map `operator[]` | `0x685060` / 75 | `0x5669AC` / 60 | `0x1001A6938` / 148 | `0x1A6074` / 237 |
| state-map clear | reset 内联 | `0x5638D8` / 17 | `0x1001B7CD4` / 22 | `0x1B7536` / 20 |
| mapped-value / node destroy | `0x681220` / 30 | `0x563906` / 17 | `0x1001B7D2C` / 32 | `0x1B7562` / 31 |
| TimelineData dtor | `0x681298` / 49 | `0x563940` / 11 | `0x1001C44FC` / 18 | `0x1C1C2C` / 15 |
| Track deque contents destroy | `0x6813C4` / 108 | `0x56395C` / 38 | `0x1001C4544` / 75 | `0x1C1C54` / 80 |

所有入口都同时取得了完整 xrefs。关键 caller closure 包括 apply-metadata → builder、
play / blend → lazy initializer、play / pre-progress → seek、Engine / D3D facade → pass、
top-level state → timeline serialize/restore，以及 Engine reset/dtor → state/track teardown。

## 3. 共同源码容器和 owner 结构

四端共同证明的 source-level 类型为：

```text
unordered_map<ttstr, TimelineState> timelineStates
TimelineState
  unique_ptr<TimelineData> timelineData
  unique_ptr<EmoteVarController> blendController
  uint32 flags
  Variant rawElement
  double loopBegin, loopEnd, lastTime, currentTime
  float blendWeight
  double autoStop
  vector<int32_t> frameCursors

TimelineData
  deque<TimelineTrack> variableList

TimelineTrack
  ttstr label
  bool instantVariable
  vector<Frame24B> frameList
  unique_ptr<EmoteVarController> controller
  float output

Frame24B
  double time
  bool typeZero
  float value
  double easingWeight

vector<ttstr> timelineLabels
vector<ttstr> timelineDiffLabels
vector<ttstr> activeTimelineLabels
```

ABI 展开提供了额外的、但不应硬编码进 portable C++ layout 的确认：

| 项目 | Android arm64 | Android armv7 | iOS arm64 | iOS armv7 |
|---|---:|---:|---:|---:|
| `TimelineData` / deque header allocation | `0x50` | `0x28` | `0x30` | `0x18` |
| Track stride | 56 | 28 | 56 | 28 |
| Frame stride | 24 | 24 | 24 | 24 |
| Track per deque block | 9 | 18 | 73 | 146 |
| state-map node allocation | `0x88` | `0x70` | `0x88` | `0x60` |

Android 的 deque block 是 504 字节有效 Track 区（9×56 或 18×28）；Apple libc++ 的
block 是 4088 字节有效 Track 区（73×56 或 146×28）。这是标准库 ABI 差异，不是四套
源码选择了不同容器。Frame 始终是连续 24-byte vector element。

## 4. metadata → state-map → decoded tracks 的数据流

```text
applyMetadata
  ├─ resetMetadataState
  │    ├─ clear timelineStates (destroy nodes, retain buckets)
  │    └─ does NOT clear activeTimelineLabels
  └─ buildTimelineControl
       ├─ clear timelineLabels
       ├─ clear timelineDiffLabels
       └─ for each raw element
            ├─ choose main/diff vector
            ├─ append label (duplicates retained)
            └─ timelineStates[label].rawElement = element

play(label, flags)
  ├─ optional clear active log
  ├─ non-inserting state find
  ├─ append active label if full-range count == 0
  ├─ lazy initialize state if timelineData == null
  ├─ initialize track/blend controllers
  └─ seek(0) → rebuild frameCursors
```

`buildTimelineControl` 只 clear 两个 declared vector；它不 clear state map，也不 clear active
vector。典型 apply-metadata 调用在它之前已经 clear state map，但 active vector仍保留。因此：

- 新 metadata 仍包含 active label：builder 创建新的 dormant state，active log重新指向它；
- 新 metadata 不包含旧 active label：stale active entry保留；后续不同 consumer 可能 skip、
  `at` 抛出、`operator[]` 物化默认 state、null-deref，甚至在 malformed loop 范围中不终止；
- reset/build 中途抛出：已 clear/append/insert 的前缀全部保留，没有事务回滚。

duplicate label 在 declared vector 中保留每一次出现；state map只保留一个 node。第二次
`operator[]` 命中已有 node，只替换 `rawElement`，不会清除已经 decoded 的 tracks、controller、
flags、times 或 cursor。这一点四端完全相同。

## 5. lazy initialize 的精确 publication frontier

四端 lazy initializer 的共同提交顺序是：

1. 分配并默认构造 `TimelineData`；立即替换 `state.timelineData`，删除旧 owner；
2. 从 `rawElement` 依次读取并立刻写入 `loopBegin → loopEnd → lastTime`；
3. 写 `blendWeight=1.0f`、`autoStop=0.0`；
4. 分配 count=1 的 blend controller，替换旧 owner，立即以 blendWeight 为目标 setTarget；
5. 读取 `variableList` 并 snapshot `Count`；
6. 每个 variable 先读取 `frameList` 和它的 `Count`，然后才向 Track deque append；
7. Track append 后读取 label，并从 instant-variable unordered_set snapshot `instantVariable`；
8. 每帧先读取 raw frame，之后 append 一个全零 `Frame24B`，再逐字段写 time/type/content；
9. `lastTime < 0.0` 时以所有已读 frame time 的最大值替换，否则保留 metadata 值。

因此 exception 不回滚：

- 第一个属性读取就抛出时，fresh empty `TimelineData` 已经发布；
- `frameList.Count` 抛出发生在 Track append 前；
- Track label/instant 查询抛出发生在一个 default Track 已 append 后；
- frame property/content 抛出发生在一个 default/partial Frame 已 append 后；
- 后续 track/frame 失败保留所有早先完成的 deque/vector 元素和 controller owners；
- `lastTime` 的 fallback 只在整个循环正常完成后执行。

frame builder 不额外制造 sentinel。consumer 将 metadata `frameList` 的最后一个元素视为
不派发的尾部边界；因此 malformed 0/1-frame track不会自动补全。

## 6. controller、cursor 与 window 的精确行为

### 6.1 controller 初始化

`state.flags = flags` 是第一项提交。bit 1（值 `2`）未置位时立即返回，甚至不要求
`timelineData` 非空。bit 1 置位后：

- empty frame list 或 instant track 被跳过，并保留任何已有 controller owner；
- eligible track 没有 controller 时分配 count=1 owner；
- 已有 owner 不替换，只 setTarget 到零；
- track.output 不在这里直接清零，要等 controller step 写回。

### 6.2 seek 与 compact cursor

seek 先 `frameCursors.clear()`，保留 vector capacity。对每个 Track：

- `flags & 4` 且 instant 时完全跳过，**不 append cursor**；
- 其余 Track append 一个 signed int32 cursor；
- frame count 小于 2 时 cursor 为 0，没有 action；
- 扫描范围是 `[0, size-1)`；最后一帧只作为 next-time sentinel；
- 扫描期间保存最近的 non-typeZero frame；找到 `time<=target<next.time` 后停止；
- internal parallel route写 Track controller，普通/instant route调用 Engine `setVariable`；
- cursor append 先于 variable/controller dispatch，后续 track 的 allocation/dispatch failure
  不回滚前面的 cursor或变量副作用；
- 最后无条件写 `currentTime=time`，但只有函数正常走到尾部才提交。

### 6.3 apply window

window helper 对 null `timelineData` 不读 Track/cursor，但仍提交 `currentTime=targetTime`。
非空时使用**物理 Track index**访问 cursor vector；即使 `flags&4 + instant` 被跳过，index
仍递增。这与 seek 的 compact cursor故意不一致，混合 Track 排列可导致 unchecked
out-of-bounds。portable 实现没有加“安全修复”。

每个 cursor 后续只向前；inclusive 选择 `next.time<=target`，strict 选择 `<target`。
crossed frame只有在 non-typeZero 且它后面仍有 sentinel 时才 dispatch。transition 为：

```text
max(next_next.time - targetTime - 1.0, 0.0)
```

两套 arm64 用 `FMAX`，两套 armv7 用比较并仅在负值时置零；NaN 都保留。它对应本地
`std::max`，不是 C `fmax`。AArch64 对 `-0/+0` 的指令选择可能与 armv7 的保留首操作数
不同，这是 optimizer/FP instruction boundary，不应误改为跨平台统一 `FMAXNM`。

## 7. active play-log 状态机

`activeTimelineLabels` 是允许外部 malformed duplicate 的 `vector<ttstr>`，不是 set：

- play 使用 full-range `std::count`，正常入口只有 count==0 才 append；不是 first-find；
- append 在 lazy decode / controller init / seek 之前提交；后续失败保留 active entry；
- flags bit0 先 clear active vector，再查 state；unknown label不会回滚 clear；
- named stop删除第一个 equal item；empty/null-backed label clear全部；都保留 capacity；
- is-playing(empty) 只问 vector 是否非空；named lookup是线性 find；
- pass 和 timeline contribution使用 state-map `at`，stale active label抛 `out_of_range` 且
  不物化 state；
- pre-progress、reset-controller、serialize使用 `operator[]`，stale label会物化默认 state；
- query info用 `find` 并跳过 missing state，不修复 active vector；
- getAnimating也用 `find`，missing或 `timelineData==null` 整项跳过。

pass 对 ordinary non-loop timeline从 `uint32(cursor)+1` 开始 flush remaining frames，然后
erase current active item；signed cursor先按 uint32 wrap再回 int32，最后转 size_t。parallel
timeline第一次 pass排 fade-to-zero、置 bit4并保留 active；重复 pass不再排第二次。looping
timeline直接保留。所有 Track、cursor、controller owner 都没有防御性长度/null检查。

## 8. pre-progress 与 timeline controller step

pre-progress 的入口条件是 `dt != 0.0 || force`。它按 active vector live index扫描并通过
state-map `operator[]`取 state，因而 stale label可立即物化默认 state。

共同关键点：

- 一个 `remaining` 在整个 active scan中共享；loop wrap消耗后，后续 label只收到余数；
- `loopBegin >= 0` 是 loop；NaN走 non-loop 分支；
- loop到 end时先 strict window，再 seek(loopBegin)，可重复多次；
- 最后 target使用 `current + fmax(remaining,0)`；两套 arm64 是 `FMAXNM`，armv7 是
  “`remaining>0` 才保留”，NaN和负数都得到正零；本地 `std::fmax` 正确；
- flags bit1置位才 step blend controller和所有 nonempty/noninstant Track controller；
- non-loop `lastTime<=currentTime` 的完成检查优先于 autoStop owner dereference；
-完成或 blend auto-stop 时 erase current active item而不递增 index；否则递增。

Android arm64把 per-state controller step内联在 pre-progress；其余三端保留独立 helper。
这只是优化差异。malformed `loopEnd<=currentTime`、零长度 loop和不减少的 residual可能形成
无限循环；默认 stale state的 `{loopBegin=0, loopEnd=0}` 尤其危险。参考实现没有 range
校验，本地也没有伪造修复。

## 9. getAnimating、query、serialize 与 restore

### 9.1 getAnimating 临时 set

getAnimating 创建一个临时 `unordered_set<ttstr>`，按 active vector顺序处理：

- missing state和 null `timelineData`跳过；
- decoded state先把全部 Track label插入 set，再检测 blend controller activity或
  `loopBegin<0`；此处直接解引用 blend owner；
- set随后抑制 selector、transition、eye、eyebrow、mouth 的自有 activity；
- mouth必须 label和talkLabel都被timeline驱动才被抑制；
- set node/buckets在所有 return路径正确释放。

注意它用 `loopBegin<0`，NaN不构成 animating；pre-progress却用 `!(loopBegin>=0)` 把NaN
归为non-loop。这是四端共同的 API间不对称。

### 9.2 query vectors

main/diff list每次创建新的TJS Array，逐 vector顺序 append String Variant；null label、
duplicate label都保留。playing info按 active顺序、对每个 map hit创建独立 Dictionary，写
`label/flags/blendRatio`；missing node跳过，duplicate active hit产生duplicate Dictionary。
这些 query都不改 state map或 active vector。

### 9.3 serialize / restore

serialize按 active vector顺序用 state-map `operator[]`：stale label先物化默认 state，随后
直接 dereference null blend owner，参考实现没有 guard。每项 schema固定为：

```text
label
flags = state.flags | 1
curTime
blendRatioCtrl
stopWhenBlendDone
```

它不直接保存 decoded Track、Frame、cursor或 TimelineData；这些由目标对象已有 metadata
重建。Dictionary完整写完后才 append到 result Array；失败不回滚已经物化的 map node或
早先 live state side effect。

restore第一步无条件 `stopTimeline(null-empty)`；输入不是Array、item不是Object、缺label或
unknown label都发生在这个 destructive clear之后。known item以 flags/time默认0调用play，
再 inclusive window到curTime，然后按属性存在性部分写autoStop和blend-controller state；
任何中途异常保留已经恢复的 active/state/cursor/controller 前缀。

## 10. state-map、clear 与析构

state-map `operator[]` 的四端 miss路径分配一个 owning-key node，默认 state是：

```text
timelineData = null
blendController = null
flags = 0
rawElement = Void
loopBegin/loopEnd/lastTime/currentTime = 0
blendWeight = 1.0f
autoStop = 0
frameCursors = empty
```

node在 rehash/link前完成 key/state构造；标准库异常清理保持未插入路径的强保证。
duplicate命中返回现有 mapped value。clear销毁每个node/value，清零bucket predecessor表、
first-node和size，但保留bucket allocation/count、max-load与rehash policy。ordinary map dtor
在同样的node teardown后再释放bucket allocation。

mapped value的逆序释放为：

```text
frameCursors vector allocation
rawElement Variant owner
blendController unique owner
TimelineData unique owner
state-map key ttstr owner
```

`TimelineData`销毁 deque内所有live Track，再释放deque blocks/map。每个Track的非平凡成员
逆序释放为 controller owner → Frame24B vector allocation → label ttstr；output和instant
是trivial。Engine普通析构在Player与七个direct controller之后，按
active labels → diff labels → main labels → state map的顺序释放这组timeline容器。

clear和析构都不会调用脚本，不存在reentrant callback；controller/frame/key owners在各自
node/element内唯一持有，没有跨state共享删除责任。

## 11. 边界矩阵

| 边界 | 四端共同结果 |
|---|---|
| null-backed empty stop label | clear全部active，保留vector capacity |
| named duplicate active entries | stop只删除第一个；pass/pre-progress按live vector逐项处理 |
| duplicate declared label | vector保留duplicate；map复用一个node，只替换rawElement |
| stale active + query info/getAnimating | skip，不修复active |
| stale active + `at` consumer | 抛出，不物化state；早先循环副作用保留 |
| stale active + subscript consumer | 物化blendWeight=1的default state；后续可能null-deref/loop |
| 0或1个Frame | seek append cursor 0但不dispatch；不会补sentinel |
| tail Frame |永不作为action dispatch，只提供next-time边界 |
| bit4 + instant Track | seek不append cursor；window仍递增物理index，长度错配未检查 |
| negative/corrupt cursor | signed/uint32/size_t转换按各consumer原样发生，无bounds guard |
| NaN `loopBegin` | pre-progress走non-loop；loop query/getAnimating的有序比较返回false |
| NaN loop residual | `fmax`/FMAXNM得到+0；window transition的`max`保留NaN |
| zero/reversed loop range | 没有校验，可能不终止或产生反向residual |
| vector/deque/map allocation failure | 已完成的clear/append/node/controller/variable前缀不回滚；标准容器当前单次插入自身保持保证 |
| property/dispatch exception | 已发布state/data/track/frame和更早脚本可观察副作用保留 |
| normal clear | vector/deque retain可复用storage的语义按各container操作决定；state map clear保留buckets |
| ordinary destruction | 释放全部elements、owners和storage；不调用脚本，无reentry |

## 12. 本地逐行对照

| 共同语义 | 本地位置 | 结论 |
|---|---|---|
| Frame/Track/Data/State/Map类型 | `cpp/plugins/motionplayer/EmoteEngine.h:162` | 匹配 |
| state map与三个label vector字段 | `cpp/plugins/motionplayer/EmoteEngine.h:778` | 匹配 |
| Engine normal teardown timeline phase | `cpp/plugins/motionplayer/EmoteEngine.cpp:904` | 匹配 |
| metadata reset state-map clear | `cpp/plugins/motionplayer/EmoteEngine.cpp:976` | 匹配 |
| buildTimelineControl | `cpp/plugins/motionplayer/EmoteEngine.cpp:2268` | 匹配 |
| lazy state/track/frame initialize | `cpp/plugins/motionplayer/EmoteEngine.cpp:2692` | 匹配 |
| controller initialize | `cpp/plugins/motionplayer/EmoteEngine.cpp:2788` | 匹配 |
| seek compact cursors | `cpp/plugins/motionplayer/EmoteEngine.cpp:2812` | 匹配 |
| live window和physical-index错配 | `cpp/plugins/motionplayer/EmoteEngine.cpp:2867` | 匹配 |
| play/stop/is-playing/blend | `cpp/plugins/motionplayer/EmoteEngine.cpp:2926` | 匹配 |
| enumeration/list/info | `cpp/plugins/motionplayer/EmoteEngine.cpp:3014` | 匹配 |
| pass与checked state access | `cpp/plugins/motionplayer/EmoteEngine.cpp:3144` | 匹配 |
| contribution与checked accumulation | `cpp/plugins/motionplayer/EmoteEngine.cpp:3208` | 匹配 |
| pre-progress shared residual | `cpp/plugins/motionplayer/EmoteEngine.cpp:3241` | 匹配 |
| serialize/restore partial commit | `cpp/plugins/motionplayer/EmoteEngine.cpp:3295`; `:3510` | 匹配 |
| getAnimating temporary set | `cpp/plugins/motionplayer/EmoteEngine.cpp:1344` | 匹配 |

尤其重新检查了容易误修的 FP clamp：window必须继续使用 `std::max`，loop residual必须继续
使用 `std::fmax`。本轮没有 C++ 语义修改。

## 13. 现有测试映射

本轮没有把“测试源存在”冒充“已正式运行”；正式 unit/Web build属于 `MP-V06..V08`。
当前测试源已经直接覆盖本任务的静态oracle，包括：

- `tests/unit-tests/plugins/motionplayer-dll.cpp:11452`：Track/State unique owner move；
- `:21155`、`:21246`、`:21358`：builder owner与lazy initialize partial publication；
- `:29872`、`:29911`：timeline serialize flag与destructive missing restore；
- `:30214`：duplicate labels、state reuse、active preservation；
- `:30338`：stale active subscript materialization；
- `:30455`：shared loop residual、NaN与finish gate；
- `:30571`：checked ordered contribution与partial side effect；
- `:30684`：pass普通/parallel/loop与cursor wrap；
- `:30795`、`:30837`、`:30866`：compact cursor、capacity reuse、controller owner；
- `:30995`、`:31023`、`:31036`：tail sentinel、null data time commit、strict loop window；
- `:32102`～`:32390`：stale pass、enumeration、sparse info、first erase、play/fade边界。

## 14. IDB 改进与 disposition

四份 IDB 已完成并保存：

- 47 个原 `sub_*` helper确定性命名（Android arm64的一个helper被内联，故11+12+12+12）；
- 每份 IDB追加19条 `MP-C03` task comment，共76条；
- 每份在 timeline builder root新增1个bookmark；
- map subscript/clear、node teardown、TimelineData/Track teardown、pre-progress/window/seek/build
  的名称和caller closure均持久化。

最终 disposition：

- 原始任务：`MP-C03`；
- coverage slice：`MP-C03-TIMELINE-TRACK-CURSOR-PLAYLOG-STATE-MAP-CONTAINERS`；
- evidence status：`IMPLEMENTED`；
- task status：`CLOSED_STATIC`；
- semantic C++ edit：无；
- 剩余事项：只有独立的正式构建、unit/Web runtime/differential验证，不是本任务静态缺口。
