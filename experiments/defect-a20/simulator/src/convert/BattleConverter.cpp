#include "convert/BattleConverter.h"
#include "combat/BattleContext.h"
#include "game/GameContext.h"

#include <nlohmann/json.hpp>
#include <array>
#include <stdexcept>

using namespace sts;

namespace {

bool shouldImportMonster(const nlohmann::json &monster) {
    // CommunicationMod marks temporarily untargetable, reviving monsters as
    // gone as well.  Unlike an ordinary dead summon, a half-dead monster must
    // remain in the simulator so its queued rebirth move can run.
    return !monster.value("is_gone", false)
        || monster.value("half_dead", false);
}

bool isAwakenedOnePhaseTwoMove(const MonsterMoveId move) {
    switch (move) {
        case MMID::AWAKENED_ONE_DARK_ECHO:
        case MMID::AWAKENED_ONE_SLUDGE:
        case MMID::AWAKENED_ONE_TACKLE:
            return true;
        default:
            return false;
    }
}

bool jsonMonsterMoveIs(
    const nlohmann::json &monster,
    const char *field,
    MonsterId monsterId,
    MonsterMoveId expected
) {
    const auto it = monster.find(field);
    return it != monster.end()
        && it->is_number_integer()
        && getMonsterMoveFromId(monsterId, it->get<int>()) == expected;
}

CardInstance importCardInstance(
    const nlohmann::json &card,
    int uniqueCardId
) {
    const CardId cardId = getCardIdFromId(card["id"]);
    const int upgradeCount = card.value("upgrades", 0);
    CardInstance instance(cardId, upgradeCount > 0);
    instance.cost = static_cast<std::int8_t>(
        card.value("combat_cost", static_cast<int>(instance.cost))
    );
    instance.costForTurn = static_cast<std::int8_t>(
        card.value("cost", static_cast<int>(instance.costForTurn))
    );
    instance.uniqueId = uniqueCardId;
    if (cardId == CardId::SEARING_BLOW) {
        instance.specialData = static_cast<std::int16_t>(upgradeCount);
    } else if (cardId == CardId::CLAW) {
        if (!card.contains("base_damage") || !card["base_damage"].is_number_integer()) {
            throw std::runtime_error(
                "Defect Claw state is missing integer base_damage from AgentStateFixes"
            );
        }
        const int initialDamage = instance.upgraded ? 5 : 3;
        instance.specialData = static_cast<std::int16_t>(
            std::max(0, card["base_damage"].get<int>() - initialDamage)
        );
    } else if (cardId == CardId::STEAM_BARRIER) {
        if (!card.contains("base_block") || !card["base_block"].is_number_integer()) {
            throw std::runtime_error(
                "Defect Steam Barrier state is missing integer base_block from AgentStateFixes"
            );
        }
        const int initialBlock = instance.upgraded ? 8 : 6;
        instance.specialData = static_cast<std::int16_t>(
            std::max(0, initialBlock - card["base_block"].get<int>())
        );
    } else {
        instance.specialData = static_cast<std::int16_t>(card.value("misc", 0));
    }
    return instance;
}

int inferGremlinWizardCharge(const nlohmann::json &monster) {
    if (jsonMonsterMoveIs(
            monster,
            "move_id",
            MonsterId::GREMLIN_WIZARD,
            MMID::GREMLIN_WIZARD_ULTIMATE_BLAST
        )) {
        return 3;
    }
    if (!jsonMonsterMoveIs(
            monster,
            "move_id",
            MonsterId::GREMLIN_WIZARD,
            MMID::GREMLIN_WIZARD_CHARGING
        )) {
        return 0;
    }

    // CommunicationMod does not expose the private charge counter. A freshly
    // created Wizard starts at one; two consecutive Charging intents mean the
    // next monster turn completes the charge and reveals Ultimate Blast.
    if (jsonMonsterMoveIs(
            monster,
            "last_move_id",
            MonsterId::GREMLIN_WIZARD,
            MMID::GREMLIN_WIZARD_ULTIMATE_BLAST
        )) {
        return 0;
    }
    if (jsonMonsterMoveIs(
            monster,
            "last_move_id",
            MonsterId::GREMLIN_WIZARD,
            MMID::GREMLIN_WIZARD_CHARGING
        )) {
        // Below A17 the Wizard starts another cycle after blasting. During
        // that cycle, second_last_move_id disambiguates its first charge from
        // the initial encounter's second charge.
        if (jsonMonsterMoveIs(
                monster,
                "second_last_move_id",
                MonsterId::GREMLIN_WIZARD,
                MMID::GREMLIN_WIZARD_ULTIMATE_BLAST
            )) {
            return 1;
        }
        return 2;
    }
    return 1;
}

int inferThiefStolenGold(
    const nlohmann::json &monster,
    MonsterId monsterId,
    int combatTurn,
    int thieveryAmount
) {
    if (thieveryAmount <= 0 || combatTurn <= 1) {
        return 0;
    }

    // CommunicationMod exposes Thievery but not the private counter holding
    // already stolen gold. Both thieves always Mug on their first two monster
    // turns; their only additional stealing move is Lunge. Reconstruct that
    // private state so a mid-combat MCTS search can value killing the thief and
    // recovering the gold instead of treating escape as a free victory.
    int stealingMoves = std::min(combatTurn - 1, 2);
    const auto lunge = monsterId == MonsterId::LOOTER
        ? MMID::LOOTER_LUNGE
        : MMID::MUGGER_LUNGE;
    if (jsonMonsterMoveIs(monster, "last_move_id", monsterId, lunge)
        || jsonMonsterMoveIs(
            monster,
            "second_last_move_id",
            monsterId,
            lunge
        )) {
        ++stealingMoves;
    }
    return stealingMoves * thieveryAmount;
}

int inferHexaghostActiveOrbs(
    int combatTurn,
    MonsterMoveId currentMove
) {
    if (combatTurn < 1) {
        throw std::runtime_error(
            "cannot infer Hexaghost active_orbs before combat turn 1"
        );
    }

    int activeOrbs = 0;
    MonsterMoveId expectedMove;
    if (combatTurn == 1) {
        expectedMove = MMID::HEXAGHOST_ACTIVATE;
    } else if (combatTurn == 2) {
        expectedMove = MMID::HEXAGHOST_DIVIDER;
    } else {
        activeOrbs = (combatTurn - 3) % 7;
        switch (activeOrbs) {
            case 0:
            case 2:
            case 5:
                expectedMove = MMID::HEXAGHOST_SEAR;
                break;
            case 1:
            case 4:
                expectedMove = MMID::HEXAGHOST_TACKLE;
                break;
            case 3:
                expectedMove = MMID::HEXAGHOST_INFLAME;
                break;
            case 6:
                expectedMove = MMID::HEXAGHOST_INFERNO;
                break;
            default:
                throw std::runtime_error(
                    "invalid inferred Hexaghost active_orbs"
                );
        }
    }

    if (currentMove != expectedMove) {
        throw std::runtime_error(
            "cannot infer Hexaghost active_orbs: combat turn "
            + std::to_string(combatTurn)
            + " is inconsistent with current move "
            + std::to_string(static_cast<int>(currentMove))
        );
    }
    return activeOrbs;
}

int inferBronzeAutomatonBoostPhase(
    int combatTurn,
    int ascension,
    MonsterMoveId currentMove
) {
    if (combatTurn < 1) {
        throw std::runtime_error(
            "cannot infer Bronze Automaton phase before combat turn 1"
        );
    }

    // Bronze Automaton's move sequence is deterministic, but the simulator
    // stores one private bit to distinguish the first Boost (followed by
    // Flail) from the second Boost (followed by Hyper Beam). CommunicationMod
    // omits that bit, so replay the fixed transition sequence up to the
    // imported player turn and validate it against the visible intent.
    // 0: the next ordinary Boost is followed by Flail.
    // 1: the next ordinary Boost is followed by Hyper Beam.
    // -1: A19+'s immediate post-Hyper-Beam Boost; it is followed by Flail
    //     without advancing the ordinary two-Boost cycle.
    int boostPhase = 0;
    MonsterMoveId expectedMove = MMID::BRONZE_AUTOMATON_SPAWN_ORBS;
    for (int turn = 1; turn < combatTurn; ++turn) {
        switch (expectedMove) {
            case MMID::BRONZE_AUTOMATON_SPAWN_ORBS:
            case MMID::BRONZE_AUTOMATON_STUNNED:
                expectedMove = MMID::BRONZE_AUTOMATON_FLAIL;
                break;
            case MMID::BRONZE_AUTOMATON_FLAIL:
                expectedMove = MMID::BRONZE_AUTOMATON_BOOST;
                break;
            case MMID::BRONZE_AUTOMATON_BOOST:
                if (boostPhase < 0) {
                    expectedMove = MMID::BRONZE_AUTOMATON_FLAIL;
                    boostPhase = 0;
                } else if (boostPhase > 0) {
                    expectedMove = MMID::BRONZE_AUTOMATON_HYPER_BEAM;
                    boostPhase = 0;
                } else {
                    expectedMove = MMID::BRONZE_AUTOMATON_FLAIL;
                    boostPhase = 1;
                }
                break;
            case MMID::BRONZE_AUTOMATON_HYPER_BEAM:
                if (ascension >= 19) {
                    expectedMove = MMID::BRONZE_AUTOMATON_BOOST;
                    boostPhase = -1;
                } else {
                    expectedMove = MMID::BRONZE_AUTOMATON_STUNNED;
                }
                break;
            default:
                throw std::runtime_error(
                    "invalid Bronze Automaton move while inferring phase"
                );
        }
    }

    if (currentMove != expectedMove) {
        throw std::runtime_error(
            "cannot infer Bronze Automaton phase: combat turn "
            + std::to_string(combatTurn)
            + " is inconsistent with current move "
            + std::to_string(static_cast<int>(currentMove))
        );
    }
    return boostPhase;
}

void importExternalCardSelect(
    const nlohmann::json &json,
    BattleContext &bc
) {
    const auto selectionIt = json.find("mcts_card_select");
    if (selectionIt == json.end()) {
        return;
    }
    if (!selectionIt->is_object()) {
        throw std::runtime_error("mcts_card_select must be an object");
    }

    const auto task = selectionIt->value("task", "");
    const bool isDiscovery = task == "DISCOVERY";
    const bool isCodex = task == "CODEX";
    const bool isToolbox = task == "TOOLBOX";
    const bool isGamble = task == "GAMBLE";
    const bool isHologram = task == "HOLOGRAM";
    const bool isRecycle = task == "RECYCLE";
    const bool isSeek = task == "SEEK";
    if (!isDiscovery && !isCodex && !isToolbox && !isGamble
        && !isHologram && !isRecycle && !isSeek) {
        throw std::runtime_error(
            "unsupported mcts_card_select task: " + task
        );
    }

    const auto &gameState = json.at("game_state");
    if (isHologram || isRecycle || isSeek) {
        const auto expectedScreen = isRecycle ? "HAND_SELECT" : "GRID";
        const auto expectedAction = isHologram
            ? "BetterDiscardPileToHandAction"
            : (isRecycle ? "RecycleAction" : "SeekAction");
        if (gameState.value("screen_type", "") != expectedScreen
            || gameState.value("room_phase", "") != "COMBAT"
            || gameState.value("current_action", "") != expectedAction) {
            throw std::runtime_error(
                task + " search requires an in-combat " + expectedScreen
                + " " + expectedAction
            );
        }
        const auto &screenState = gameState.at("screen_state");
        const int count = screenState.value(
            isRecycle ? "max_cards" : "num_cards", 1
        );
        if (count < 1) {
            throw std::runtime_error(task + " search requires a card choice");
        }
        bc.openSimpleCardSelectScreen(
            isHologram ? CardSelectTask::HOLOGRAM
                : (isRecycle ? CardSelectTask::RECYCLE
                             : CardSelectTask::SEEK),
            count
        );
        return;
    }

    if (isGamble) {
        const auto &screenState = gameState.at("screen_state");
        if (gameState.value("screen_type", "") != "HAND_SELECT"
            || gameState.value("room_phase", "") != "COMBAT"
            || gameState.value("current_action", "")
                   != "GamblingChipAction"
            || !screenState.value("can_pick_zero", false)) {
            throw std::runtime_error(
                "GAMBLE search requires an in-combat GamblingChipAction "
                "HAND_SELECT"
            );
        }
        bc.inputState = InputState::CARD_SELECT;
        bc.cardSelectInfo.cardSelectTask = CardSelectTask::GAMBLE;
        return;
    }

    const auto expectedAction = isDiscovery
        ? "DiscoveryAction"
        : (isCodex ? "CodexAction" : "ChooseOneColorless");
    if (gameState.value("screen_type", "") != "CARD_REWARD"
        || gameState.value("room_phase", "") != "COMBAT"
        || gameState.value("current_action", "") != expectedAction) {
        throw std::runtime_error(
            task + " search requires an in-combat CARD_REWARD "
            + expectedAction
        );
    }

    const auto &screenState = gameState.at("screen_state");
    const auto &choices = screenState.at("cards");
    if (!choices.is_array() || choices.size() != 3) {
        throw std::runtime_error(
            task + " search requires exactly three visible card candidates"
        );
    }

    std::array<CardId, 3> cardIds;
    for (int i = 0; i < static_cast<int>(cardIds.size()); ++i) {
        if (!choices[i].is_object() || !choices[i].contains("id")
            || !choices[i]["id"].is_string()) {
            throw std::runtime_error(
                task + " candidate is missing its string card id"
            );
        }
        cardIds[i] = getCardIdFromId(choices[i]["id"].get<std::string>());
        if (cardIds[i] == CardId::INVALID) {
            throw std::runtime_error(
                "unknown " + task + " candidate card id: "
                + choices[i]["id"].get<std::string>()
            );
        }
    }

    if (isDiscovery) {
        const int copyCount = selectionIt->value("copy_count", 1);
        if (copyCount < 1 || copyCount > 2) {
            throw std::runtime_error(
                "DISCOVERY copy_count must be either 1 or 2"
            );
        }
        bc.haveUsedDiscoveryAction = true;
        bc.openDiscoveryScreen(cardIds, copyCount);
        return;
    }

    if (isToolbox) {
        bc.openToolboxScreen(cardIds);
        return;
    }

    // The real Codex screen pauses an already-ending turn. Effects queued
    // before Codex (for example Orichalcum) are visible in the imported state;
    // rebuild only the effects that run after the selection, then continue to
    // discard, monster actions, and the next player turn.
    bc.queueEndOfTurnActionsAfterCodex();
    bc.endTurnQueued = true;
    bc.openCodexScreen(cardIds);
}

} // namespace

