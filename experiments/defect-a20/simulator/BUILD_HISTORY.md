# 故障机器人可用的模拟器树（2026-09-19）

上游 `gamerpuppy/sts_lightspeed@7476a81` 的故障机器人卡牌几乎全是空壳（连起始卡 Zap / Dualcast
都报 `attempted to use unimplemented card`，随机策略 A0 均层 5–6）。`Attemory/sts_lightspeed@46e14e4`
（spire-agent 使用的 fork）补齐了故障机器人卡牌、充能球/集中、大量怪物状态，并重写了 MCTS
（按 RNG 世界做根节点均衡、置换表、每步 1s 时间上限），但删掉了 Python 绑定。

本树 = fork 的 `src/ include/ apps/` + 本仓库沿用的上游 `bindings/ pybind11/ json/ CMakeLists.txt`
+ `combat_rules.patch` + `sim_rl_hooks.patch` 的非绑定 hunk + 下列绑定改动。

## 组装

```bash
U=<upstream 工作区, 已打两个补丁>   F=<Attemory fork clone>   N=<新树目录>
cp -R $F/src $F/include $F/apps $N/ ; cp -R $U/bindings $U/pybind11 $U/json $U/CMakeLists.txt $N/
cp $U/apps/small-test.cpp $N/apps/
cd $N && git init -q && git add -A && git commit -qm base
git apply sim_patch/combat_rules.patch
git apply --exclude='bindings/*' sim_patch/sim_rl_hooks.patch
```

## 绑定层改动（fork API 对齐 + 观测扩展）

- `monsterIdStrings` → `monsterIdEnumNames`
- `BattleContext::init(gc, encounter)` → fork 多一个 `allowInvalidEncounter`，绑定用 lambda 传 `false`
- `BattleScumSearcher2::search(sims)` → `search(sims, maxTimeMillis)`，`mcts_recommend` 传 `LONG_MAX`
- `Edge::node` 变成 `shared_ptr`，`e.node.simulationCount` → `e.node->simulationCount`
- 观测 412 → **590**：卡组段从 220（红 75 + 无色 35，×升级）扩成 398（红 75 + 蓝 75 + 无色 35 + 诅咒 14，×升级）；
  未映射卡返回 -1 并跳过（上游把蓝卡和诅咒都撞到 0 号槽）
- 新增 `NNInterface.getCardIdx(card)` / `card_slot_count`，训练脚本用它替代动态词表
- `CardColor` 枚举补 `BLUE`

## 编译（macOS CommandLineTools clang 21 找不到 libc++ 头文件时）

```bash
SDK=$(xcrun --show-sdk-path)
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_SYSROOT="$SDK" \
  -DCMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES="$SDK/usr/include/c++/v1" \
  -DPython_EXECUTABLE=<venv python> ..
ninja slaythespire
```

## 与旧树不可比

fork 的战斗规则更完整、搜索更强：同为 sim2000，铁甲随机非战斗策略 A0 均层 34.2（旧 18.8），
sim10 下 21.8（旧 11.6）；每局 4.8s（旧 0.3s）。旧评估表的所有数字在新树上都要重测。

## 追加（同日）

- fork 的 `ScumSearchAgent2` 每步搜索硬编码 1000ms 上限。已加成员 `searchTimeLimitMillis`（默认仍 1000）并绑定为
  `Agent.search_time_limit_ms`，评测大预算阶梯时传一个大数放开（0 会立即超时，不表示不限）。
- 编译产物：`build312/`（训练在用，勿覆盖）与 `build312_v2/`（含 `search_time_limit_ms`）。训练结束后统一到 v2。
- 评测入口：`eval/hybrid_eval.py --policy learned|random|native --sims 2000,10000,50000 --ckpt <pt>`，
  环境变量 `STS_CLASS / ASC / STS_LIGHTSPEED_BUILD / STS_BOT_DIR / STS_SEARCH_TIME_MS`。
- 参照线（DEFECT A20, 50 eval seed, sim2000, 1000ms 上限）：随机非战斗 15.8 层；模拟器自带启发式 bot 14.3 层。

## 真机桥接对故障机器人的适配（同日晚）

- `BattleContext.from_snapshot`：职业从 `snapshot["class"]` 读（默认铁甲）；新增 `player.orb_slots` / `player.orbs`
  （`[{type, data}]`，type: 0 EMPTY 1 DARK 2 FROST 3 LIGHTNING 4 PLASMA，DARK 的 data = 累积伤害）。
  Python 侧 `Player.orb_slots` / `Player.orbs` 只读属性，`Orb` 枚举导出。
