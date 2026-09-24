#!/usr/bin/env python3
"""从《杀戮尖塔》游戏字节码生成 getEnergyCost 的费用表。

背景：include/constants/Cards.h 里的 getEnergyCost 原本是一张手抄的 switch，
只显式列了 144 张牌，其余 227 张全部落到 `default: return 1`。抄漏一张不会报错，
只会让这张牌的基础费用静默变成 1 —— 战斗中回合结束时 setCostForTurn(cost) 会把
costForTurn 拉回这个错值，于是"仿真和真机对不上"以一种极难定位的方式出现
（f39 蒸汽护壁 cost 真机 0 / 仿真 1 就是这么来的）。

这里改成用真机 class 字节码当 oracle 生成整张表：
  - 基础费用 = AbstractCard.<init> 的第 4 个参数（int cost）
  - 升级费用 = upgrade() 里 upgradeBaseCost(n) 的 n；没有这个调用则不变
  - 诅咒/状态牌：真机统一是 -2，仿真沿用自己的哨兵（诅咒 -3、状态 -2），保持不变
  - X 费牌：两边都是 -1，一致

用法（需要先解出 class 文件）：
  JAR=".../desktop-1.0.jar"
  unzip -q -o "$JAR" 'com/megacrit/cardcrawl/cards/*' -d /tmp/cards
  cd /tmp/cards && javap -p -c -classpath . $(...) > /tmp/allcards.txt
  python3 tools/gen_card_costs.py /tmp/allcards.txt include/constants/Cards.h
"""
import re
import sys

PUSH = re.compile(r"^\s*\d+:\s+(iconst_(?:m1|\d)|bipush\s+(-?\d+)|sipush\s+(-?\d+))\s*$")
LDC = re.compile(r"ldc\s+#\d+\s+// String (.*)$")
INIT = re.compile(
    r'invokespecial\s+#\d+\s+// Method com/megacrit/cardcrawl/cards/AbstractCard\.'
    r'"<init>":\(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I'
)
CURSE_COST = -3      # 仿真对诅咒的哨兵值
STATUS_COST = -2     # 仿真对状态牌的哨兵值

# upgrade() 里出现多次 upgradeBaseCost 的牌:升级费用取决于运行时 cost,静态推不出来。
# 全游戏只有 Blood for Blood 一张:`if (cost < 4) upgradeBaseCost(cost-1) else upgradeBaseCost(3)`,
# 未被降过费的牌 cost==4 走 else 分支,所以静态表就是 3。下面的 assert 保证真机再出现
# 第二张这种牌时生成器直接报错,而不是静默退回基础费用。
BRANCHED_UPGRADE = {"com.megacrit.cardcrawl.cards.red.BloodForBlood": 3}


def push_value(line):
    m = PUSH.match(line)
    if not m:
        return None
    tok = m.group(1)
    if tok.startswith("iconst_"):
        return -1 if tok == "iconst_m1" else int(tok[-1])
    return int(m.group(2) or m.group(3))


def parse_real_costs(javap_path):
    """-> {card_id_string: (base, upgraded, java_class)}"""
    text = open(javap_path).read()
    out = {}
    for block in re.split(r"(?m)^Compiled from ", text)[1:]:
        m = re.search(r"class (\S+)", block)
        cls = m.group(1) if m else "?"
        if ".deprecated." in cls or ".optionCards." in cls:
            continue
        mi = INIT.search(block)
        if not mi:
            continue
        head = block[: mi.start()].splitlines()

        # 基础费用：构造参数里 cost 紧挨在 DESCRIPTION 那两条指令之前
        anchor = max(
            (i for i, l in enumerate(head)
             if "CardStrings.DESCRIPTION" in l or "CardStrings.UPGRADE_DESCRIPTION" in l),
            default=None,
        )
        base = None
        if anchor is not None:
            for j in range(anchor - 1, max(-1, anchor - 6), -1):
                v = push_value(head[j])
                if v is not None:
                    base = v
                    break
        if base is None:
            continue

        # 卡 ID 字符串：构造器里第一个 ldc String，否则 static 初始化区里的
        cid = next((LDC.search(l).group(1) for l in head if LDC.search(l)), None)
        if cid is None:
            sm = re.search(r"static \{\};(.*?)(?=\n\n|\Z)", block, re.S)
            if sm:
                cid = next((LDC.search(l).group(1) for l in sm.group(1).splitlines()
                            if LDC.search(l)), None)
        if cid is None:
            continue

        # 升级费用：只认 upgrade() 里唯一一次 upgradeBaseCost(n)。出现多次说明有
        # 分支（如 Blood for Blood），启发式不可靠，宁可保持基础值也不猜。
        up = base
        um = re.search(r"public void upgrade\(\);(.*?)(?=\n\n  \S|\Z)", block, re.S)
        if um:
            ul = um.group(1).splitlines()
            calls = [i for i, l in enumerate(ul) if "upgradeBaseCost:(I)V" in l]
            if len(calls) > 1:
                assert cls in BRANCHED_UPGRADE, f"未登记的分支式升级费用:{cls}"
                up = BRANCHED_UPGRADE[cls]
            elif len(calls) == 1:
                for j in range(calls[0] - 1, max(-1, calls[0] - 4), -1):
                    v = push_value(ul[j])
                    if v is not None:
                        up = v
                        break

        if ".curses." in cls and base == -2:
            base = up = CURSE_COST
        elif ".status." in cls and base == -2:
            base = up = STATUS_COST
        out[cid] = (base, up, cls)
    return out