BattleConverter::BattleConverter() {}

BattleConverter::~BattleConverter() {}

int countMonsterOccurrences(const nlohmann::json &monsters, const MonsterId id) {
    int count = 0;
    for (int i = 0; i < static_cast<int>(monsters.size()); ++i) {
        if (shouldImportMonster(monsters[i])
            && getMonsterIdFromId(monsters[i]["id"]) == id) {
            count += 1;
        }
    }
    return count;
}

bool containsMonsterHistory(
    const nlohmann::json &monsters,
    const MonsterId id
) {
    for (const auto &monster : monsters) {
        if (getMonsterIdFromId(monster["id"]) == id) {
            return true;
        }
    }
    return false;
}

int computePreplacedIdx(const nlohmann::json &monsters) {
    // the simulator special cases bronze automaton, collector, gremlin leader, and reptomancer with their summons
    // as such I can't just place every monster at an arbitrary position since these monsters must be positioned in specific locations
    // so assert that there is at most one of these (since multiple cannot be placed correctly)
    int countBronzeAutomaton = countMonsterOccurrences(monsters, MonsterId::BRONZE_AUTOMATON);
    int countTheCollector = countMonsterOccurrences(monsters, MonsterId::THE_COLLECTOR);
    int countGremlinLeader = countMonsterOccurrences(monsters, MonsterId::GREMLIN_LEADER);
    int countReptomancer = countMonsterOccurrences(monsters, MonsterId::REPTOMANCER);
    int countSpikeSlimeL = countMonsterOccurrences(monsters, MonsterId::SPIKE_SLIME_L);
    int countAcidSlimeL = countMonsterOccurrences(monsters, MonsterId::ACID_SLIME_L);
    int countSpikeSlimeM = countMonsterOccurrences(monsters, MonsterId::SPIKE_SLIME_M);
    int countAcidSlimeM = countMonsterOccurrences(monsters, MonsterId::ACID_SLIME_M);
    const bool isSlimeBossBattle = containsMonsterHistory(
        monsters,
        MonsterId::SLIME_BOSS
    );
#ifdef sts_asserts
    assert(countBronzeAutomaton + countTheCollector + countGremlinLeader + countReptomancer <= 1);
    assert(countSpikeSlimeL <= 1);
    assert(countAcidSlimeL <= 1);
#endif

    if (countBronzeAutomaton >= 1) {
        return 1;
    } else if (countTheCollector >= 1) {
        return 2;
    } else if (countGremlinLeader >= 1) {
        return 3;
    } else if (countReptomancer >= 1) {
        return 2;
    } else if (countSpikeSlimeL + countAcidSlimeL >= 2) {
        return 2; // according to the simulator slime boss needs the second slime to be positioned at idx 2
    } else if (isSlimeBossBattle
               && countSpikeSlimeL >= 1
               && countAcidSlimeM >= 1) {
        // The right large slime has already split. Keep slot 1 free for the
        // left large slime's second child and place the surviving right-side
        // children at slots 2 and 3. CommunicationMod retains the gone
        // parents in its history, so compacting only the living monsters here
        // would let the later left split overwrite one Acid Slime M.
        return 1;
    } else if (isSlimeBossBattle
               && countAcidSlimeL >= 1
               && countSpikeSlimeM >= 1) {
        // Mirror case: the left large slime has split while the right large
        // slime still owns slots 2 and 3.
        return 2;
    } else {
        return -1;
    }
}