- `steam_mcts.build_snapshot`：加 `class`（CommunicationMod 的 THE_SILENT → SILENT）、`orb_slots`、`orbs`；
  空槽 CommunicationMod 不给 id、只有本地化名字「充能球栏位」，按空槽处理。`assert_snapshot_matches` 校验球。
- 卡牌 id 别名统一到 `steam_mcts.CARD_ALIASES`（Strike_B/Defend_B、Conserve Battery→CHARGE_BATTERY、Gash→CLAW、
  Undo→EQUILIBRIUM、Redo→RECURSION、Steam→STEAM_BARRIER、Lockon→BULLSEYE、Ghostly、J.A.X.、AscendersBane 等），
  `live_model_bridge` 复用它和 `enum_key`。
- `live_model_bridge`：`STS_CLASS / STS_ASCENSION / STS_MODEL` 环境变量控制影子 GameContext、START 命令与权重；
  候选卡描述符用 `AG.card_idx(Card)`；遗物段偏移按 `AG.OBS_DIM - 178` 算；GRID 多选按已选数量递增下标
  （重复 `CHOOSE 0` 会取消选择，曾在鲸鱼「变化 2 张牌」处死锁）。自测里依赖旧铁甲权重的固定判定改为条件断言。
- `run_live_bridge.sh` 用 `set -a` 导出 `.env`。仓库根 `.env` 现指向 `build312_v2`、DEFECT、A20、step8500 权重。
- CommunicationMod `START DEFECT 20 <seed>` 不查解锁进度（源码无 UnlockTracker 引用），存档未解锁故障机器人也能直接开 A20。
- 首局真机（seed JIALEIV，sims 2000，det 1）：MCTS 每步中位 36 ms，整局 4.5 分钟，第 6 层死于地精大块头（它剩 9 血）。
  录制时可把 `STS_MCTS_SIMULATIONS` 提到 5 万（每步约 1 s）。

## 真机 vs 模拟分叉根因 + 并行搜索（9/19 深夜）

- **卡池**：故障机器人 9 张卡（Echo Form / Hyperbeam / Meteor Strike / Core Surge / Rebound / Recycle / Sunder / Turbo / Undo=Equilibrium）
  和 6 件遗物（Cables / DataDisk / Emotion Chip / Runic Capacitor / Turnip / Symbiotic Virus）需解锁进度才进池，
  零解锁存档下第 1 层奖励就与模拟器分叉。解锁名单从 `desktop-1.0.jar` 的 `com/megacrit/cardcrawl/unlock/cards|relics/defect/*.class`
  常量池提取；按 `STSUnlocks` 现有格式（key → "2"）写入并把 `DEFECTUnlockLevel` 设 5 后，第 1 层奖励与模拟器一致。
  修改前备份在 `preferences.bak-<时间>`。
- **并行 MCTS**：绑定新增 `mcts_recommend_parallel(bc, sims, threads)`：threads 份世界副本，RNG 计数器各错开 1000
  （同 fork 的 battle-sim 做法），各搜 sims 次，按推荐动作投票，平票看累计根访问；释放 GIL。`steam_mcts` 读 `STS_MCTS_THREADS`。
  实测 12 线程 × 50k 0.75 s（单线程 50k 0.30 s，同场面）。构建目录 `build312_v3`。
- **面板**：`steam/live_panel.py`（端口 8772）读 runs jsonl，含卡组/遗物/药水（需新版桥接日志）、决策滚动历史（最近 8 次）、
  战斗搜索（中文化动作）、血量轨迹；≤980px 单列，适合放在 1920 宽游戏窗口右侧 640 宽录屏。

## 映射系统性 review（9/19 深夜）

数据源:游戏 jar `localization/eng/{cards,relics,potions,powers,monsters}.json` 的键就是 CommunicationMod 给的 id。
脚本 `steam/validate_mappings.py` 逐条过桥接的映射(别名表 + `enum_key` 规则 + `*_from_name`),报告现役条目的缺口。

本轮补的别名:卡 `Steam Power`→OVERCLOCK(第一次录制就死在这)、其他职业基础卡与改名卡 16 条;
能力 `Heatsink`→HEATSINKS、`Repair`→SELF_REPAIR、`Retain Hand`/`Equilibrium`→EQUILIBRIUM;药水 `Elixir`→ELIXIR_POTION。
仍映射不上的都是已废弃/测试条目或模拟器未实现的机制(如玩家侧 Poison、第四幕 BackAttack)。

