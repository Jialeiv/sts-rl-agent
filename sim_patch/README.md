# 固定模拟器版本的战士战斗规则修复

对应 [issue #1](https://github.com/Jialeiv/sts-rl-agent/issues/1)。本补丁覆盖战士能够触发的药水、铁斩波和耗尽牌堆后的战斗结算，不增加角色支持。

## 应用补丁

在 `sts_lightspeed` 的 `7476a81954020087da31d41d16fddf475746ec2d` 提交上执行：

```bash
git apply /path/to/sts-rl-agent/sim_patch/sim_rl_hooks.patch
git apply /path/to/sts-rl-agent/sim_patch/combat_rules.patch
```

第一份补丁提供本项目的接口和暂停功能；第二份修改战斗规则。应用后需要重新编译模拟器及 Python 模块，避免继续加载旧模块。

## 改动与验证条件

- **血液药水**：普通效果回复最大血量的 20%，神圣树皮加成为 40%。回归覆盖普通效果、树皮、整数截断和血量上限。
- **铁斩波**：格挡修正计算一次，覆盖敏捷、负敏捷、升级和脆弱，检查攻击伤害没有改变。该项与上游 [gamerpuppy/sts_lightspeed#9](https://github.com/gamerpuppy/sts_lightspeed/pull/9) 的已有修复一致。
- **无牌状态**：删除提前判负的有限条件列表。战士使用恶魔之焰消耗剩余手牌后，可以使用火焰药水，或使用毒药水后结束回合获胜；没有后续伤害来源时，由敌人的实际攻击结算败北。
- **毒药水结算**：原 `PoisonLoseHpAction` 返回空动作，敌人回合开始时执行会抛出异常。补全有目标的毒伤动作，保留入队时的目标与毒伤量，绕过格挡，复用无格挡伤害与死亡处理，并减少一层毒。回归覆盖击杀先于敌人攻击、格挡、无实体、毒层数归零、人工制品及多敌人隔离。

测试从战士对邪教徒的状态构造合成场景，通过模拟器出牌、药水和结束回合入口执行。原版预期根据本地原版 Java 规则核对；这些用例不构成完整的原版双引擎回放验证。

## 原生回归命令

不依赖 Python 绑定、PyTorch 或模型权重。需要 C++17 编译器、CMake，以及模拟器的 `json` 子模块。运行目录可以是模拟器根目录：

```bash
cmake -S /path/to/sts-rl-agent/sim_patch/tests -B build-combat-tests \
  -DSTS_SIM_ROOT="$PWD" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-combat-tests -j 4
ctest --test-dir build-combat-tests --output-on-failure
```

若使用已有的 nlohmann/json 头文件目录，可以在配置时设置 `-DSTS_JSON_INCLUDE=/path/to/json/single_include`。

测试使用显式检查与非零退出码，发布构建关闭 `assert` 不会关闭测试判定。每个测试由独立进程执行，超时为 10 秒。要复现修复前失败，在只应用 `sim_rl_hooks.patch` 的同一版本上运行同一套测试。

## 结果边界

移除无牌提前判负后，一些没有获胜路线的搜索分支可能需要模拟更多回合。上游现有的 500 回合保护及其他搜索限制不在本次修改范围；本补丁不提供搜索耗时或全局最优性的保证。

上游将敌人毒层数保存在有符号 8 位整数中；累加到 150 层会读出 -106。这一高层数存储问题不在本补丁范围，毒药水回归覆盖的层数没有跨过该边界。

项目现有评估表来自该规则补丁之前，未用修复后的模拟器重新评估；历史数字保留其原有版本含义。
