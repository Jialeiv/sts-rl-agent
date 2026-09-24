#include <thread>
#include <limits>
//
// Created by keega on 9/16/2021.
//

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>
#include <pybind11/functional.h>

#include <sstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "sim/ConsoleSimulator.h"
#include "sim/search/ScumSearchAgent2.h"
#include "sim/search/GameAction.h"
#include "sim/search/BattleScumSearcher2.h"
#include "sim/search/ExpertKnowledge.h"
#include "game/Shop.h"
#include "sim/SimHelpers.h"
#include "sim/PrintHelpers.h"
#include "game/Game.h"

#include "combat/BattleContext.h"
#include "combat/Player.h"
#include "combat/Monster.h"
#include "combat/CardInstance.h"
#include "combat/InputState.h"
#include "sim/search/Action.h"
#include "constants/MonsterMoves.h"
#include "constants/PlayerStatusEffects.h"
#include "constants/MonsterStatusEffects.h"

#include "slaythespire.h"


using namespace sts;

namespace {
    namespace py = pybind11;

    std::string normalizedName(const std::string &value) {
        std::string out;
        for (const unsigned char ch : value) {
            if (std::isalnum(ch)) out.push_back(static_cast<char>(std::tolower(ch)));
        }
        return out;
    }

    template <typename T, std::size_t N>
    int enumFromNames(const std::string &name, const T (&names)[N]) {
        const auto wanted = normalizedName(name);
        for (std::size_t i = 0; i < N; ++i) {
            if (normalizedName(names[i]) == wanted) return static_cast<int>(i);
        }
        throw std::invalid_argument("unknown enum name: " + name);
    }

    CardInstance snapshotCard(const py::dict &src, int uniqueId) {
        const auto id = static_cast<CardId>(py::cast<int>(src["id"]));
        if (id == CardId::INVALID) throw std::invalid_argument("snapshot card id is INVALID");
        const int upgrades = src.contains("upgrades") ? py::cast<int>(src["upgrades"]) : 0;
        CardInstance card(id, upgrades > 0);
        card.uniqueId = static_cast<std::int16_t>(uniqueId);
        // specialData 的语义按牌而异,必须和 BattleConverter::convertCard 保持一致,
        // 否则同一份真机状态走两条还原路径会得到两种结果。
        //   灼热打击 = 升级次数;爪 = 已累加的伤害;蒸汽护壁 = 本场已出次数;其余 = misc。
        // base_block / base_damage 由 SteamStateExport 按 uuid 补导;老日志没有这两个
        // 字段,此时退回默认值(按"没出过"还原),不抛异常,保证旧日志仍可回放。
        if (src.contains("misc")) card.specialData = static_cast<std::int16_t>(py::cast<int>(src["misc"]));
        if (id == CardId::SEARING_BLOW) {
            card.specialData = static_cast<std::int16_t>(upgrades);
        } else if (id == CardId::CLAW && src.contains("base_damage")) {
            const int initialDamage = card.upgraded ? 5 : 3;
            card.specialData = static_cast<std::int16_t>(
                std::max(0, py::cast<int>(src["base_damage"]) - initialDamage));
        } else if (id == CardId::STEAM_BARRIER && src.contains("base_block")) {
            const int initialBlock = card.upgraded ? 8 : 6;
            card.specialData = static_cast<std::int16_t>(
                std::max(0, initialBlock - py::cast<int>(src["base_block"])));
        }
        // CommunicationMod 导出的 "cost" 是 AbstractCard.costForTurn(当回合费用),永久费用
        // AbstractCard.cost 由 SteamStateExport 以 "base_cost" 补导。不还原永久费用的话,
        // Streamline 这类"本场战斗永久降费"的牌会在回合结束 setCostForTurn(cost) 时弹回基础费用。
        if (src.contains("base_cost")) {
            const int baseCost = py::cast<int>(src["base_cost"]);
            if (baseCost >= 0) card.cost = static_cast<std::int8_t>(baseCost);
        }
        if (src.contains("cost")) card.costForTurn = static_cast<std::int8_t>(py::cast<int>(src["cost"]));
        return card;
    }

    void setPlayerStatus(Player &player, PlayerStatus status, int amount, bool justApplied) {
        switch (status) {
            case PlayerStatus::ARTIFACT: player.artifact = amount; break;
            case PlayerStatus::DEXTERITY: player.dexterity = amount; break;
            case PlayerStatus::FOCUS: player.focus = amount; break;
            case PlayerStatus::STRENGTH: player.strength = amount; break;
            default: {
                const int idx = static_cast<int>(status);
                if (idx <= 0 || idx >= static_cast<int>(PlayerStatus::THE_BOMB) + 1) {
                    throw std::invalid_argument("invalid player status id");
                }
                if (idx < 64) player.statusBits0 |= 1ULL << idx;
                else player.statusBits1 |= 1ULL << (idx - 64);
                player.statusMap[status] = static_cast<std::int16_t>(amount);
            }
        }
        if (justApplied) player.justAppliedBits |= 1ULL << static_cast<int>(status);
    }

    void setMonsterStatus(Monster &monster, MonsterStatus status, int amount) {
        const int idx = static_cast<int>(status);
        if (idx < 0 || status == MonsterStatus::INVALID) throw std::invalid_argument("invalid monster status id");
        monster.statusBits |= 1ULL << idx;
        switch (status) {
            case MonsterStatus::ARTIFACT: monster.artifact = amount; break;
            case MonsterStatus::BLOCK_RETURN: monster.blockReturn = amount; break;
            case MonsterStatus::CHOKED: monster.choked = amount; break;
            case MonsterStatus::CORPSE_EXPLOSION: monster.corpseExplosion = amount; break;
            case MonsterStatus::LOCK_ON: monster.lockOn = amount; break;
            case MonsterStatus::MARK: monster.mark = amount; break;
            case MonsterStatus::METALLICIZE: monster.metallicize = amount; break;
            case MonsterStatus::PLATED_ARMOR: monster.platedArmor = amount; break;
            case MonsterStatus::POISON: monster.poison = amount; break;
            case MonsterStatus::REGEN: monster.regen = amount; break;
            case MonsterStatus::SHACKLED: monster.shackled = amount; break;
            case MonsterStatus::STRENGTH: monster.strength = amount; break;
            case MonsterStatus::VULNERABLE: monster.vulnerable = amount; break;
            case MonsterStatus::WEAK: monster.weak = amount; break;
            case MonsterStatus::ANGRY:
            case MonsterStatus::BEAT_OF_DEATH:
            case MonsterStatus::CURIOSITY:
            case MonsterStatus::CURL_UP:
            case MonsterStatus::ENRAGE:
            case MonsterStatus::FADING:
            case MonsterStatus::FLIGHT:
            case MonsterStatus::GENERIC_STRENGTH_UP:
            case MonsterStatus::INTANGIBLE:
            case MonsterStatus::MALLEABLE:
            case MonsterStatus::MODE_SHIFT:
            case MonsterStatus::RITUAL:
            case MonsterStatus::SLOW:
            case MonsterStatus::SPORE_CLOUD:
            case MonsterStatus::THIEVERY:
            case MonsterStatus::THORNS:
            case MonsterStatus::TIME_WARP: monster.uniquePower0 = amount; break;
            case MonsterStatus::INVINCIBLE:
            case MonsterStatus::REACTIVE:
            case MonsterStatus::SHARP_HIDE: monster.uniquePower1 = amount; break;
            default: break;
        }
    }

    Random randomFromSnapshot(const py::dict &src) {
        Random rng;
        rng.seed0 = py::cast<std::uint64_t>(src["seed0"]);
        rng.seed1 = py::cast<std::uint64_t>(src["seed1"]);
        rng.counter = py::cast<std::int32_t>(src["counter"]);
        return rng;
    }

    void setRelicCounter(Player &player, RelicId id, int counter) {
        switch (id) {
            case RelicId::HAPPY_FLOWER: player.happyFlowerCounter = counter; break;
            case RelicId::INCENSE_BURNER: player.incenseBurnerCounter = counter; break;
            case RelicId::INK_BOTTLE: player.inkBottleCounter = counter; break;
            case RelicId::INSERTER: player.inserterCounter = counter; break;
            case RelicId::NUNCHAKU: player.nunchakuCounter = counter; break;
            case RelicId::PEN_NIB: player.penNibCounter = counter; break;
            case RelicId::SUNDIAL: player.sundialCounter = counter; break;
            default: break;
        }
    }