桥接两处"界面有状态而桥接无记忆"的死循环已修:GRID 多选按已选数递增下标;跳过过的卡牌奖励不再重新点开(`SKIPPED_CARD_REWARD_FLOORS`)。
选牌界面遇到未知卡 id 现在降级到未映射槽位并记 warning,不再中断整局;战斗快照里的未知卡仍然是致命错误(必须精确)。

## 9/20 凌晨:真机 4 次尝试的教训 + parity 工具

- 尝试 1(6SZQB8, 20k×12):16 层死于守护者。尝试 2/3(7E1N3W):开局 `Steam Power` 未映射中断;第 7 层跳过奖励死循环。
  尝试 4(7E1N3W, 20k×12):史莱姆老大分裂后桥接被杀——根因是 `canonical_monsters` 过滤了 is_gone 的老大导致槽位错位,
  fork 的 `getAliveMonsterIdx` 越界,`sts_asserts` 分支往 stderr 狂刷,CommunicationMod 不读 stderr → 管道写满 → 子进程被判死。
  修:保留 is_gone 怪占位;`sts_common.h` 关掉 `sts_asserts`;桥接 stdout/stderr 全部改道到 `runs/bridge-stderr.log`,
  协议独占一个 dup 出来的 fd;主入口加 crash 兜底日志。
- **怪物状态整体错位一格**:绑定 `monster_status_id_from_name` 的名字表比枚举少一个前缀项,ARTIFACT 查成 0=INVALID,
  RITUAL 查成 MODE_SHIFT。改为从 `include/constants/*StatusEffects.h` 解析 `enum class` 声明顺序建表
  (`steam_mcts._enum_ordinals_from_header`),并用 Python 可见枚举值交叉校验;旧的手写 `MONSTER_POWER_IDS` 废弃。
- **parity 工具** `steam/parity_check.py`:按真机日志在模拟器重放同 seed 的非战斗决策,逐层比对 hp/max_hp/gold/卡组/遗物,
  逐决策比对奖励候选、商店在售与价格、事件选项数;战斗后同步 hp/gold 继续。首轮结论(6SZQB8):
  1. **鲸鱼「失去初始遗物换 boss 遗物」两边不同**:模拟器给 PANDORAS_BOX 且未移除 CRACKED_CORE(起始牌被全部变形),游戏给 Runic Pyramid。
     模型对该选项偏好 100%,因此几乎每局都从第 0 层分叉。待查模拟器 `returnRandomRelic(BOSS, fromFront=true)` 与游戏 boss 池取法差异。
  2. 商店价格:模拟器 = 基价 50/75/150 × U(0.9,1.1),无 A16 涨价;真机 ≈ 模拟 × 1.38。
  3. 配对小游戏(事件 51)模拟器一步解决,真机需翻牌 10 次;卡奖励 RNG 由此分叉的可能性待验证。
- 录制流程:`screencapture -v` 被信号中断不落盘,改用 ffmpeg avfoundation 分片 mp4;设备号每次现查(见下),编码必须用硬件 `h264_videotoolbox`(libx264 veryfast 录 5K 屏会吃到 900%+ CPU,把单线程 MCTS 饿死;videotoolbox + `scale=2560:-2` 只占 ~7%)。窗口布局脚本 `steam/arrange_windows.sh`。

## 9/20 凌晨 parity 第一轮修复

- **遗物池与存档解锁**:游戏会把存档里未解锁的遗物从五个遗物池中剔除。零解锁存档下 boss 池只有 21 个(缺 Pandora's Box,
  它在静默的解锁包里),洗牌结果整体不同,鲸鱼「换 boss 遗物」两边不一致。给 `state_export_mod` 加了 `RunStatePatch`
  (导出 `relic_pools` 五个池 + `run_rngs` 计数器),把静默/观者解锁包里的共享遗物与卡(39 项)写进 `STSUnlocks`
  后,五个池与模拟器 `*_relic_pool` 逐项一致;Neow 取池首(上游实现正确,我中途改成取尾的修改已回退)。
- **起始遗物移除 bug**:`getStarterRelicForClass` 用 `CharacterClass` 下标直接查表,但枚举含 INVALID=0,表却从铁甲起排,
  故障机器人取到观者的纯净之水,「失去初始遗物」对所有职业都不生效。已改为 `cc - 1`。