bool isSpecialCase(const MonsterId id) {
    return id == MonsterId::BRONZE_AUTOMATON || id == MonsterId::THE_COLLECTOR || id == MonsterId::GREMLIN_LEADER || id == MonsterId::REPTOMANCER
        || id == MonsterId::ACID_SLIME_L; // only the acid slime needs to be special-cased
                                          // note that in reality any L slime that has another monster after it needs to have the following monster special-cased
                                          // to an idx 1 further over but in vanilla this only matters during slime boss so we can just move the following monster
                                          // ACID_SLIME_L to position 2 always
}

int getEnergyPerTurnFromJsonState(const GameContext &gc, const nlohmann::json &json) {
    int energyPerTurn = 3;
    constexpr RelicId energyRelics[] = {
        RelicId::BUSTED_CROWN,
        RelicId::COFFEE_DRIPPER,
        RelicId::CURSED_KEY,
        RelicId::ECTOPLASM,
        RelicId::FUSION_HAMMER,
        RelicId::MARK_OF_PAIN,
        RelicId::PHILOSOPHERS_STONE,
        RelicId::RUNIC_DOME,
        RelicId::SOZU,
        RelicId::VELVET_CHOKER,
    };

    for (const auto relic : energyRelics) {
        if (gc.relics.has(relic)) {
            ++energyPerTurn;
        }
    }

    if (gc.relics.has(RelicId::SLAVERS_COLLAR)) {
        const auto roomType = json["game_state"].value("room_type", "");
        if (roomType == "MonsterRoomElite" || roomType == "MonsterRoomBoss") {
            ++energyPerTurn;
        }
    }

    return energyPerTurn;
}