    BattleContext battleFromSnapshot(const py::dict &snapshot, std::uint64_t determinizationSeed) {
        BattleContext bc;
        bc.seed = py::cast<std::uint64_t>(snapshot["seed"]);
        bc.floorNum = py::cast<int>(snapshot["floor"]);
        bc.ascension = snapshot.contains("ascension") ? py::cast<int>(snapshot["ascension"]) : 0;
        bc.encounter = snapshot.contains("encounter")
            ? static_cast<MonsterEncounter>(py::cast<int>(snapshot["encounter"]))
            : MonsterEncounter::INVALID;
        bc.turn = std::max(0, py::cast<int>(snapshot["turn"]) - 1);
        bc.outcome = Outcome::UNDECIDED;
        bc.inputState = InputState::PLAYER_NORMAL;
        bc.isBattleOver = false;
        bc.endTurnQueued = false;
        bc.turnHasEnded = false;
        bc.skipMonsterTurn = false;
        bc.monsterTurnIdx = 6;
        bc.actionQueue.clear();
        bc.cardQueue.clear();
        bc.miscBits.reset();

        if (snapshot.contains("rngs")) {
            const auto rngs = py::cast<py::dict>(snapshot["rngs"]);
            bc.aiRng = randomFromSnapshot(py::cast<py::dict>(rngs["ai"]));
            bc.cardRandomRng = randomFromSnapshot(py::cast<py::dict>(rngs["card_random"]));
            bc.miscRng = randomFromSnapshot(py::cast<py::dict>(rngs["misc"]));
            bc.monsterHpRng = randomFromSnapshot(py::cast<py::dict>(rngs["monster_hp"]));
            bc.potionRng = randomFromSnapshot(py::cast<py::dict>(rngs["potion"]));
            bc.shuffleRng = randomFromSnapshot(py::cast<py::dict>(rngs["shuffle"]));
        } else {
            bc.aiRng = Random(determinizationSeed + 1);
            bc.cardRandomRng = Random(determinizationSeed + 2);
            bc.miscRng = Random(determinizationSeed + 3);
            bc.monsterHpRng = Random(determinizationSeed + 4);
            bc.potionRng = Random(determinizationSeed + 5);
            bc.shuffleRng = Random(determinizationSeed + 6);
        }

        bc.player = Player();
        const auto player = py::cast<py::dict>(snapshot["player"]);
        bc.player.cc = snapshot.contains("class")
            ? static_cast<CharacterClass>(py::cast<int>(snapshot["class"]))
            : CharacterClass::IRONCLAD;
        // Defect orbs: slots + per-slot type/data (DARK stores charge in data). Absent keys keep zero orbs.
        if (player.contains("orb_slots")) {
            bc.player.orbSlots = static_cast<std::int8_t>(std::min<int>(py::cast<int>(player["orb_slots"]), Player::MAX_ORB_SLOTS));
        }
        if (player.contains("orbs")) {
            int slot = 0;
            for (const auto item : py::cast<py::list>(player["orbs"])) {
                if (slot >= Player::MAX_ORB_SLOTS) throw std::invalid_argument("snapshot orbs exceed simulator capacity");
                const auto orb = py::cast<py::dict>(item);
                bc.player.orbTypes[slot] = static_cast<Orb>(py::cast<int>(orb["type"]));
                bc.player.orbData[slot] = static_cast<std::int16_t>(orb.contains("data") ? py::cast<int>(orb["data"]) : 0);
                ++slot;
            }
        }
        bc.player.curHp = py::cast<int>(player["current_hp"]);
        bc.player.maxHp = py::cast<int>(player["max_hp"]);
        bc.player.block = py::cast<int>(player["block"]);
        bc.player.energy = py::cast<int>(player["energy"]);
        bc.player.energyPerTurn = static_cast<std::int8_t>(player.contains("energy_per_turn") ? py::cast<int>(player["energy_per_turn"]) : 3);
        bc.player.cardsPlayedThisTurn = snapshot.contains("cards_played_this_turn") ? py::cast<int>(snapshot["cards_played_this_turn"]) : 0;
        bc.player.attacksPlayedThisTurn = snapshot.contains("attacks_played_this_turn") ? py::cast<int>(snapshot["attacks_played_this_turn"]) : 0;
        bc.player.skillsPlayedThisTurn = snapshot.contains("skills_played_this_turn") ? py::cast<int>(snapshot["skills_played_this_turn"]) : 0;
        // 回响成型本回合已复制几张。真机 EchoPower.cardsDoubledThisTurn 是 private,
        // 每回合开始清零;门限是 cardsPlayedThisTurn - 它 <= amount。
        // cardsPlayedThisTurn 含复制件,这里不还原就等于每复制一次门限往前挪一格。
        bc.player.echoFormCardsDoubled = static_cast<std::int8_t>(
            snapshot.contains("echo_form_cards_doubled") ? py::cast<int>(snapshot["echo_form_cards_doubled"]) : 0);
        bc.player.cardsDiscardedThisTurn = snapshot.contains("cards_discarded_this_turn") ? py::cast<int>(snapshot["cards_discarded_this_turn"]) : 0;
        bc.player.timesDamagedThisCombat = snapshot.contains("times_damaged") ? py::cast<int>(snapshot["times_damaged"]) : 0;
        // 本场战斗的充能历史计数。真机 ThunderStrike.use() 遍历
        // GameActionManager.orbsChanneledThisCombat 数 Lightning 来定段数,
        // 这份历史不随球被引爆而减少,和当前球位完全是两回事。CommunicationMod 不导出,
        // 由 SteamStateExport mod 补;缺省回落到 0 会让雷霆一击在仿真里打 0 段。
        bc.player.lightningChanneled = static_cast<std::int16_t>(
            snapshot.contains("lightning_channeled_this_combat")
                ? py::cast<int>(snapshot["lightning_channeled_this_combat"]) : 0);
        bc.player.frostChanneled = static_cast<std::int16_t>(
            snapshot.contains("frost_channeled_this_combat")
                ? py::cast<int>(snapshot["frost_channeled_this_combat"]) : 0);
        bc.powersPlayedThisCombat = snapshot.contains("powers_played_this_combat")
            ? py::cast<int>(snapshot["powers_played_this_combat"]) : 0;
        // 每回合抽牌张数的基线由遗物决定,真机在开战时算一次(AbstractPlayer.gameHandSize)。
        // 快照重建走的是这条手工路径,不经过 BattleContext::partialInitTwo,
        // 于是一直停在 Player 结构体默认的 5 —— 带异蛇之眼时真机抽 7 张、仿真抽 5 张,
        // 手牌数、抽牌堆数、连带 cardRandomRng 的调用次数从此全程错位
        // (f35/36/37 实测:真机手牌 7/抽牌堆 22/card_random 15,仿真 5/24/14)。
        // 必须放在 powers 之前:Draw Reduction 状态会在 setStatus 里把这个值减 1,
        // 放后面会把那次减法冲掉。
        {
            int baseDraw = 5;
            if (snapshot.contains("relics")) {
                for (const auto item : py::cast<py::list>(snapshot["relics"])) {
                    const auto id = static_cast<RelicId>(
                        py::cast<int>(py::cast<py::dict>(item)["id"]));
                    if (id == RelicId::SNECKO_EYE) baseDraw += 2;
                    else if (id == RelicId::RING_OF_THE_SERPENT) baseDraw += 1;
                }
            }
            bc.player.cardDrawPerTurn = static_cast<std::int8_t>(baseDraw);
        }
        if (player.contains("powers")) {
            for (const auto item : py::cast<py::list>(player["powers"])) {
                const auto power = py::cast<py::dict>(item);
                const auto status = static_cast<PlayerStatus>(py::cast<int>(power["id"]));
                const int amount = py::cast<int>(power["amount"]);
                if (status == PlayerStatus::THE_BOMB && power.contains("bomb_turns")) {
                    switch (py::cast<int>(power["bomb_turns"])) {
                        case 1: bc.player.bomb1 += amount; break;
                        case 2: bc.player.bomb2 += amount; break;
                        default: bc.player.bomb3 += amount; break;
                    }
                } else if (status == PlayerStatus::PANACHE) {
                    // 真机 PanachePower: amount = 剩余计数(5->0), storedAmount/damage = 伤害(10/14)
                    // 仿真反过来: statusMap[PANACHE] 存伤害, player.panacheCounter 存计数
                    // 与 BattleConverter.cpp 的 PANACHE 分支保持一致
                    const int panacheDamage = power.contains("damage") ? py::cast<int>(power["damage"]) : 10;
                    setPlayerStatus(bc.player, status, panacheDamage,
                        power.contains("just_applied") && py::cast<bool>(power["just_applied"]));
                    bc.player.panacheCounter = static_cast<std::int8_t>(amount);
                } else {
                    setPlayerStatus(bc.player, status, amount,
                        power.contains("just_applied") && py::cast<bool>(power["just_applied"]));
                }
            }
        }

        bc.cards = CardManager();
        int uniqueId = 0;
        const auto loadCards = [&](const char *key, auto &target, auto notify) {
            for (const auto item : py::cast<py::list>(snapshot[key])) {
                auto card = snapshotCard(py::cast<py::dict>(item), uniqueId++);
                bc.cards.notifyAddCardToCombat(card);
                (bc.cards.*notify)(card);
                target.push_back(card);
            }
        };
        for (const auto item : py::cast<py::list>(snapshot["hand"])) {
            if (bc.cards.cardsInHand >= CardManager::MAX_HAND_SIZE) throw std::invalid_argument("snapshot hand exceeds simulator capacity");
            auto card = snapshotCard(py::cast<py::dict>(item), uniqueId++);
            bc.cards.notifyAddCardToCombat(card);
            bc.cards.notifyAddToHand(card);
            bc.cards.hand[bc.cards.cardsInHand++] = card;
        }
        loadCards("draw_pile", bc.cards.drawPile, &CardManager::notifyAddToDrawPile);
        loadCards("discard_pile", bc.cards.discardPile, &CardManager::notifyAddToDiscardPile);
        for (const auto item : py::cast<py::list>(snapshot["exhaust_pile"])) {
            auto card = snapshotCard(py::cast<py::dict>(item), uniqueId++);
            bc.cards.notifyAddCardToCombat(card);
            bc.cards.exhaustPile.push_back(card);
        }
        bc.cards.nextUniqueCardId = uniqueId;

        bc.monsters = MonsterGroup();
        const auto monsters = py::cast<py::list>(snapshot["monsters"]);
        if (monsters.size() > bc.monsters.arr.size()) throw std::invalid_argument("snapshot monster count exceeds simulator capacity");
        bc.monsters.monsterCount = static_cast<int>(monsters.size());
        bc.monsters.monstersAlive = 0;
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            const auto src = py::cast<py::dict>(monsters[i]);
            auto &monster = bc.monsters.arr[i];
            monster = Monster();
            monster.idx = i;
            monster.id = static_cast<MonsterId>(py::cast<int>(src["id"]));
            if (monster.id == MonsterId::INVALID) throw std::invalid_argument("snapshot monster id is INVALID");
            monster.curHp = py::cast<int>(src["current_hp"]);
            monster.maxHp = py::cast<int>(src["max_hp"]);
            monster.block = py::cast<int>(src["block"]);
            monster.halfDead = src.contains("half_dead") && py::cast<bool>(src["half_dead"]);
            monster.isEscapingB = src.contains("is_gone") && py::cast<bool>(src["is_gone"]);
            monster.moveHistory[0] = static_cast<MMID>(py::cast<int>(src["move"]));
            monster.moveHistory[1] = src.contains("last_move")
                ? static_cast<MMID>(py::cast<int>(src["last_move"])) : MMID::INVALID;
            monster.miscInfo = src.contains("misc_info") ? py::cast<int>(src["misc_info"]) : 0;
            // 黑暗爬虫的啃咬伤害真机存在 Darkling.nipDmg 里,而 A2+ 那个随机数本身
            // 就是按 9-13 摇的 —— 升变加成已经算进去了。模拟器的 miscInfo 约定存的是
            // 未加成基线,伤害公式会再补一次 +2(MonsterMoveDamage.cpp:62)。
            // 把真值原样塞进去等于每次啃咬多打 2 点:f37 实测两只啃咬,玩家多掉 4 血;
            // f35 带 10 格挡+缓冲时,这 2 点的溢出被放大成 13 点血差。
            // BattleConverter 对同一件事已经做了换算(miscInfo = move_base_damage - 2),
            // 这条手工快照路径此前漏了,这里对齐。
            if (monster.id == MonsterId::DARKLING && bc.ascension >= 2) {
                monster.miscInfo = std::max(0, monster.miscInfo - 2);
            }
            // Lagavulin 的沉睡在真实游戏里是 AbstractMonster 上的 isOpen 布尔字段,不是 Power,
            // 所以 CommunicationMod 的 powers 列表里没有它。不还原这个标记的话,沉睡的 Lagavulin
            // 既不会被伤害唤醒(那 8 层 Metallicize 不会掉),也会在第一次 end turn 就跳到 ATTACK。
            // SteamStateExport 现在反射出了 isOpen 真值;缺失时退回按招式反推(还停在 SLEEP 上就是睡着),
            // 但那个反推在"刚被打醒、还没轮到它行动"这一帧会判错,所以真值优先。
            if (monster.id == MonsterId::LAGAVULIN) {
                const bool asleep = src.contains("asleep")
                    ? py::cast<bool>(src["asleep"])
                    : (monster.moveHistory[0] == MonsterMoveId::LAGAVULIN_SLEEP);
                if (asleep) setMonsterStatus(monster, MonsterStatus::ASLEEP, 1);
            }
            if (src.contains("powers")) {
                for (const auto item : py::cast<py::list>(src["powers"])) {
                    const auto power = py::cast<py::dict>(item);
                    setMonsterStatus(monster,
                        static_cast<MonsterStatus>(py::cast<int>(power["id"])),
                        py::cast<int>(power["amount"]));
                }
            }
            if (!monster.isEscapingB && monster.curHp > 0) ++bc.monsters.monstersAlive;
        }

        bc.potionCapacity = 0;
        bc.potionCount = 0;
        if (snapshot.contains("potions")) {
            const auto potions = py::cast<py::list>(snapshot["potions"]);
            if (potions.size() > bc.potions.size()) throw std::invalid_argument("snapshot potion count exceeds simulator capacity");
            bc.potionCapacity = static_cast<int>(potions.size());
            for (int i = 0; i < bc.potionCapacity; ++i) {
                bc.potions[i] = static_cast<Potion>(py::cast<int>(potions[i]));
                if (bc.potions[i] != Potion::EMPTY_POTION_SLOT) ++bc.potionCount;
            }
        }
        if (snapshot.contains("relics")) {
            for (const auto item : py::cast<py::list>(snapshot["relics"])) {
                const auto relic = py::cast<py::dict>(item);
                const int id = py::cast<int>(relic["id"]);
                if (id < 64) bc.player.relicBits0 |= 1ULL << id;
                else if (id < 128) bc.player.relicBits1 |= 1ULL << (id - 64);
                if (relic.contains("counter")) {
                    const int counter = py::cast<int>(relic["counter"]);
                    if (counter >= 0) setRelicCounter(bc.player, static_cast<RelicId>(id), counter);
                }
            }
        }
        return bc;
    }
}