- **商店 A16**:`Shop::setup` 对 ascension≥16 打八折,游戏是涨价 10%,改为 `applyDiscount(1.10f)`。
- 工具:`steam/parity_check.py`(逐层比对)、`/tmp/sts_research/relic_rng.py`(XS128 + java.util.Random + Collections.shuffle 的 Python 复刻,
  可复现模拟器遗物池洗牌)、`steam/probe/neow_probe.py`(开局到鲸鱼选项 3 并记录遗物池的探针,seed 从 `/tmp/sts_probe_seed` 读)。

## 9/20 凌晨 parity 第二轮:非战斗路径全程对齐

- **配对小游戏(MATCH_AND_KEEP)**:模拟器原来 `disableMatchAndKeep=true` 直接跳过 → 事件流 RNG 与游戏分叉;改为启用,
  但真机桥接翻牌的选牌逻辑与模拟器 agent 不同,故模拟器侧只消耗 RNG、一张不留(`toSelectCards.clear(); eventData=0; regainControl()`),
  真机桥接也不留牌,两边状态一致。
- **宝箱**:fork 的 `ScumSearchAgent2` 在 TREASURE_ROOM 用 `takeAction(gc, takeChest)`,bool 隐式转成 `GameAction(idx1=1)`=跳过,
  所以模拟器 agent 从来没开过箱(训练分布也是"无宝箱");先改成 `GameAction(takeChest?0:1)`,后发现它带诅咒钥匙时只开大箱,
  与桥接"一律开箱"不一致,最终改为一律 `GameAction(0)`。构建 `build312_v4`。
- **偷走的金币**:桥接领奖只领 GOLD/RELIC,`STOLEN_GOLD` 项(盗贼类战斗)从不领 → f21 金币少 26;桥接 `preferred` 加入 STOLEN_GOLD。
- **parity 工具修正**:战斗中药水弹出的选牌界面不算卡牌奖励(`_is_potion_card_screen`);同一购买被桥接记两次视为无操作;
  卡名带 `+` 的升级牌按 multiset 比;重放期间模拟器 hp 恒 999 免死(战斗结果不比,只比非战斗状态),金币在 MAP 处同步。
- **结果**:6SZQB8 两局真机(2000 次单线程)非战斗路径与模拟器逐层一致:房间类型、卡牌奖励候选、商店商品/价格、事件选项、
  金币、卡组、遗物,分别对齐到第 35/39 层(战死为止),仅剩 f21 偷金(桥接已修,下一局生效)。
  逐层 hp 也接近(真机/模拟 f23 6/6,f24 27/27,f29 17/13,f32 32/24),说明战斗侧快照+MCTS 同样忠实,差的是预算。
- 模拟器 v4 A20 该 seed 2000 次即通关(终 hp 57);真机 2000 次两局分别 35/39 层战死 → 03:47 起用 50k×12 正式录制。
- 录屏设备号**每次都要现查**,不要记住任何数字(外接显示器插拔后 `Capture screen 0` 在 2/4 之间漂)。启动前先 `ffmpeg -f avfoundation -list_devices true -i ""` 取 `Capture screen 0` 的序号;
  分片 mp4 在录制中 `ls` 可能显示 0 字节,用 `stat -f %z` 看真实大小。
- **ffmpeg 只能 `kill -INT`,绝不能 `kill -9`**:mp4 的 moov atom 在切片收尾时才写,-9 掉的那一片
  `ffprobe` 直接报 `moov atom not found`,整片作废(9/20 已因此废掉一段 134MB)。用 `-f segment
  -segment_time 600` 把损失上限压到 10 分钟。正在写的当前片查不到时长是正常的,不是坏了。
- **先开录再开游戏**:9/20 正式跑是先起 JVM 再补录像,前 3m58s(约 1~9 层)没录上。顺序应为
  ffmpeg → 面板 → `arrange_windows.sh` → 启动游戏。

## 9/20 上午 根并行 MCTS 在精确 RNG 下是负优化

- **现象**:50000 次 ×12 线程那局比 2000 次 ×1 线程死得更早(f22 vs f35/f39),且进 f22 时血更多。预算涨 25 倍、结果更差,
  说明问题不在预算而在搜索本身。
- **根因**:`SteamStateExport` 导出真机六路战斗 RNG(`ai/card_random/misc/monster_hp/potion/shuffle`)的精确计数器,
  `BattleContext.from_snapshot` 逐字段断言还原一致(全程 0 次 fallback 证明断言一直通过)——**还原出来的战斗是完全信息博弈**,
  接下来抽什么牌、怪物掷什么骰子都是已知量。而 `mcts_recommend_parallel` 为了做 determinization,
  把 1..N-1 号线程的五路 RNG 计数器错开 `i*1000`,让它们去搜索**永远不会发生的未来**,再多数投票:11 张假票淹没 1 张真票。
  determinization 只在真拿不到精确 RNG(不完全信息)时才成立。