BattleContext BattleConverter::convertFromJson(const nlohmann::json &json, int *monsterIdxMap) {
    GameContext gc;
    gc.initFromJson(json);
    BattleContext bc;
    bc.partialInitOne(gc, MonsterEncounter::INVALID);
    int uniqueCardId = 0;

    const auto &combatState = json["game_state"]["combat_state"];
    const int combatTurn = combatState.value("turn", 1);
    auto monsters = combatState["monsters"];
    int monstersIdx = 0;
    int preplacedIdx = computePreplacedIdx(monsters);
    for (int i = 0; i < static_cast<int>(monsters.size()); ++i) {
        auto m = monsters[i];
        MonsterId monsterId = getMonsterIdFromId(m["id"]);

        // Skip permanently gone monsters: simulator monster array supports at
        // most 5 slots, while CommunicationMod keeps historical summons.  A
        // half-dead monster is different: it is temporarily untargetable but
        // still owns a pending rebirth move and must remain in the battle.
        if (!shouldImportMonster(m)) {
            continue;
        }

        Monster *monster;

        // ensure that monstersIdx and the MonsterGroup always skips past the preplaced position
        if (monstersIdx == preplacedIdx) {
            monstersIdx += 1;
            bc.monsters.monsterCount += 1;
        }

        if (preplacedIdx >= 0 && isSpecialCase(monsterId)) {
            // preplaced monster gets put into its position
            int cachedCount = bc.monsters.monsterCount;
            bc.monsters.monsterCount = preplacedIdx;
            bc.monsters.createMonster(bc, monsterId);
            monsterIdxMap[preplacedIdx] = i;
            monster = &bc.monsters.arr[preplacedIdx];
            // restore the previous position in the MonsterGroup
            bc.monsters.monsterCount = cachedCount;
        } else {
            bc.monsters.createMonster(bc, monsterId);
            monsterIdxMap[monstersIdx] = i;
            monster = &bc.monsters.arr[monstersIdx++];
        }

        monster->curHp = m["current_hp"];
        monster->maxHp = m["max_hp"];
        monster->block = m["block"];

        // monster->isEscapingB = m["is_escaping"];
        if(m.contains("is_escaping"))
            monster->isEscapingB = m["is_escaping"];
        monster->halfDead = m["half_dead"];

        // createMonster increments monstersAlive,
        // which gets deceremented when a monster leaves battle
        // in a typical fashion (hp goes to 0 or escapes)
        // but that has to be explicitly done during this conversion process
        if (monster->curHp <= 0 || monster->isEscapingB) {
            --bc.monsters.monstersAlive;
        }

        monster->moveHistory[0] = getMonsterMoveFromId(monsterId, m["move_id"]);
        if (m.contains("last_move_id")) {
            monster->moveHistory[1] = getMonsterMoveFromId(monsterId, m["last_move_id"]);
        }
        const bool hasExternalMiscInfo =
            m.contains("miscInt") || m.contains("miscBool");
        if (m.contains("miscInt")) {
            monster->miscInfo = m["miscInt"];
        } else if (m.contains("miscBool")) {
            monster->miscInfo = m["miscBool"];
        } else if (monsterId == MonsterId::AWAKENED_ONE) {
            // CommunicationMod does not expose Awakened One's private phase
            // flag.  Phase-two-only moves are authoritative evidence that the
            // rebirth action has already completed.  A pending REBIRTH stays
            // phase one here; executing that move sets miscInfo itself.
            monster->miscInfo =
                isAwakenedOnePhaseTwoMove(monster->moveHistory[0])
                || isAwakenedOnePhaseTwoMove(monster->moveHistory[1]);
        } else if (monsterId == MonsterId::TIME_EATER) {
            // New AgentStateFixes snapshots expose usedHaste as miscBool.
            // For older real-run fixtures, any visible Haste intent in the
            // three-move history is authoritative evidence that the one-shot
            // phase transition has already been scheduled.
            monster->miscInfo =
                jsonMonsterMoveIs(
                    m, "move_id", monsterId, MMID::TIME_EATER_HASTE
                )
                || jsonMonsterMoveIs(
                    m, "last_move_id", monsterId, MMID::TIME_EATER_HASTE
                )
                || jsonMonsterMoveIs(
                    m,
                    "second_last_move_id",
                    monsterId,
                    MMID::TIME_EATER_HASTE
                );
        } else if (monsterId == MonsterId::BOOK_OF_STABBING) {
            // CommunicationMod exposes the current multi-stab hit count but
            // not BookOfStabbing's private stab counter.  That counter is
            // also used as the hit count by the simulator, so leaving the
            // default value of zero turns every imported multi-stab into a
            // zero-hit attack.
            if (monster->moveHistory[0] == MMID::BOOK_OF_STABBING_MULTI_STAB) {
                monster->miscInfo = std::max(1, m.value("move_hits", 1));
            } else if (gc.ascension >= 18) {
                // At A18+, the counter advances once for every selected move,
                // including Single Stab. preBattleAction initializes it to 1,
                // so the current move's counter is combatTurn + 1.
                monster->miscInfo = std::max(1, combatTurn + 1);
            } else {
                // A lower-ascension Single Stab does not expose enough
                // history to recover the exact number of prior multi-stabs.
                // Preserve the correct initial lower bound instead of the
                // invalid zero-hit default.
                monster->miscInfo = 1;
            }
        } else if (
            monsterId == MonsterId::DARKLING
            && monster->moveHistory[0] == MMID::DARKLING_NIP
        ) {
            const int baseDamage = m.value("move_base_damage", -1);
            if (baseDamage > 0) {
                monster->miscInfo = baseDamage - (gc.ascension >= 2 ? 2 : 0);
            }
        } else if (monsterId == MonsterId::GREMLIN_WIZARD) {
            monster->miscInfo = inferGremlinWizardCharge(m);
        } else if (monsterId == MonsterId::BRONZE_AUTOMATON) {
            monster->miscInfo = inferBronzeAutomatonBoostPhase(
                combatTurn,
                gc.ascension,
                monster->moveHistory[0]
            );
        } else if (
            monsterId == MonsterId::HEXAGHOST
            && monster->moveHistory[0] == MMID::HEXAGHOST_DIVIDER
        ) {
            // Divider stores its per-hit damage in the monster's private
            // miscInfo field when Activate resolves. CommunicationMod does
            // not expose that field, but it does expose the current move's
            // base damage. Leaving miscInfo at zero makes an imported 7x6
            // Divider deal no damage in simulation.
            monster->miscInfo = std::max(
                1,
                m.value(
                    "move_base_damage",
                    m.value("move_adjusted_damage", 1)
                )
            );
        }

        // some monster specific information
        // TODO: still missing Shield Gremlin target
        switch(monster->id) {
            case MonsterId::HEXAGHOST: {
                const auto activeOrbs = m.find("active_orbs");
                if (activeOrbs != m.end()
                    && activeOrbs->is_number_integer()) {
                    monster->uniquePower0 = activeOrbs->get<int>();
                    if (monster->uniquePower0 < 0
                        || monster->uniquePower0 > 6) {
                        throw std::runtime_error(
                            "Hexaghost active_orbs must be between 0 and 6"
                        );
                    }
                } else {
                    // CommunicationMod normally omits this private counter.
                    // Hexaghost follows a fixed seven-turn cycle, so recover
                    // it from the combat turn and verify against its intent.
                    monster->uniquePower0 = inferHexaghostActiveOrbs(
                        combatTurn,
                        monster->moveHistory[0]
                    );
                }
                break;
            }
            default:
                break;
        };


        // monster powers
        auto powers = m["powers"];
        for (int j = 0; j < static_cast<int>(powers.size()); ++j) {
            auto p = powers[j];
            MonsterStatus monsterStatus = getMonsterStatusFromId(p["id"]);
            monster->setStatus(monsterStatus, p["amount"]);
            // Corrupt Heart's Invincible power remains present after its
            // per-turn allowance reaches zero and resets at the start of the
            // next turn.  The generic status setter treats zero as removal,
            // which made mid-turn imports incorrectly lose the future cap.
            if (monsterStatus == MonsterStatus::INVINCIBLE) {
                monster->setHasStatus<MonsterStatus::INVINCIBLE>(true);
            }
            if (p.contains("just_applied")) {
                monster->setJustApplied(monsterStatus, p["just_applied"]);
            }
            // handle the stasis power since it keeps track of a card
            if (p.contains("card") && monsterStatus == MonsterStatus::STASIS) {
                auto c = p["card"];
                CardInstance cardInstance = importCardInstance(
                    c, uniqueCardId++
                );
                bc.cards.stasisCards[std::min(1, monster->idx)] = cardInstance;
            }
        }

        if (!hasExternalMiscInfo
            && (monsterId == MonsterId::LOOTER
                || monsterId == MonsterId::MUGGER)) {
            monster->miscInfo = inferThiefStolenGold(
                m,
                monsterId,
                combatTurn,
                monster->getStatus<MonsterStatus::THIEVERY>()
            );
        }

        monster->syncMoveBasedStatuses();

        // Lagavulin uses the same private move-id family for sleeping and for
        // the zero-damage wake-up turn. CommunicationMod's visible intent is
        // the authoritative distinction: STUN means it has already woken and
        // will attack after this monster turn. Treating that state as ASLEEP
        // lets END_TURN incorrectly buy another harmless sleep turn, while an
        // otherwise beneficial attack appears to wake it.
        if (
            monsterId == MonsterId::LAGAVULIN
            && m.value("intent", std::string{}) == "STUN"
        ) {
            monster->setHasStatus<MonsterStatus::ASLEEP>(false);
        }
    }

    // ensure the MonsterGroup position includes the preplacedIdx
    bc.monsters.monsterCount = std::max(preplacedIdx + 1, bc.monsters.monsterCount);

    // special case for bronze automaton, ensure monsterCount is at least preplacedIdx + 2
    if (countMonsterOccurrences(monsters, MonsterId::BRONZE_AUTOMATON) >= 1) {
        bc.monsters.monsterCount = std::max(preplacedIdx + 2, bc.monsters.monsterCount);
    }



    bc.partialInitTwo(gc);
    bc.player.relicBits0 = gc.relics.relicBits0;
    bc.player.relicBits1 = gc.relics.relicBits1;
    bc.player.relicBits2 = gc.relics.relicBits2;
    if (gc.relics.has(RelicId::LIZARD_TAIL)
        && gc.relics.getRelicValue(RelicId::LIZARD_TAIL) == 0) {
        // Keep the relic in the run inventory, but do not give combat a second
        // revival after CommunicationMod reports that the one-shot has fired.
        bc.player.setHasRelic<R::LIZARD_TAIL>(false);
    }
    if (combatState.value("centennial_puzzle_used_this_combat", false)) {
        // BattleContext represents this once-per-combat trigger as relic
        // availability. Keep it in GameContext, but consume it for rollouts
        // reconstructed after the real relic has already fired.
        bc.player.setHasRelic<R::CENTENNIAL_PUZZLE>(false);
    }
    // CommunicationMod exposes combat turns as 1-based (the opening player
    // turn is 1), while BattleContext stores them as 0-based and
    // getMonsterTurnNumber() adds one.  Keeping the external value here makes
    // every imported mid-combat rollout advance one turn too far: scaling
    // enemies such as Transient deal their next turn's damage, and turn-based
    // relics trigger one turn early or late.
    bc.turn = std::max(0, combatTurn - 1);
    bc.player.energy = combatState["player"]["energy"];
    bc.player.block = combatState["player"]["block"];
    bc.player.energyPerTurn = getEnergyPerTurnFromJsonState(gc, json);
    bc.player.cardsPlayedThisTurn =
        combatState.value("cards_played_this_turn", 0);
    bc.player.attacksPlayedThisTurn =
        combatState.value("attacks_played_this_turn", 0);
    bc.player.skillsPlayedThisTurn =
        combatState.value("skills_played_this_turn", 0);
    bc.powersPlayedThisCombat =
        combatState.value("powers_played_this_combat", 0);
    bc.player.cardsDiscardedThisTurn = combatState.value("cards_discarded_this_turn", 0);
    bc.player.timesDamagedThisCombat = combatState.value("times_damaged", 0);

    // Defect combat history is not reconstructible from the visible piles.
    // AgentStateFixes exposes these counters from GameActionManager so
    // Thunder Strike and Blizzard resume with their exact current values.
    if (gc.cc == CharacterClass::DEFECT) {
        const auto requireCombatInteger = [&combatState] (const char *field) {
            const auto it = combatState.find(field);
            if (it == combatState.end() || !it->is_number_integer()) {
                throw std::runtime_error(
                    std::string("Defect combat state is missing integer ") + field
                    + " from AgentStateFixes"
                );
            }
            return it->get<int>();
        };
        bc.player.lightningChanneled = static_cast<std::int16_t>(
            requireCombatInteger("lightning_channeled_this_combat")
        );
        bc.player.frostChanneled = static_cast<std::int16_t>(
            requireCombatInteger("frost_channeled_this_combat")
        );
        bc.powersPlayedThisCombat =
            requireCombatInteger("powers_played_this_combat");
        if (!combatState.contains("emotion_chip_pending")
            || !combatState["emotion_chip_pending"].is_boolean()) {
            throw std::runtime_error(
                "Defect combat state is missing boolean emotion_chip_pending from AgentStateFixes"
            );
        }
        // Player's relic implementation consumes this previous-turn marker at
        // the next start of turn. A positive value is sufficient; the amount
        // of HP lost does not affect Emotion Chip.
        bc.player.lastDamageTaken =
            combatState["emotion_chip_pending"].get<bool>() ? 1 : 0;
    }

    // Player orbs. Vanilla CommunicationMod serializes one entry per slot,
    // including EmptyOrbSlot, but does not provide orb_slots. New snapshots
    // expose maxOrbs explicitly; array length remains a compatible fallback.
    {
        const auto &playerJson = combatState["player"];
        const auto orbsIt = playerJson.find("orbs");
        if (orbsIt == playerJson.end() || !orbsIt->is_array()) {
            if (gc.cc == CharacterClass::DEFECT) {
                throw std::runtime_error(
                    "Defect player state is missing CommunicationMod orbs array"
                );
            }
            bc.player.orbSlots = 0;
        } else {
            const int orbSlots = playerJson.value(
                "orb_slots",
                static_cast<int>(orbsIt->size())
            );
            if (orbSlots < 0 || orbSlots > Player::MAX_ORB_SLOTS
                || static_cast<int>(orbsIt->size()) < orbSlots) {
                throw std::runtime_error("invalid Defect orb slot state");
            }
            bc.player.orbSlots = static_cast<std::int8_t>(orbSlots);
            for (int idx = 0; idx < orbSlots; ++idx) {
                const auto &orbJson = (*orbsIt)[idx];
                const std::string orbId = orbJson.value("id", "");
                Orb orb = Orb::EMPTY;
                if (orbId == "Lightning") {
                    orb = Orb::LIGHTNING;
                } else if (orbId == "Frost") {
                    orb = Orb::FROST;
                } else if (orbId == "Dark") {
                    orb = Orb::DARK;
                } else if (orbId == "Plasma") {
                    orb = Orb::PLASMA;
                } else if (orbId == "Fusion") {
                    orb = Orb::PLASMA;
                }
                bc.player.orbTypes[idx] = orb;
                // Dark.evokeAmount is its currently stored damage. Its
                // passiveAmount is only the Focus-adjusted growth per turn.
                bc.player.orbData[idx] = (orb == Orb::DARK)
                    ? static_cast<std::int16_t>(
                        orbJson.value("evoke_amount", 6)
                      )
                    : 0;
            }
        }
    }

    auto drawPile = json["game_state"]["combat_state"]["draw_pile"];
    for (int i = 0; i < static_cast<int>(drawPile.size()); ++i) {
        auto c = drawPile[i];
        CardInstance cardInstance = importCardInstance(c, uniqueCardId++);
        bc.cards.moveToDrawPileTop(cardInstance);
        bc.cards.notifyAddCardToCombat(cardInstance);
    }

    auto discardPile = json["game_state"]["combat_state"]["discard_pile"];
    for (int i = 0; i < static_cast<int>(discardPile.size()); ++i) {
        auto c = discardPile[i];
        CardInstance cardInstance = importCardInstance(c, uniqueCardId++);
        bc.cards.moveToDiscardPile(cardInstance);
        bc.cards.notifyAddCardToCombat(cardInstance);
    }

    auto hand = json["game_state"]["combat_state"]["hand"];
    for (int i = 0; i < static_cast<int>(hand.size()); ++i) {
        auto c = hand[i];
        CardInstance cardInstance = importCardInstance(c, uniqueCardId++);
        bc.cards.moveToHand(cardInstance);
        bc.cards.notifyAddCardToCombat(cardInstance);
    }

    auto exhaustPile = json["game_state"]["combat_state"]["exhaust_pile"];
    for (int i = 0; i < static_cast<int>(exhaustPile.size()); ++i) {
        auto c = exhaustPile[i];
        CardInstance cardInstance = importCardInstance(c, uniqueCardId++);
        bc.cards.exhaustPile.push_back(cardInstance);
    }

    bc.cards.nextUniqueCardId = uniqueCardId;

    const auto restoreForceFieldHistory = [&bc] (CardInstance &card) {
        if (card.getId() == CardId::FORCE_FIELD) {
            // combat_cost already contains all past power discounts. Record
            // how many have been applied so moving the card later cannot
            // apply the same history a second time.
            card.specialData = static_cast<std::int16_t>(
                bc.powersPlayedThisCombat
            );
        }
    };
    for (int i = 0; i < bc.cards.cardsInHand; ++i) {
        restoreForceFieldHistory(bc.cards.hand[i]);
    }
    for (auto &card : bc.cards.drawPile) {
        restoreForceFieldHistory(card);
    }
    for (auto &card : bc.cards.discardPile) {
        restoreForceFieldHistory(card);
    }
    for (auto &card : bc.cards.exhaustPile) {
        restoreForceFieldHistory(card);
    }

    auto powers = json["game_state"]["combat_state"]["player"]["powers"];
    for (int j = 0; j < static_cast<int>(powers.size()); ++j) {
        auto p = powers[j];
        PlayerStatus playerStatus = getPlayerStatusFromId(p["id"]);
        bc.player.setHasStatus(playerStatus, true);
        bc.player.setStatusValueNoChecks(playerStatus, p["amount"]);
        if (p.contains("just_applied")) {
            bc.player.setJustApplied(playerStatus, p["just_applied"]);
        }

        // special case powers that need extra info
        switch(playerStatus) {
            case PlayerStatus::COMBUST:
                bc.player.combustHpLoss = p["misc"];
                break;
            case PlayerStatus::DEVA:
                bc.player.devaFormEnergyPerTurn = p["misc"];
                break;
            case PlayerStatus::ECHO_FORM:
                // 真机 EchoPower 的计数存在 private 的 cardsDoubledThisTurn 上,不是
                // AbstractPower.misc —— p["misc"] 一直读的是没人写过的 0。
                // SteamStateExport 用反射把真值导到 combat_state.echo_form_cards_doubled,
                // 有就用真值,没有(纯 CommunicationMod)才退回旧的 misc。
                bc.player.echoFormCardsDoubled = json["game_state"]["combat_state"].value(
                    "echo_form_cards_doubled", p.value("misc", 0));
                break;
            case PlayerStatus::PANACHE:
                bc.player.panacheCounter = p["amount"];
                // override original status value since it needs to be the damage not the amount
                bc.player.setStatusValueNoChecks(playerStatus, p["damage"]);
                break;
            case PlayerStatus::THE_BOMB:
                if (p["amount"] == 1) {
                    bc.player.bomb1 = p["amount"];
                } else if (p["amount"] == 2) {
                    bc.player.bomb2 = p["amount"];
                } else if (p["amount"] == 3) {
                    bc.player.bomb3 = p["amount"];
                } else {
                    std::cerr << "the bomb must have an amount of either 1, 2, or 3 - no other values are valid - got " << p["amount"] << std::endl;
                    assert(false);
                }
                // status value is unused for the bomb
                bc.player.setStatusValueNoChecks(playerStatus, 0);
            default:
                break;
        };
    }

    const auto &playerState = combatState["player"];
    if (playerState.contains("facing_left")) {
        const MonsterId facingMonster =
            playerState["facing_left"].get<bool>()
                ? MonsterId::SPIRE_SHIELD
                : MonsterId::SPIRE_SPEAR;
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            if (bc.monsters.arr[i].id == facingMonster) {
                bc.player.lastTargetedMonster = i;
                break;
            }
        }
    }

    // these would typically be initialized in BattleContext::initRelics
    // but that performs additional initialization that only occurs at the start of battle
    // which would be invalid for loading a state that is already in the middle of combat
    bc.player.happyFlowerCounter = gc.relics.getRelicValue(RelicId::HAPPY_FLOWER);
    bc.player.incenseBurnerCounter = gc.relics.getRelicValue(RelicId::INCENSE_BURNER);
    bc.player.inkBottleCounter = gc.relics.getRelicValue(RelicId::INK_BOTTLE);
    bc.player.inserterCounter = gc.relics.getRelicValue(RelicId::INSERTER);
    bc.player.nunchakuCounter = gc.relics.getRelicValue(RelicId::NUNCHAKU);
    bc.player.penNibCounter = gc.relics.getRelicValue(RelicId::PEN_NIB);
    bc.player.sundialCounter = gc.relics.getRelicValue(RelicId::SUNDIAL);

    // TODO: have communication mod provide the "activated" state of the necronomicon

    // TODO: expose Orange Pellets' private per-type flags. They cannot be
    // reconstructed from the cards played list after the relic has triggered
    // and reset itself.

    bc.executeActions();
    importExternalCardSelect(json, bc);
    return bc;
}