def parse_sim(cards_h):
    src = open(cards_h).read().splitlines()
    names = re.findall(r'"([A-Z_]+)"', src[
        next(i for i, l in enumerate(src) if "cardEnumStrings" in l)])
    start = next(i for i, l in enumerate(src) if "static constexpr int getEnergyCost" in l)
    depth = 0
    end = None
    for i in range(start, len(src)):
        depth += src[i].count("{") - src[i].count("}")
        if depth == 0 and i > start:
            end = i
            break
    old, pending = {}, []
    for l in src[start:end + 1]:
        m = re.search(r"case CardId::(\w+):", l)
        if m:
            pending.append(m.group(1))
            continue
        m = re.search(r"return\s+(.+?);", l)
        if m and pending:
            e = m.group(1).strip()
            mu = re.match(r"upgraded\s*\?\s*(-?\d+)\s*:\s*(-?\d+)$", e)
            if mu:
                val = (int(mu.group(2)), int(mu.group(1)))
            else:
                try:
                    val = (int(e), int(e))
                except ValueError:
                    val = None
            if val:
                for c in pending:
                    old[c] = val
            pending = []
    return names, old, src, start, end


def main():
    javap_path, cards_h = sys.argv[1], sys.argv[2]
    real = parse_real_costs(javap_path)
    names, old, src, start, end = parse_sim(cards_h)

    mapping = {}
    mappings_h = cards_h.replace("Cards.h", "SaveFileMappings.h")
    for l in open(mappings_h):
        m = re.search(r"\{\s*CardId::(\w+)\s*,\s*\"([^\"]+)\"", l)
        if m:
            mapping.setdefault(m.group(1), m.group(2))

    rows, changed, missing = [], [], []
    for name in names:
        cur = old.get(name, (1, 1))
        cid = mapping.get(name)
        got = real.get(cid) if cid else None
        if got is None:
            missing.append(name)
            rows.append((name, cur, "沿用旧值(真机无对应类)"))
            continue
        new = (got[0], got[1])
        rows.append((name, new, ""))
        if new != cur:
            changed.append((name, cid, cur, new, got[2].split(".")[-2]))

    body = ["    // 由 tools/gen_card_costs.py 从真机 class 字节码生成，勿手改。",
            "    // 索引即 CardId 序号；[0]=基础费用，[1]=升级后费用。",
            "    // 负值沿用仿真自己的哨兵：-1=X 费，-2=状态牌，-3=诅咒。",
            "    static constexpr std::int8_t cardEnergyCosts[][2] = {"]
    for name, val, note in rows:
        c = f"        {{{val[0]:>2}, {val[1]:>2}}},"
        body.append(f"{c:<24}// {name}{(' — ' + note) if note else ''}")
    body += ["    };", "",
             "    static constexpr int getEnergyCost(CardId id, bool upgraded) {",
             "        return cardEnergyCosts[static_cast<int>(id)][upgraded ? 1 : 0];",
             "    }"]

    out = src[:start] + body + src[end + 1:]
    open(cards_h, "w").write("\n".join(out) + "\n")

    print(f"写入 {len(rows)} 条；真机无对应类 {len(missing)}：{missing}")
    print(f"=== 与旧表不同 {len(changed)} 条 ===")
    for c in sorted(changed, key=lambda x: (x[4], x[0])):
        print(f"  {c[4]:<10} {c[0]:<22} {c[2]} -> {c[3]}")


if __name__ == "__main__":
    main()