- **修复**:`steam/steam_mcts.py` 的 `recommend()` 增加 `exact_rng = bool(snapshot.get("rngs"))`,
  只要快照带 RNG 计数器就强制单世界单线程;返回值记录 `exact_rng`/`threads` 便于事后核查。
- **搜索的两条提前出口**(读 `BattleScumSearcher2::search` 确认,绑定只设 `allowPotions`,`allowRootPotions` 仍为 false):
  `immediate_lethal`(存在一击终结且不损失续航资源的牌 → 零模拟直接返回)与 `converged_forced_win`(根节点收敛为必胜 → 提前退出)。
  两者都只在**赢**的局面触发,所以打不过的局面必定烧满预算,不影响上面的诊断。实测满预算 50000 次约 1.5–2 秒。
- **新工具**:
  - `steam/replay_snapshot.py` —— 从日志取某一手战斗快照,在模拟器里用任意预算把这场仗打完。
    用于区分死因:同一快照加大预算能赢 = 搜索质量问题;任何预算都输 = 局面在更早就已经输了。
  - `steam/combat_parity.py` —— 逐场把"模拟器从该场第一手快照独自打完"的剩余血量与真机实际血量对比,
    相等即证明这一场**逐动作完全一致**。非战斗 parity 只能证明地图/掉落一致,这个才证明战斗本身一致。
  - `steam/live_vs_archive.py` —— 对局进行中按层与归档轨迹对照(gold/deck 判非战斗路径,hp 仅参考)。
- **注意**:上述脚本依赖 `build312_v4` 的 `slaythespire.cpython-312-darwin.so`,必须用
  `${USER_HOME}/lab/physical-world-ai/.venv/bin/python`(3.12)跑,系统 `python3` 是 3.14 会 ModuleNotFoundError。
- **ModTheSpire 启动卡死**:workshop 三个 mod 是"已安装未订阅",MTS 子进程 `SteamWorkshop` 在 `getNumSubscribedItems()==0` 时
  用空列表建 UGC query,回调永不触发,`while(!kill)` 死循环,MTS 阻塞在 `waitFor()` 永远起不来游戏(`sample` 可见主线程停在
  `SteamAPI_RunCallbacks`)。MTS 无 `--skip-workshop` 选项。解法:把 `SteamWorkshop` 换成直接退出的桩,
  打进私有副本 `~/lab/sts-rl-agent/mts/ModTheSpire.jar`(原件备份 `.orig`,Steam 自身文件未动),用它启动:
  `cd <游戏>/Contents/Resources && jre/bin/java -jar ~/lab/sts-rl-agent/mts/ModTheSpire.jar --skip-launcher --mods basemod,CommunicationMod,steamstateexport`
  不选重新订阅,是因为会拉到更新版 mod,破坏 parity 的可复现性。
- **窗口脚本**:MTS 起的游戏窗口名是 `Modded Slay the Spire`,不是 `Slay the Spire`;`arrange_windows.sh` 原来匹配不到,静默空跑。
- **锁屏会废掉录像**:游戏逻辑走 stdout 不依赖显示,锁屏后对局照常推进,但 ffmpeg 录的是登录界面。
  `caffeinate` 只防睡眠不防手动锁屏。种子固定 + 单线程精确 RNG = 整局可确定性复现,录废了重录即可,不必抢救。

## 9/20 上午 转移级 parity:找到 f28 暴毙真因(Monster::miscInfo 全是 0)

### 死因

9/20 那局在 f28(扎人的书 Book of Stabbing,170 血精英)以 23 血 0 格挡结束回合被打死。
根因不在搜索质量,在状态还原:

模拟器把每只怪的「隐藏暂存状态」统一塞在 `Monster::miscInfo` 一个 int 里,语义按怪而异 ——
扎人的书 = 刺击段数、守护者 = 模式转换阈值、虱子 = 出生时摇到的咬伤伤害、
黑暗爬虫 = 啃咬伤害、地精法师 = 蓄力计数。真实游戏里没有这个统一字段,
它们是各怪类自己的 private 成员,**CommunicationMod 一个都不导出**。
`steam_mcts.monster_snapshot()` 以前一律填 0,于是:

```
MULTI_STAB -> attackPlayerHelper(bc, 7, miscInfo)   // 第 3 个参数是段数
miscInfo = 0  =>  7 伤害 x 0 段 = 0
```

模拟器预测这只精英本回合打 **0 点**伤害,决策端据此认为不需要格挡。

离线注入实验把 f28 每一步的 `misc_info` 从 0 扫到 5:每 +1 段正好多 7 点伤害
(44→37→30→23→16→9),在 **stabs=3** 时 `f28 回合2 end turn` 逐字段**完全一致**。
死因就此坐实,不是猜测。

反过来看模拟器自己的 roll 逻辑是对的:`MonsterSpecific.cpp:2268` 的 `getMoveForRoll`
用 `auto &stabCount = monsterData;` 起了别名(所以 grep `++miscInfo` 搜不到它),
MULTI_STAB 与 asc18 的 SINGLE_STAB 都会 `++stabCount`,和真实 `BookOfStabbing.getMove` 一致;
prebattle 初值也都是 1。**唯一的缺陷是快照还原。**

### 修法:反射真值,而不是从意图数据反推

在自己的 `SteamStateExport` mod 里按类名反射出真实私有字段补进 monster 快照,
一次性关掉整类问题,而不是一只怪一只怪打补丁:

| 类名 | 反射字段 | 对应 miscInfo 语义 |
| --- | --- | --- |
| BookOfStabbing | stabCount | 刺击段数 |
| TheGuardian | dmgThreshold | 模式转换阈值 |
| LouseNormal / LouseDefensive | biteDamage | 出生摇到的咬伤 |
| Darkling | nipDmg | 啃咬伤害 |
| GremlinWizard | currentCharge | 蓄力计数 |

顺带补导:
- `is_open`(乐加维林的沉睡在真机是 `AbstractMonster.isOpen` 布尔字段,**不是 Power**,
  powers 列表里查不到;只靠 moveHistory 反推会在「刚被打醒、还没轮到它行动」这一帧判错)
- `move_history`(CommunicationMod 只给 last / second_last 两步)
- `next_move_id` / `move_base_damage` / `move_multiplier` / `move_is_multi`(来自私有 `EnemyMoveInfo`)
- `export_name`(反射是按下标贴回 CommunicationMod 槽位的,python 侧据此断言没错位 ——
  错位会把甲怪的隐藏状态静默安到乙怪头上,宁可炸)

踩过的坑:第一版写的是 `out.put("move_id", ...)`,直接覆盖 CommunicationMod 自己的键。
`steam_mcts.move_id()` 对 `MOVE_NAMES` 查不到的值会抛 `ValueError` 中断整局 ——
给一个原本不存在的键填值就会触发。改成独立键名 `next_move_id`。

### 顺带修的两处

- **Streamline 永久降费**:CommunicationMod 的 `cost` 导出的是 `costForTurn`,
  永久费用 `AbstractCard.cost` 没导。「本场战斗永久 -1 费」这类牌回合结束
  `setCostForTurn(cost)` 会被弹回基础费用。mod 增导 `card_base_costs`(uuid -> cost),
  绑定层 `snapshotCard()` 据此还原 base cost。
  f16 连续 5 步的 `仿真缺 [STREAMLINE(1)] 多 [STREAMLINE(2)]` 就是它。
- **乐加维林 ASLEEP**:绑定层优先吃 mod 的 `is_open` 真值,缺失时才退回按 moveHistory 反推。
  不还原这个标记,沉睡的乐加维林既不会被伤害唤醒(8 层 Metallicize 不掉),
  也会在第一次 end turn 就跳到 ATTACK。

### 已知偏差(记录,暂不修)

**药水的 `card_random` 计数**:真机 Attack Potion 烧 54 次 roll、Colorless Potion 50 次,
模拟器只烧 3 次。真机 `returnTrulyRandomCardInCombat(CardType)` 是在
common/uncommon/rare 三个牌池上做**拒绝采样**(while 循环摇到类型匹配为止)。
精确复现要对齐三个牌池的确切顺序,成本很高;而且实战里每一帧都从真机重新还原 RNG,
偏差不会累积,只会让 MCTS 前瞻里对药水的估值有偏。

**地精首领召唤的槽位顺序**:反编译 `SummonGremlinAction` 后,真机机制是

1. `identifySlot(gremlins[])` 从**下标 0 往上**扫,返回第一个 null 或 isDying 的槽位;
   构造函数里立刻把新怪写回该槽,所以第二个 `SummonGremlinAction` 会拿到更大的下标。