PYBIND11_MODULE(slaythespire, m) {
    m.doc() = "pybind11 example plugin"; // optional module docstring
    m.def("play", &sts::py::play, "play Slay the Spire Console");
    m.def("get_seed_str", &SeedHelper::getString, "gets the integral representation of seed string used in the game ui");
    m.def("get_seed_long", &SeedHelper::getLong, "gets the seed string representation of an integral seed");
    m.def("getNNInterface", &sts::NNInterface::getInstance, "gets the NNInterface object");
    m.def("card_id_from_name", [](const std::string &name) {
        try { return enumFromNames(name, cardEnumStrings); }
        catch (const std::invalid_argument &) { return enumFromNames(name, cardNames); }
    });
    m.def("monster_id_from_name", [](const std::string &name) { return enumFromNames(name, monsterIdEnumNames); });
    m.def("monster_move_id_from_name", [](const std::string &name) { return enumFromNames(name, monsterMoveStrings); });
    m.def("player_status_id_from_name", [](const std::string &name) {
        try { return enumFromNames(name, playerStatusEnumStrings); }
        catch (const std::invalid_argument &) { return enumFromNames(name, playerStatusStrings); }
    });
    m.def("monster_status_id_from_name", [](const std::string &name) {
        try { return enumFromNames(name, monsterStatusEnumStrings); }
        catch (const std::invalid_argument &) { return enumFromNames(name, enemyStatusStrings); }
    });
    m.def("relic_id_from_name", [](const std::string &name) {
        const auto wanted = normalizedName(name);
        for (int i = 0; i <= static_cast<int>(RelicId::INVALID); ++i) {
            if (normalizedName(getRelicName(static_cast<RelicId>(i))) == wanted ||
                normalizedName(relicIds[i]) == wanted ||
                normalizedName(relicEnumNames[i]) == wanted) return i;
        }
        throw std::invalid_argument("unknown relic name: " + name);
    });
    m.def("potion_id_from_name", [](const std::string &name) {
        try { return enumFromNames(name, potionEnumNames); }
        catch (const std::invalid_argument &) {
            try { return enumFromNames(name, potionIds); }
            catch (const std::invalid_argument &) { return enumFromNames(name, potionNames); }
        }
    });
    m.def("boss_relic_ordering", [](int id) {
        return search::Expert::getBossRelicOrdering(static_cast<RelicId>(id));
    });

    pybind11::class_<NNInterface> nnInterface(m, "NNInterface");
    nnInterface.def("getObservation", &NNInterface::getObservation, "get observation array given a GameContext")
        .def("getObservationMaximums", &NNInterface::getObservationMaximums, "get the defined maximum values of the observation space")
        .def_property_readonly("observation_space_size", []() { return NNInterface::observation_space_size; })
        .def("getCardIdx", &NNInterface::getCardIdx, "unified card slot (base*2 + upgraded) used by the deck part of the observation; -1 if unmapped")
        .def_property_readonly("card_slot_count", [](const NNInterface &) { return NNInterface::cardSlotCount; });

    pybind11::class_<search::ScumSearchAgent2> agent(m, "Agent");
    agent.def(pybind11::init<>());
    agent.def_readwrite("simulation_count_base", &search::ScumSearchAgent2::simulationCountBase, "number of simulations the agent uses for monte carlo tree search each turn")
        .def_readwrite("boss_simulation_multiplier", &search::ScumSearchAgent2::bossSimulationMultiplier, "bonus multiplier to the simulation count for boss fights")
        .def_readwrite("search_time_limit_ms", &search::ScumSearchAgent2::searchTimeLimitMillis, "per-decision wall-clock cap (ms) for combat MCTS; fork default 1000")
        .def_readwrite("pause_on_card_reward", &search::ScumSearchAgent2::pauseOnCardReward, "causes the agent to pause so as to cede control to the user when it encounters a card reward choice")
        .def_readwrite("pause_on_rewards", &search::ScumSearchAgent2::pauseOnRewards, "pause and cede control on rewards screen so model can make all reward choices")
        .def_readwrite("pause_on_map", &search::ScumSearchAgent2::pauseOnMap, "pause and cede control on the map (path choice) screen")
        .def_readwrite("pause_on_rest", &search::ScumSearchAgent2::pauseOnRest, "pause and cede control on the rest site (campfire) screen")
        .def_readwrite("pause_on_shop", &search::ScumSearchAgent2::pauseOnShop, "pause and cede control on the shop screen")
        .def_readwrite("pause_on_event", &search::ScumSearchAgent2::pauseOnEvent, "pause and cede control on event screens that have a real choice")
        .def_readwrite("pause_on_battle", &search::ScumSearchAgent2::pauseOnBattle, "pause at the start of each battle so the caller can drive combat with its own model")
        .def_readwrite("print_logs", &search::ScumSearchAgent2::printLogs, "when set to true, the agent prints state information as it makes actions")
        .def("playout", &search::ScumSearchAgent2::playout);

    pybind11::class_<GameContext> gameContext(m, "GameContext");
    gameContext.def(pybind11::init<CharacterClass, std::uint64_t, int>())
        .def("pick_reward_card", &sts::py::pickRewardCard, "choose to obtain the card at the specified index in the card reward list")
        .def("skip_reward_cards", &sts::py::skipRewardCards, "choose to skip the card reward (increases max_hp by 2 with singing bowl)")
        .def("get_card_reward", &sts::py::getCardReward, "return the current card reward list")
        .def_property_readonly("encounter", [](const GameContext &gc) { return gc.info.encounter; })
        .def_property_readonly("deck",
               [](const GameContext &gc) { return std::vector(gc.deck.cards.begin(), gc.deck.cards.end());},
               "returns a copy of the list of cards in the deck"
        )
        .def("obtain_card",
             [](GameContext &gc, Card card) { gc.deck.obtain(gc, card); },
             "add a card to the deck"
        )
        .def("remove_card",
            [](GameContext &gc, int idx) {
                if (idx < 0 || idx >= gc.deck.size()) {
                    std::cerr << "invalid remove deck remove idx" << std::endl;
                    return;
                }
                gc.deck.remove(gc, idx);
            },
             "remove a card at a idx in the deck"
        )
        .def_property_readonly("relics",
               [] (const GameContext &gc) { return std::vector(gc.relics.relics); },
               "returns a copy of the list of relics")
        // --- 状态注入用(把真机中局状态灌进 GameContext 做离线评估)---
        // relics 是只读拷贝,改拷贝不会回写 C++ 侧,所以必须显式给 add/remove/has。
        // 这里走的是 RelicContainer 的裸 add,不触发 onEquip;最大血量/金币/能量之类的
        // 副作用请在注入时单独设好,不要指望这个接口替你结算。
        .def("add_relic",
               [] (GameContext &gc, RelicId r, int data) { gc.relics.add({r, data}); },
               pybind11::arg("relic_id"), pybind11::arg("data") = 0,
               "add a relic (raw container add, no onEquip side effects)")
        .def("remove_relic",
               [] (GameContext &gc, RelicId r) { gc.relics.remove(r); },
               pybind11::arg("relic_id"), "remove a relic by id")
        .def("has_relic",
               [] (const GameContext &gc, RelicId r) { return gc.relics.has(r); },
               pybind11::arg("relic_id"), "whether the run currently has this relic")
        .def("relic_value",
               [] (const GameContext &gc, RelicId r) { return gc.relics.getRelicValue(r); },
               pybind11::arg("relic_id"), "the per-relic counter (RelicInstance::data)")
        .def("set_relic_value",
               [] (GameContext &gc, RelicId r, int v) { gc.relics.getRelicValueRef(r) = v; },
               pybind11::arg("relic_id"), pybind11::arg("value"),
               "set the per-relic counter; relic must already be present")
        .def_property_readonly("boss_relic_pool",
               [] (const GameContext &gc) { return std::vector(gc.bossRelicPool); },
               "shuffled boss relic pool (front = next boss reward)")
        .def_property_readonly("common_relic_pool",
               [] (const GameContext &gc) { return std::vector(gc.commonRelicPool); }, "shuffled common relic pool")
        .def_property_readonly("uncommon_relic_pool", [] (const GameContext &gc) { return std::vector(gc.uncommonRelicPool); })
        .def_property_readonly("rare_relic_pool", [] (const GameContext &gc) { return std::vector(gc.rareRelicPool); })
        .def_property_readonly("shop_relic_pool", [] (const GameContext &gc) { return std::vector(gc.shopRelicPool); }, "shuffled shop relic pool"
        )
        .def_property_readonly("cur_event",
               [](const GameContext &gc) { return static_cast<int>(gc.curEvent); },
               "integer id of the current event (Event enum); for encoding event-screen choices"
        )
        .def_property_readonly("burning_elite_x", [](const GameContext &gc) { return gc.map ? gc.map->burningEliteX : -1; })
        .def_property_readonly("burning_elite_y", [](const GameContext &gc) { return gc.map ? gc.map->burningEliteY : -1; })
        .def("map_node_room",
             [](const GameContext &gc, int x, int y) {
                 if (!gc.map || x < 0 || x > 6 || y < 0 || y > 14) return Room::INVALID;
                 return gc.map->getNode(x, y).room;
             },
             "room type of the map node at (x, y); used to describe path-choice candidates"
        )
        .def("get_map_nn",
             [](const GameContext &gc) {
                 if (!gc.map) return std::vector<int>();
                 return sts::py::getNNMapRepresentation(*gc.map);   // full map graph (edges + room types) as a vector
             },
             "NN encoding of the whole current map (edges + room types); global lookahead features for path planning"
        )
        .def("map_node_children",
             [](const GameContext &gc, int x, int y) {
                 std::vector<int> kids;
                 if (!gc.map || x < 0 || x > 6 || y < 0 || y > 13) return kids;
                 const auto &node = gc.map->getNode(x, y);
                 for (int i = 0; i < node.edgeCount; ++i) kids.push_back(node.edges[i]);
                 return kids;
             },
             "x-coords of the child nodes (next row) reachable from node (x,y); for position-relative path lookahead"
        )
        .def("get_shop_cards",
             [](const GameContext &gc) {
                 std::vector<std::pair<Card,int>> out;          // (card, price); price -1 if slot empty/sold
                 const auto &s = gc.info.shop;
                 for (int i = 0; i < 7; ++i) out.emplace_back(s.cards[i], s.cardPrice(i));
                 return out;
             },
             "the 7 shop card slots as (Card, price) pairs (price -1 if empty)"
        )
        .def("__repr__", [](const GameContext &gc) {
            std::ostringstream oss;
            oss << "<" << gc << ">";
            return oss.str();
        }, "returns a string representation of the GameContext");

    gameContext.def_readwrite("outcome", &GameContext::outcome)
        .def_readwrite("act", &GameContext::act)
        .def_readwrite("floor_num", &GameContext::floorNum)
        .def_readwrite("screen_state", &GameContext::screenState)

        .def_readwrite("seed", &GameContext::seed)
        .def_readwrite("cur_map_node_x", &GameContext::curMapNodeX)
        .def_readwrite("cur_map_node_y", &GameContext::curMapNodeY)
        .def_readwrite("cur_room", &GameContext::curRoom)
//        .def_readwrite("cur_event", &GameContext::curEvent) // todo standardize event names
        .def_readwrite("boss", &GameContext::boss)

        .def_readwrite("cur_hp", &GameContext::curHp)
        .def_readwrite("max_hp", &GameContext::maxHp)
        .def_readwrite("gold", &GameContext::gold)

        .def_readwrite("blue_key", &GameContext::blueKey)
        .def_readwrite("green_key", &GameContext::greenKey)
        .def_readwrite("red_key", &GameContext::redKey)

        .def_readwrite("card_rarity_factor", &GameContext::cardRarityFactor)
        .def_readwrite("potion_chance", &GameContext::potionChance)
        .def_readwrite("monster_chance", &GameContext::monsterChance)
        .def_readwrite("shop_chance", &GameContext::shopChance)
        .def_readwrite("treasure_chance", &GameContext::treasureChance)

        .def_readwrite("shop_remove_count", &GameContext::shopRemoveCount)
        .def_readwrite("speedrun_pace", &GameContext::speedrunPace)
        .def_readwrite("note_for_yourself_card", &GameContext::noteForYourselfCard);

    pybind11::class_<RelicInstance> relic(m, "Relic");
    relic.def_readwrite("id", &RelicInstance::id)
        .def_readwrite("data", &RelicInstance::data);

    pybind11::class_<Map> map(m, "SpireMap");
    map.def(pybind11::init<std::uint64_t, int,int,bool>());
    map.def("get_room_type", &sts::py::getRoomType);
    map.def("has_edge", &sts::py::hasEdge);
    map.def("get_nn_rep", &sts::py::getNNMapRepresentation);
    map.def("__repr__", [](const Map &m) {
        return m.toString(true);
    });

    pybind11::class_<Card> card(m, "Card");
    card.def(pybind11::init<CardId>())
        .def("__repr__", [](const Card &c) {
            std::string s("<slaythespire.Card ");
            s += c.getName();
            if (c.isUpgraded()) {
                s += '+';
                if (c.id == sts::CardId::SEARING_BLOW) {
                    s += std::to_string(c.getUpgraded());
                }
            }
            return s += ">";
        }, "returns a string representation of a Card")
        .def("upgrade", &Card::upgrade)
        .def_readwrite("misc", &Card::misc, "value internal to the simulator used for things like ritual dagger damage");

    card.def_property_readonly("id", &Card::getId)
        .def_property_readonly("upgraded", &Card::isUpgraded)
        .def_property_readonly("upgrade_count", &Card::getUpgraded)
        .def_property_readonly("innate", &Card::isInnate)
        .def_property_readonly("transformable", &Card::canTransform)
        .def_property_readonly("upgradable", &Card::canUpgrade)
        .def_property_readonly("is_strikeCard", &Card::isStrikeCard)
        .def_property_readonly("is_starter_strike_or_defend", &Card::isStarterStrikeOrDefend)
        .def_property_readonly("rarity", &Card::getRarity)
        .def_property_readonly("type", &Card::getType);

    pybind11::enum_<GameOutcome> gameOutcome(m, "GameOutcome");
    gameOutcome.value("UNDECIDED", GameOutcome::UNDECIDED)
        .value("PLAYER_VICTORY", GameOutcome::PLAYER_VICTORY)
        .value("PLAYER_LOSS", GameOutcome::PLAYER_LOSS);

    pybind11::enum_<ScreenState> screenState(m, "ScreenState");
    screenState.value("INVALID", ScreenState::INVALID)
        .value("EVENT_SCREEN", ScreenState::EVENT_SCREEN)
        .value("REWARDS", ScreenState::REWARDS)
        .value("BOSS_RELIC_REWARDS", ScreenState::BOSS_RELIC_REWARDS)
        .value("CARD_SELECT", ScreenState::CARD_SELECT)
        .value("MAP_SCREEN", ScreenState::MAP_SCREEN)
        .value("TREASURE_ROOM", ScreenState::TREASURE_ROOM)
        .value("REST_ROOM", ScreenState::REST_ROOM)
        .value("SHOP_ROOM", ScreenState::SHOP_ROOM)
        .value("BATTLE", ScreenState::BATTLE);

    pybind11::enum_<CharacterClass> characterClass(m, "CharacterClass");
    characterClass.value("IRONCLAD", CharacterClass::IRONCLAD)
            .value("SILENT", CharacterClass::SILENT)
            .value("DEFECT", CharacterClass::DEFECT)
            .value("WATCHER", CharacterClass::WATCHER)
            .value("INVALID", CharacterClass::INVALID);

    pybind11::enum_<Room> roomEnum(m, "Room");
    roomEnum.value("SHOP", Room::SHOP)
        .value("REST", Room::REST)
        .value("EVENT", Room::EVENT)
        .value("ELITE", Room::ELITE)
        .value("MONSTER", Room::MONSTER)
        .value("TREASURE", Room::TREASURE)
        .value("BOSS", Room::BOSS)
        .value("BOSS_TREASURE", Room::BOSS_TREASURE)
        .value("NONE", Room::NONE)
        .value("INVALID", Room::INVALID);

    pybind11::enum_<CardRarity>(m, "CardRarity")
        .value("COMMON", CardRarity::COMMON)
        .value("UNCOMMON", CardRarity::UNCOMMON)
        .value("RARE", CardRarity::RARE)
        .value("BASIC", CardRarity::BASIC)
        .value("SPECIAL", CardRarity::SPECIAL)
        .value("CURSE", CardRarity::CURSE)
        .value("INVALID", CardRarity::INVALID);

    pybind11::enum_<CardColor>(m, "CardColor")
        .value("RED", CardColor::RED)
        .value("GREEN", CardColor::GREEN)
        .value("BLUE", CardColor::BLUE)
        .value("PURPLE", CardColor::PURPLE)
        .value("COLORLESS", CardColor::COLORLESS)
        .value("CURSE", CardColor::CURSE)
        .value("INVALID", CardColor::INVALID);

    pybind11::enum_<CardType>(m, "CardType")
        .value("ATTACK", CardType::ATTACK)
        .value("SKILL", CardType::SKILL)
        .value("POWER", CardType::POWER)
        .value("CURSE", CardType::CURSE)
        .value("STATUS", CardType::STATUS)
        .value("INVALID", CardType::INVALID);

    pybind11::enum_<CardId>(m, "CardId")
        .value("INVALID", CardId::INVALID)
        .value("ACCURACY", CardId::ACCURACY)
        .value("ACROBATICS", CardId::ACROBATICS)
        .value("ADRENALINE", CardId::ADRENALINE)
        .value("AFTER_IMAGE", CardId::AFTER_IMAGE)
        .value("AGGREGATE", CardId::AGGREGATE)
        .value("ALCHEMIZE", CardId::ALCHEMIZE)
        .value("ALL_FOR_ONE", CardId::ALL_FOR_ONE)
        .value("ALL_OUT_ATTACK", CardId::ALL_OUT_ATTACK)
        .value("ALPHA", CardId::ALPHA)
        .value("AMPLIFY", CardId::AMPLIFY)
        .value("ANGER", CardId::ANGER)
        .value("APOTHEOSIS", CardId::APOTHEOSIS)
        .value("APPARITION", CardId::APPARITION)
        .value("ARMAMENTS", CardId::ARMAMENTS)
        .value("ASCENDERS_BANE", CardId::ASCENDERS_BANE)
        .value("AUTO_SHIELDS", CardId::AUTO_SHIELDS)
        .value("A_THOUSAND_CUTS", CardId::A_THOUSAND_CUTS)
        .value("BACKFLIP", CardId::BACKFLIP)
        .value("BACKSTAB", CardId::BACKSTAB)
        .value("BALL_LIGHTNING", CardId::BALL_LIGHTNING)
        .value("BANDAGE_UP", CardId::BANDAGE_UP)
        .value("BANE", CardId::BANE)
        .value("BARRAGE", CardId::BARRAGE)
        .value("BARRICADE", CardId::BARRICADE)
        .value("BASH", CardId::BASH)
        .value("BATTLE_HYMN", CardId::BATTLE_HYMN)
        .value("BATTLE_TRANCE", CardId::BATTLE_TRANCE)
        .value("BEAM_CELL", CardId::BEAM_CELL)
        .value("BECOME_ALMIGHTY", CardId::BECOME_ALMIGHTY)
        .value("BERSERK", CardId::BERSERK)
        .value("BETA", CardId::BETA)
        .value("BIASED_COGNITION", CardId::BIASED_COGNITION)
        .value("BITE", CardId::BITE)
        .value("BLADE_DANCE", CardId::BLADE_DANCE)
        .value("BLASPHEMY", CardId::BLASPHEMY)
        .value("BLIND", CardId::BLIND)
        .value("BLIZZARD", CardId::BLIZZARD)
        .value("BLOODLETTING", CardId::BLOODLETTING)
        .value("BLOOD_FOR_BLOOD", CardId::BLOOD_FOR_BLOOD)
        .value("BLUDGEON", CardId::BLUDGEON)
        .value("BLUR", CardId::BLUR)
        .value("BODY_SLAM", CardId::BODY_SLAM)
        .value("BOOT_SEQUENCE", CardId::BOOT_SEQUENCE)
        .value("BOUNCING_FLASK", CardId::BOUNCING_FLASK)
        .value("BOWLING_BASH", CardId::BOWLING_BASH)
        .value("BRILLIANCE", CardId::BRILLIANCE)
        .value("BRUTALITY", CardId::BRUTALITY)
        .value("BUFFER", CardId::BUFFER)
        .value("BULLET_TIME", CardId::BULLET_TIME)
        .value("BULLSEYE", CardId::BULLSEYE)
        .value("BURN", CardId::BURN)
        .value("BURNING_PACT", CardId::BURNING_PACT)
        .value("BURST", CardId::BURST)
        .value("CALCULATED_GAMBLE", CardId::CALCULATED_GAMBLE)
        .value("CALTROPS", CardId::CALTROPS)
        .value("CAPACITOR", CardId::CAPACITOR)
        .value("CARNAGE", CardId::CARNAGE)
        .value("CARVE_REALITY", CardId::CARVE_REALITY)
        .value("CATALYST", CardId::CATALYST)
        .value("CHAOS", CardId::CHAOS)
        .value("CHARGE_BATTERY", CardId::CHARGE_BATTERY)
        .value("CHILL", CardId::CHILL)
        .value("CHOKE", CardId::CHOKE)
        .value("CHRYSALIS", CardId::CHRYSALIS)
        .value("CLASH", CardId::CLASH)
        .value("CLAW", CardId::CLAW)
        .value("CLEAVE", CardId::CLEAVE)
        .value("CLOAK_AND_DAGGER", CardId::CLOAK_AND_DAGGER)
        .value("CLOTHESLINE", CardId::CLOTHESLINE)
        .value("CLUMSY", CardId::CLUMSY)
        .value("COLD_SNAP", CardId::COLD_SNAP)
        .value("COLLECT", CardId::COLLECT)
        .value("COMBUST", CardId::COMBUST)
        .value("COMPILE_DRIVER", CardId::COMPILE_DRIVER)
        .value("CONCENTRATE", CardId::CONCENTRATE)
        .value("CONCLUDE", CardId::CONCLUDE)
        .value("CONJURE_BLADE", CardId::CONJURE_BLADE)
        .value("CONSECRATE", CardId::CONSECRATE)
        .value("CONSUME", CardId::CONSUME)
        .value("COOLHEADED", CardId::COOLHEADED)
        .value("CORE_SURGE", CardId::CORE_SURGE)
        .value("CORPSE_EXPLOSION", CardId::CORPSE_EXPLOSION)
        .value("CORRUPTION", CardId::CORRUPTION)
        .value("CREATIVE_AI", CardId::CREATIVE_AI)
        .value("CRESCENDO", CardId::CRESCENDO)
        .value("CRIPPLING_CLOUD", CardId::CRIPPLING_CLOUD)
        .value("CRUSH_JOINTS", CardId::CRUSH_JOINTS)
        .value("CURSE_OF_THE_BELL", CardId::CURSE_OF_THE_BELL)
        .value("CUT_THROUGH_FATE", CardId::CUT_THROUGH_FATE)
        .value("DAGGER_SPRAY", CardId::DAGGER_SPRAY)
        .value("DAGGER_THROW", CardId::DAGGER_THROW)
        .value("DARKNESS", CardId::DARKNESS)
        .value("DARK_EMBRACE", CardId::DARK_EMBRACE)
        .value("DARK_SHACKLES", CardId::DARK_SHACKLES)
        .value("DASH", CardId::DASH)
        .value("DAZED", CardId::DAZED)
        .value("DEADLY_POISON", CardId::DEADLY_POISON)
        .value("DECAY", CardId::DECAY)
        .value("DECEIVE_REALITY", CardId::DECEIVE_REALITY)
        .value("DEEP_BREATH", CardId::DEEP_BREATH)
        .value("DEFEND_BLUE", CardId::DEFEND_BLUE)
        .value("DEFEND_GREEN", CardId::DEFEND_GREEN)
        .value("DEFEND_PURPLE", CardId::DEFEND_PURPLE)
        .value("DEFEND_RED", CardId::DEFEND_RED)
        .value("DEFLECT", CardId::DEFLECT)
        .value("DEFRAGMENT", CardId::DEFRAGMENT)
        .value("DEMON_FORM", CardId::DEMON_FORM)
        .value("DEUS_EX_MACHINA", CardId::DEUS_EX_MACHINA)
        .value("DEVA_FORM", CardId::DEVA_FORM)
        .value("DEVOTION", CardId::DEVOTION)
        .value("DIE_DIE_DIE", CardId::DIE_DIE_DIE)
        .value("DISARM", CardId::DISARM)
        .value("DISCOVERY", CardId::DISCOVERY)
        .value("DISTRACTION", CardId::DISTRACTION)
        .value("DODGE_AND_ROLL", CardId::DODGE_AND_ROLL)
        .value("DOOM_AND_GLOOM", CardId::DOOM_AND_GLOOM)
        .value("DOPPELGANGER", CardId::DOPPELGANGER)
        .value("DOUBLE_ENERGY", CardId::DOUBLE_ENERGY)
        .value("DOUBLE_TAP", CardId::DOUBLE_TAP)
        .value("DOUBT", CardId::DOUBT)
        .value("DRAMATIC_ENTRANCE", CardId::DRAMATIC_ENTRANCE)
        .value("DROPKICK", CardId::DROPKICK)
        .value("DUALCAST", CardId::DUALCAST)
        .value("DUAL_WIELD", CardId::DUAL_WIELD)
        .value("ECHO_FORM", CardId::ECHO_FORM)
        .value("ELECTRODYNAMICS", CardId::ELECTRODYNAMICS)
        .value("EMPTY_BODY", CardId::EMPTY_BODY)
        .value("EMPTY_FIST", CardId::EMPTY_FIST)
        .value("EMPTY_MIND", CardId::EMPTY_MIND)
        .value("ENDLESS_AGONY", CardId::ENDLESS_AGONY)
        .value("ENLIGHTENMENT", CardId::ENLIGHTENMENT)
        .value("ENTRENCH", CardId::ENTRENCH)
        .value("ENVENOM", CardId::ENVENOM)
        .value("EQUILIBRIUM", CardId::EQUILIBRIUM)
        .value("ERUPTION", CardId::ERUPTION)
        .value("ESCAPE_PLAN", CardId::ESCAPE_PLAN)
        .value("ESTABLISHMENT", CardId::ESTABLISHMENT)
        .value("EVALUATE", CardId::EVALUATE)
        .value("EVISCERATE", CardId::EVISCERATE)
        .value("EVOLVE", CardId::EVOLVE)
        .value("EXHUME", CardId::EXHUME)
        .value("EXPERTISE", CardId::EXPERTISE)
        .value("EXPUNGER", CardId::EXPUNGER)
        .value("FAME_AND_FORTUNE", CardId::FAME_AND_FORTUNE)
        .value("FASTING", CardId::FASTING)
        .value("FEAR_NO_EVIL", CardId::FEAR_NO_EVIL)
        .value("FEED", CardId::FEED)
        .value("FEEL_NO_PAIN", CardId::FEEL_NO_PAIN)
        .value("FIEND_FIRE", CardId::FIEND_FIRE)
        .value("FINESSE", CardId::FINESSE)
        .value("FINISHER", CardId::FINISHER)
        .value("FIRE_BREATHING", CardId::FIRE_BREATHING)
        .value("FISSION", CardId::FISSION)
        .value("FLAME_BARRIER", CardId::FLAME_BARRIER)
        .value("FLASH_OF_STEEL", CardId::FLASH_OF_STEEL)
        .value("FLECHETTES", CardId::FLECHETTES)
        .value("FLEX", CardId::FLEX)
        .value("FLURRY_OF_BLOWS", CardId::FLURRY_OF_BLOWS)
        .value("FLYING_KNEE", CardId::FLYING_KNEE)
        .value("FLYING_SLEEVES", CardId::FLYING_SLEEVES)
        .value("FOLLOW_UP", CardId::FOLLOW_UP)
        .value("FOOTWORK", CardId::FOOTWORK)
        .value("FORCE_FIELD", CardId::FORCE_FIELD)
        .value("FOREIGN_INFLUENCE", CardId::FOREIGN_INFLUENCE)
        .value("FORESIGHT", CardId::FORESIGHT)
        .value("FORETHOUGHT", CardId::FORETHOUGHT)
        .value("FTL", CardId::FTL)
        .value("FUSION", CardId::FUSION)
        .value("GENETIC_ALGORITHM", CardId::GENETIC_ALGORITHM)
        .value("GHOSTLY_ARMOR", CardId::GHOSTLY_ARMOR)
        .value("GLACIER", CardId::GLACIER)
        .value("GLASS_KNIFE", CardId::GLASS_KNIFE)
        .value("GOOD_INSTINCTS", CardId::GOOD_INSTINCTS)
        .value("GO_FOR_THE_EYES", CardId::GO_FOR_THE_EYES)
        .value("GRAND_FINALE", CardId::GRAND_FINALE)
        .value("HALT", CardId::HALT)
        .value("HAND_OF_GREED", CardId::HAND_OF_GREED)
        .value("HAVOC", CardId::HAVOC)
        .value("HEADBUTT", CardId::HEADBUTT)
        .value("HEATSINKS", CardId::HEATSINKS)
        .value("HEAVY_BLADE", CardId::HEAVY_BLADE)
        .value("HEEL_HOOK", CardId::HEEL_HOOK)
        .value("HELLO_WORLD", CardId::HELLO_WORLD)
        .value("HEMOKINESIS", CardId::HEMOKINESIS)
        .value("HOLOGRAM", CardId::HOLOGRAM)
        .value("HYPERBEAM", CardId::HYPERBEAM)
        .value("IMMOLATE", CardId::IMMOLATE)
        .value("IMPATIENCE", CardId::IMPATIENCE)
        .value("IMPERVIOUS", CardId::IMPERVIOUS)
        .value("INDIGNATION", CardId::INDIGNATION)
        .value("INFERNAL_BLADE", CardId::INFERNAL_BLADE)
        .value("INFINITE_BLADES", CardId::INFINITE_BLADES)
        .value("INFLAME", CardId::INFLAME)
        .value("INJURY", CardId::INJURY)
        .value("INNER_PEACE", CardId::INNER_PEACE)
        .value("INSIGHT", CardId::INSIGHT)
        .value("INTIMIDATE", CardId::INTIMIDATE)
        .value("IRON_WAVE", CardId::IRON_WAVE)
        .value("JAX", CardId::JAX)
        .value("JACK_OF_ALL_TRADES", CardId::JACK_OF_ALL_TRADES)
        .value("JUDGMENT", CardId::JUDGMENT)
        .value("JUGGERNAUT", CardId::JUGGERNAUT)
        .value("JUST_LUCKY", CardId::JUST_LUCKY)
        .value("LEAP", CardId::LEAP)
        .value("LEG_SWEEP", CardId::LEG_SWEEP)
        .value("LESSON_LEARNED", CardId::LESSON_LEARNED)
        .value("LIKE_WATER", CardId::LIKE_WATER)
        .value("LIMIT_BREAK", CardId::LIMIT_BREAK)
        .value("LIVE_FOREVER", CardId::LIVE_FOREVER)
        .value("LOOP", CardId::LOOP)
        .value("MACHINE_LEARNING", CardId::MACHINE_LEARNING)
        .value("MADNESS", CardId::MADNESS)
        .value("MAGNETISM", CardId::MAGNETISM)
        .value("MALAISE", CardId::MALAISE)
        .value("MASTERFUL_STAB", CardId::MASTERFUL_STAB)
        .value("MASTER_OF_STRATEGY", CardId::MASTER_OF_STRATEGY)
        .value("MASTER_REALITY", CardId::MASTER_REALITY)
        .value("MAYHEM", CardId::MAYHEM)
        .value("MEDITATE", CardId::MEDITATE)
        .value("MELTER", CardId::MELTER)
        .value("MENTAL_FORTRESS", CardId::MENTAL_FORTRESS)
        .value("METALLICIZE", CardId::METALLICIZE)
        .value("METAMORPHOSIS", CardId::METAMORPHOSIS)
        .value("METEOR_STRIKE", CardId::METEOR_STRIKE)
        .value("MIND_BLAST", CardId::MIND_BLAST)
        .value("MIRACLE", CardId::MIRACLE)
        .value("MULTI_CAST", CardId::MULTI_CAST)
        .value("NECRONOMICURSE", CardId::NECRONOMICURSE)
        .value("NEUTRALIZE", CardId::NEUTRALIZE)
        .value("NIGHTMARE", CardId::NIGHTMARE)
        .value("NIRVANA", CardId::NIRVANA)
        .value("NORMALITY", CardId::NORMALITY)
        .value("NOXIOUS_FUMES", CardId::NOXIOUS_FUMES)
        .value("OFFERING", CardId::OFFERING)
        .value("OMEGA", CardId::OMEGA)
        .value("OMNISCIENCE", CardId::OMNISCIENCE)
        .value("OUTMANEUVER", CardId::OUTMANEUVER)
        .value("OVERCLOCK", CardId::OVERCLOCK)
        .value("PAIN", CardId::PAIN)
        .value("PANACEA", CardId::PANACEA)
        .value("PANACHE", CardId::PANACHE)
        .value("PANIC_BUTTON", CardId::PANIC_BUTTON)
        .value("PARASITE", CardId::PARASITE)
        .value("PERFECTED_STRIKE", CardId::PERFECTED_STRIKE)
        .value("PERSEVERANCE", CardId::PERSEVERANCE)
        .value("PHANTASMAL_KILLER", CardId::PHANTASMAL_KILLER)
        .value("PIERCING_WAIL", CardId::PIERCING_WAIL)
        .value("POISONED_STAB", CardId::POISONED_STAB)
        .value("POMMEL_STRIKE", CardId::POMMEL_STRIKE)
        .value("POWER_THROUGH", CardId::POWER_THROUGH)
        .value("PRAY", CardId::PRAY)
        .value("PREDATOR", CardId::PREDATOR)
        .value("PREPARED", CardId::PREPARED)
        .value("PRESSURE_POINTS", CardId::PRESSURE_POINTS)
        .value("PRIDE", CardId::PRIDE)
        .value("PROSTRATE", CardId::PROSTRATE)
        .value("PROTECT", CardId::PROTECT)
        .value("PUMMEL", CardId::PUMMEL)
        .value("PURITY", CardId::PURITY)
        .value("QUICK_SLASH", CardId::QUICK_SLASH)
        .value("RAGE", CardId::RAGE)
        .value("RAGNAROK", CardId::RAGNAROK)
        .value("RAINBOW", CardId::RAINBOW)
        .value("RAMPAGE", CardId::RAMPAGE)
        .value("REACH_HEAVEN", CardId::REACH_HEAVEN)
        .value("REAPER", CardId::REAPER)
        .value("REBOOT", CardId::REBOOT)
        .value("REBOUND", CardId::REBOUND)
        .value("RECKLESS_CHARGE", CardId::RECKLESS_CHARGE)
        .value("RECURSION", CardId::RECURSION)
        .value("RECYCLE", CardId::RECYCLE)
        .value("REFLEX", CardId::REFLEX)
        .value("REGRET", CardId::REGRET)
        .value("REINFORCED_BODY", CardId::REINFORCED_BODY)
        .value("REPROGRAM", CardId::REPROGRAM)
        .value("RIDDLE_WITH_HOLES", CardId::RIDDLE_WITH_HOLES)
        .value("RIP_AND_TEAR", CardId::RIP_AND_TEAR)
        .value("RITUAL_DAGGER", CardId::RITUAL_DAGGER)
        .value("RUPTURE", CardId::RUPTURE)
        .value("RUSHDOWN", CardId::RUSHDOWN)
        .value("SADISTIC_NATURE", CardId::SADISTIC_NATURE)
        .value("SAFETY", CardId::SAFETY)
        .value("SANCTITY", CardId::SANCTITY)
        .value("SANDS_OF_TIME", CardId::SANDS_OF_TIME)
        .value("SASH_WHIP", CardId::SASH_WHIP)
        .value("SCRAPE", CardId::SCRAPE)
        .value("SCRAWL", CardId::SCRAWL)
        .value("SEARING_BLOW", CardId::SEARING_BLOW)
        .value("SECOND_WIND", CardId::SECOND_WIND)
        .value("SECRET_TECHNIQUE", CardId::SECRET_TECHNIQUE)
        .value("SECRET_WEAPON", CardId::SECRET_WEAPON)
        .value("SEEING_RED", CardId::SEEING_RED)
        .value("SEEK", CardId::SEEK)
        .value("SELF_REPAIR", CardId::SELF_REPAIR)
        .value("SENTINEL", CardId::SENTINEL)
        .value("SETUP", CardId::SETUP)
        .value("SEVER_SOUL", CardId::SEVER_SOUL)
        .value("SHAME", CardId::SHAME)
        .value("SHIV", CardId::SHIV)
        .value("SHOCKWAVE", CardId::SHOCKWAVE)
        .value("SHRUG_IT_OFF", CardId::SHRUG_IT_OFF)
        .value("SIGNATURE_MOVE", CardId::SIGNATURE_MOVE)
        .value("SIMMERING_FURY", CardId::SIMMERING_FURY)
        .value("SKEWER", CardId::SKEWER)
        .value("SKIM", CardId::SKIM)
        .value("SLICE", CardId::SLICE)
        .value("SLIMED", CardId::SLIMED)
        .value("SMITE", CardId::SMITE)
        .value("SNEAKY_STRIKE", CardId::SNEAKY_STRIKE)
        .value("SPIRIT_SHIELD", CardId::SPIRIT_SHIELD)
        .value("SPOT_WEAKNESS", CardId::SPOT_WEAKNESS)
        .value("STACK", CardId::STACK)
        .value("STATIC_DISCHARGE", CardId::STATIC_DISCHARGE)
        .value("STEAM_BARRIER", CardId::STEAM_BARRIER)
        .value("STORM", CardId::STORM)
        .value("STORM_OF_STEEL", CardId::STORM_OF_STEEL)
        .value("STREAMLINE", CardId::STREAMLINE)
        .value("STRIKE_BLUE", CardId::STRIKE_BLUE)
        .value("STRIKE_GREEN", CardId::STRIKE_GREEN)
        .value("STRIKE_PURPLE", CardId::STRIKE_PURPLE)
        .value("STRIKE_RED", CardId::STRIKE_RED)
        .value("STUDY", CardId::STUDY)
        .value("SUCKER_PUNCH", CardId::SUCKER_PUNCH)
        .value("SUNDER", CardId::SUNDER)
        .value("SURVIVOR", CardId::SURVIVOR)
        .value("SWEEPING_BEAM", CardId::SWEEPING_BEAM)
        .value("SWIFT_STRIKE", CardId::SWIFT_STRIKE)
        .value("SWIVEL", CardId::SWIVEL)
        .value("SWORD_BOOMERANG", CardId::SWORD_BOOMERANG)
        .value("TACTICIAN", CardId::TACTICIAN)
        .value("TALK_TO_THE_HAND", CardId::TALK_TO_THE_HAND)
        .value("TANTRUM", CardId::TANTRUM)
        .value("TEMPEST", CardId::TEMPEST)
        .value("TERROR", CardId::TERROR)
        .value("THE_BOMB", CardId::THE_BOMB)
        .value("THINKING_AHEAD", CardId::THINKING_AHEAD)
        .value("THIRD_EYE", CardId::THIRD_EYE)
        .value("THROUGH_VIOLENCE", CardId::THROUGH_VIOLENCE)
        .value("THUNDERCLAP", CardId::THUNDERCLAP)
        .value("THUNDER_STRIKE", CardId::THUNDER_STRIKE)
        .value("TOOLS_OF_THE_TRADE", CardId::TOOLS_OF_THE_TRADE)
        .value("TRANQUILITY", CardId::TRANQUILITY)
        .value("TRANSMUTATION", CardId::TRANSMUTATION)
        .value("TRIP", CardId::TRIP)
        .value("TRUE_GRIT", CardId::TRUE_GRIT)
        .value("TURBO", CardId::TURBO)
        .value("TWIN_STRIKE", CardId::TWIN_STRIKE)
        .value("UNLOAD", CardId::UNLOAD)
        .value("UPPERCUT", CardId::UPPERCUT)
        .value("VAULT", CardId::VAULT)
        .value("VIGILANCE", CardId::VIGILANCE)
        .value("VIOLENCE", CardId::VIOLENCE)
        .value("VOID", CardId::VOID)
        .value("WALLOP", CardId::WALLOP)
        .value("WARCRY", CardId::WARCRY)
        .value("WAVE_OF_THE_HAND", CardId::WAVE_OF_THE_HAND)
        .value("WEAVE", CardId::WEAVE)
        .value("WELL_LAID_PLANS", CardId::WELL_LAID_PLANS)
        .value("WHEEL_KICK", CardId::WHEEL_KICK)
        .value("WHIRLWIND", CardId::WHIRLWIND)
        .value("WHITE_NOISE", CardId::WHITE_NOISE)
        .value("WILD_STRIKE", CardId::WILD_STRIKE)
        .value("WINDMILL_STRIKE", CardId::WINDMILL_STRIKE)
        .value("WISH", CardId::WISH)
        .value("WORSHIP", CardId::WORSHIP)
        .value("WOUND", CardId::WOUND)
        .value("WRAITH_FORM", CardId::WRAITH_FORM)
        .value("WREATH_OF_FLAME", CardId::WREATH_OF_FLAME)
        .value("WRITHE", CardId::WRITHE)
        .value("ZAP", CardId::ZAP);

    pybind11::enum_<MonsterEncounter> meEnum(m, "MonsterEncounter");
    meEnum.value("INVALID", ME::INVALID)
        .value("CULTIST", ME::CULTIST)
        .value("JAW_WORM", ME::JAW_WORM)
        .value("TWO_LOUSE", ME::TWO_LOUSE)
        .value("SMALL_SLIMES", ME::SMALL_SLIMES)
        .value("BLUE_SLAVER", ME::BLUE_SLAVER)
        .value("GREMLIN_GANG", ME::GREMLIN_GANG)
        .value("LOOTER", ME::LOOTER)
        .value("LARGE_SLIME", ME::LARGE_SLIME)
        .value("LOTS_OF_SLIMES", ME::LOTS_OF_SLIMES)
        .value("EXORDIUM_THUGS", ME::EXORDIUM_THUGS)
        .value("EXORDIUM_WILDLIFE", ME::EXORDIUM_WILDLIFE)
        .value("RED_SLAVER", ME::RED_SLAVER)
        .value("THREE_LOUSE", ME::THREE_LOUSE)
        .value("TWO_FUNGI_BEASTS", ME::TWO_FUNGI_BEASTS)
        .value("GREMLIN_NOB", ME::GREMLIN_NOB)
        .value("LAGAVULIN", ME::LAGAVULIN)
        .value("THREE_SENTRIES", ME::THREE_SENTRIES)
        .value("SLIME_BOSS", ME::SLIME_BOSS)
        .value("THE_GUARDIAN", ME::THE_GUARDIAN)
        .value("HEXAGHOST", ME::HEXAGHOST)
        .value("SPHERIC_GUARDIAN", ME::SPHERIC_GUARDIAN)
        .value("CHOSEN", ME::CHOSEN)
        .value("SHELL_PARASITE", ME::SHELL_PARASITE)
        .value("THREE_BYRDS", ME::THREE_BYRDS)
        .value("TWO_THIEVES", ME::TWO_THIEVES)
        .value("CHOSEN_AND_BYRDS", ME::CHOSEN_AND_BYRDS)
        .value("SENTRY_AND_SPHERE", ME::SENTRY_AND_SPHERE)
        .value("SNAKE_PLANT", ME::SNAKE_PLANT)
        .value("SNECKO", ME::SNECKO)
        .value("CENTURION_AND_HEALER", ME::CENTURION_AND_HEALER)
        .value("CULTIST_AND_CHOSEN", ME::CULTIST_AND_CHOSEN)
        .value("THREE_CULTIST", ME::THREE_CULTIST)
        .value("SHELLED_PARASITE_AND_FUNGI", ME::SHELLED_PARASITE_AND_FUNGI)
        .value("GREMLIN_LEADER", ME::GREMLIN_LEADER)
        .value("SLAVERS", ME::SLAVERS)
        .value("BOOK_OF_STABBING", ME::BOOK_OF_STABBING)
        .value("AUTOMATON", ME::AUTOMATON)
        .value("COLLECTOR", ME::COLLECTOR)
        .value("CHAMP", ME::CHAMP)
        .value("THREE_DARKLINGS", ME::THREE_DARKLINGS)
        .value("ORB_WALKER", ME::ORB_WALKER)
        .value("THREE_SHAPES", ME::THREE_SHAPES)
        .value("SPIRE_GROWTH", ME::SPIRE_GROWTH)
        .value("TRANSIENT", ME::TRANSIENT)
        .value("FOUR_SHAPES", ME::FOUR_SHAPES)
        .value("MAW", ME::MAW)
        .value("SPHERE_AND_TWO_SHAPES", ME::SPHERE_AND_TWO_SHAPES)
        .value("JAW_WORM_HORDE", ME::JAW_WORM_HORDE)
        .value("WRITHING_MASS", ME::WRITHING_MASS)
        .value("GIANT_HEAD", ME::GIANT_HEAD)
        .value("NEMESIS", ME::NEMESIS)
        .value("REPTOMANCER", ME::REPTOMANCER)
        .value("AWAKENED_ONE", ME::AWAKENED_ONE)
        .value("TIME_EATER", ME::TIME_EATER)
        .value("DONU_AND_DECA", ME::DONU_AND_DECA)
        .value("SHIELD_AND_SPEAR", ME::SHIELD_AND_SPEAR)
        .value("THE_HEART", ME::THE_HEART)
        .value("LAGAVULIN_EVENT", ME::LAGAVULIN_EVENT)
        .value("COLOSSEUM_EVENT_SLAVERS", ME::COLOSSEUM_EVENT_SLAVERS)
        .value("COLOSSEUM_EVENT_NOBS", ME::COLOSSEUM_EVENT_NOBS)
        .value("MASKED_BANDITS_EVENT", ME::MASKED_BANDITS_EVENT)
        .value("MUSHROOMS_EVENT", ME::MUSHROOMS_EVENT)
        .value("MYSTERIOUS_SPHERE_EVENT", ME::MYSTERIOUS_SPHERE_EVENT);

    pybind11::enum_<RelicId> relicEnum(m, "RelicId");
    relicEnum.value("AKABEKO", RelicId::AKABEKO)
        .value("ART_OF_WAR", RelicId::ART_OF_WAR)
        .value("BIRD_FACED_URN", RelicId::BIRD_FACED_URN)
        .value("BLOODY_IDOL", RelicId::BLOODY_IDOL)
        .value("BLUE_CANDLE", RelicId::BLUE_CANDLE)
        .value("BRIMSTONE", RelicId::BRIMSTONE)
        .value("CALIPERS", RelicId::CALIPERS)
        .value("CAPTAINS_WHEEL", RelicId::CAPTAINS_WHEEL)
        .value("CENTENNIAL_PUZZLE", RelicId::CENTENNIAL_PUZZLE)
        .value("CERAMIC_FISH", RelicId::CERAMIC_FISH)
        .value("CHAMPION_BELT", RelicId::CHAMPION_BELT)
        .value("CHARONS_ASHES", RelicId::CHARONS_ASHES)
        .value("CHEMICAL_X", RelicId::CHEMICAL_X)
        .value("CLOAK_CLASP", RelicId::CLOAK_CLASP)
        .value("DARKSTONE_PERIAPT", RelicId::DARKSTONE_PERIAPT)
        .value("DEAD_BRANCH", RelicId::DEAD_BRANCH)
        .value("DUALITY", RelicId::DUALITY)
        .value("ECTOPLASM", RelicId::ECTOPLASM)
        .value("EMOTION_CHIP", RelicId::EMOTION_CHIP)
        .value("FROZEN_CORE", RelicId::FROZEN_CORE)
        .value("FROZEN_EYE", RelicId::FROZEN_EYE)
        .value("GAMBLING_CHIP", RelicId::GAMBLING_CHIP)
        .value("GINGER", RelicId::GINGER)
        .value("GOLDEN_EYE", RelicId::GOLDEN_EYE)
        .value("GREMLIN_HORN", RelicId::GREMLIN_HORN)
        .value("HAND_DRILL", RelicId::HAND_DRILL)
        .value("HAPPY_FLOWER", RelicId::HAPPY_FLOWER)
        .value("HORN_CLEAT", RelicId::HORN_CLEAT)
        .value("HOVERING_KITE", RelicId::HOVERING_KITE)
        .value("ICE_CREAM", RelicId::ICE_CREAM)
        .value("INCENSE_BURNER", RelicId::INCENSE_BURNER)
        .value("INK_BOTTLE", RelicId::INK_BOTTLE)
        .value("INSERTER", RelicId::INSERTER)
        .value("KUNAI", RelicId::KUNAI)
        .value("LETTER_OPENER", RelicId::LETTER_OPENER)
        .value("LIZARD_TAIL", RelicId::LIZARD_TAIL)
        .value("MAGIC_FLOWER", RelicId::MAGIC_FLOWER)
        .value("MARK_OF_THE_BLOOM", RelicId::MARK_OF_THE_BLOOM)
        .value("MEDICAL_KIT", RelicId::MEDICAL_KIT)
        .value("MELANGE", RelicId::MELANGE)
        .value("MERCURY_HOURGLASS", RelicId::MERCURY_HOURGLASS)
        .value("MUMMIFIED_HAND", RelicId::MUMMIFIED_HAND)
        .value("NECRONOMICON", RelicId::NECRONOMICON)
        .value("NILRYS_CODEX", RelicId::NILRYS_CODEX)
        .value("NUNCHAKU", RelicId::NUNCHAKU)
        .value("ODD_MUSHROOM", RelicId::ODD_MUSHROOM)
        .value("OMAMORI", RelicId::OMAMORI)
        .value("ORANGE_PELLETS", RelicId::ORANGE_PELLETS)
        .value("ORICHALCUM", RelicId::ORICHALCUM)
        .value("ORNAMENTAL_FAN", RelicId::ORNAMENTAL_FAN)
        .value("PAPER_KRANE", RelicId::PAPER_KRANE)
        .value("PAPER_PHROG", RelicId::PAPER_PHROG)
        .value("PEN_NIB", RelicId::PEN_NIB)
        .value("PHILOSOPHERS_STONE", RelicId::PHILOSOPHERS_STONE)
        .value("POCKETWATCH", RelicId::POCKETWATCH)
        .value("RED_SKULL", RelicId::RED_SKULL)
        .value("RUNIC_CUBE", RelicId::RUNIC_CUBE)
        .value("RUNIC_DOME", RelicId::RUNIC_DOME)
        .value("RUNIC_PYRAMID", RelicId::RUNIC_PYRAMID)
        .value("SACRED_BARK", RelicId::SACRED_BARK)
        .value("SELF_FORMING_CLAY", RelicId::SELF_FORMING_CLAY)
        .value("SHURIKEN", RelicId::SHURIKEN)
        .value("SNECKO_EYE", RelicId::SNECKO_EYE)
        .value("SNECKO_SKULL", RelicId::SNECKO_SKULL)
        .value("SOZU", RelicId::SOZU)
        .value("STONE_CALENDAR", RelicId::STONE_CALENDAR)
        .value("STRANGE_SPOON", RelicId::STRANGE_SPOON)
        .value("STRIKE_DUMMY", RelicId::STRIKE_DUMMY)
        .value("SUNDIAL", RelicId::SUNDIAL)
        .value("THE_ABACUS", RelicId::THE_ABACUS)
        .value("THE_BOOT", RelicId::THE_BOOT)
        .value("THE_SPECIMEN", RelicId::THE_SPECIMEN)
        .value("TINGSHA", RelicId::TINGSHA)
        .value("TOOLBOX", RelicId::TOOLBOX)
        .value("TORII", RelicId::TORII)
        .value("TOUGH_BANDAGES", RelicId::TOUGH_BANDAGES)
        .value("TOY_ORNITHOPTER", RelicId::TOY_ORNITHOPTER)
        .value("TUNGSTEN_ROD", RelicId::TUNGSTEN_ROD)
        .value("TURNIP", RelicId::TURNIP)
        .value("TWISTED_FUNNEL", RelicId::TWISTED_FUNNEL)
        .value("UNCEASING_TOP", RelicId::UNCEASING_TOP)
        .value("VELVET_CHOKER", RelicId::VELVET_CHOKER)
        .value("VIOLET_LOTUS", RelicId::VIOLET_LOTUS)
        .value("WARPED_TONGS", RelicId::WARPED_TONGS)
        .value("WRIST_BLADE", RelicId::WRIST_BLADE)
        .value("BLACK_BLOOD", RelicId::BLACK_BLOOD)
        .value("BURNING_BLOOD", RelicId::BURNING_BLOOD)
        .value("MEAT_ON_THE_BONE", RelicId::MEAT_ON_THE_BONE)
        .value("FACE_OF_CLERIC", RelicId::FACE_OF_CLERIC)
        .value("ANCHOR", RelicId::ANCHOR)
        .value("ANCIENT_TEA_SET", RelicId::ANCIENT_TEA_SET)
        .value("BAG_OF_MARBLES", RelicId::BAG_OF_MARBLES)
        .value("BAG_OF_PREPARATION", RelicId::BAG_OF_PREPARATION)
        .value("BLOOD_VIAL", RelicId::BLOOD_VIAL)
        .value("BOTTLED_FLAME", RelicId::BOTTLED_FLAME)
        .value("BOTTLED_LIGHTNING", RelicId::BOTTLED_LIGHTNING)
        .value("BOTTLED_TORNADO", RelicId::BOTTLED_TORNADO)
        .value("BRONZE_SCALES", RelicId::BRONZE_SCALES)
        .value("BUSTED_CROWN", RelicId::BUSTED_CROWN)
        .value("CLOCKWORK_SOUVENIR", RelicId::CLOCKWORK_SOUVENIR)
        .value("COFFEE_DRIPPER", RelicId::COFFEE_DRIPPER)
        .value("CRACKED_CORE", RelicId::CRACKED_CORE)
        .value("CURSED_KEY", RelicId::CURSED_KEY)
        .value("DAMARU", RelicId::DAMARU)
        .value("DATA_DISK", RelicId::DATA_DISK)
        .value("DU_VU_DOLL", RelicId::DU_VU_DOLL)
        .value("ENCHIRIDION", RelicId::ENCHIRIDION)
        .value("FOSSILIZED_HELIX", RelicId::FOSSILIZED_HELIX)
        .value("FUSION_HAMMER", RelicId::FUSION_HAMMER)
        .value("GIRYA", RelicId::GIRYA)
        .value("GOLD_PLATED_CABLES", RelicId::GOLD_PLATED_CABLES)
        .value("GREMLIN_VISAGE", RelicId::GREMLIN_VISAGE)
        .value("HOLY_WATER", RelicId::HOLY_WATER)
        .value("LANTERN", RelicId::LANTERN)
        .value("MARK_OF_PAIN", RelicId::MARK_OF_PAIN)
        .value("MUTAGENIC_STRENGTH", RelicId::MUTAGENIC_STRENGTH)
        .value("NEOWS_LAMENT", RelicId::NEOWS_LAMENT)
        .value("NINJA_SCROLL", RelicId::NINJA_SCROLL)
        .value("NUCLEAR_BATTERY", RelicId::NUCLEAR_BATTERY)
        .value("ODDLY_SMOOTH_STONE", RelicId::ODDLY_SMOOTH_STONE)
        .value("PANTOGRAPH", RelicId::PANTOGRAPH)
        .value("PRESERVED_INSECT", RelicId::PRESERVED_INSECT)
        .value("PURE_WATER", RelicId::PURE_WATER)
        .value("RED_MASK", RelicId::RED_MASK)
        .value("RING_OF_THE_SERPENT", RelicId::RING_OF_THE_SERPENT)
        .value("RING_OF_THE_SNAKE", RelicId::RING_OF_THE_SNAKE)
        .value("RUNIC_CAPACITOR", RelicId::RUNIC_CAPACITOR)
        .value("SLAVERS_COLLAR", RelicId::SLAVERS_COLLAR)
        .value("SLING_OF_COURAGE", RelicId::SLING_OF_COURAGE)
        .value("SYMBIOTIC_VIRUS", RelicId::SYMBIOTIC_VIRUS)
        .value("TEARDROP_LOCKET", RelicId::TEARDROP_LOCKET)
        .value("THREAD_AND_NEEDLE", RelicId::THREAD_AND_NEEDLE)
        .value("VAJRA", RelicId::VAJRA)
        .value("ASTROLABE", RelicId::ASTROLABE)
        .value("BLACK_STAR", RelicId::BLACK_STAR)
        .value("CALLING_BELL", RelicId::CALLING_BELL)
        .value("CAULDRON", RelicId::CAULDRON)
        .value("CULTIST_HEADPIECE", RelicId::CULTIST_HEADPIECE)
        .value("DOLLYS_MIRROR", RelicId::DOLLYS_MIRROR)
        .value("DREAM_CATCHER", RelicId::DREAM_CATCHER)
        .value("EMPTY_CAGE", RelicId::EMPTY_CAGE)
        .value("ETERNAL_FEATHER", RelicId::ETERNAL_FEATHER)
        .value("FROZEN_EGG", RelicId::FROZEN_EGG)
        .value("GOLDEN_IDOL", RelicId::GOLDEN_IDOL)
        .value("JUZU_BRACELET", RelicId::JUZU_BRACELET)
        .value("LEES_WAFFLE", RelicId::LEES_WAFFLE)
        .value("MANGO", RelicId::MANGO)
        .value("MATRYOSHKA", RelicId::MATRYOSHKA)
        .value("MAW_BANK", RelicId::MAW_BANK)
        .value("MEAL_TICKET", RelicId::MEAL_TICKET)
        .value("MEMBERSHIP_CARD", RelicId::MEMBERSHIP_CARD)
        .value("MOLTEN_EGG", RelicId::MOLTEN_EGG)
        .value("NLOTHS_GIFT", RelicId::NLOTHS_GIFT)
        .value("NLOTHS_HUNGRY_FACE", RelicId::NLOTHS_HUNGRY_FACE)
        .value("OLD_COIN", RelicId::OLD_COIN)
        .value("ORRERY", RelicId::ORRERY)
        .value("PANDORAS_BOX", RelicId::PANDORAS_BOX)
        .value("PEACE_PIPE", RelicId::PEACE_PIPE)
        .value("PEAR", RelicId::PEAR)
        .value("POTION_BELT", RelicId::POTION_BELT)
        .value("PRAYER_WHEEL", RelicId::PRAYER_WHEEL)
        .value("PRISMATIC_SHARD", RelicId::PRISMATIC_SHARD)
        .value("QUESTION_CARD", RelicId::QUESTION_CARD)
        .value("REGAL_PILLOW", RelicId::REGAL_PILLOW)
        .value("SSSERPENT_HEAD", RelicId::SSSERPENT_HEAD)
        .value("SHOVEL", RelicId::SHOVEL)
        .value("SINGING_BOWL", RelicId::SINGING_BOWL)
        .value("SMILING_MASK", RelicId::SMILING_MASK)
        .value("SPIRIT_POOP", RelicId::SPIRIT_POOP)
        .value("STRAWBERRY", RelicId::STRAWBERRY)
        .value("THE_COURIER", RelicId::THE_COURIER)
        .value("TINY_CHEST", RelicId::TINY_CHEST)
        .value("TINY_HOUSE", RelicId::TINY_HOUSE)
        .value("TOXIC_EGG", RelicId::TOXIC_EGG)
        .value("WAR_PAINT", RelicId::WAR_PAINT)
        .value("WHETSTONE", RelicId::WHETSTONE)
        .value("WHITE_BEAST_STATUE", RelicId::WHITE_BEAST_STATUE)
        .value("WING_BOOTS", RelicId::WING_BOOTS)
        .value("CIRCLET", RelicId::CIRCLET)
        .value("RED_CIRCLET", RelicId::RED_CIRCLET)
        .value("INVALID", RelicId::INVALID);

    // ******************** BATTLE / FORWARD-SEARCH API ********************

    pybind11::enum_<Outcome>(m, "Outcome")
        .value("UNDECIDED", Outcome::UNDECIDED)
        .value("PLAYER_VICTORY", Outcome::PLAYER_VICTORY)
        .value("PLAYER_LOSS", Outcome::PLAYER_LOSS);

    pybind11::enum_<InputState>(m, "InputState")
        .value("EXECUTING_ACTIONS", InputState::EXECUTING_ACTIONS)
        .value("PLAYER_NORMAL", InputState::PLAYER_NORMAL)
        .value("CARD_SELECT", InputState::CARD_SELECT);

    pybind11::enum_<search::ActionType>(m, "SearchActionType")
        .value("CARD", search::ActionType::CARD)
        .value("POTION", search::ActionType::POTION)
        .value("SINGLE_CARD_SELECT", search::ActionType::SINGLE_CARD_SELECT)
        .value("MULTI_CARD_SELECT", search::ActionType::MULTI_CARD_SELECT)
        .value("END_TURN", search::ActionType::END_TURN);

    // a useful subset of player statuses for has_status / get_status
    pybind11::enum_<PlayerStatus>(m, "PlayerStatus")
        .value("VULNERABLE", PlayerStatus::VULNERABLE)
        .value("WEAK", PlayerStatus::WEAK)
        .value("FRAIL", PlayerStatus::FRAIL)
        .value("STRENGTH", PlayerStatus::STRENGTH)
        .value("DEXTERITY", PlayerStatus::DEXTERITY)
        .value("FOCUS", PlayerStatus::FOCUS)
        .value("ARTIFACT", PlayerStatus::ARTIFACT)
        .value("METALLICIZE", PlayerStatus::METALLICIZE)
        .value("REGEN", PlayerStatus::REGEN)
        .value("PLATED_ARMOR", PlayerStatus::PLATED_ARMOR)
        .value("DEMON_FORM", PlayerStatus::DEMON_FORM)
        .value("RITUAL", PlayerStatus::RITUAL)
        .value("THORNS", PlayerStatus::THORNS)
        .value("BARRICADE", PlayerStatus::BARRICADE)
        .value("INTANGIBLE", PlayerStatus::INTANGIBLE)
        .value("NO_BLOCK", PlayerStatus::NO_BLOCK)
        .value("ENTANGLED", PlayerStatus::ENTANGLED);

    pybind11::enum_<MonsterStatus>(m, "MonsterStatus")
        .value("VULNERABLE", MonsterStatus::VULNERABLE)
        .value("WEAK", MonsterStatus::WEAK)
        .value("POISON", MonsterStatus::POISON)
        .value("STRENGTH", MonsterStatus::STRENGTH)
        .value("ARTIFACT", MonsterStatus::ARTIFACT)
        .value("METALLICIZE", MonsterStatus::METALLICIZE)
        .value("REGEN", MonsterStatus::REGEN)
        .value("PLATED_ARMOR", MonsterStatus::PLATED_ARMOR)
        .value("BLOCK_RETURN", MonsterStatus::BLOCK_RETURN)
        .value("MARK", MonsterStatus::MARK)
        .value("LOCK_ON", MonsterStatus::LOCK_ON)
        .value("SHACKLED", MonsterStatus::SHACKLED)
        .value("INTANGIBLE", MonsterStatus::INTANGIBLE)
        .value("CHOKED", MonsterStatus::CHOKED);

    pybind11::class_<DamageInfo>(m, "DamageInfo")
        .def_readonly("damage", &DamageInfo::damage)
        .def_readonly("attack_count", &DamageInfo::attackCount);

    pybind11::class_<CardInstance>(m, "CardInstance")
        .def_property_readonly("id", &CardInstance::getId)
        .def_property_readonly("name", [](const CardInstance &c) { return std::string(c.getName()); })
        .def_property_readonly("type", &CardInstance::getType)
        .def_property_readonly("upgraded", &CardInstance::isUpgraded)
        .def_readonly("cost", &CardInstance::cost)
        .def_readonly("cost_for_turn", &CardInstance::costForTurn)
        .def_property_readonly("requires_target", &CardInstance::requiresTarget)
        .def("__repr__", [](const CardInstance &c) {
            std::ostringstream os;
            os << "<slaythespire.CardInstance " << c.getName() << ">";
            return os.str();
        });

    pybind11::enum_<Orb>(m, "Orb")
        .value("EMPTY", Orb::EMPTY).value("DARK", Orb::DARK).value("FROST", Orb::FROST)
        .value("LIGHTNING", Orb::LIGHTNING).value("PLASMA", Orb::PLASMA);

    pybind11::class_<Player>(m, "Player")
        .def_readonly("cur_hp", &Player::curHp)
        .def_readonly("max_hp", &Player::maxHp)
        .def_readonly("block", &Player::block)
        .def_readonly("energy", &Player::energy)
        .def_readonly("strength", &Player::strength)
        .def_readonly("dexterity", &Player::dexterity)
        .def_readonly("focus", &Player::focus)
        .def_readonly("artifact", &Player::artifact)
        .def_readonly("orb_slots", &Player::orbSlots)
        .def_property_readonly("orbs", [](const Player &p) {
            std::vector<std::pair<int,int>> out;
            for (int i = 0; i < p.orbSlots && i < Player::MAX_ORB_SLOTS; ++i)
                out.emplace_back(static_cast<int>(p.orbTypes[i]), static_cast<int>(p.orbData[i]));
            return out;
        }, "list of (orb type, orb data) per slot; type: 0 EMPTY 1 DARK 2 FROST 3 LIGHTNING 4 PLASMA")
        .def("has_status", &Player::hasStatusRuntime)
        .def("get_status", &Player::getStatusRuntime);

    pybind11::class_<Monster>(m, "Monster")
        .def_readonly("cur_hp", &Monster::curHp)
        .def_readonly("max_hp", &Monster::maxHp)
        .def_readonly("block", &Monster::block)
        .def_readonly("strength", &Monster::strength)
        .def_readonly("vulnerable", &Monster::vulnerable)
        .def_readonly("weak", &Monster::weak)
        .def_readonly("poison", &Monster::poison)
        .def_property_readonly("name", [](const Monster &mo) { return std::string(mo.getName()); })
        .def_property_readonly("alive", &Monster::isAlive)
        .def_property_readonly("targetable", &Monster::isTargetable)
        .def_property_readonly("is_attacking", &Monster::isAttacking)
        .def_property_readonly("intent", [](const Monster &mo) {
            if (mo.moveHistory[0] == MMID::INVALID) {
                return std::string("UNKNOWN");
            }
            return std::string(monsterMoveStrings[static_cast<int>(mo.moveHistory[0])]);
        })
        .def("intent_damage", &Monster::getMoveBaseDamage)
        .def("has_status", &Monster::hasStatusInternal)
        .def("get_status", &Monster::getStatusInternal);

    pybind11::class_<search::Action>(m, "SearchAction")
        .def(pybind11::init<search::ActionType>())
        .def(pybind11::init<search::ActionType, int>())
        .def(pybind11::init<search::ActionType, int, int>())
        .def_property_readonly("action_type", &search::Action::getActionType)
        .def_property_readonly("source_idx", &search::Action::getSourceIdx)
        .def_property_readonly("target_idx", &search::Action::getTargetIdx)
        .def_property_readonly("select_idx", &search::Action::getSelectIdx)
        .def_property_readonly("selected_idxs", [](const search::Action &a) {
            const auto idxs = a.getSelectedIdxs();
            return std::vector<int>(idxs.begin(), idxs.end());
        })
        .def_readonly("bits", &search::Action::bits)
        .def("is_valid", &search::Action::isValidAction)
        .def("execute", &search::Action::execute)
        .def("desc", [](const search::Action &a, const BattleContext &bc) {
            std::ostringstream os;
            a.printDesc(os, bc);
            return os.str();
        })
        .def("__repr__", [](const search::Action &a) {
            std::ostringstream os;
            os << "<slaythespire.SearchAction type=" << static_cast<int>(a.getActionType())
               << " src=" << a.getSourceIdx() << " tgt=" << a.getTargetIdx() << ">";
            return os.str();
        });

    pybind11::class_<BattleContext>(m, "BattleContext")
        .def(pybind11::init<>())
        .def("init", pybind11::overload_cast<const GameContext&>(&BattleContext::init),
             "initialize the battle from a GameContext that is in the BATTLE screen state")
        .def("init_encounter",
             [](BattleContext &bc, const GameContext &gc, MonsterEncounter me) { bc.init(gc, me, false); },
             "initialize the battle from a GameContext with an explicit monster encounter")
        .def("clone", [](const BattleContext &bc) { return BattleContext(bc); },
             "returns a deep copy of the battle (value semantics, RNG included) for deterministic lookahead")
        .def_static("from_snapshot", &battleFromSnapshot,
             "reconstruct a search state from a validated Steam combat snapshot and a determinization seed")
        .def("exit_battle", &BattleContext::exitBattle,
             "write the battle results back into the GameContext")
        .def_readonly("outcome", &BattleContext::outcome)
        .def_readonly("input_state", &BattleContext::inputState)
        .def_readonly("turn", &BattleContext::turn)
        .def_readonly("encounter", &BattleContext::encounter)
        .def_readonly("player", &BattleContext::player)
        .def_property_readonly("monsters", [](const BattleContext &bc) {
            const int count = bc.monsters.monsterCount;
            return std::vector<Monster>(bc.monsters.arr.begin(), bc.monsters.arr.begin() + count);
        }, "returns the live monsters in the battle (truncated to monsterCount)")
        .def_property_readonly("hand", [](const BattleContext &bc) {
            const int count = bc.cards.cardsInHand;
            return std::vector<CardInstance>(bc.cards.hand.begin(), bc.cards.hand.begin() + count);
        }, "returns the cards currently in hand (truncated to cardsInHand)")
        .def_property_readonly("draw_pile", [](const BattleContext &bc) {
            return std::vector<CardInstance>(bc.cards.drawPile.begin(), bc.cards.drawPile.end());
        }, "cards currently in the draw pile")
        .def_property_readonly("discard_pile", [](const BattleContext &bc) {
            return std::vector<CardInstance>(bc.cards.discardPile.begin(), bc.cards.discardPile.end());
        }, "cards currently in the discard pile")
        .def_property_readonly("exhaust_pile", [](const BattleContext &bc) {
            return std::vector<CardInstance>(bc.cards.exhaustPile.begin(), bc.cards.exhaustPile.end());
        }, "cards currently in the exhaust pile")
        .def_property_readonly("rng_states", [](const BattleContext &bc) {
            const auto state = [](const Random &rng) {
                pybind11::dict value;
                value["seed0"] = rng.seed0;
                value["seed1"] = rng.seed1;
                value["counter"] = rng.counter;
                return value;
            };
            pybind11::dict values;
            values["ai"] = state(bc.aiRng);
            values["card_random"] = state(bc.cardRandomRng);
            values["misc"] = state(bc.miscRng);
            values["monster_hp"] = state(bc.monsterHpRng);
            values["potion"] = state(bc.potionRng);
            values["shuffle"] = state(bc.shuffleRng);
            return values;
        })
        .def_property_readonly("snapshot_counters", [](const BattleContext &bc) {
            pybind11::dict values;
            values["energy_per_turn"] = bc.player.energyPerTurn;
            values["happy_flower"] = bc.player.happyFlowerCounter;
            values["incense_burner"] = bc.player.incenseBurnerCounter;
            values["ink_bottle"] = bc.player.inkBottleCounter;
            values["inserter"] = bc.player.inserterCounter;
            values["nunchaku"] = bc.player.nunchakuCounter;
            values["pen_nib"] = bc.player.penNibCounter;
            values["sundial"] = bc.player.sundialCounter;
            return values;
        })
        .def("__repr__", [](const BattleContext &bc) {
            std::ostringstream os;
            os << bc;
            return os.str();
        });

    m.def("get_legal_actions", &sts::py::getLegalActions,
          "enumerate all legal actions in the current battle state for forward search");

    // 根并行 MCTS:threads 份世界副本(错开 RNG 计数器 = 不同未来抽牌序列),各搜 sims 次,按每份的推荐动作投票;
    // 平票取该动作累计根访问最多者。释放 GIL 让 std::thread 真并行。
    m.def("mcts_recommend_parallel",
          [](const BattleContext &bc, int sims, int threads) {
              threads = std::max(1, threads);
              std::vector<std::unique_ptr<search::BattleScumSearcher2>> searchers;
              std::vector<BattleContext> worlds;
              worlds.reserve(threads);
              for (int i = 0; i < threads; ++i) {
                  BattleContext world = bc;
                  if (i > 0) {
                      world.cardRandomRng.setCounter(world.cardRandomRng.counter + i * 1000);
                      world.aiRng.setCounter(world.aiRng.counter + i * 1000);
                      world.shuffleRng.setCounter(world.shuffleRng.counter + i * 1000);
                      world.miscRng.setCounter(world.miscRng.counter + i * 1000);
                      world.potionRng.setCounter(world.potionRng.counter + i * 1000);
                  }
                  worlds.push_back(world);
              }
              for (int i = 0; i < threads; ++i) {
                  searchers.push_back(std::make_unique<search::BattleScumSearcher2>(worlds[i]));
                  searchers.back()->allowPotions = true;
              }
              {
                  pybind11::gil_scoped_release release;
                  std::vector<std::thread> pool;
                  for (int i = 0; i < threads; ++i) {
                      pool.emplace_back([&, i]() { searchers[i]->search(sims, std::numeric_limits<long>::max()); });
                  }
                  for (auto &t : pool) t.join();
              }
              auto pick = [](const search::BattleScumSearcher2 &s) -> std::pair<search::Action, long> {
                  if (s.outcomePlayerHp > 0 && !s.bestActionSequence.empty()) return {s.bestActionSequence.front(), s.root.simulationCount};
                  const search::Action *best = nullptr; long maxsim = -1;
                  for (const auto &e : s.root.edges) if (e.node->simulationCount > maxsim) { maxsim = e.node->simulationCount; best = &e.action; }
                  return {best ? *best : search::Action(search::ActionType::END_TURN), maxsim};
              };
              std::vector<std::pair<search::Action, int>> tally;      // (action, votes)
              std::vector<long> weight;
              for (const auto &s : searchers) {
                  auto [a, w] = pick(*s);
                  bool found = false;
                  for (size_t k = 0; k < tally.size(); ++k) if (tally[k].first == a) { tally[k].second++; weight[k] += w; found = true; break; }
                  if (!found) { tally.emplace_back(a, 1); weight.push_back(w); }
              }
              size_t bi = 0;
              for (size_t k = 1; k < tally.size(); ++k)
                  if (tally[k].second > tally[bi].second || (tally[k].second == tally[bi].second && weight[k] > weight[bi])) bi = k;
              std::vector<std::pair<std::string, int>> votes;
              for (const auto &[a, n] : tally) { std::ostringstream os; a.printDesc(os, bc); votes.emplace_back(os.str(), n); }
              return std::make_pair(tally[bi].first, votes);
          },
          "root-parallel MCTS over `threads` RNG-offset worlds, `sims` simulations each; returns (action, [(desc, votes)])");

    m.def("mcts_recommend",
          [](const BattleContext &bc, int sims) {
              search::BattleScumSearcher2 searcher(bc);     // copies bc internally
              searcher.allowPotions = true;                  // 与模拟器 ScumSearchAgent2 一致:搜索可用药水(掩码 0 = 不限槽位)
              searcher.search(sims, std::numeric_limits<long>::max());
              if (searcher.outcomePlayerHp > 0 && !searcher.bestActionSequence.empty()) {
                  return searcher.bestActionSequence.front();   // first action of the best found line
              }
              const search::Action *best = nullptr; long maxsim = -1;   // else most-visited root edge
              for (const auto &e : searcher.root.edges) {
                  if (e.node->simulationCount > maxsim) { maxsim = e.node->simulationCount; best = &e.action; }
              }
              return best ? *best : search::Action(search::ActionType::END_TURN);
          },
          "run MCTS from the given battle state and return its recommended next action (BC teacher oracle)");

    // ******************** OUT-OF-COMBAT GAME ACTION API ********************
    // generic interface to drive map / rest / shop / event / rewards decisions from python,
    // mirroring the in-combat forward-search api (get_legal_game_actions + GameAction.execute)

    pybind11::enum_<search::GameAction::RewardsActionType>(m, "RewardsActionType")
        .value("CARD", search::GameAction::RewardsActionType::CARD)
        .value("GOLD", search::GameAction::RewardsActionType::GOLD)
        .value("KEY", search::GameAction::RewardsActionType::KEY)
        .value("POTION", search::GameAction::RewardsActionType::POTION)
        .value("RELIC", search::GameAction::RewardsActionType::RELIC)
        .value("CARD_REMOVE", search::GameAction::RewardsActionType::CARD_REMOVE)
        .value("SKIP", search::GameAction::RewardsActionType::SKIP);

    pybind11::class_<search::GameAction>(m, "GameAction")
        .def(pybind11::init<std::uint32_t>())
        .def_readonly("bits", &search::GameAction::bits)
        .def_property_readonly("idx1", &search::GameAction::getIdx1, "primary index: map node x / rest option / event option / shop slot / reward slot")
        .def_property_readonly("idx2", &search::GameAction::getIdx2)
        .def_property_readonly("idx3", &search::GameAction::getIdx3)
        .def_property_readonly("is_potion_action", &search::GameAction::isPotionAction)
        .def_property_readonly("rewards_action_type", &search::GameAction::getRewardsActionType,
                               "only meaningful on REWARDS / SHOP screens (CARD/RELIC/POTION/CARD_REMOVE/SKIP)")
        .def("is_valid", &search::GameAction::isValidAction)
        .def("execute", &search::GameAction::execute, "apply this choice to the GameContext")
        .def("__repr__", [](const search::GameAction &a) {
            std::ostringstream os;
            os << "<slaythespire.GameAction bits=" << a.bits
               << " idx1=" << a.getIdx1() << " idx2=" << a.getIdx2() << ">";
            return os.str();
        });

    m.def("get_legal_game_actions",
          [](const GameContext &gc) { return search::GameAction::getAllActionsInState(gc); },
          "enumerate all legal out-of-combat game actions in the current screen state");

#ifdef VERSION_INFO
    m.attr("__version__") = MACRO_STRINGIFY(VERSION_INFO);
#else
    m.attr("__version__") = "dev";
#endif
}

// os.add_dll_directory("C:\\Program Files\\mingw-w64\\x86_64-8.1.0-posix-seh-rt_v6-rev0\\mingw64\\bin")