2. 槽位下标只决定**屏幕坐标** `POSX = [-366, -170, -532]`(即 2 最左、0 居中、1 最右)。
3. `update()` 用 `getSmartPosition()` 把新怪插进怪物组 —— 位置 = 组里 drawX 比它小的怪的个数,
   也就是**怪物组始终按屏幕 X 从左到右排序**。
4. `usePreBattleAction` 里 `gremlins[0]=组[0]`、`gremlins[1]=组[1]`、`gremlins[2]=null`。

模拟器 `Actions::SummonGremlins()` 则是直接在怪物组数组上按 1、2、0 找空位并原地复用。
两套模型不同,f24 回合2 表现为 `真机=[14, X, 4]` / `仿真=[4, X, 14]`(同一个多重集,顺序相反)。
要对齐得在模拟器里单独建 `gremlins[]` 数组 + drawX 排序,为一个精英战做结构改动;
而实战每帧都从真机重新还原怪物顺序,偏差只作用于 MCTS 前瞻的选目标。**暂记不修。**

### 工具侧改进(让下次不用重开游戏才能验证假设)

- `live_model_bridge.py` 落盘 `game_state_raw`(原始 CommunicationMod 态,剔掉 map)
  与 `combat_counters`。以前只落 `mcts_snapshot`(已经建好的仿真态),
  任何「某字段该怎么还原」的假设都没法离线验证,只能重开一局现采数据 ——
  找 miscInfo 时就是这么卡住的。
- `transition_parity.py` 优先用 `game_state_raw` 现建快照(老日志自动回退 `mcts_snapshot`),
  并把 `STS_LIGHTSPEED_BUILD` 对齐到 `STS_SIM_BUILD`,避免「以为在测新 build、实际先 import 了旧的」。
- 牌堆差异从「内容不同」改成打具体差异:
  `仿真缺 [STREAMLINE(1)] 多 [STREAMLINE(2)]` / `顺序不同(首个差异位 1: ...)`。

### 当前状态(旧日志 learned-seed-357136893,跑不出新字段,仅作回归基线)

`可校验转移 205 步 | 一致 193 | 不一致 12`

| 处 | 现象 | 状态 |
| --- | --- | --- |
| f16 x5 | STREAMLINE 费用 1 vs 2 | 已修,待新局验证 |
| f28 回合1 | BALL_LIGHTNING 费用 1 vs 0 | 疑同一类,待新局验证 |
| f28 回合2 | hp 23 vs 44 | 已修(注入实验已证),待新局验证 |
| f28 回合1 | 药水 card_random 54 vs 3 | 已知偏差,不修 |
| f24 回合2 | 地精召唤槽位顺序 | 已知偏差,不修 |
| f18 回合4 | draw_pile 顺序不同(ZAP vs DAZED) | 未查 |

## 9/20 中午 parity 第三轮:球的出手顺序 + 地精首领槽位

上一轮修完 `miscInfo` 之后重跑 `calib-v5-seed-357136893`,转移级 parity 收到
`165 步 | 一致 163 | 不一致 2`。剩下两处的根因都不在数值上,而在**顺序/身份**这两类
最容易被"看起来差不多"糊弄过去的地方。

### 1. 球的引爆走 addToTop,不是 addToBot

真机 `AbstractOrb` 的四个子类 (`Lightning` / `Frost` / `Dark` / `Plasma`) 的
`onEvoke()` 一律用 `addToTop`,而 `onEndOfTurn()` 的被动一律用 `addToBottom`。
模拟器 `Player::triggerOrbEvoke` 原本用 `addToBot`,于是"一次性通道多颗球"时
引爆效果和后续通道动作的交错顺序和真机相反。

两处修改:

- `Player::triggerOrbEvoke` 改 `addToBot` -> `addToTop`。
- `Actions::ChannelOrb` 重写成递归形式:`addToTop(ChannelOrb(orb, count-1))`,
  让"通道 N 颗"退化成 N 次单颗通道,每次通道挤爆一颗时引爆动作能正确插到队首。
  原来的 for 循环会把 N 次通道压成一个原子动作,中间挤出的引爆全部堆到队尾。

### 2. 地精首领的召唤槽位(本轮主菜)

现象:f24 回合 2 结束时,真机和仿真的 monster0 / monster2 互换
(真机 `(14,0)/(1,0)`,仿真 `(1,0)/(14,0)`)。

把 `GremlinLeader` / `SummonGremlinAction` / `MonsterGroup` / `MonsterHelper`
反编译出来之后,真机的机制是这样的:

- `GremlinLeader` 自带 `AbstractMonster[] gremlins`(3 个槽),
  `usePreBattleAction` 把开局两只填进 `gremlins[0]`、`gremlins[1]`,`[2]` 留 null。
- 三个槽的屏幕横坐标 `POSX = {-366, -170, -532}` ——
  **slot2 的 x 最小,也就是在最左边**,槽号和屏幕顺序不是一回事。
- RALLY 会 `addToBottom` 两个 `SummonGremlinAction(this.gremlins)`。
  每个 action 在构造时取 `identifySlot(arr)` = 第一个 `null`/`isDying` 的下标,
  并且**立刻把新怪写回 `arr[slot]`**,所以第二个 action 必然选到另一个槽。
- 新怪的 x 取 `POSX[slot]`,入列时走 `monsters.add(getSmartPosition(), m)`,
  而 `getSmartPosition()` = 现有怪里 drawX 严格更小的个数 —— 纯粹按屏幕从左到右排。
  死怪从不移除,所以 `monsters` 只会越来越长(3 -> 5 -> 7)。

用这套模型回放录到的两次 RALLY,逐位命中:

- 回合 3:`gremlins = [初始A(dying), 初始B(alive), null]` -> action1 取 slot0(x=-366)
  插到 index0;action2 取 slot2(x=-532,更左)插到 index0 并把前者顶到 index1。
  最终 `[slot2怪, slot0怪, 初始A(死), 初始B, 首领]`,与真机导出一致。
- 回合 7:小怪全死 -> action1 取 slot0 落 index1;action2 取 slot1(x=-170)落 index4。
  最终 `[卑鄙死, 持盾新, 胖死, 胖死, 卑鄙新, 持盾死, 首领]`,与真机导出一致。

**根因不在 C++。** `Actions::SummonGremlins()` 注释写的搜索顺序 `1, 2, 0` 正好复刻
`identifySlot` 扫 `gremlins[0..2]` —— 因为模拟器定长 `arr[0..2]` 是按**屏幕序**排的,
对应槽号 `[2, 0, 1]`;`MonsterGroup.cpp` 建组时也确实把开局两只放进 `arr[1]`、`arr[2]`,
`arr[0]`(= slot2,最左)留空。两边本来就是对齐的。

真正的 bug 在 `steam_mcts.canonical_monsters()`:它把**存活**小怪依次压到数组前部
(`minions[:3]`),槽位语义当场丢失。f24 回合 2 末只剩一只持盾地精(占 slot1),被压到
`arr[0]`,仿真于是认为 `arr[1]`/`arr[2]` 空着并往那儿召唤,而真机用的是 slot2 ——
两只小怪下标互换。后果不只是显示错位:随机闪电选靶、玩家指定目标全都会打到错的怪。

修法选的是**把槽位当语义真值导出**,而不是从 `drawX` 相对顺序反推(活怪不足 3 只时
x 值不够区分槽位)、也不是重放召唤历史(parity 每帧无状态重建,历史不可得):

- mod 侧 `CombatStatePatch.collectGremlinSlots()` 反射 `leader.gremlins`,
  给每只小地精打上 `gremlin_slot`;顺带导出 `draw_x` 便于交叉验证。
- python 侧 `steam_mcts.GREMLIN_SLOT_TO_ARR = {2:0, 0:1, 1:2}`,
  `canonical_monsters` 的地精首领分支按槽位落位,空槽用 `dead_monster()` 补,
  `target_map` 同步对齐;老日志没有该字段时退回旧的 `[:3]` 行为(不会越界崩)。
- `transition_parity.canonical_sim_monsters` 同步改成直接读仿真 `arr[0..2]`,
  死槽补占位,保证两边在同一坐标系下比较。

### 3. f12 无色药水:有界偏差,不修

`ColorlessPotion.use` -> `DiscoveryAction(true, 1)` 的三选一。候选生成两边都恰好
消耗 3 次 `cardRandomRng`,但真机在 CHOOSE 之后**多走了 47 次**且没有任何可观测的
状态变化,`DiscoveryAction.update()` 的选后分支里也读不到 RNG 调用,源头未定位。

影响是有界的:`live_model_bridge.build_live_choices` 把战斗中的 `CARD_REWARD`
交给学习到的策略网络处理,不走模拟器 MCTS;且实时桥每步都从真机重新同步 RNG 状态,
不会把动作翻译错。整局只有这一瓶药水。先记录,不阻塞。
