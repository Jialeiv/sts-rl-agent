//
// Created by gamerpuppy on 7/8/2021.
//

#include <iostream>
#include <chrono>
#include <cstdint>
#include <thread>
#include <memory>
#include <mutex>
#include <cmath>

#include "data_structure/fixed_list.h"
#include "constants/Cards.h"
#include "constants/Events.h"
#include "constants/CardPools.h"
#include "game/Game.h"
#include "game/Map.h"
#include "game/Neow.h"
#include "game/SaveFile.h"
#include "combat/BattleContext.h"
#include "convert/BattleConverter.h"
#include "sim/ConsoleSimulator.h"
#include "sim/PrintHelpers.h"
#include "sim/RandomAgent.h"
#include "sim/search/ScumSearchAgent2.h"
#include "sim/search/SimpleAgent.h"

#include "sim/search/BattleScumSearcher2.h"
#include "sim/search/BattleEvaluator.h"
#include "sim/search/RecoveryRootPolicy.h"
#include "sim/search/RootActionPolicy.h"

using namespace sts;

void printSizes() {
    std::cout << "sizeof Map:" << sizeof(Map) << '\n';
    std::cout << "sizeof Player: " << sizeof(Player) << '\n';
    std::cout << "sizeof Monster: " << sizeof(Monster) << '\n';
    std::cout << "sizeof MonsterGroup : " << sizeof(MonsterGroup) << '\n';
    std::cout << "sizeof CardInstance: " << sizeof(CardInstance) << '\n';
    std::cout << "sizeof CardManager : " << sizeof(CardManager) << '\n';
    std::cout << "sizeof ActionFunction : " << sizeof(ActionFunction) << '\n';
    std::cout << "sizeof ActionQueue<40> : " << sizeof(ActionQueue<40>) << '\n';
    std::cout << "sizeof BattleContext: " << sizeof(BattleContext) << '\n';

    std::cout << "sizeof GameContext: " << sizeof(GameContext) << '\n';
    std::cout << "sizeof Deck: " << sizeof(Deck) << '\n';
    std::cout << "sizeof Card: " << sizeof(Card) << '\n';
    std::cout << "sizeof SelectScreenCard: " << sizeof(SelectScreenCard) << '\n';
}

void playFromSaveFile(const std::string &fname, const std::string &actionFile) {
    CharacterClass cc;
    switch (tolower(fname[0])) {
        case 'i':
            cc = sts::CharacterClass::IRONCLAD;
            break;
        default:
            cc = sts::CharacterClass::IRONCLAD;
    }

    SaveFile saveFile = SaveFile::loadFromPath(fname, cc);

    ConsoleSimulator sim;
    sim.setupGameFromSaveFile(saveFile);
    SimulatorContext simContext;
    simContext.quitOnTestFailed = false;



    std::ifstream actionListInputStream(actionFile);

    sim.play(actionListInputStream, std::cout, simContext);
    actionListInputStream.close();

//    simContext.printFirstLine = true;
    simContext.quitCommandGiven = false;
    sim.play(std::cin, std::cout, simContext);
}

void replayActionFile(const GameContext &startState, const std::string &fname) {
    std::ifstream ifs(fname);
    GameContext gc(startState);
    BattleContext bc;


    bool inBattle = false;

    std::uint32_t actionBits;
    while (true) {
        if (inBattle) {
            if (bc.outcome != sts::Outcome::UNDECIDED) {
                bc.exitBattle(gc);
                inBattle = false;

            } else {
                ifs >> std::hex >> actionBits;
                search::Action a(actionBits);
                a.printDesc(std::cout, bc) << std::endl;
                a.execute(bc);
            }

        } else {
            if (gc.outcome != GameOutcome::UNDECIDED) {
                break;
            }
            if (gc.screenState == sts::ScreenState::BATTLE) {
                bc = {};
                bc.init(gc);
                inBattle = true;

            } else {
                ifs >> std::hex >> actionBits;
                search::GameAction a(actionBits);
                a.printDesc(std::cout, gc) << std::endl;
                a.execute(gc);
            }
        }
    }
}

struct AgentMtInfo {
    std::mutex m;

    std::uint64_t curSeed;
    std::uint64_t seedStart;
    std::uint64_t seedEnd;

    std::int64_t winCount = 0;
    std::int64_t lossCount = 0;
    std::int64_t floorSum = 0;
    std::int64_t totalSimulations = 0;
};

static int g_searchAscension = 0;
static int g_simulationCount = 5;
static int g_print_level = 0;

void agentMtRunner(AgentMtInfo *info) {
    std::uint64_t seed;
    {
        std::scoped_lock lock(info->m);
        seed = info->curSeed++;
    }

    while(true) {
        if (seed >= info->seedEnd) {
            break;
        }

        GameContext gc(CharacterClass::IRONCLAD, seed, g_searchAscension);
        search::ScumSearchAgent2 agent;
        agent.simulationCountBase = g_simulationCount;
        agent.rng = std::default_random_engine(gc.seed);

        agent.printActions = g_print_level & 0x1;
        agent.printLogs = g_print_level & 0x2;

        agent.playout(gc);

        printOutcome(std::cout, gc);

        {
            std::scoped_lock lock(info->m);
            info->floorSum += gc.floorNum;
            if (gc.outcome == sts::GameOutcome::PLAYER_VICTORY) {
                ++info->winCount;
            } else {
                ++info->lossCount;
            }
            info->totalSimulations += agent.simulationCountTotal;

            seed = info->curSeed++;
        }
    }
}

void agentMt(int threadCount, std::uint64_t startSeed, int playoutCount) {
    auto startTime = std::chrono::high_resolution_clock::now();
    std::vector<std::unique_ptr<std::thread>> threads;

    AgentMtInfo info;
    info.curSeed = startSeed;
    info.seedStart = startSeed;
    info.seedEnd = startSeed + playoutCount;


    if (threadCount == 1) { // doing this for more consistency when benchmarking
        agentMtRunner(&info);

    } else {
        for (int tid = 0; tid < threadCount; ++tid) {
            threads.emplace_back(new std::thread(agentMtRunner, &info));
        }
    }

    for (int tid = 0; tid < threadCount; ++tid) {
        if (threadCount > 1) {
            threads[tid]->join();
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(endTime-startTime).count();

    std::cout << "w/l: (" << info.winCount  << ", " << info.lossCount << ")"
        << " percentWin: " << static_cast<double>(info.winCount) / playoutCount * 100 << "%"
        << " avgFloorReached: " << static_cast<double>(info.floorSum) / playoutCount << '\n'
        << " totalSimulations: " << info.totalSimulations
        << " avgPerFloor: " << (double)info.totalSimulations/info.floorSum << '\n';

    std::cout << "threads: " << threadCount
              << " playoutCount: " << playoutCount
              << " depth: " << g_simulationCount
        << " asc: " << g_searchAscension
        << " elapsed: " << duration
        << std::endl;
}

int mcts(int, const char *argv[]) {
    const auto saveFilePath = argv[2];
    const auto simulationCount = std::stoll(argv[3]);

    SaveFile saveFile = SaveFile::loadFromPath(saveFilePath, sts::CharacterClass::IRONCLAD);
    GameContext gc;
    gc.initFromSave(saveFile);

    std::cout << SeedHelper::getString(gc.seed) << std::endl;

    BattleContext bc = BattleContext();
    bc.init(gc);

    search::BattleScumSearcher2 searcher(bc);

    auto startTime = std::chrono::high_resolution_clock::now();
    searcher.search(simulationCount, 1000);
    auto endTime = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(endTime-startTime).count();

    std::cout << "steps: " << simulationCount << " search time: " << duration << "s\n";
    std::cout << "best search value: " << searcher.bestActionValue << " depth: " << searcher.bestActionSequence.size() << '\n';
    if (searcher.bestActionSequence.empty()) {
        std::cout << "bestActionSequenceIsEmpty" << std::endl;
        return 0;
    }

    for (auto bestAction : searcher.bestActionSequence) {
        bestAction.printDesc(std::cout, bc) << '\n';
        bestAction.execute(bc);
    }

    std::cout << "ending hp: " << bc.player.curHp << '\n';

    searcher.printSearchTree(std::cout, 3);

    std::cout.flush();
    return 0;
}

int mctsRegressionTests() {
    int failures = 0;
    const auto check = [&failures] (bool condition, const char *message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAILED: " << message << std::endl;
        }
    };

    const CharacterClass classes[] = {
        CharacterClass::IRONCLAD,
        CharacterClass::SILENT,
        CharacterClass::DEFECT,
        CharacterClass::WATCHER,
    };
    const CardType generatedTypes[] = {
        CardType::ATTACK,
        CardType::SKILL,
        CardType::POWER,
    };
    const int expectedCombatPoolSizes[] = {70, 71, 70, 71};
    for (int classIdx = 0; classIdx < 4; ++classIdx) {
        const auto cc = classes[classIdx];
        check(CombatCardPool::getPoolSize(cc)
                  == expectedCombatPoolSizes[classIdx],
              "combat random-card pool must exclude only healing cards");
        for (int i = 0; i < CombatCardPool::getPoolSize(cc); ++i) {
            const auto id = CombatCardPool::getCardAt(cc, i);
            check(isCombatRandomCard(id),
                  "combat random-card pool must exclude healing cards");
        }
        for (const auto type : generatedTypes) {
            for (int i = 0; i < CombatTypeCardPool::getPoolSize(cc, type); ++i) {
                const auto id = CombatTypeCardPool::getCardAt(cc, type, i);
                check(getCardType(id) == type,
                      "typed combat pool must match the requested card type");
                check(isCombatRandomCard(id),
                      "typed combat pool must exclude healing cards");
            }
        }
    }
    const auto poolContains = [] (
        CharacterClass cc,
        CardType type,
        CardId expected
    ) {
        for (int i = 0; i < CombatTypeCardPool::getPoolSize(cc, type); ++i) {
            if (CombatTypeCardPool::getCardAt(cc, type, i) == expected) {
                return true;
            }
        }
        return false;
    };
    check(poolContains(CharacterClass::DEFECT, CardType::ATTACK,
                       CardId::BALL_LIGHTNING)
              && !poolContains(CharacterClass::DEFECT, CardType::ATTACK,
                               CardId::IMMOLATE),
          "Defect random Attacks must come from the Defect pool");
    check(poolContains(CharacterClass::DEFECT, CardType::SKILL,
                       CardId::COOLHEADED)
              && !poolContains(CharacterClass::DEFECT, CardType::SKILL,
                               CardId::IMPERVIOUS),
          "Defect random Skills must come from the Defect pool");
    check(poolContains(CharacterClass::DEFECT, CardType::POWER,
                       CardId::CREATIVE_AI)
              && !poolContains(CharacterClass::DEFECT, CardType::POWER,
                               CardId::BARRICADE),
          "Defect random Powers must come from the Defect pool");

    BattleContext darklingState;
    darklingState.ascension = 20;
    Monster darkling;
    darkling.construct(darklingState, MonsterId::DARKLING, 0);
    darkling.moveHistory[0] = MMID::DARKLING_NIP;
    const auto nip = darkling.getMoveBaseDamage(darklingState);
    check(nip.damage >= 9 && nip.damage <= 13 && nip.attackCount == 1,
          "Darkling Nip must retain its A20 9-13 damage range");
    darkling.moveHistory[0] = MMID::DARKLING_CHOMP;
    const auto chomp = darkling.getMoveBaseDamage(darklingState);
    check(chomp.damage == 9 && chomp.attackCount == 2,
          "Darkling Chomp must deal 9 damage twice at A20");

    const auto awakenedOneJson = [] (
        bool isGone,
        bool halfDead,
        int currentHp,
        int moveId,
        int lastMoveId
    ) {
        return nlohmann::json{
            {"game_state", {
                {"seed", 1},
                {"ascension_level", 20},
                {"act", 3},
                {"floor", 50},
                {"class", "IRONCLAD"},
                {"current_hp", 96},
                {"max_hp", 96},
                {"gold", 0},
                {"room_type", "MonsterRoomBoss"},
                {"relics", nlohmann::json::array()},
                {"potions", nlohmann::json::array()},
                {"combat_state", {
                    {"turn", 8},
                    {"cards_discarded_this_turn", 0},
                    {"times_damaged", 0},
                    {"draw_pile", nlohmann::json::array()},
                    {"discard_pile", nlohmann::json::array()},
                    {"hand", nlohmann::json::array({{
                        {"id", "Strike_R"},
                        {"upgrades", 0},
                        {"cost", 1},
                    }})},
                    {"exhaust_pile", nlohmann::json::array()},
                    {"player", {
                        {"energy", 3},
                        {"block", 0},
                        {"powers", nlohmann::json::array()},
                    }},
                    {"monsters", nlohmann::json::array({{
                        {"id", "AwakenedOne"},
                        {"is_gone", isGone},
                        {"half_dead", halfDead},
                        {"current_hp", currentHp},
                        {"max_hp", 320},
                        {"block", 0},
                        {"move_id", moveId},
                        {"last_move_id", lastMoveId},
                        {"powers", nlohmann::json::array()},
                    }})},
                }},
            }},
        };
    };

    BattleConverter converter;

    // Confusion randomizes the combat cost on every actual draw. A Pyramid-
    // retained card is not redrawn, so its randomized cost must survive the
    // turn boundary; after discard and redraw, a fresh RNG roll replaces it.
    int confusionSeed = 1;
    int expectedConfusedCost = 0;
    int expectedRedrawnCost = 0;
    for (;; ++confusionSeed) {
        Random candidateRng(confusionSeed);
        expectedConfusedCost = candidateRng.random(3);
        expectedRedrawnCost = candidateRng.random(3);
        if (expectedConfusedCost != expectedRedrawnCost) {
            break;
        }
    }
    BattleContext confusedCostState;
    confusedCostState.player.buff<PS::CONFUSED>(1);
    confusedCostState.cardRandomRng = Random(confusionSeed);
    const CardId confusedCardId = CardId::DEMON_FORM;
    confusedCostState.cards.drawPile.push_back(
        CardInstance(confusedCardId)
    );
    confusedCostState.cards.draw(confusedCostState, 1);
    check(confusedCostState.cards.cardsInHand == 1
              && confusedCostState.cards.hand[0].cost
                     == expectedConfusedCost
              && confusedCostState.cards.hand[0].costForTurn
                     == expectedConfusedCost,
          "Confusion must randomize the card's combat and turn costs");

    confusedCostState.cardSelectInfo.dualWield_CopyCount() = 1;
    confusedCostState.chooseDualWieldCard(0);
    check(confusedCostState.cards.cardsInHand == 2
              && confusedCostState.cards.hand[0].cost
                     == expectedConfusedCost
              && confusedCostState.cards.hand[1].cost
                     == expectedConfusedCost
              && confusedCostState.cards.hand[0].costForTurn
                     == expectedConfusedCost
              && confusedCostState.cards.hand[1].costForTurn
                     == expectedConfusedCost,
          "Dual Wield must copy the currently randomized cost");

    confusedCostState.cards.resetAttributesAtEndOfTurn();
    check(confusedCostState.cards.hand[0].costForTurn
                  == expectedConfusedCost
              && confusedCostState.cards.hand[1].costForTurn
                     == expectedConfusedCost,
          "a retained Confusion cost must survive the turn boundary");

    CardInstance redrawnConfusedCard = confusedCostState.cards.hand[0];
    confusedCostState.cards.removeFromHandAtIdx(0);
    confusedCostState.cards.moveToDrawPileTop(redrawnConfusedCard);
    confusedCostState.cards.draw(confusedCostState, 1);
    check(confusedCostState.cards.hand[1].cost == expectedRedrawnCost
              && confusedCostState.cards.hand[1].costForTurn
                     == expectedRedrawnCost,
          "Confusion must rerandomize the same card when it is drawn again");

    BattleContext sneckoOilCostState;
    sneckoOilCostState.cards.cardsInHand = 1;
    sneckoOilCostState.cards.hand[0] = CardInstance(CardId::DEMON_FORM);
    sneckoOilCostState.cardRandomRng = Random(456);
    Random expectedOilRng(456);
    const int expectedOilCost = expectedOilRng.random(3);
    Actions::RandomizeHandCost().actFunc(sneckoOilCostState);
    check(sneckoOilCostState.cards.hand[0].cost == expectedOilCost
              && sneckoOilCostState.cards.hand[0].costForTurn
                     == expectedOilCost,
          "Snecko Oil must randomize the card's combat and turn costs");
    sneckoOilCostState.cards.resetAttributesAtEndOfTurn();
    check(sneckoOilCostState.cards.hand[0].costForTurn == expectedOilCost,
          "Snecko Oil cost must survive the turn boundary");

    CardInstance confusedSeeingRed(CardId::SEEING_RED);
    confusedSeeingRed.cost = 3;
    confusedSeeingRed.costForTurn = 3;
    confusedSeeingRed.upgrade();
    check(confusedSeeingRed.cost == 0
              && confusedSeeingRed.costForTurn == 0,
          "upgrading a randomized cost-changing card must retain the existing upgrade behavior");

    auto importedCostJson = awakenedOneJson(false, false, 100, 2, 1);
    auto &importedCard =
        importedCostJson["game_state"]["combat_state"]["hand"][0];
    importedCard["combat_cost"] = 2;
    importedCard["cost"] = 0;
    int importedCostMonsterMap[5] = {-1, -1, -1, -1, -1};
    const BattleContext importedCostState = converter.convertFromJson(
        importedCostJson,
        importedCostMonsterMap
    );
    check(importedCostState.cards.hand[0].cost == 2
              && importedCostState.cards.hand[0].costForTurn == 0,
          "combat import must preserve separate combat and turn costs");

    BattleContext retainedBurnState;
    retainedBurnState.outcome = Outcome::UNDECIDED;
    retainedBurnState.inputState = InputState::PLAYER_NORMAL;
    retainedBurnState.skipMonsterTurn = true;
    retainedBurnState.player.curHp = 7;
    retainedBurnState.player.maxHp = 78;
    retainedBurnState.player.cardDrawPerTurn = 0;
    retainedBurnState.player.setHasRelic<RelicId::RUNIC_PYRAMID>(true);
    retainedBurnState.cards.cardsInHand = 3;
    retainedBurnState.cards.hand[0] = CardInstance(CardId::BURN);
    retainedBurnState.cards.hand[0].setUniqueId(1);
    retainedBurnState.cards.hand[1] = CardInstance(CardId::BURN);
    retainedBurnState.cards.hand[1].setUniqueId(2);
    retainedBurnState.cards.hand[2] = CardInstance(CardId::DEFEND_RED);
    retainedBurnState.cards.hand[2].setUniqueId(3);
    search::Action(search::ActionType::END_TURN).execute(retainedBurnState);
    check(retainedBurnState.player.curHp == 3,
          "two retained Burns must deal four end-of-turn damage under "
          "Runic Pyramid");
    check(retainedBurnState.cards.cardsInHand == 1
              && retainedBurnState.cards.hand[0].getId()
                     == CardId::DEFEND_RED,
          "end-of-turn Burn handling must remove only the queued Burns from "
          "a retained hand");

    BattleContext giantHeadBurnState;
    giantHeadBurnState.outcome = Outcome::UNDECIDED;
    giantHeadBurnState.inputState = InputState::PLAYER_NORMAL;
    giantHeadBurnState.ascension = 20;
    giantHeadBurnState.turn = 6;
    giantHeadBurnState.player.curHp = 7;
    giantHeadBurnState.player.maxHp = 78;
    giantHeadBurnState.player.block = 39;
    giantHeadBurnState.player.cardDrawPerTurn = 0;
    giantHeadBurnState.player.setHasRelic<RelicId::RUNIC_PYRAMID>(true);
    giantHeadBurnState.player.setHasRelic<RelicId::SELF_FORMING_CLAY>(true);
    giantHeadBurnState.cards.cardsInHand = 3;
    giantHeadBurnState.cards.hand[0] = CardInstance(CardId::BURN);
    giantHeadBurnState.cards.hand[0].setUniqueId(1);
    giantHeadBurnState.cards.hand[1] = CardInstance(CardId::BURN);
    giantHeadBurnState.cards.hand[1].setUniqueId(2);
    giantHeadBurnState.cards.hand[2] = CardInstance(CardId::DEFEND_RED);
    giantHeadBurnState.cards.hand[2].setUniqueId(3);
    giantHeadBurnState.monsters.monsterCount = 1;
    giantHeadBurnState.monsters.monstersAlive = 1;
    auto &giantHead = giantHeadBurnState.monsters.arr[0];
    giantHead.idx = 0;
    giantHead.id = MonsterId::GIANT_HEAD;
    giantHead.curHp = 220;
    giantHead.maxHp = 520;
    giantHead.moveHistory[0] = MMID::GIANT_HEAD_IT_IS_TIME;
    giantHead.setStatus<MonsterStatus::WEAK>(2);
    check(giantHead.getMoveBaseDamage(giantHeadBurnState).damage == 55,
          "Giant Head turn seven must expose 55 base attack damage");
    search::Action(search::ActionType::END_TURN).execute(giantHeadBurnState);
    check(giantHeadBurnState.player.curHp == 1,
          "run 95 Giant Head turn seven must include both retained Burns and "
          "the correctly scaled 41-damage weak attack");
    check(giantHeadBurnState.player.block == 3,
          "Self-Forming Clay must preserve the run 95 next-turn block state");

    // Feed and other max-HP gains can heal the player across Red Skull's
    // half-health boundary.  Crossing upward must remove its 3 Strength;
    // a positive strength debuff accidentally doubled it in the simulator.
    Player redSkullPlayer;
    redSkullPlayer.curHp = 36;
    redSkullPlayer.maxHp = 75;
    redSkullPlayer.strength = 3;
    redSkullPlayer.setHasRelic<RelicId::RED_SKULL>(true);
    redSkullPlayer.increaseMaxHp(4);
    check(redSkullPlayer.curHp == 40
              && redSkullPlayer.maxHp == 79
              && redSkullPlayer.strength == 0,
          "max-HP healing across half health must remove Red Skull Strength");

    const auto lagavulinJson = [] (int turn, int metallicize) {
        return nlohmann::json{
            {"game_state", {
                {"seed", 1},
                {"ascension_level", 20},
                {"act", 1},
                {"floor", 10},
                {"class", "IRONCLAD"},
                {"current_hp", 60},
                {"max_hp", 75},
                {"gold", 0},
                {"room_type", "MonsterRoomElite"},
                {"relics", nlohmann::json::array()},
                {"potions", nlohmann::json::array()},
                {"combat_state", {
                    {"turn", turn},
                    {"cards_discarded_this_turn", 0},
                    {"times_damaged", 0},
                    {"draw_pile", nlohmann::json::array()},
                    {"discard_pile", nlohmann::json::array()},
                    {"hand", nlohmann::json::array({{
                        {"id", "Defend_R"},
                        {"upgrades", 0},
                        {"cost", 1},
                    }})},
                    {"exhaust_pile", nlohmann::json::array()},
                    {"player", {
                        {"energy", 3},
                        {"block", 0},
                        {"powers", nlohmann::json::array()},
                    }},
                    {"monsters", nlohmann::json::array({{
                        {"id", "Lagavulin"},
                        {"is_gone", false},
                        {"half_dead", false},
                        {"current_hp", 115},
                        {"max_hp", 115},
                        {"block", metallicize},
                        {"intent", "SLEEP"},
                        {"move_id", 5},
                        {"last_move_id", 5},
                        {"powers", nlohmann::json::array({{
                            {"id", "Metallicize"},
                            {"amount", metallicize},
                        }})},
                    }})},
                }},
            }},
        };
    };

    const auto hexaghostJson = [&lagavulinJson] (
        int turn,
        int moveId
    ) {
        auto json = lagavulinJson(turn, 0);
        json["game_state"]["room_type"] = "MonsterRoomBoss";
        json["game_state"]["combat_state"]["monsters"][0] = {
            {"id", "Hexaghost"},
            {"is_gone", false},
            {"half_dead", false},
            {"current_hp", 250},
            {"max_hp", 250},
            {"block", 0},
            {"move_id", moveId},
            {"powers", nlohmann::json::array()},
        };
        return json;
    };

    auto importedDividerJson = hexaghostJson(2, 1);
    importedDividerJson["game_state"]["combat_state"]["monsters"][0]
        ["move_base_damage"] = 7;
    importedDividerJson["game_state"]["combat_state"]["monsters"][0]
        ["move_adjusted_damage"] = 7;
    importedDividerJson["game_state"]["combat_state"]["monsters"][0]
        ["move_hits"] = 6;
    int dividerMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedDivider = converter.convertFromJson(
        importedDividerJson,
        dividerMonsterIdxMap
    );
    check(importedDivider.monsters.arr[0].miscInfo == 7,
          "an imported Hexaghost Divider must restore its visible per-hit "
          "damage instead of simulating a zero-damage attack");
    const int dividerStartHp = importedDivider.player.curHp;
    search::Action(search::ActionType::END_TURN).execute(importedDivider);
    check(importedDivider.player.curHp == dividerStartHp - 42,
          "an imported 7x6 Hexaghost Divider must deal all six hits");

    auto depletedHeartInvincibleJson = lagavulinJson(7, 0);
    depletedHeartInvincibleJson["game_state"]["room_type"] =
        "MonsterRoomBoss";
    depletedHeartInvincibleJson["game_state"]["combat_state"]
        ["monsters"][0] = {
            {"id", "CorruptHeart"},
            {"is_gone", false},
            {"half_dead", false},
            {"current_hp", 399},
            {"max_hp", 800},
            {"block", 0},
            {"move_id", 4},
            {"powers", nlohmann::json::array({{
                {"id", "Invincible"},
                {"amount", 0},
            }})},
        };
    int depletedHeartMonsterMap[5] {-1, -1, -1, -1, -1};
    BattleContext depletedHeart = converter.convertFromJson(
        depletedHeartInvincibleJson,
        depletedHeartMonsterMap
    );
    check(
        depletedHeart.monsters.arr[0]
            .hasStatus<MonsterStatus::INVINCIBLE>(),
        "a depleted imported Invincible power must remain present"
    );
    check(
        depletedHeart.monsters.arr[0]
            .getStatus<MonsterStatus::INVINCIBLE>() == 0,
        "a depleted imported Invincible power must retain zero allowance"
    );
    depletedHeart.monsters.arr[0]
        .setStatus<MonsterStatus::INVINCIBLE>(200);
    depletedHeart.monsters.arr[0].damage(depletedHeart, 200);
    check(
        depletedHeart.monsters.arr[0]
            .hasStatus<MonsterStatus::INVINCIBLE>()
            && depletedHeart.monsters.arr[0]
                   .getStatus<MonsterStatus::INVINCIBLE>() == 0,
        "consuming Invincible in simulation must preserve the power at zero"
    );
    depletedHeart.monsters.arr[0].applyStartOfTurnPowers(depletedHeart);
    check(
        depletedHeart.monsters.arr[0]
            .getStatus<MonsterStatus::INVINCIBLE>() == 200,
        "Corrupt Heart must restore its A20 Invincible allowance next turn"
    );

    auto surroundedFacingJson = lagavulinJson(3, 0);
    surroundedFacingJson["game_state"]["act"] = 4;
    surroundedFacingJson["game_state"]["combat_state"]["player"]
        ["facing_left"] = true;
    surroundedFacingJson["game_state"]["combat_state"]["player"]
        ["powers"] = nlohmann::json::array({{
            {"id", "Surrounded"},
            {"amount", -1},
        }});
    surroundedFacingJson["game_state"]["combat_state"]["monsters"] =
        nlohmann::json::array({
            {
                {"id", "SpireShield"},
                {"is_gone", false},
                {"half_dead", false},
                {"current_hp", 125},
                {"max_hp", 125},
                {"block", 0},
                {"move_id", 2},
                {"powers", nlohmann::json::array()},
            },
            {
                {"id", "SpireSpear"},
                {"is_gone", false},
                {"half_dead", false},
                {"current_hp", 180},
                {"max_hp", 180},
                {"block", 0},
                {"move_id", 1},
                {"powers", nlohmann::json::array()},
            },
        });
    int surroundedMonsterMap[5] {-1, -1, -1, -1, -1};
    const BattleContext surroundedFacing = converter.convertFromJson(
        surroundedFacingJson,
        surroundedMonsterMap
    );
    check(
        surroundedFacing.monsters
            .arr[surroundedFacing.player.lastTargetedMonster].id
            == MonsterId::SPIRE_SHIELD,
        "a left-facing imported player must face Spire Shield"
    );
    surroundedFacingJson["game_state"]["combat_state"]["player"]
        ["facing_left"] = false;
    const BattleContext surroundedFacingRight = converter.convertFromJson(
        surroundedFacingJson,
        surroundedMonsterMap
    );
    check(
        surroundedFacingRight.monsters
            .arr[surroundedFacingRight.player.lastTargetedMonster].id
            == MonsterId::SPIRE_SPEAR,
        "a right-facing imported player must face Spire Spear"
    );

    const auto checkInferredHexaghostOrbs = [
        &converter,
        &check,
        &hexaghostJson
    ] (int turn, int moveId, int expectedOrbs) {
        int monsterIdxMap[5] {-1, -1, -1, -1, -1};
        const BattleContext imported = converter.convertFromJson(
            hexaghostJson(turn, moveId),
            monsterIdxMap
        );
        check(
            imported.monsters.arr[0].uniquePower0 == expectedOrbs,
            "a Hexaghost state without active_orbs must recover its cycle counter"
        );
    };
    checkInferredHexaghostOrbs(1, 5, 0);
    checkInferredHexaghostOrbs(2, 1, 0);
    checkInferredHexaghostOrbs(3, 4, 0);
    checkInferredHexaghostOrbs(5, 4, 2);
    checkInferredHexaghostOrbs(6, 3, 3);
    checkInferredHexaghostOrbs(9, 6, 6);
    checkInferredHexaghostOrbs(10, 4, 0);

    // Seed 85 reached the asymmetric Slime Boss state where Acid Slime L had
    // split but Spike Slime L had not. CommunicationMod keeps both gone
    // parents in the monster history. The converter must preserve slot 1 for
    // Spike Slime L's future child instead of compacting an Acid Slime M into
    // it; otherwise that split overwrites a live monster while monstersAlive
    // still counts it and random-target attacks eventually assert.
    auto partialSlimeSplitJson = lagavulinJson(10, 0);
    partialSlimeSplitJson["game_state"]["room_type"] = "MonsterRoomBoss";
    partialSlimeSplitJson["game_state"]["combat_state"]["monsters"] =
        nlohmann::json::array({
            {
                {"id", "SpikeSlime_L"},
                {"is_gone", false},
                {"half_dead", false},
                {"current_hp", 34},
                {"max_hp", 55},
                {"block", 0},
                {"move_id", 1},
                {"last_move_id", 4},
                {"powers", nlohmann::json::array()},
            },
            {
                {"id", "AcidSlime_M"},
                {"is_gone", false},
                {"half_dead", false},
                {"current_hp", 15},
                {"max_hp", 15},
                {"block", 0},
                {"move_id", 4},
                {"powers", nlohmann::json::array()},
            },
            {
                {"id", "SlimeBoss"},
                {"is_gone", true},
                {"half_dead", false},
                {"current_hp", 0},
                {"max_hp", 150},
                {"block", 0},
                {"move_id", 3},
                {"powers", nlohmann::json::array()},
            },
            {
                {"id", "AcidSlime_L"},
                {"is_gone", true},
                {"half_dead", false},
                {"current_hp", 0},
                {"max_hp", 55},
                {"block", 0},
                {"move_id", 3},
                {"powers", nlohmann::json::array()},
            },
            {
                {"id", "AcidSlime_M"},
                {"is_gone", false},
                {"half_dead", false},
                {"current_hp", 15},
                {"max_hp", 15},
                {"block", 0},
                {"move_id", 2},
                {"powers", nlohmann::json::array()},
            },
        });
    int partialSlimeMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedPartialSlime = converter.convertFromJson(
        partialSlimeSplitJson,
        partialSlimeMonsterIdxMap
    );
    check(importedPartialSlime.monsters.monsterCount == 4
              && importedPartialSlime.monsters.monstersAlive == 3,
          "seed 85 partial Slime Boss split must preserve three live monsters in four slots");
    check(importedPartialSlime.monsters.arr[0].id == MonsterId::SPIKE_SLIME_L
              && importedPartialSlime.monsters.arr[1].idx == -1
              && importedPartialSlime.monsters.arr[2].id == MonsterId::ACID_SLIME_M
              && importedPartialSlime.monsters.arr[3].id == MonsterId::ACID_SLIME_M,
          "seed 85 partial Slime Boss split must reserve slot 1 for the pending left split");
    check(partialSlimeMonsterIdxMap[0] == 0
              && partialSlimeMonsterIdxMap[1] == -1
              && partialSlimeMonsterIdxMap[2] == 1
              && partialSlimeMonsterIdxMap[3] == 4,
          "seed 85 partial Slime Boss split must retain external target ids");
    importedPartialSlime.monsters.arr[0].largeSlimeSplit(
        importedPartialSlime,
        MonsterId::SPIKE_SLIME_M,
        0,
        17
    );
    check(importedPartialSlime.monsters.monsterCount == 4
              && importedPartialSlime.monsters.monstersAlive == 4
              && importedPartialSlime.monsters.getTargetableCount() == 4,
          "seed 85 pending left split must produce four consistent live targets");

    auto externalHexaghostOrbsJson = hexaghostJson(7, 2);
    externalHexaghostOrbsJson["game_state"]["combat_state"]["monsters"][0]
                             ["active_orbs"] = 4;
    int externalHexaghostMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedExternalHexaghostOrbs =
        converter.convertFromJson(
            externalHexaghostOrbsJson,
            externalHexaghostMonsterIdxMap
        );
    check(importedExternalHexaghostOrbs.monsters.arr[0].uniquePower0 == 4,
          "an explicit valid Hexaghost active_orbs value must be preserved");

    const auto automatonJson = [&lagavulinJson] (
        int turn,
        int moveId
    ) {
        auto json = lagavulinJson(turn, 0);
        json["game_state"]["act"] = 2;
        json["game_state"]["room_type"] = "MonsterRoomBoss";
        json["game_state"]["combat_state"]["monsters"][0] = {
            {"id", "BronzeAutomaton"},
            {"is_gone", false},
            {"half_dead", false},
            {"current_hp", 300},
            {"max_hp", 300},
            {"block", 0},
            {"move_id", moveId},
            {"powers", nlohmann::json::array({{
                {"id", "Artifact"},
                {"amount", 3},
            }})},
        };
        return json;
    };
    const auto checkInferredAutomatonPhase = [
        &converter,
        &check,
        &automatonJson
    ] (int turn, int moveId, int expectedBoostPhase) {
        int monsterIdxMap[5] {-1, -1, -1, -1, -1};
        const BattleContext imported = converter.convertFromJson(
            automatonJson(turn, moveId),
            monsterIdxMap
        );
        check(
            imported.monsters.arr[1].miscInfo == expectedBoostPhase,
            "Bronze Automaton import must recover its private Boost phase"
        );
    };
    checkInferredAutomatonPhase(3, 5, 0);
    checkInferredAutomatonPhase(4, 1, 1);
    checkInferredAutomatonPhase(5, 5, 1);
    checkInferredAutomatonPhase(6, 2, 0);
    checkInferredAutomatonPhase(7, 5, -1);
    checkInferredAutomatonPhase(8, 1, 0);
    checkInferredAutomatonPhase(9, 5, 0);
    checkInferredAutomatonPhase(10, 1, 1);
    checkInferredAutomatonPhase(11, 5, 1);
    checkInferredAutomatonPhase(12, 2, 0);

    auto discoveryJson = lagavulinJson(1, 12);
    discoveryJson["game_state"]["screen_type"] = "CARD_REWARD";
    discoveryJson["game_state"]["room_phase"] = "COMBAT";
    discoveryJson["game_state"]["current_action"] = "DiscoveryAction";
    discoveryJson["game_state"]["screen_state"] = {
        {"cards", nlohmann::json::array({
            {{"id", "Sword Boomerang"}},
            {{"id", "Fiend Fire"}},
            {{"id", "Pommel Strike"}},
        })},
    };
    discoveryJson["mcts_card_select"] = {
        {"task", "DISCOVERY"},
        {"copy_count", 2},
    };
    int discoveryMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedDiscovery = converter.convertFromJson(
        discoveryJson, discoveryMonsterIdxMap
    );
    check(importedDiscovery.inputState == InputState::CARD_SELECT
              && importedDiscovery.cardSelectInfo.cardSelectTask
                     == CardSelectTask::DISCOVERY
              && importedDiscovery.cardSelectInfo.discovery_CopyCount() == 2,
          "an external DiscoveryAction must import as a DISCOVERY CARD_SELECT root");
    check(importedDiscovery.cardSelectInfo.discovery_Cards()[0]
                  == CardId::SWORD_BOOMERANG
              && importedDiscovery.cardSelectInfo.discovery_Cards()[1]
                     == CardId::FIEND_FIRE
              && importedDiscovery.cardSelectInfo.discovery_Cards()[2]
                     == CardId::POMMEL_STRIKE,
          "external Discovery candidates must preserve the real screen order");
    auto discoveryActions = search::Action::enumerateCardSelectActions(
        importedDiscovery
    );
    check(discoveryActions.size() == 3,
          "an imported Discovery root must expose exactly three MCTS actions");
    if (discoveryActions.size() == 3) {
        discoveryActions[1].execute(importedDiscovery);
        int generatedFiendFires = 0;
        int zeroCostFiendFires = 0;
        for (int i = 0; i < importedDiscovery.cards.cardsInHand; ++i) {
            const auto &card = importedDiscovery.cards.hand[i];
            if (card.getId() == CardId::FIEND_FIRE) {
                ++generatedFiendFires;
                zeroCostFiendFires += card.costForTurn == 0 ? 1 : 0;
            }
        }
        check(importedDiscovery.inputState == InputState::PLAYER_NORMAL
                  && generatedFiendFires == 2
                  && zeroCostFiendFires == 2,
              "choosing an imported Sacred Bark candidate must add two zero-cost copies and resume combat");
    }

    auto toolboxJson = lagavulinJson(1, 12);
    toolboxJson["game_state"]["screen_type"] = "CARD_REWARD";
    toolboxJson["game_state"]["room_phase"] = "COMBAT";
    toolboxJson["game_state"]["current_action"] = "ChooseOneColorless";
    toolboxJson["game_state"]["screen_state"] = {
        {"cards", nlohmann::json::array({
            {{"id", "Master of Strategy"}},
            {{"id", "The Bomb"}},
            {{"id", "HandOfGreed"}},
        })},
    };
    toolboxJson["mcts_card_select"] = {
        {"task", "TOOLBOX"},
    };
    int toolboxMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedToolbox = converter.convertFromJson(
        toolboxJson, toolboxMonsterIdxMap
    );
    check(importedToolbox.inputState == InputState::CARD_SELECT
              && importedToolbox.cardSelectInfo.cardSelectTask
                     == CardSelectTask::TOOLBOX,
          "an external ChooseOneColorless must import as a TOOLBOX CARD_SELECT root");
    auto toolboxActions = search::Action::enumerateCardSelectActions(
        importedToolbox
    );
    check(toolboxActions.size() == 3,
          "an imported Toolbox root must expose exactly three MCTS actions");
    if (toolboxActions.size() == 3) {
        const int handSizeBefore = importedToolbox.cards.cardsInHand;
        toolboxActions[1].execute(importedToolbox);
        int generatedBombs = 0;
        int normalCostBombs = 0;
        for (int i = 0; i < importedToolbox.cards.cardsInHand; ++i) {
            const auto &card = importedToolbox.cards.hand[i];
            if (card.getId() == CardId::THE_BOMB) {
                ++generatedBombs;
                normalCostBombs += card.costForTurn == 2 ? 1 : 0;
            }
        }
        check(importedToolbox.inputState == InputState::PLAYER_NORMAL
                  && importedToolbox.cards.cardsInHand == handSizeBefore + 1
                  && generatedBombs == 1
                  && normalCostBombs == 1,
              "choosing an imported Toolbox candidate must add one normal-cost card and resume combat");
    }

    auto gambleJson = lagavulinJson(1, 12);
    gambleJson["game_state"]["screen_type"] = "HAND_SELECT";
    gambleJson["game_state"]["room_phase"] = "COMBAT";
    gambleJson["game_state"]["current_action"] = "GamblingChipAction";
    gambleJson["game_state"]["screen_state"] = {
        {"can_pick_zero", true},
        {"max_cards", 10},
        {"selected", nlohmann::json::array()},
    };
    gambleJson["game_state"]["combat_state"]["hand"] =
        nlohmann::json::array({
            {{"id", "Defend_R"}, {"upgrades", 0}, {"cost", 1}},
            {{"id", "Strike_R"}, {"upgrades", 0}, {"cost", 1}},
            {{"id", "Bash"}, {"upgrades", 0}, {"cost", 2}},
        });
    gambleJson["mcts_card_select"] = {{"task", "GAMBLE"}};
    int gambleMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedGamble = converter.convertFromJson(
        gambleJson, gambleMonsterIdxMap
    );
    check(importedGamble.inputState == InputState::CARD_SELECT
              && importedGamble.cardSelectInfo.cardSelectTask
                     == CardSelectTask::GAMBLE,
          "an external GamblingChipAction must import as a GAMBLE CARD_SELECT root");
    const auto gambleActions = search::Action::enumerateCardSelectActions(
        importedGamble
    );
    check(gambleActions.size() == 8,
          "an imported three-card Gambling Chip hand must expose all 2^3 subsets");

    auto hologramJson = lagavulinJson(1, 12);
    hologramJson["game_state"]["screen_type"] = "GRID";
    hologramJson["game_state"]["room_phase"] = "COMBAT";
    hologramJson["game_state"]["current_action"] =
        "BetterDiscardPileToHandAction";
    hologramJson["game_state"]["screen_state"] = {{"num_cards", 1}};
    hologramJson["game_state"]["combat_state"]["discard_pile"] =
        nlohmann::json::array({
            {{"id", "Strike_R"}, {"upgrades", 0}, {"cost", 1}},
            {{"id", "Defend_R"}, {"upgrades", 0}, {"cost", 1}},
        });
    hologramJson["mcts_card_select"] = {{"task", "HOLOGRAM"}};
    int hologramMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedHologram = converter.convertFromJson(
        hologramJson, hologramMonsterIdxMap
    );
    check(importedHologram.inputState == InputState::CARD_SELECT
              && importedHologram.cardSelectInfo.cardSelectTask
                     == CardSelectTask::HOLOGRAM
              && importedHologram.cardSelectInfo.pickCount == 1,
          "an external Hologram grid must import as a HOLOGRAM CARD_SELECT root");
    check(search::Action::enumerateCardSelectActions(importedHologram).size() == 2,
          "an imported Hologram root must expose every discard-pile card");

    auto codexJson = lagavulinJson(1, 12);
    codexJson["game_state"]["screen_type"] = "CARD_REWARD";
    codexJson["game_state"]["room_phase"] = "COMBAT";
    codexJson["game_state"]["current_action"] = "CodexAction";
    codexJson["game_state"]["screen_state"] = {
        {"cards", nlohmann::json::array({
            {{"id", "Sword Boomerang"}},
            {{"id", "Fiend Fire"}},
            {{"id", "Pommel Strike"}},
        })},
        {"skip_available", true},
    };
    codexJson["mcts_card_select"] = {
        {"task", "CODEX"},
    };
    int codexMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedCodex = converter.convertFromJson(
        codexJson, codexMonsterIdxMap
    );
    check(importedCodex.inputState == InputState::CARD_SELECT
              && importedCodex.cardSelectInfo.cardSelectTask
                     == CardSelectTask::CODEX,
          "an external CodexAction must import as a CODEX CARD_SELECT root");
    auto codexActions = search::Action::enumerateCardSelectActions(importedCodex);
    check(codexActions.size() == 4,
          "an imported Codex root must expose three cards and skip");
    const auto combatCardCount = [] (const BattleContext &bc) {
        return bc.cards.cardsInHand
            + bc.cards.drawPile.size()
            + bc.cards.discardPile.size()
            + bc.cards.exhaustPile.size();
    };
    if (codexActions.size() == 4) {
        const auto beforePickCount = combatCardCount(importedCodex);
        codexActions[1].execute(importedCodex);
        check(importedCodex.inputState == InputState::PLAYER_NORMAL
                  && importedCodex.turn == 1
                  && combatCardCount(importedCodex) == beforePickCount + 1,
              "choosing an imported Codex card must add it, finish the monster turn, and resume on the next player turn");

        int skipMonsterIdxMap[5] {-1, -1, -1, -1, -1};
        BattleContext importedCodexSkip = converter.convertFromJson(
            codexJson, skipMonsterIdxMap
        );
        const auto beforeSkipCount = combatCardCount(importedCodexSkip);
        codexActions[3].execute(importedCodexSkip);
        check(importedCodexSkip.inputState == InputState::PLAYER_NORMAL
                  && importedCodexSkip.turn == 1
                  && combatCardCount(importedCodexSkip) == beforeSkipCount,
              "skipping an imported Codex card must add nothing and still finish the turn");
    }

    int lagavulinMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext sleepingLagavulin = converter.convertFromJson(
        lagavulinJson(1, 12), lagavulinMonsterIdxMap
    );
    check(sleepingLagavulin.turn == 0
              && sleepingLagavulin.getMonsterTurnNumber() == 1,
          "CommunicationMod turn 1 must import as internal turn 0");
    const search::Action endLagavulinTurn(search::ActionType::END_TURN);
    check(endLagavulinTurn.isValidAction(sleepingLagavulin),
          "a sleeping Lagavulin turn must allow END_TURN");
    for (int completedSleepTurns = 1; completedSleepTurns <= 2; ++completedSleepTurns) {
        endLagavulinTurn.execute(sleepingLagavulin);
        const auto &lagavulin = sleepingLagavulin.monsters.arr[0];
        check(sleepingLagavulin.turn == completedSleepTurns
                  && lagavulin.moveHistory[0] == MMID::LAGAVULIN_SLEEP
                  && lagavulin.hasStatus<MS::ASLEEP>()
                  && lagavulin.getStatus<MS::METALLICIZE>() == 12
                  && sleepingLagavulin.player.curHp == 60,
              "Lagavulin must remain asleep and harmless for its first three player turns");
    }
    endLagavulinTurn.execute(sleepingLagavulin);
    const auto &naturallyAwakeLagavulin = sleepingLagavulin.monsters.arr[0];
    check(sleepingLagavulin.turn == 3
              && naturallyAwakeLagavulin.moveHistory[0]
                     == MMID::LAGAVULIN_ATTACK
              && !naturallyAwakeLagavulin.hasStatus<MS::ASLEEP>()
              && naturallyAwakeLagavulin.getStatus<MS::METALLICIZE>() == 4
              && sleepingLagavulin.player.curHp == 60,
          "Lagavulin must wake after its third Sleep, remove only its base Metallicize, and attack on turn 4");

    // run_bc/147 floor 10: CommunicationMod reports the zero-damage wake-up
    // move as move_id=4 with intent=STUN. It is not another sleeping turn.
    auto wakingLagavulinJson = lagavulinJson(1, 0);
    auto &wakingLagavulinMonster =
        wakingLagavulinJson["game_state"]["combat_state"]["monsters"][0];
    wakingLagavulinMonster["current_hp"] = 82;
    wakingLagavulinMonster["max_hp"] = 114;
    wakingLagavulinMonster["intent"] = "STUN";
    wakingLagavulinMonster["move_id"] = 4;
    wakingLagavulinMonster["last_move_id"] = 5;
    wakingLagavulinMonster["powers"] = nlohmann::json::array();
    int wakingLagavulinMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext wakingLagavulin = converter.convertFromJson(
        wakingLagavulinJson, wakingLagavulinMonsterIdxMap
    );
    check(!wakingLagavulin.monsters.arr[0].hasStatus<MS::ASLEEP>(),
          "Lagavulin STUN intent must import as already awake");
    endLagavulinTurn.execute(wakingLagavulin);
    check(wakingLagavulin.turn == 1
              && wakingLagavulin.monsters.arr[0].moveHistory[0]
                     == MMID::LAGAVULIN_ATTACK
              && !wakingLagavulin.monsters.arr[0].hasStatus<MS::ASLEEP>()
              && wakingLagavulin.player.curHp == 60,
          "ending Lagavulin's wake-up turn must advance to its attack intent without granting another sleep turn");

    // remote/35 floor 14 reached its second Gremlin Wizard charge with
    // move_id=2 and last_move_id=2. CommunicationMod does not expose the
    // private counter, so conversion must reconstruct it from move history.
    auto firstWizardChargeJson = lagavulinJson(1, 0);
    firstWizardChargeJson["game_state"]["current_hp"] = 15;
    firstWizardChargeJson["game_state"]["room_type"] = "MonsterRoom";
    firstWizardChargeJson["game_state"]["combat_state"]["monsters"][0] = {
        {"id", "GremlinWizard"},
        {"half_dead", false},
        {"current_hp", 26},
        {"max_hp", 26},
        {"block", 0},
        {"move_id", 2},
        {"powers", nlohmann::json::array()},
    };
    int firstWizardMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedFirstWizardCharge = converter.convertFromJson(
        firstWizardChargeJson, firstWizardMonsterIdxMap
    );
    check(importedFirstWizardCharge.monsters.arr[0].miscInfo == 1,
          "a first-turn Gremlin Wizard must import with one accumulated charge");
    endLagavulinTurn.execute(importedFirstWizardCharge);
    check(importedFirstWizardCharge.monsters.arr[0].miscInfo == 2
              && importedFirstWizardCharge.monsters.arr[0].moveHistory[0]
                     == MMID::GREMLIN_WIZARD_CHARGING,
          "ending the first imported Wizard turn must advance to its second charge");

    auto secondWizardChargeJson = firstWizardChargeJson;
    secondWizardChargeJson["game_state"]["combat_state"]["turn"] = 2;
    secondWizardChargeJson["game_state"]["combat_state"]["monsters"][0]
                          ["current_hp"] = 17;
    secondWizardChargeJson["game_state"]["combat_state"]["monsters"][0]
                          ["block"] = 11;
    secondWizardChargeJson["game_state"]["combat_state"]["monsters"][0]
                          ["last_move_id"] = 2;
    int secondWizardMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedSecondWizardCharge = converter.convertFromJson(
        secondWizardChargeJson, secondWizardMonsterIdxMap
    );
    check(importedSecondWizardCharge.monsters.arr[0].miscInfo == 2,
          "remote/35's second Gremlin Wizard charge must not reset during MCTS import");
    endLagavulinTurn.execute(importedSecondWizardCharge);
    check(importedSecondWizardCharge.monsters.arr[0].miscInfo == 3
              && importedSecondWizardCharge.monsters.arr[0].moveHistory[0]
                     == MMID::GREMLIN_WIZARD_ULTIMATE_BLAST,
          "ending remote/35's second charge must reveal Ultimate Blast for the next turn");

    int halfDeadMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleContext importedHalfDead = converter.convertFromJson(
        awakenedOneJson(true, true, 0, 3, 3),
        halfDeadMonsterIdxMap
    );
    check(importedHalfDead.monsters.monsterCount == 1,
          "a half-dead Awakened One must not be dropped as an ordinary gone monster");
    if (importedHalfDead.monsters.monsterCount == 1) {
        const auto &awakenedOne = importedHalfDead.monsters.arr[0];
        check(awakenedOne.id == MonsterId::AWAKENED_ONE
                  && awakenedOne.halfDead
                  && awakenedOne.moveHistory[0] == MMID::AWAKENED_ONE_REBIRTH
                  && importedHalfDead.monsters.monstersAlive == 0,
              "the imported Awakened One must preserve its pending rebirth state");

        const search::Action endTurn(search::ActionType::END_TURN);
        check(endTurn.isValidAction(importedHalfDead),
              "the half-dead Awakened One transition must allow ending the turn");
        if (endTurn.isValidAction(importedHalfDead)) {
            endTurn.execute(importedHalfDead);
            const auto &revived = importedHalfDead.monsters.arr[0];
            check(!revived.halfDead
                      && revived.miscInfo
                      && revived.curHp == 320
                      && revived.moveHistory[0] == MMID::AWAKENED_ONE_DARK_ECHO
                      && importedHalfDead.monsters.monstersAlive == 1
                      && importedHalfDead.outcome == Outcome::UNDECIDED,
                  "ending the transition turn must revive Awakened One into phase two");
        }
    }

    int phaseTwoMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedPhaseTwo = converter.convertFromJson(
        awakenedOneJson(false, false, 317, 5, 3),
        phaseTwoMonsterIdxMap
    );
    check(importedPhaseTwo.monsters.monsterCount == 1
              && importedPhaseTwo.monsters.arr[0].miscInfo,
          "an Awakened One on Dark Echo must be restored as phase two when miscInfo is absent");

    auto activeLizardJson = awakenedOneJson(false, false, 317, 5, 3);
    activeLizardJson["game_state"]["relics"] = nlohmann::json::array({{
        {"id", "Lizard Tail"},
        {"counter", -1},
    }});
    int activeLizardMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedActiveLizard = converter.convertFromJson(
        activeLizardJson, activeLizardMonsterIdxMap
    );
    check(importedActiveLizard.player.hasRelic<R::LIZARD_TAIL>(),
          "CommunicationMod counter -1 must import an unused Lizard Tail");

    auto consumedLizardJson = activeLizardJson;
    consumedLizardJson["game_state"]["relics"][0]["counter"] = -2;
    int consumedLizardMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedConsumedLizard = converter.convertFromJson(
        consumedLizardJson, consumedLizardMonsterIdxMap
    );
    check(!importedConsumedLizard.player.hasRelic<R::LIZARD_TAIL>(),
          "CommunicationMod counter -2 must import a consumed Lizard Tail");

    auto activePuzzleJson = awakenedOneJson(false, false, 317, 5, 3);
    activePuzzleJson["game_state"]["relics"] = nlohmann::json::array({{
        {"id", "Centennial Puzzle"},
        {"counter", -1},
    }});
    activePuzzleJson["game_state"]["combat_state"]
                    ["centennial_puzzle_used_this_combat"] = false;
    int activePuzzleMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedActivePuzzle = converter.convertFromJson(
        activePuzzleJson, activePuzzleMonsterIdxMap
    );
    check(importedActivePuzzle.player.hasRelic<R::CENTENNIAL_PUZZLE>(),
          "an unused Centennial Puzzle must remain available in an imported combat");

    auto consumedPuzzleJson = activePuzzleJson;
    consumedPuzzleJson["game_state"]["combat_state"]
                      ["centennial_puzzle_used_this_combat"] = true;
    int consumedPuzzleMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedConsumedPuzzle = converter.convertFromJson(
        consumedPuzzleJson, consumedPuzzleMonsterIdxMap
    );
    check(!importedConsumedPuzzle.player.hasRelic<R::CENTENNIAL_PUZZLE>(),
          "a fired Centennial Puzzle must not trigger again in a reconstructed rollout");

    auto rampageJson = lagavulinJson(4, 0);
    rampageJson["game_state"]["combat_state"]["hand"][0] = {
        {"id", "Rampage"},
        {"upgrades", 1},
        {"cost", 1},
        {"misc", 24},
    };
    int rampageMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedRampage = converter.convertFromJson(
        rampageJson, rampageMonsterIdxMap
    );
    check(importedRampage.cards.cardsInHand == 1
              && importedRampage.cards.hand[0].id == CardId::RAMPAGE
              && importedRampage.cards.hand[0].specialData == 24,
          "Rampage accumulated damage must survive a mid-combat state import");

    auto searingBlowJson = lagavulinJson(4, 0);
    searingBlowJson["game_state"]["combat_state"]["hand"][0] = {
        {"id", "Searing Blow"},
        {"upgrades", 4},
        {"cost", 2},
        {"misc", 0},
    };
    int searingBlowMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedSearingBlow = converter.convertFromJson(
        searingBlowJson, searingBlowMonsterIdxMap
    );
    check(importedSearingBlow.cards.hand[0].id == CardId::SEARING_BLOW
              && importedSearingBlow.cards.hand[0].getUpgradeCount() == 4,
          "Searing Blow must preserve its full upgrade count during import");

    auto timeEaterJson = lagavulinJson(9, 0);
    timeEaterJson["game_state"]["act"] = 3;
    timeEaterJson["game_state"]["room_type"] = "MonsterRoomBoss";
    timeEaterJson["game_state"]["combat_state"]["monsters"][0] = {
        {"id", "TimeEater"},
        {"is_gone", false},
        {"half_dead", false},
        {"current_hp", 220},
        {"max_hp", 480},
        {"block", 0},
        {"move_id", 2},
        {"last_move_id", 3},
        {"second_last_move_id", 2},
        {"miscBool", true},
        {"powers", nlohmann::json::array()},
    };
    int timeEaterMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedTimeEater = converter.convertFromJson(
        timeEaterJson, timeEaterMonsterIdxMap
    );
    check(importedTimeEater.monsters.arr[0].miscInfo,
          "Time Eater usedHaste must survive an exact state import");

    auto historicalTimeEaterJson = timeEaterJson;
    historicalTimeEaterJson["game_state"]["combat_state"]["monsters"][0]
                           .erase("miscBool");
    historicalTimeEaterJson["game_state"]["combat_state"]["monsters"][0]
                           ["second_last_move_id"] = 5;
    int historicalTimeEaterMonsterIdxMap[5] {-1, -1, -1, -1, -1};
    const BattleContext importedHistoricalTimeEater = converter.convertFromJson(
        historicalTimeEaterJson, historicalTimeEaterMonsterIdxMap
    );
    check(importedHistoricalTimeEater.monsters.arr[0].miscInfo,
          "an older Time Eater snapshot must infer usedHaste from visible move history");

    Monster importedMonster;
    importedMonster.setStatus(MS::SHARP_HIDE, 3);
    check(importedMonster.hasStatus<MS::SHARP_HIDE>()
              && importedMonster.getStatus<MS::SHARP_HIDE>() == 3,
          "runtime monster status import must set both presence and amount");
    importedMonster.setStatus(MS::SHARP_HIDE, 0);
    check(!importedMonster.hasStatus<MS::SHARP_HIDE>()
              && importedMonster.getStatus<MS::SHARP_HIDE>() == 0,
          "runtime monster status import must clear presence with zero amount");

    BattleContext transientShackledState;
    transientShackledState.monsters.monsterCount = 1;
    transientShackledState.monsters.monstersAlive = 1;
    auto &transientWithLargeShackled =
        transientShackledState.monsters.arr[0];
    transientWithLargeShackled.idx = 0;
    transientWithLargeShackled.id = MonsterId::TRANSIENT;
    transientWithLargeShackled.curHp = 811;
    transientWithLargeShackled.maxHp = 999;
    transientWithLargeShackled.setStatus(MS::SHIFTING, -1);
    transientWithLargeShackled.setStatus(MS::STRENGTH, -90);
    transientWithLargeShackled.setStatus(MS::SHACKLED, 90);
    transientWithLargeShackled.attacked(transientShackledState, 39);
    check(transientWithLargeShackled.getStatus<MS::STRENGTH>() == -129
              && transientWithLargeShackled.getStatus<MS::SHACKLED>() == 129,
          "Transient Shackled must not overflow after more than 127 temporary Strength loss");
    transientWithLargeShackled.applyEndOfTurnTriggers(
        transientShackledState
    );
    check(transientWithLargeShackled.getStatus<MS::STRENGTH>() == 0
              && !transientWithLargeShackled.hasStatus<MS::SHACKLED>(),
          "large Transient Shackled must restore Strength exactly at end of turn");

    BattleContext bufferedHpLossState;
    bufferedHpLossState.player.curHp = 68;
    bufferedHpLossState.player.maxHp = 75;
    bufferedHpLossState.player.buff<PS::BUFFER>(1);
    bufferedHpLossState.player.buff<PS::INTANGIBLE>(1);
    bufferedHpLossState.addToBot(Actions::PlayerLoseHp(2, true));
    bufferedHpLossState.executeActions();
    check(bufferedHpLossState.player.curHp == 68
              && !bufferedHpLossState.player.hasStatus<PS::BUFFER>(),
          "Regret-style HP loss must consume Buffer without reducing HP");
    bufferedHpLossState.player.removeStatus<PS::INTANGIBLE>();
    bufferedHpLossState.player.damage(bufferedHpLossState, 20, false);
    check(bufferedHpLossState.player.curHp == 48,
          "Buffer consumed by Regret-style HP loss must not block the next attack");

    BattleContext brutalityState;
    brutalityState.player.curHp = 20;
    brutalityState.player.maxHp = 80;
    brutalityState.player.buff<PS::BRUTALITY>(1);
    brutalityState.player.buff<PS::RUPTURE>(2);
    brutalityState.cards.drawPile.push_back(CardInstance(CardId::STRIKE_RED));
    brutalityState.player.applyStartOfTurnPostDrawPowers(brutalityState);
    brutalityState.executeActions();
    check(brutalityState.player.curHp == 19,
          "Brutality must lose one HP at start of turn");
    check(brutalityState.cards.cardsInHand == 1
              && brutalityState.cards.hand[0].getId() == CardId::STRIKE_RED,
          "Brutality must draw one card");
    check(brutalityState.player.getStatus<PS::STRENGTH>() == 2,
          "Brutality HP loss must trigger upgraded Rupture");

    BattleContext lethalBrutalityState;
    lethalBrutalityState.player.curHp = 1;
    lethalBrutalityState.player.maxHp = 80;
    lethalBrutalityState.player.buff<PS::BRUTALITY>(1);
    lethalBrutalityState.cards.drawPile.push_back(CardInstance(CardId::DEFEND_RED));
    lethalBrutalityState.player.applyStartOfTurnPostDrawPowers(
        lethalBrutalityState
    );
    lethalBrutalityState.executeActions();
    check(lethalBrutalityState.outcome == Outcome::PLAYER_LOSS
              && lethalBrutalityState.cards.cardsInHand == 1,
          "Brutality must draw before lethal HP loss resolves");

    BattleContext bookMoveState;
    bookMoveState.ascension = 20;
    Monster book;
    book.id = MonsterId::BOOK_OF_STABBING;
    book.moveHistory[0] = MMID::BOOK_OF_STABBING_MULTI_STAB;
    book.moveHistory[1] = MMID::BOOK_OF_STABBING_SINGLE_STAB;
    int stabCount = 4;
    check(book.getMoveForRoll(bookMoveState, stabCount, 0)
              == MMID::BOOK_OF_STABBING_SINGLE_STAB
              && stabCount == 5,
          "A18 Book random Single Stab must still advance the stab counter");

    book.moveHistory[1] = MMID::BOOK_OF_STABBING_MULTI_STAB;
    stabCount = 4;
    check(book.getMoveForRoll(bookMoveState, stabCount, 99)
              == MMID::BOOK_OF_STABBING_SINGLE_STAB
              && stabCount == 5,
          "A18 Book forced Single Stab must still advance the stab counter");

    bookMoveState.ascension = 17;
    book.moveHistory[1] = MMID::BOOK_OF_STABBING_SINGLE_STAB;
    stabCount = 4;
    check(book.getMoveForRoll(bookMoveState, stabCount, 0)
              == MMID::BOOK_OF_STABBING_SINGLE_STAB
              && stabCount == 4,
          "pre-A18 Book Single Stab must not advance the stab counter");

    BattleContext terminalState;
    terminalState.outcome = Outcome::PLAYER_VICTORY;
    terminalState.player.curHp = 10;
    int evaluatorCalls = 0;
    search::BattleScumSearcher2 terminalSearcher(
        terminalState,
        [&evaluatorCalls] (const BattleContext &, const BattleContext &) {
            ++evaluatorCalls;
            return 0.375;
        }
    );
    terminalSearcher.search(100, 1000);
    check(evaluatorCalls == 1, "custom evaluator must be used exactly once for a terminal root");
    check(terminalSearcher.root.simulationCount == 1, "terminal root must stop without extra simulations");
    check(std::abs(terminalSearcher.root.evaluationSum - 0.375) < 1e-12,
          "terminal root must store the custom evaluation");
    check(terminalSearcher.root.winSamples == 1
              && terminalSearcher.root.lossSamples == 0
              && terminalSearcher.root.cutoffSamples == 0,
          "a terminal winning root must record one winning sample");
    check(std::abs(terminalSearcher.root.winEndHpSum - 10.0) < 1e-12
              && std::abs(terminalSearcher.root.winEndHpSquaredSum - 100.0) < 1e-12,
          "a terminal winning root must record winning end-HP moments");
    check(
        std::abs(
            terminalSearcher.root.winUtilitySum
            - search::BattleScumSearcher2::evaluateEndState(
                terminalState,
                terminalState
            )
        ) < 1e-12,
          "a terminal winning root must record final-selection utility");

    BattleContext terminalLossState(terminalState);
    terminalLossState.outcome = Outcome::PLAYER_LOSS;
    terminalLossState.player.curHp = 10;
    search::BattleScumSearcher2 terminalLossSearcher(terminalLossState);
    terminalLossSearcher.search(100, 1000);
    check(terminalLossSearcher.outcomePlayerHp == 0,
          "a terminal loss with HP remaining must not be reported as a winning solution");
    check(terminalLossSearcher.root.winSamples == 0
              && terminalLossSearcher.root.lossSamples == 1
              && terminalLossSearcher.root.cutoffSamples == 0,
          "a terminal losing root must record one losing sample");

    BattleContext rootState;
    rootState.outcome = Outcome::UNDECIDED;
    rootState.player.curHp = 50;
    rootState.monsters.monsterCount = 1;
    rootState.monsters.monstersAlive = 1;
    rootState.monsters.arr[0].id = MonsterId::CULTIST;
    rootState.monsters.arr[0].maxHp = 100;
    rootState.monsters.arr[0].curHp = 100;

    BattleContext streamlineCostState(rootState);
    streamlineCostState.player.energy = 2;
    streamlineCostState.inputState = InputState::PLAYER_NORMAL;
    streamlineCostState.monsters.arr[0].idx = 0;
    streamlineCostState.cards.cardsInHand = 1;
    streamlineCostState.cards.hand[0] = CardInstance(CardId::STREAMLINE);
    search::Action(search::ActionType::CARD, 0, 0).execute(
        streamlineCostState
    );
    check(streamlineCostState.player.energy == 0
              && !streamlineCostState.cards.discardPile.empty()
              && streamlineCostState.cards.discardPile.back().costForTurn == 1,
          "Streamline must pay its pre-play cost before retaining the reduced cost");
    check(getPlayerStatusFromId("Repair") == PS::SELF_REPAIR,
          "CommunicationMod Repair power must import as Self Repair");

    check(search::BattleScumSearcher2::canUseImmediateVictoryShortcut(rootState),
          "ordinary encounters must retain the immediate-victory shortcut");
    BattleContext transientState(rootState);
    transientState.monsters.arr[0].id = MonsterId::TRANSIENT;
    check(!search::BattleScumSearcher2::canUseImmediateVictoryShortcut(transientState),
          "a living Transient must bypass the immediate-victory shortcut");
    transientState.monsters.arr[0].curHp = 0;
    check(search::BattleScumSearcher2::canUseImmediateVictoryShortcut(transientState),
          "a defeated Transient must not disable the shortcut");

    BattleContext immediateLethalState(rootState);
    immediateLethalState.player.maxHp = 50;
    immediateLethalState.player.energy = 1;
    immediateLethalState.inputState = InputState::PLAYER_NORMAL;
    immediateLethalState.monsters.arr[0].idx = 0;
    immediateLethalState.monsters.arr[0].curHp = 6;
    immediateLethalState.monsters.arr[0].moveHistory[0] =
        MMID::CULTIST_INCANTATION;
    immediateLethalState.cards.cardsInHand = 1;
    immediateLethalState.cards.hand[0] =
        CardInstance(CardId::STRIKE_RED);
    search::BattleScumSearcher2 immediateLethalSearcher(
        immediateLethalState
    );
    check(immediateLethalSearcher.balanceRootActions,
          "immediate-lethal regression must exercise balanced root sampling");
    immediateLethalSearcher.search(100000, 1000);
    check(immediateLethalSearcher.stopReason == "immediate_lethal"
              && immediateLethalSearcher.bestActionSequence.size() == 1,
          "balanced roots must retain the exact immediate-lethal shortcut");

    BattleContext damagingLethalState(immediateLethalState);
    damagingLethalState.player.energy = 2;
    damagingLethalState.monsters.arr[0].id = MonsterId::SPIKER;
    damagingLethalState.monsters.arr[0].buff<MS::THORNS>(3);
    damagingLethalState.cards.cardsInHand = 2;
    damagingLethalState.cards.hand[1] = CardInstance(CardId::DEFEND_RED);
    search::BattleScumSearcher2 damagingLethalSearcher(
        damagingLethalState
    );
    damagingLethalSearcher.search(100000, 1000);
    check(damagingLethalSearcher.stopReason != "immediate_lethal"
              && damagingLethalSearcher.outcomePlayerHp
                 > immediateLethalState.player.curHp - 3,
          "a damaging immediate lethal must remain a fallback while MCTS "
          "searches for a higher-HP winning line");

    BattleContext forcedEndState(rootState);
    forcedEndState.player.maxHp = 50;
    forcedEndState.player.energy = 0;
    forcedEndState.inputState = InputState::PLAYER_NORMAL;
    forcedEndState.monsters.arr[0].idx = 0;
    forcedEndState.monsters.arr[0].moveHistory[0] =
        MMID::CULTIST_INCANTATION;
    forcedEndState.cards.cardsInHand = 1;
    forcedEndState.cards.hand[0] = CardInstance(CardId::DEFEND_RED);
    search::BattleScumSearcher2 forcedEndSearcher(forcedEndState);
    forcedEndSearcher.stopOnForcedRootAction = true;
    forcedEndSearcher.search(100000, 1000);
    check(forcedEndSearcher.stopReason == "forced_root_action"
              && forcedEndSearcher.root.simulationCount == 1
              && forcedEndSearcher.root.edges.size() == 1
              && forcedEndSearcher.root.edges[0].action.getActionType()
                     == search::ActionType::END_TURN,
          "a certified single root action must stop after one rollout");

    BattleContext forcedEndWithPotionState(forcedEndState);
    forcedEndWithPotionState.potionCapacity = 1;
    forcedEndWithPotionState.potionCount = 1;
    forcedEndWithPotionState.potions.fill(Potion::EMPTY_POTION_SLOT);
    forcedEndWithPotionState.potions[0] = Potion::BLOCK_POTION;
    search::BattleScumSearcher2 forcedEndWithPotionSearcher(
        forcedEndWithPotionState
    );
    forcedEndWithPotionSearcher.allowedPotionSlotMask = 1;
    forcedEndWithPotionSearcher.stopOnForcedRootAction = true;
    forcedEndWithPotionSearcher.search(2, 1000);
    check(forcedEndWithPotionSearcher.stopReason != "forced_root_action"
              && forcedEndWithPotionSearcher.root.edges.size() > 1,
          "an authorized potion must remain an alternative to ending the turn");

    const auto worldCandidate = [] (
        int winningWorlds,
        int rngWorlds,
        double winSampleRate,
        double lowerTailUtility,
        double meanUtility,
        double meanEndHp,
        double meanPotionCount,
        double meanValue = 0.0,
        double standardError = 0.0
    ) {
        search::RootActionCandidate candidate;
        candidate.meanValue = meanValue;
        candidate.successUtility = winSampleRate * meanUtility;
        candidate.visits = 100000;
        candidate.winSampleRate = winSampleRate;
        candidate.winningRngWorlds = winningWorlds;
        candidate.rngWorlds = rngWorlds;
        candidate.winWorldRate = rngWorlds > 0
            ? static_cast<double>(winningWorlds)
                / static_cast<double>(rngWorlds)
            : 0.0;
        candidate.lowerQuartileBestWinUtility = lowerTailUtility;
        candidate.meanBestWinUtility = meanUtility;
        candidate.bestWinUtilityStandardError = standardError;
        candidate.meanBestWinEndHp = meanEndHp;
        candidate.expectedEndHpOnWin = meanEndHp;
        candidate.meanBestWinPotionCount = meanPotionCount;
        return candidate;
    };

    check(search::selectRootActionByWorldBest({
              worldCandidate(11, 12, 0.20, 0.90, 0.94, 84.0, 1.0),
              worldCandidate(12, 12, 0.20, 0.20, 0.30, 27.0, 0.0),
          }) == 0,
          "statistically compatible complete plans must maximize the mean "
          "best terminal result instead of raw world coverage");
    check(search::selectRootActionByWorldBest({
              worldCandidate(12, 12, 0.20, 0.70, 0.75, 67.0, 0.0),
              worldCandidate(12, 12, 0.20, 0.60, 0.90, 80.0, 1.0),
          }) == 1,
          "mean best terminal utility must remain the complete-plan objective; "
          "the lower tail is only a tie-break");
    check(search::selectRootActionByWorldBest({
              worldCandidate(12, 12, 0.20, 0.70, 0.80, 71.0, 0.0),
              worldCandidate(12, 12, 0.20, 0.70, 0.85, 76.0, 0.0),
          }) == 1,
          "equal coverage and lower tail must maximize mean best terminal "
          "utility");
    check(search::selectRootActionByWorldBest({
              worldCandidate(
                  12, 12, 0.001, 0.90, 0.95, 85.0, 1.0, 0.0, 0.40
              ),
              worldCandidate(11, 12, 0.040, 0.30, 0.60, 54.0, 0.0),
          }) == 1,
          "one lucky high-HP plan must not outrank a materially more reliable "
          "complete-plan search basin");
    check(search::selectRootActionByWorldBest({
              worldCandidate(
                  4, 12, 0.000575, 0.0, 0.038889, 5.25, 2.0
              ),
              worldCandidate(
                  3, 12, 0.000475, 0.0, 0.031481, 5.67, 2.0
              ),
              worldCandidate(
                  4, 12, 0.000250, 0.0, 0.057407, 7.75, 2.0
              ),
          }) == 0,
          "four winning RNG worlds out of twelve remain sparse evidence; "
          "prefer probability-weighted terminal evidence among the actions "
          "that reach the widest set of worlds");
    check(search::selectRootActionByWorldBest({
              worldCandidate(
                  4, 12, 0.000575, 0.0, 0.038889, 5.25, 2.0
              ),
              worldCandidate(
                  3, 12, 0.000475, 0.0, 0.031481, 5.67, 2.0
              ),
          }, false) == 0,
          "four independent winning worlds must remain usable when isolated "
          "one-to-three-world wins are rejected");

    auto defectxx03Coolheaded = worldCandidate(
        5, 12, 0.0001421, 0.0, 0.05516, 9.4, 0.0
    );
    defectxx03Coolheaded.successUtility = 0.0000175;
    defectxx03Coolheaded.expectedEndHpOnWin = 8.75;
    auto defectxx03DoomAndGloom = worldCandidate(
        1, 12, 0.0000288, 0.0, 0.00822, 7.0, 0.0
    );
    defectxx03DoomAndGloom.successUtility = 0.00000162;
    defectxx03DoomAndGloom.expectedEndHpOnWin = 4.0;
    check(search::applyCompleteCombatParetoVeto({
              defectxx03Coolheaded, defectxx03DoomAndGloom,
          }, 1) == 0,
          "DEFECTXX03 heart must not let short recovery quality override a "
          "strictly stronger complete-combat Coolheaded plan");
    defectxx03Coolheaded.winningRngWorlds = 1;
    defectxx03Coolheaded.winWorldRate = 1.0 / 12.0;
    check(search::applyCompleteCombatParetoVeto({
              defectxx03Coolheaded, defectxx03DoomAndGloom,
          }, 1) == 1,
          "one lucky winning world must not override recovery even when its "
          "sampled terminal metrics dominate");
    defectxx03Coolheaded.winningRngWorlds = 5;
    defectxx03Coolheaded.winWorldRate = 5.0 / 12.0;
    defectxx03DoomAndGloom.winningRngWorlds = 0;
    check(search::applyCompleteCombatParetoVeto({
              defectxx03Coolheaded, defectxx03DoomAndGloom,
          }, 1) == 1,
          "missing complete wins for the recovery action are insufficient "
          "evidence for a complete-combat veto");
    defectxx03DoomAndGloom.winningRngWorlds = 1;
    defectxx03Coolheaded.expectedEndHpOnWin = 3.0;
    check(search::applyCompleteCombatParetoVeto({
              defectxx03Coolheaded, defectxx03DoomAndGloom,
          }, 1) == 1,
          "recovery selection must remain unchanged when complete-combat "
          "evidence has a real tradeoff");
    auto rareEqualCoverage = defectxx03Coolheaded;
    rareEqualCoverage.winningRngWorlds = 11;
    rareEqualCoverage.winWorldRate = 11.0 / 12.0;
    rareEqualCoverage.lowerQuartileWinSampleRate = 0.001;
    rareEqualCoverage.visits = 18000;
    auto recoveryEqualCoverage = defectxx03DoomAndGloom;
    recoveryEqualCoverage.winningRngWorlds = 11;
    recoveryEqualCoverage.winWorldRate = 11.0 / 12.0;
    check(search::applyCompleteCombatParetoVeto({
              rareEqualCoverage, recoveryEqualCoverage,
          }, 1) == 1,
          "equal sparse world coverage must not override recovery using "
          "correlated terminal hits");
    check(search::selectRootActionByWorldBest({
              worldCandidate(
                  8, 12, 0.48, 0.0, 0.20686, 10.0, 0.0, 0.0, 0.04339
              ),
              worldCandidate(
                  12, 12, 0.14, 0.0, 0.28824, 14.0, 0.0, 0.0, 0.01602
              ),
          }) == 1,
          "a high-frequency line confined to eight RNG worlds must not beat "
          "a complete plan found in every world");
    check(search::selectRootActionByWorldBest({
              worldCandidate(
                  10, 12, 0.00164, 0.0, 0.11778, 6.59, 0.0, 0.0, 0.02346
              ),
              worldCandidate(
                  6, 12, 0.00537, 0.0, 0.11333, 11.66, 0.0, 0.0, 0.03898
              ),
          }) == 1,
          "overlapping complete-plan utility intervals must leave the more "
          "reliable Lagavulin sleep-turn line eligible for terminal ranking");
    check(search::selectRootActionByWorldBest({
              worldCandidate(
                  12, 12, 0.3846, 0.0, 0.166667, 13.0, 0.0
              ),
              worldCandidate(
                  12, 12, 0.1270, 0.0, 0.423077, 3.14, 0.0
              ),
          }) == 0,
          "run 95 Giant Head must prefer Panic Button's reliable complete "
          "outcomes over Defend's rare high-utility continuation");
    check(search::selectRootActionByWorldBest({
              worldCandidate(
                  12, 12, 0.38176, 0.0, 0.064103, 4.71, 0.0
              ),
              worldCandidate(
                  12, 12, 0.01176, 0.0, 0.474359, 1.29, 0.0
              ),
          }) == 0,
          "run 95 Giant Head must prefer Immolate's reliable complete "
          "outcomes over Uppercut's rare high-utility continuation");

    auto run203Strike = worldCandidate(
        12, 12, 0.9704856, 0.25, 0.250000, 21.0, 0.0
    );
    run203Strike.lowerQuartileWinSampleRate = 0.9569509;
    auto run203Defend = worldCandidate(
        11, 12, 0.8994240, 0.18254, 0.250992, 22.876, 0.0,
        0.0, 0.021846
    );
    run203Defend.lowerQuartileWinSampleRate = 0.6460865;
    check(search::selectRootActionByWorldBest({
              run203Strike, run203Defend,
          }) == 0,
          "run 203 Gremlin Nob must prefer Strike's materially higher "
          "complete-combat win rate over Defend's small ending-HP advantage");

    auto run211Corruption = worldCandidate(
        12, 12, 0.5903, 0.0, 0.3174, 29.83, 0.0
    );
    run211Corruption.expectedEndHpOnWin = 13.95;
    auto run211Carnage = worldCandidate(
        12, 12, 0.9590, 0.0, 0.06383, 6.0, 0.0
    );
    run211Carnage.expectedEndHpOnWin = 6.0;
    auto lethalEnd = worldCandidate(
        0, 12, 0.0, 0.0, 0.0, 0.0, 0.0
    );
    lethalEnd.isEndTurn = true;
    lethalEnd.directEndBoundaryReachRate = 0.0;
    check(search::isDangerousRootActionSet({
              run211Corruption, run211Carnage, lethalEnd,
          }),
          "a root where END_TURN is fatal must be treated as dangerous");
    auto safeEnd = lethalEnd;
    safeEnd.directEndBoundaryReachRate = 1.0;
    check(!search::isDangerousRootActionSet({
              run211Corruption, run211Carnage, safeEnd,
          }),
          "a root where END_TURN safely replans must keep the normal policy");
    check(search::shouldPreferDangerWinRatePolicy(
              {run211Corruption, run211Carnage, lethalEnd}, 0
          ),
          "run 211 must detect the reliability-for-ending-HP inversion");
    check(search::selectRootActionByWinRateBandThenEndHp({
              run211Corruption, run211Carnage,
          }) == 1,
          "run 211 Darklings must prefer the reliable Carnage combat line "
          "over Corruption's rarer high-HP victories");

    auto survivorBiasedEnd = worldCandidate(
        12, 12, 0.7885, 1.0, 1.0, 73.0, 0.0
    );
    survivorBiasedEnd.isEndTurn = true;
    survivorBiasedEnd.visits = 750000;
    survivorBiasedEnd.lowerQuartileWinSampleRate = 0.6787;
    survivorBiasedEnd.successUtility = 0.6001;
    auto reliableCard = survivorBiasedEnd;
    reliableCard.isEndTurn = false;
    reliableCard.winSampleRate = 0.8506;
    reliableCard.lowerQuartileWinSampleRate = 0.6981;
    reliableCard.successUtility = 0.5446;
    check(search::applyReliableActionEndTurnVeto(
              {survivorBiasedEnd, reliableCard}, 0
          ) == 1,
          "a card that wins significantly more often across the same RNG "
          "worlds must override survivor-biased END_TURN utility");
    auto noisyCard = reliableCard;
    noisyCard.winSampleRate = survivorBiasedEnd.winSampleRate + 0.0001;
    noisyCard.lowerQuartileWinSampleRate =
        survivorBiasedEnd.lowerQuartileWinSampleRate - 0.01;
    check(search::applyReliableActionEndTurnVeto(
              {survivorBiasedEnd, noisyCard}, 0
          ) == 0,
          "an aggregate-only sampling fluctuation must not override END_TURN");

    auto sparseEnd = worldCandidate(
        2, 12, 0.00009, 0.0, 0.0, 8.5, 1.0
    );
    sparseEnd.isEndTurn = true;
    sparseEnd.directEndBoundaryReachRate = 1.0;
    sparseEnd.meanDirectEndBoundaryHp = 26.0;
    sparseEnd.meanDirectEndBoundaryPotionCount = 1.0;
    sparseEnd.meanDirectEndBoundaryValue = -0.80;
    auto sparseCard = worldCandidate(
        1, 12, 0.00105, 0.0, 0.0, 15.9, 1.0
    );
    sparseCard.directEndBoundaryReachRate = 1.0;
    sparseCard.meanDirectEndBoundaryHp = 27.0;
    sparseCard.meanDirectEndBoundaryPotionCount = 1.0;
    sparseCard.meanDirectEndBoundaryValue = -0.75;
    check(search::applyReliableActionEndTurnVeto(
              {sparseEnd, sparseCard}, 0
          ) == 0,
          "correlated sparse rollout counts must not override END_TURN");

    sparseEnd.directEndBoundaryReached = {1, 1};
    sparseEnd.directEndBoundaryHps = {50.0, 2.0};
    sparseEnd.directEndBoundaryPotionCounts = {1.0, 1.0};
    sparseEnd.directEndBoundaryValues = {-0.6, -1.0};
    sparseCard.directEndBoundaryReached = {1, 1};
    sparseCard.directEndBoundaryHps = {26.0, 26.0};
    sparseCard.directEndBoundaryPotionCounts = {1.0, 1.0};
    sparseCard.directEndBoundaryValues = {-0.75, -0.75};
    sparseCard.meanDirectEndBoundaryHp = 26.0;
    check(search::applyPairedBoundaryEndTurnDominanceVeto(
              {sparseEnd, sparseCard}, 0
          ) == 0,
          "equal boundary means must not hide one degraded RNG world");

    sparseEnd.directEndBoundaryHps = {26.0, 26.0};
    sparseEnd.directEndBoundaryValues = {-0.8, -0.8};
    sparseCard.directEndBoundaryHps = {26.0, 26.0};
    sparseCard.directEndBoundaryValues = {-0.7, -0.7};
    sparseCard.zeroEnergyRootAction = true;
    check(search::applyPairedBoundaryEndTurnDominanceVeto(
              {sparseEnd, sparseCard}, 0
          ) == 0,
          "one winning world must not override two from correlated rollout "
          "frequency alone");
    sparseCard.winningRngWorlds = 2;
    check(search::applyPairedBoundaryEndTurnDominanceVeto(
              {sparseEnd, sparseCard}, 0
          ) == 1,
          "a zero-energy value improvement may use no-worse independent "
          "world coverage without a pooled confidence interval");

    sparseCard.winningRngWorlds = 1;
    sparseCard.zeroEnergyRootAction = false;
    sparseCard.directEndBoundaryHps = {27.0, 26.0};
    sparseCard.directEndBoundaryValues = {-0.8, -0.7};
    sparseCard.meanDirectEndBoundaryHp = 26.5;
    check(search::applyPairedBoundaryEndTurnDominanceVeto(
              {sparseEnd, sparseCard}, 0
          ) == 1,
          "per-world boundary Pareto dominance may override END_TURN without "
          "using sparse rollout frequency");
    sparseCard.directEndBoundaryValues[1] = -0.81;
    check(search::applyPairedBoundaryEndTurnDominanceVeto(
              {sparseEnd, sparseCard}, 0
          ) == 0,
          "one degraded RNG world must veto aggregate boundary improvement");

    check(search::selectRootPotionAction({
              worldCandidate(12, 12, 0.20, 0.70, 0.80, 71.0, 0.0),
              worldCandidate(12, 12, 0.20, 0.70, 0.80, 71.0, 1.0),
          }) == 1,
          "an exactly equivalent winning plan must preserve the potion");
    check(search::selectRootActionBySuccessUtility({
              {-0.34, 0.0, 0.0, 0.0, 900000},
              {-0.38, 0.0, 0.0, 0.0, 200000},
          }) == 0,
          "an all-loss root must retain dense enemy-HP progress fallback");
    check(search::selectRootActionBySuccessUtility({
              {-0.544, 0.0, 0.0, 0.0, 90000, 0.0, 0.0, false, -0.332},
              {-0.537, 0.0, 0.0, 0.0, 90000, 0.0, 0.0, false, -0.384},
          }) == 0,
          "an all-loss root must prefer the stronger best continuation "
          "across RNG worlds over a slightly better random-play mean");

    std::vector<search::RootActionCandidate> safeReplanCandidates{
        {-0.40, 0.0, 0.0, 0.0, 90000, 0.0, 0.0, false,
         -0.20, 1.0, 10.0, 10.0, 1.0, 50.0},
        {-0.55, 0.0, 0.0, 0.0, 90000, 0.0, 0.0, false,
         -0.45, 1.0, 28.0, 28.0, 1.0, 50.0},
    };
    check(search::selectRootActionByWorldBest(safeReplanCandidates) == 1,
          "when no complete win exists, normalized combat progress and HP at "
          "the common replanning boundary must select the healthier future");
    safeReplanCandidates = {
        {-0.489, 0.0, 0.0, 0.0, 90000, 0.0, 0.0, false,
         -0.229, 1.0, 43.0, 43.0, 1.0, 86.0},
        {-0.572, 0.0, 0.0, 0.0, 90000, 0.0, 0.0, false,
         -0.250, 1.0, 49.0, 49.0, 1.0, 86.0},
    };
    check(search::selectRootActionByWorldBest(safeReplanCandidates) == 0,
          "the no-win fallback must balance normalized survival with combat "
          "progress instead of maximizing either one alone");

    const auto recoveryCandidate = [] (
        bool reached,
        int turns,
        int hp,
        double engine,
        double progress
    ) {
        search::RecoverySnapshot snapshot;
        snapshot.observed = true;
        snapshot.reachedHorizon = reached;
        snapshot.survivedTurns = turns;
        snapshot.hp = hp;
        snapshot.effectiveHp = hp;
        snapshot.engineScore = engine;
        snapshot.enemyProgress = progress;
        return snapshot;
    };
    check(search::selectRecoveryRootAction({
              search::aggregateRecoveryWorlds({
                  recoveryCandidate(true, 3, 20, 2.0, 0.2),
                  recoveryCandidate(false, 2, 0, 3.0, 0.4),
              }),
              search::aggregateRecoveryWorlds({
                  recoveryCandidate(true, 3, 16, 1.0, 0.1),
                  recoveryCandidate(true, 3, 12, 1.0, 0.1),
              }),
          }) == 1,
          "recovery search must maximize fixed-horizon survival coverage "
          "before HP or setup quality");
    check(search::selectRecoveryRootAction({
              search::aggregateRecoveryWorlds({
                  recoveryCandidate(false, 1, 0, 5.0, 0.8),
              }),
              search::aggregateRecoveryWorlds({
                  recoveryCandidate(false, 2, 0, 0.0, 0.1),
              }),
          }) == 1,
          "when no action reaches the horizon, recovery search must keep the "
          "line that survives more turns");

    BattleContext defectRecoveryRoot;
    BattleContext extraDraw(defectRecoveryRoot);
    ++extraDraw.player.cardDrawPerTurn;
    check(std::abs(search::recoverySnapshot(
                       defectRecoveryRoot, extraDraw, 2
                   ).engineScore - 0.75) < 1e-9,
          "recovery search must value persistent extra draw");
    BattleContext extraOrbSlots(defectRecoveryRoot);
    extraOrbSlots.player.orbSlots += 2;
    check(std::abs(search::recoverySnapshot(
                       defectRecoveryRoot, extraOrbSlots, 2
                   ).engineScore - 1.0) < 1e-9,
          "recovery search must value additional orb slots");
    defectRecoveryRoot.cards.cardsInHand = 1;
    defectRecoveryRoot.cards.hand[0] = CardInstance(CardId::ECHO_FORM);
    BattleContext recurringCardCopy(defectRecoveryRoot);
    recurringCardCopy.cards.cardsInHand = 0;
    recurringCardCopy.player.buff<PS::ECHO_FORM>(1);
    check(std::abs(search::recoverySnapshot(
                       defectRecoveryRoot, recurringCardCopy, 2
                   ).engineScore - 0.65) < 1e-9,
          "an active recurring card copy must be worth more than preserving "
          "the unplayed Power");

    search::RootActionCandidate noWin;
    noWin.visits = 100000;
    noWin.rngWorlds = 12;
    noWin.directEndBoundaryReachRate = 1.0;
    noWin.meanDirectEndBoundaryHp = 34.0;
    auto isolatedWin = noWin;
    isolatedWin.winningRngWorlds = 1;
    isolatedWin.winWorldRate = 1.0 / 12.0;
    check(search::shouldUseRecoverySearch({noWin, isolatedWin}, 0),
          "an isolated winning world is not trustworthy terminal evidence");
    auto sparseWin = noWin;
    sparseWin.winningRngWorlds = 4;
    sparseWin.winWorldRate = 1.0 / 3.0;
    check(!search::shouldUseRecoverySearch({noWin, sparseWin}, 1),
          "broad wins without a safety conflict retain the terminal objective");
    auto rareBroadWin = noWin;
    rareBroadWin.winningRngWorlds = 11;
    rareBroadWin.winWorldRate = 11.0 / 12.0;
    rareBroadWin.winSampleRate = 0.0128;
    rareBroadWin.lowerQuartileWinSampleRate = 0.0017;
    rareBroadWin.visits = 18480;
    check(!search::hasStableWinBasin(rareBroadWin),
          "broad lucky leaves need not have a stable lower-tail basin");
    check(search::shouldUseRecoverySearch({noWin, rareBroadWin}, 1),
          "a broad but vanishing win basin must use recovery search");
    rareBroadWin.lowerQuartileWinSampleRate = 0.03;
    check(search::hasStableWinBasin(rareBroadWin),
          "a converged lower-tail basin is recognized");
    auto stableLowerCoverage = rareBroadWin;
    stableLowerCoverage.winningRngWorlds = 4;
    stableLowerCoverage.winWorldRate = 1.0 / 3.0;
    rareBroadWin.lowerQuartileWinSampleRate = 0.0017;
    check(search::shouldUseRecoverySearch({
              rareBroadWin, stableLowerCoverage,
          }, 0),
          "another action's stable basin must not validate the selected "
          "action's sparse winning paths");
    check(!search::shouldUseRecoverySearch({
              rareBroadWin, stableLowerCoverage,
          }, 1),
          "stability evidence must remain attached to its selected action");
    auto saferLowerCoverage = sparseWin;
    saferLowerCoverage.winningRngWorlds = 5;
    saferLowerCoverage.winWorldRate = 5.0 / 12.0;
    saferLowerCoverage.meanDirectEndBoundaryHp = 51.0;
    sparseWin.winningRngWorlds = 8;
    sparseWin.winWorldRate = 8.0 / 12.0;
    sparseWin.meanDirectEndBoundaryHp = 48.0;
    check(search::shouldUseRecoverySearch({sparseWin, saferLowerCoverage}, 0),
          "lower terminal coverage with a safer common boundary must be "
          "compared by recovery search");
    sparseWin.directEndBoundaryReachRate = 0.0;
    check(search::shouldUseRecoverySearch({noWin, sparseWin}, 1),
          "broad but fragile wins must not override a safe next decision");
    sparseWin.directEndBoundaryReachRate = 1.0;
    sparseWin.meanDirectEndBoundaryHp = 15.0;
    check(search::shouldUseRecoverySearch({noWin, sparseWin}, 1),
          "a healthier common boundary must trigger fixed-horizon comparison");
    sparseWin.lowerQuartileWinSampleRate = 0.0001;
    check(!search::shouldUseRecoverySearch({noWin, sparseWin}, 1),
          "non-zero lower-tail wins retain the complete-combat objective");
    sparseWin.visits = 0;
    check(!search::shouldUseRecoverySearch({noWin, sparseWin}, 1),
          "recovery search must wait until every root action is evaluated");

    auto sparseLuckyWin = safeReplanCandidates[1];
    sparseLuckyWin.winningRngWorlds = 1;
    sparseLuckyWin.rngWorlds = 12;
    sparseLuckyWin.winWorldRate = 1.0 / 12.0;
    sparseLuckyWin.winSampleRate = 0.00001;
    sparseLuckyWin.successUtility = 0.00001;
    sparseLuckyWin.meanBestWinUtility = 0.9;
    check(search::selectRootActionByWorldBest(
              {safeReplanCandidates[0], sparseLuckyWin}, false
          ) == 0,
          "a non-credible one-world win must not bypass the all-loss "
          "survival replanning policy");

    std::vector<search::RootActionThreadSample> worldSamples(4);
    for (int i = 0; i < 4; ++i) {
        worldSamples[i].visits = 100;
        worldSamples[i].maxEvaluation = -0.2;
        worldSamples[i].foundWinningLine = i != 0;
        worldSamples[i].bestWinUtility = i == 0 ? 0.0 : 0.5 + i * 0.1;
        worldSamples[i].bestWinEndHp = i == 0 ? 0.0 : 50.0 + i;
        worldSamples[i].bestWinPotionCount = i == 0 ? 0.0 : 1.0;
    }
    const auto worldAggregate =
        search::aggregateRootActionThreads(worldSamples);
    check(worldAggregate.winningRngWorlds == 3
              && std::abs(worldAggregate.winWorldRate - 0.75) < 1e-12
              && std::abs(worldAggregate.meanBestWinUtility - 0.525) < 1e-12
              && worldAggregate.lowerQuartileBestWinUtility == 0.0,
          "root aggregation must count each RNG world once and include an "
          "unwon world as zero in the lower tail");

    check(search::selectRootActionByWorldBest({
              worldCandidate(12, 12, 0.0383, 0.1535, 0.2996, 26.67, 0.0, -0.4),
              worldCandidate(11, 12, 0.1193, 0.0150, 0.0693, 6.73, 0.0, 0.1),
          }) == 0,
          "seed 80 Champ Execute setup must choose Power Through before "
          "Second Wind even when random rollout hit frequency favors the latter");

    auto run242SecondWind = worldCandidate(
        12, 12, 0.539978, 0.325843, 0.338951, 30.17, 0.0,
        0.0, 0.00559
    );
    run242SecondWind.successUtility = 0.144013;
    auto run242BurningPact = worldCandidate(
        12, 12, 0.323799, 0.539326, 0.581461, 51.75, 0.0,
        0.0, 0.00956
    );
    run242BurningPact.successUtility = 0.093803;
    const std::vector<search::RootActionCandidate> run242Candidates{
        run242SecondWind,
        run242BurningPact,
    };
    const int run242Baseline =
        search::selectRootActionByWorldBest(run242Candidates);
    check(run242Baseline == 0,
          "run 242 must reproduce UCT hit frequency preferring Second Wind");
    check(search::applyRobustContinuationDominance(
              run242Candidates,
              run242Baseline
          ) == 1,
          "run 242 Spire Spear and Shield must prefer Burning Pact's robust "
          "high-quality continuations over Second Wind's compact subtree "
          "despite the latter's higher UCT win-hit frequency");

    auto run95ReliableImmolate = worldCandidate(
        12, 12, 0.057067, 0.012821, 0.012821, 1.0, 0.0
    );
    run95ReliableImmolate.successUtility = 0.000732;
    auto run95RareSelection = worldCandidate(
        12, 12, 0.000267, 0.269231, 0.269231, 21.0, 0.0
    );
    run95RareSelection.successUtility = 0.000072;
    const std::vector<search::RootActionCandidate> run95Candidates{
        run95ReliableImmolate,
        run95RareSelection,
    };
    const int run95Baseline =
        search::selectRootActionByWorldBest(run95Candidates);
    check(run95Baseline == 0
              && search::applyRobustContinuationDominance(
                     run95Candidates,
                     run95Baseline
                 ) == 0,
          "run 95 Giant Head must not replace a reliable lethal line with a "
          "vanishingly rare high-HP selection branch");

    check(search::applyCommonBoundaryEndTurnDominanceVeto({
              {-0.46, 0.01, 0.008, 4.0, 300000,
               0.0, 0.0, false, 0.18, 1.0, 72.0, 72.0, 1.0,
               75.0, -0.95, 1.0, 72.0, 1.0, -0.95, {101, 102}},
              {-0.45, 0.02, 0.012, 5.0, 300000,
               0.0, 0.0, true, 0.19, 1.0, 72.0, 72.0, 1.0,
               75.0, -1.0, 1.0, 72.0, 1.0, -1.0, {101, 102}},
          }, 1) == 0,
          "a non-END action that strictly improves the common replanning "
          "boundary must override END_TURN's deeper terminal rollout");
    check(search::applyCommonBoundaryEndTurnDominanceVeto({
              {-0.46, 0.01, 0.008, 4.0, 300000,
               0.0, 0.0, false, 0.18, 1.0, 65.0, 65.0, 1.0,
               75.0, -0.80, 1.0, 65.0, 1.0, -0.80, {101, 102}},
              {-0.45, 0.02, 0.012, 5.0, 300000,
               0.0, 0.0, true, 0.19, 1.0, 72.0, 72.0, 1.0,
               75.0, -1.0, 1.0, 72.0, 1.0, -1.0, {101, 102}},
          }, 1) == 1,
          "common-boundary correction must remain Pareto-conservative when "
          "combat progress costs player HP");
    check(search::applyCommonBoundaryEndTurnDominanceVeto({
              {-0.46, 0.01, 0.008, 4.0, 300000,
               0.0, 0.0, false, 0.18, 1.0, 72.0, 72.0, 1.0,
               75.0, -0.95, 1.0, 72.0, 1.0, -0.95, {201, 202}},
              {-0.45, 0.02, 0.012, 5.0, 300000,
               0.0, 0.0, true, 0.19, 1.0, 72.0, 72.0, 1.0,
               75.0, -1.0, 1.0, 72.0, 1.0, -1.0, {101, 102}},
          }, 1) == 1,
          "common-boundary correction must preserve END_TURN when the future "
          "monster, deck, relic-counter, or resource state differs");

    search::RootActionCandidate rampageProgress;
    rampageProgress.visits = 100000;
    rampageProgress.directEndBoundaryReachRate = 1.0;
    rampageProgress.meanDirectEndBoundaryHp = 63.0;
    rampageProgress.meanDirectEndBoundaryPotionCount = 1.0;
    rampageProgress.meanDirectEndBoundaryValue = -0.94;
    rampageProgress.directEndBoundaryStateKeys = {201, 202};
    rampageProgress.monotonicBoundaryStateKeys = {501, 502};
    rampageProgress.directEndBoundaryMonotonicCardProgress = {5, 5};
    rampageProgress.directEndBoundaryStrength = {2, 2};
    search::RootActionCandidate rampageEnd;
    rampageEnd.visits = 100000;
    rampageEnd.isEndTurn = true;
    rampageEnd.directEndBoundaryReachRate = 1.0;
    rampageEnd.meanDirectEndBoundaryHp = 63.0;
    rampageEnd.meanDirectEndBoundaryPotionCount = 1.0;
    rampageEnd.meanDirectEndBoundaryValue = -1.0;
    rampageEnd.directEndBoundaryStateKeys = {101, 102};
    rampageEnd.monotonicBoundaryStateKeys = {501, 502};
    rampageEnd.directEndBoundaryMonotonicCardProgress = {0, 0};
    rampageEnd.directEndBoundaryStrength = {1, 1};
    check(search::applyCommonBoundaryEndTurnDominanceVeto(
              {rampageProgress, rampageEnd}, 1
          ) == 0,
          "monotonic combat-local card progress must not prevent a strictly "
          "better common-boundary action from dominating END_TURN");

    search::RootActionCandidate retainedBlock;
    retainedBlock.visits = 100000;
    retainedBlock.successUtility = 0.40;
    retainedBlock.strictBlockBoundaryStateKeys = {301, 302, 303};
    retainedBlock.shuffledBlockBoundaryStateKeys = {401, 402, 403};
    retainedBlock.directEndBoundaryShuffleAdvances = {0, 0, 0};
    retainedBlock.directEndBoundaryBlocks = {18, 19, 20};
    search::RootActionCandidate immediateEnd;
    immediateEnd.visits = 100000;
    immediateEnd.successUtility = 0.60;
    immediateEnd.isEndTurn = true;
    immediateEnd.strictBlockBoundaryStateKeys = {301, 302, 303};
    immediateEnd.shuffledBlockBoundaryStateKeys = {401, 402, 403};
    immediateEnd.directEndBoundaryShuffleAdvances = {0, 0, 0};
    immediateEnd.directEndBoundaryBlocks = {10, 11, 12};
    check(search::applyStrictBlockEndTurnDominanceVeto(
              {retainedBlock, immediateEnd}, 1
          ) == 0,
          "identical next-turn states with strictly more retained Block in "
          "every RNG world must override END_TURN");

    auto changedFuture = retainedBlock;
    changedFuture.strictBlockBoundaryStateKeys = {301, 999, 303};
    check(search::applyStrictBlockEndTurnDominanceVeto(
              {changedFuture, immediateEnd}, 1
          ) == 1,
          "the strict Block override must not apply when any future state "
          "differs beyond retained Block");

    auto worldTradeoff = retainedBlock;
    worldTradeoff.directEndBoundaryBlocks = {18, 9, 20};
    check(search::applyStrictBlockEndTurnDominanceVeto(
              {worldTradeoff, immediateEnd}, 1
          ) == 1,
          "the strict Block override must reject an average improvement that "
          "loses Block in any RNG world");

    auto shuffledCandidate = retainedBlock;
    auto shuffledEnd = immediateEnd;
    shuffledCandidate.strictBlockBoundaryStateKeys = {501, 502, 503};
    shuffledEnd.strictBlockBoundaryStateKeys = {601, 602, 603};
    shuffledCandidate.shuffledBlockBoundaryStateKeys = {701, 702, 703};
    shuffledEnd.shuffledBlockBoundaryStateKeys = {701, 702, 703};
    shuffledCandidate.directEndBoundaryShuffleAdvances = {1, 1, 1};
    shuffledEnd.directEndBoundaryShuffleAdvances = {1, 1, 1};
    check(search::applyStrictBlockEndTurnDominanceVeto(
              {shuffledCandidate, shuffledEnd}, 1
          ) == 0,
          "equal uniformly shuffled card multisets may prove retained-Block "
          "dominance despite different common-random-number ordering");

    shuffledCandidate.directEndBoundaryShuffleAdvances = {1, 0, 1};
    check(search::applyStrictBlockEndTurnDominanceVeto(
              {shuffledCandidate, shuffledEnd}, 1
          ) == 1,
          "draw-pile canonicalization must not apply unless every differing "
          "world consumed the same positive shuffle work");

    BattleContext losingState(rootState);
    losingState.outcome = Outcome::PLAYER_LOSS;
    losingState.player.curHp = 0;
    losingState.monsters.arr[0].curHp = 10;
    const double losingValue = search::BattleScumSearcher2::evaluateEndState(rootState, losingState);
    check(std::abs(losingValue - (-0.1)) < 1e-12,
          "losing states must retain enemy-HP progress instead of collapsing to -1");

    BattleContext minionRoot(rootState);
    minionRoot.monsters.monsterCount = 3;
    minionRoot.monsters.monstersAlive = 3;
    minionRoot.monsters.arr[0].id = MonsterId::BRONZE_AUTOMATON;
    minionRoot.monsters.arr[0].maxHp = 300;
    minionRoot.monsters.arr[0].curHp = 300;
    for (int idx : {1, 2}) {
        auto &orb = minionRoot.monsters.arr[idx];
        orb.id = MonsterId::BRONZE_ORB;
        orb.maxHp = 50;
        orb.curHp = 50;
        orb.setHasStatus<MS::MINION>();
    }
    BattleContext minionLoss(minionRoot);
    minionLoss.outcome = Outcome::PLAYER_LOSS;
    minionLoss.player.curHp = 0;
    minionLoss.monsters.monstersAlive = 1;
    minionLoss.monsters.arr[1].curHp = 0;
    minionLoss.monsters.arr[2].curHp = 0;
    const double minionProgressValue =
        search::BattleScumSearcher2::evaluateEndState(minionRoot, minionLoss);
    check(std::abs(minionProgressValue - (-0.7)) < 1e-12,
          "defeating existing minions must improve a losing rollout without outweighing the leader");

    BattleContext spreadMinionDamage(minionRoot);
    spreadMinionDamage.outcome = Outcome::PLAYER_LOSS;
    spreadMinionDamage.player.curHp = 0;
    spreadMinionDamage.monsters.arr[1].curHp = 25;
    spreadMinionDamage.monsters.arr[2].curHp = 25;
    BattleContext focusedMinionDamage(minionRoot);
    focusedMinionDamage.outcome = Outcome::PLAYER_LOSS;
    focusedMinionDamage.player.curHp = 0;
    focusedMinionDamage.monsters.monstersAlive = 2;
    focusedMinionDamage.monsters.arr[1].curHp = 0;
    focusedMinionDamage.monsters.arr[2].curHp = 50;
    const double spreadMinionValue =
        search::BattleScumSearcher2::evaluateEndState(minionRoot, spreadMinionDamage);
    const double focusedMinionValue =
        search::BattleScumSearcher2::evaluateEndState(minionRoot, focusedMinionDamage);
    check(focusedMinionValue > spreadMinionValue,
          "defeating one minion must be better than spreading equal damage across living minions");

    BattleContext lowHpRoot(rootState);
    lowHpRoot.player.curHp = 4;
    lowHpRoot.player.maxHp = 80;
    BattleContext healedVictory(lowHpRoot);
    healedVictory.outcome = Outcome::PLAYER_VICTORY;
    healedVictory.player.curHp = 24;
    const double healedVictoryValue = search::BattleScumSearcher2::evaluateEndState(lowHpRoot, healedVictory);
    check(std::abs(healedVictoryValue - 0.3) < 1e-12,
          "winning reward must stay bounded after healing or revival");

    BattleContext lizardRoot(rootState);
    lizardRoot.player.curHp = 33;
    lizardRoot.player.maxHp = 75;
    lizardRoot.player.setHasRelic<R::LIZARD_TAIL>(true);
    BattleContext preservedLizardVictory(lizardRoot);
    preservedLizardVictory.outcome = Outcome::PLAYER_VICTORY;
    preservedLizardVictory.player.curHp = 25;
    BattleContext consumedLizardVictory(lizardRoot);
    consumedLizardVictory.outcome = Outcome::PLAYER_VICTORY;
    consumedLizardVictory.player.curHp = 37;
    consumedLizardVictory.player.setHasRelic<R::LIZARD_TAIL>(false);
    const double preservedLizardValue =
        search::BattleScumSearcher2::evaluateEndState(
            lizardRoot, preservedLizardVictory
        );
    const double consumedLizardValue =
        search::BattleScumSearcher2::evaluateEndState(
            lizardRoot, consumedLizardVictory
        );
    check(preservedLizardValue > consumedLizardValue,
          "MCTS must not prefer consuming Lizard Tail merely because revival leaves more HP");
    check(std::abs(consumedLizardValue - (1.0 / 75.0)) < 1e-12,
          "a victory that spends Lizard Tail must pay its half-max-HP opportunity cost");

    search::BattleScumSearcher2 backupWeightSearcher(rootState);
    check(backupWeightSearcher.maxBackupWeight == 0.0,
          "MCTS must use mean UCT backup by default");
    backupWeightSearcher.maxBackupWeight = 0.75;
    check(backupWeightSearcher.getEffectiveMaxBackupWeight(0) == 0.0,
          "max backup must not affect an unvisited edge");
    check(backupWeightSearcher.getEffectiveMaxBackupWeight(1)
              < backupWeightSearcher.getEffectiveMaxBackupWeight(1000),
          "max backup confidence must grow with visits");
    check(backupWeightSearcher.getEffectiveMaxBackupWeight(1000)
              < backupWeightSearcher.maxBackupWeight,
          "effective max backup must remain bounded by its configured weight");

    check(search::preferRootActionByMean(-0.2, 100, -0.3, 1000),
          "root action selection must prefer the higher mean rollout value");
    check(!search::preferRootActionByMean(-0.4, 1000, -0.3, 100),
          "root action selection must not trade expected value for more visits");
    check(search::preferRootActionByMean(-0.3, 101, -0.3, 100),
          "equal-mean root actions must use visits as a deterministic tie break");

    search::RootActionCandidate certifiedWin;
    certifiedWin.visits = 1000;
    certifiedWin.winSampleRate = 1.0;
    certifiedWin.lowerQuartileWinSampleRate = 1.0;
    certifiedWin.winWorldRate = 1.0;
    certifiedWin.successUtility = 0.05;
    certifiedWin.certifiedImmediateWin = true;
    certifiedWin.certifiedImmediateWinUtility = 0.05;
    search::RootActionCandidate highHpAlternative(certifiedWin);
    highHpAlternative.certifiedImmediateWin = false;
    highHpAlternative.winSampleRate = 0.9958;
    highHpAlternative.lowerQuartileWinSampleRate = 0.9953;
    highHpAlternative.successUtility = 0.55;
    check(search::applyCertifiedWinRiskGate(
              {certifiedWin, highHpAlternative}, 1
          ) == 1,
          "a 99.5%-reliable higher-HP line may replace a damaging certified win");
    auto unsafeBaseline = highHpAlternative;
    unsafeBaseline.lowerQuartileWinSampleRate = 0.99;
    check(search::applyCertifiedWinRiskGate(
              {certifiedWin, unsafeBaseline, highHpAlternative}, 1
          ) == 2,
          "the certified-win gate must find every reliable resource-preserving alternative");
    highHpAlternative.lowerQuartileWinSampleRate = 0.9949;
    check(search::applyCertifiedWinRiskGate(
              {certifiedWin, highHpAlternative}, 1
          ) == 0,
          "a line below the certified-win risk floor must keep the exact lethal");

    const auto equalWorldAggregate = search::aggregateRootActionThreads({
        {100, 80.0, 64.0, 0.9, 80, 20, 0, 4000.0, 40.0, 10},
        {1000, 100.0, 10.0, 0.6, 200, 800, 0, 8000.0, 20.0, 100},
    });
    check(equalWorldAggregate.rngWorlds == 2
              && equalWorldAggregate.visits == 1100,
          "root aggregation must retain RNG-world and raw-visit counts");
    check(std::abs(equalWorldAggregate.meanValue - 0.45) < 1e-12,
          "root aggregation must weight RNG worlds equally instead of pooling visits");
    check(std::abs(equalWorldAggregate.winSampleRate - 0.5) < 1e-12,
          "root win rate must weight RNG worlds equally");
    check(std::abs(equalWorldAggregate.lowerQuartileWinSampleRate - 0.2) < 1e-12,
          "lower-tail root win rate must retain the weakest RNG world");
    check(std::abs(equalWorldAggregate.lowerQuartileSuccessUtility - 0.02) < 1e-12,
          "lower-tail success utility must retain the weakest RNG world");
    check(std::abs(equalWorldAggregate.expectedEndHpOnWin - 48.0) < 1e-12,
          "winning end HP must preserve equal RNG-world weighting");
    check(std::abs(equalWorldAggregate.successUtility - 0.21) < 1e-12,
          "success utility must weight RNG worlds equally");
    check(
        std::abs(equalWorldAggregate.expectedWinUtilityOnWin - 0.42) < 1e-12,
        "conditional winning utility must exclude loss-progress rewards"
    );
    check(
        std::abs(equalWorldAggregate.lizardTailConsumedRateOnWin - 0.2) < 1e-12,
        "Lizard Tail consumption rate must be conditional on winning samples"
    );

    search::BattleScumSearcher2 fairRootSearcher(rootState);
    check(fairRootSearcher.balanceRootActions,
          "ordinary root actions must use balanced evidence by default");
    check(fairRootSearcher.minRootActionVisits >= 2048,
          "ordinary root actions need enough per-world evidence to resist early stochastic starvation");
    fairRootSearcher.minRootActionVisits = 10;
    fairRootSearcher.root.simulationCount = 12;
    fairRootSearcher.root.edges.resize(2);
    fairRootSearcher.root.edges[0].simulationCount = 10;
    fairRootSearcher.root.edges[0].evaluationSum = 10.0;
    fairRootSearcher.root.edges[1].simulationCount = 2;
    fairRootSearcher.root.edges[1].evaluationSum = -2.0;
    check(fairRootSearcher.selectBestEdgeToSearch(fairRootSearcher.root) == 1,
          "ordinary root search must satisfy the fair-visit floor before UCT");
    fairRootSearcher.root.simulationCount = 20;
    fairRootSearcher.root.edges[1].simulationCount = 10;
    fairRootSearcher.root.edges[1].evaluationSum = -10.0;
    check(fairRootSearcher.selectBestEdgeToSearch(fairRootSearcher.root) == 0,
          "balanced roots must deterministically break equal-visit ties");
    fairRootSearcher.root.simulationCount = 21;
    fairRootSearcher.root.edges[0].simulationCount = 11;
    fairRootSearcher.root.edges[0].evaluationSum = 11.0;
    check(fairRootSearcher.selectBestEdgeToSearch(fairRootSearcher.root) == 1,
          "balanced roots must continue sampling the least-visited action");
    fairRootSearcher.balanceRootActions = false;
    check(fairRootSearcher.selectBestEdgeToSearch(fairRootSearcher.root) == 0,
          "opt-out root search must return to UCT after the fair-visit floor");

    search::BattleScumSearcher2::Node transpositionParent;
    auto sharedChild = std::make_shared<search::BattleScumSearcher2::Node>();
    sharedChild->simulationCount = 1000;
    search::BattleScumSearcher2::Edge lessVisitedEdge;
    lessVisitedEdge.action = search::Action(search::ActionType::END_TURN);
    lessVisitedEdge.node = sharedChild;
    lessVisitedEdge.simulationCount = 5;
    search::BattleScumSearcher2::Edge moreVisitedEdge;
    moreVisitedEdge.action = search::Action(search::ActionType::END_TURN);
    moreVisitedEdge.node = sharedChild;
    moreVisitedEdge.simulationCount = 20;
    transpositionParent.edges.push_back(lessVisitedEdge);
    transpositionParent.edges.push_back(moreVisitedEdge);
    const auto *selectedEdge = search::ScumSearchAgent2::selectMostVisitedEdge(transpositionParent);
    check(selectedEdge == &transpositionParent.edges[1],
          "tree execution must use parent-edge visits when transpositions share a child node");

    BattleContext keyState(rootState);
    keyState.inputState = InputState::PLAYER_NORMAL;
    search::BattleScumSearcher2 keySearcher(keyState);
    const auto baseStateKey = keySearcher.buildStateKey(keyState);

    BattleContext wrathState(keyState);
    wrathState.player.stance = Stance::WRATH;
    check(keySearcher.buildStateKey(wrathState) != baseStateKey,
          "transposition keys must include the player's stance");

    BattleContext retargetedState(keyState);
    retargetedState.player.lastTargetedMonster = 2;
    check(keySearcher.buildStateKey(retargetedState) != baseStateKey,
          "transposition keys must include the player's last targeted monster");

    BattleContext necronomiconUsedState(keyState);
    necronomiconUsedState.player.haveUsedNecronomiconThisTurn = true;
    check(keySearcher.buildStateKey(necronomiconUsedState) != baseStateKey,
          "transposition keys must include once-per-turn relic state");

    BattleContext handAndDrawState(keyState);
    handAndDrawState.cards.cardsInHand = 1;
    handAndDrawState.cards.hand[0] = CardInstance(CardId::STRIKE_RED);
    handAndDrawState.cards.drawPile.clear();
    handAndDrawState.cards.drawPile.push_back(CardInstance(CardId::DEFEND_RED));
    BattleContext bothInHandState(handAndDrawState);
    bothInHandState.cards.cardsInHand = 2;
    bothInHandState.cards.hand[1] = bothInHandState.cards.drawPile.back();
    bothInHandState.cards.drawPile.clear();
    check(keySearcher.buildStateKey(handAndDrawState) != keySearcher.buildStateKey(bothInHandState),
          "transposition keys must preserve card-pile boundaries");

    BattleContext guidanceState(keyState);
    guidanceState.turn = 3;
    guidanceState.player.energy = 3;
    guidanceState.cards.cardsInHand = 2;
    guidanceState.cards.hand[0] = CardInstance(CardId::STRIKE_RED);
    guidanceState.cards.hand[1] = CardInstance(CardId::INFLAME);
    search::BattleScumSearcher2 guidanceSearcher(guidanceState);
    guidanceSearcher.setActionGuidance(
        3,
        {"Inflame", "END_TURN"},
        {"Strike"}
    );
    const search::Action strikeAction(search::ActionType::CARD, 0, 0);
    const search::Action inflameAction(search::ActionType::CARD, 1);
    const search::Action endTurnAction(search::ActionType::END_TURN);
    check(strikeAction.isValidAction(guidanceState)
              && inflameAction.isValidAction(guidanceState),
          "guidance regression fixture must contain legal card actions");
    check(guidanceSearcher.getActionGuidancePrior(guidanceState, inflameAction)
              > guidanceSearcher.getActionGuidancePrior(guidanceState, endTurnAction),
          "preferred-card order must be preserved in the LLM search prior");
    check(guidanceSearcher.getActionGuidancePrior(guidanceState, strikeAction) < 0.0,
          "discouraged cards must receive a negative LLM search prior");
    search::BattleScumSearcher2::Node unguidedNode;
    unguidedNode.edges.emplace_back();
    unguidedNode.edges.back().action = strikeAction;
    unguidedNode.edges.emplace_back();
    unguidedNode.edges.back().action = inflameAction;
    search::BattleScumSearcher2 unguidedSearcher(guidanceState);
    unguidedSearcher.initializeEdgeHeuristics(unguidedNode, guidanceState);

    search::BattleScumSearcher2::Node guidedNode;
    guidedNode.edges.emplace_back();
    guidedNode.edges.back().action = strikeAction;
    guidedNode.edges.emplace_back();
    guidedNode.edges.back().action = inflameAction;
    guidanceSearcher.initializeEdgeHeuristics(guidedNode, guidanceState);
    check(guidedNode.edges[1].heuristicPrior
              > unguidedNode.edges[1].heuristicPrior,
          "preferred cards must raise the MCTS progressive-bias prior");
    check(guidedNode.edges[0].heuristicPrior
              < unguidedNode.edges[0].heuristicPrior,
          "discouraged cards must lower the MCTS progressive-bias prior");
    search::BattleScumSearcher2 balancedGuidanceSearcher(guidanceState);
    balancedGuidanceSearcher.setActionGuidance(3, {"Inflame"}, {"Strike"});
    balancedGuidanceSearcher.root.edges.resize(2);
    balancedGuidanceSearcher.root.edges[0].action = strikeAction;
    balancedGuidanceSearcher.root.edges[1].action = inflameAction;
    check(balancedGuidanceSearcher.selectBestEdgeToSearch(
              balancedGuidanceSearcher.root) == 1,
          "balanced root ties must evaluate preferred actions first");
    search::BattleScumSearcher2 preferredVisitSearcher(guidanceState);
    preferredVisitSearcher.balanceRootActions = false;
    preferredVisitSearcher.minRootActionVisits = 10;
    preferredVisitSearcher.preferredRootActionVisits = 40;
    preferredVisitSearcher.setActionGuidance(3, {"Inflame"}, {});
    preferredVisitSearcher.root.simulationCount = 20;
    preferredVisitSearcher.root.edges.resize(2);
    preferredVisitSearcher.root.edges[0].action = strikeAction;
    preferredVisitSearcher.root.edges[0].simulationCount = 10;
    preferredVisitSearcher.root.edges[0].evaluationSum = 10.0;
    preferredVisitSearcher.root.edges[1].action = inflameAction;
    preferredVisitSearcher.root.edges[1].simulationCount = 10;
    preferredVisitSearcher.root.edges[1].evaluationSum = -10.0;
    check(preferredVisitSearcher.selectBestEdgeToSearch(
              preferredVisitSearcher.root) == 1,
          "a preferred root action must receive its bounded evidence floor");
    preferredVisitSearcher.root.simulationCount = 50;
    preferredVisitSearcher.root.edges[1].simulationCount = 40;
    preferredVisitSearcher.root.edges[1].evaluationSum = -40.0;
    check(preferredVisitSearcher.selectBestEdgeToSearch(
              preferredVisitSearcher.root) == 0,
          "preferred-root sampling must return to ordinary UCT after its floor");
    BattleContext nextTurnGuidanceState(guidanceState);
    nextTurnGuidanceState.turn = 4;
    check(guidanceSearcher.getActionGuidancePrior(nextTurnGuidanceState, inflameAction) == 0.0,
          "LLM action guidance must expire at the end of its originating turn");

    search::BattleScumSearcher2 cutoffSearcher(keyState);
    BattleContext cutoffState(keyState);
    cutoffState.player.curHp = 37;
    cutoffState.monsters.arr[0].curHp = 10;
    cutoffSearcher.updateFromPlayout(
        std::vector<search::BattleScumSearcher2::Node *>{&cutoffSearcher.root},
        {},
        {},
        cutoffState
    );
    check(cutoffSearcher.outcomePlayerHp == 0,
          "a live rollout cutoff must not be reported as a winning solution");
    check(cutoffSearcher.root.winSamples == 0
              && cutoffSearcher.root.lossSamples == 0
              && cutoffSearcher.root.cutoffSamples == 1,
          "an undecided rollout must be recorded as a cutoff sample");

    search::BattleScumSearcher2 outcomeStatsSearcher(keyState);
    outcomeStatsSearcher.root.edges.push_back(
        {search::Action(search::ActionType::END_TURN)}
    );
    auto *outcomeStatsChild = outcomeStatsSearcher.root.edges[0].node.get();
    const std::vector<search::BattleScumSearcher2::Node *> outcomeStatsStack{
        &outcomeStatsSearcher.root,
        outcomeStatsChild
    };
    const std::vector<int> outcomeStatsEdgeStack{0};
    BattleContext winningSampleA(keyState);
    winningSampleA.outcome = Outcome::PLAYER_VICTORY;
    winningSampleA.player.curHp = 42;
    outcomeStatsSearcher.updateFromPlayout(
        outcomeStatsStack, outcomeStatsEdgeStack, {}, winningSampleA
    );
    BattleContext winningSampleB(winningSampleA);
    winningSampleB.player.curHp = 38;
    outcomeStatsSearcher.updateFromPlayout(
        outcomeStatsStack, outcomeStatsEdgeStack, {}, winningSampleB
    );
    BattleContext losingSample(keyState);
    losingSample.outcome = Outcome::PLAYER_LOSS;
    losingSample.player.curHp = 0;
    outcomeStatsSearcher.updateFromPlayout(
        outcomeStatsStack, outcomeStatsEdgeStack, {}, losingSample
    );
    BattleContext cutoffSample(keyState);
    cutoffSample.player.curHp = 31;
    outcomeStatsSearcher.updateFromPlayout(
        outcomeStatsStack, outcomeStatsEdgeStack, {}, cutoffSample
    );
    const auto checkOutcomeStats = [&check] (const auto &stats) {
        check(stats.winSamples == 2
                  && stats.lossSamples == 1
                  && stats.cutoffSamples == 1,
              "playout backup must classify win, loss, and cutoff samples");
        check(std::abs(stats.winEndHpSum - 80.0) < 1e-12
                  && std::abs(stats.winEndHpSquaredSum - 3208.0) < 1e-12,
              "playout backup must accumulate winning end-HP moments");
        check(std::abs(stats.winUtilitySum - 1.0) < 1e-12,
              "playout backup must accumulate bounded winning utility");
    };
    checkOutcomeStats(outcomeStatsSearcher.root);
    checkOutcomeStats(*outcomeStatsChild);
    checkOutcomeStats(outcomeStatsSearcher.root.edges[0]);
    check(outcomeStatsSearcher.root.simulationCount
              == outcomeStatsSearcher.root.winSamples
                 + outcomeStatsSearcher.root.lossSamples
                 + outcomeStatsSearcher.root.cutoffSamples,
          "rollout outcome sample counts must partition all simulations");

    BattleContext potionState(keyState);
    potionState.potionCount = 2;
    potionState.potions.fill(Potion::EMPTY_POTION_SLOT);
    potionState.potions[0] = Potion::BLOCK_POTION;
    potionState.potions[1] = Potion::ENERGY_POTION;
    const auto hasPotionAction = [] (const search::BattleScumSearcher2::Node &node) {
        return std::any_of(node.edges.begin(), node.edges.end(), [] (const auto &edge) {
            return edge.action.getActionType() == search::ActionType::POTION;
        });
    };
    search::BattleScumSearcher2 noPotionSearcher(potionState);
    noPotionSearcher.enumerateActionsForNode(noPotionSearcher.root, potionState);
    check(!hasPotionAction(noPotionSearcher.root),
          "MCTS root must omit potion actions by default");

    search::BattleScumSearcher2 rootPotionSearcher(potionState);
    rootPotionSearcher.allowRootPotions = true;
    rootPotionSearcher.enumerateActionsForNode(rootPotionSearcher.root, potionState);
    check(hasPotionAction(rootPotionSearcher.root),
          "root-potion mode must enumerate available potions at the root");
    search::BattleScumSearcher2::Node childPotionNode;
    rootPotionSearcher.enumerateActionsForNode(childPotionNode, potionState);
    check(!hasPotionAction(childPotionNode),
          "root-potion mode must not enumerate potion actions in child nodes");
    search::BattleScumSearcher2::Node rolloutPotionNode;
    rootPotionSearcher.enumerateActionsForRollout(rolloutPotionNode, potionState);
    check(!hasPotionAction(rolloutPotionNode),
          "root-potion mode must not enumerate potion actions in rollouts");

    BattleContext deferredFlexState(keyState);
    deferredFlexState.cards.cardsInHand = 1;
    deferredFlexState.cards.hand[0] = CardInstance(CardId::DEFEND_RED);
    deferredFlexState.potionCount = 1;
    deferredFlexState.potions.fill(Potion::EMPTY_POTION_SLOT);
    deferredFlexState.potions[0] = Potion::FLEX_POTION;
    search::BattleScumSearcher2 deferredFlexSearcher(deferredFlexState);
    deferredFlexSearcher.allowedPotionSlotMask = 1;
    deferredFlexSearcher.enumerateActionsForNode(
        deferredFlexSearcher.root, deferredFlexState
    );
    check(!hasPotionAction(deferredFlexSearcher.root),
          "Flex Potion must wait until the current hand can realize its temporary Strength");

    BattleContext actionableFlexState(deferredFlexState);
    actionableFlexState.cards.hand[0] = CardInstance(CardId::STRIKE_RED);
    actionableFlexState.player.energy = 3;
    search::BattleScumSearcher2 actionableFlexSearcher(actionableFlexState);
    actionableFlexSearcher.allowedPotionSlotMask = 1;
    actionableFlexSearcher.enumerateActionsForNode(
        actionableFlexSearcher.root, actionableFlexState
    );
    check(hasPotionAction(actionableFlexSearcher.root),
          "Flex Potion must remain available when a playable attack is in hand");

    BattleContext deferredSpeedState(keyState);
    deferredSpeedState.cards.cardsInHand = 1;
    deferredSpeedState.cards.hand[0] = CardInstance(CardId::FEEL_NO_PAIN);
    deferredSpeedState.player.energy = 3;
    deferredSpeedState.potionCount = 1;
    deferredSpeedState.potions.fill(Potion::EMPTY_POTION_SLOT);
    deferredSpeedState.potions[0] = Potion::SPEED_POTION;
    search::BattleScumSearcher2 deferredSpeedSearcher(deferredSpeedState);
    deferredSpeedSearcher.allowedPotionSlotMask = 1;
    deferredSpeedSearcher.enumerateActionsForNode(
        deferredSpeedSearcher.root, deferredSpeedState
    );
    check(!hasPotionAction(deferredSpeedSearcher.root),
          "Speed Potion must wait until the current hand can realize its temporary Dexterity");

    BattleContext actionableSpeedState(deferredSpeedState);
    actionableSpeedState.cards.hand[0] = CardInstance(CardId::DEFEND_RED);
    search::BattleScumSearcher2 actionableSpeedSearcher(actionableSpeedState);
    actionableSpeedSearcher.allowedPotionSlotMask = 1;
    actionableSpeedSearcher.enumerateActionsForNode(
        actionableSpeedSearcher.root, actionableSpeedState
    );
    check(hasPotionAction(actionableSpeedSearcher.root),
          "Speed Potion must remain available when a playable Dexterity block card is in hand");

    search::BattleScumSearcher2 zeroBudgetSearcher(keyState);
    zeroBudgetSearcher.maxRolloutActions = 1;
    zeroBudgetSearcher.search(1, 0);
    check(zeroBudgetSearcher.root.simulationCount == 1 && !zeroBudgetSearcher.root.edges.empty(),
          "a positive simulation budget must produce a usable tree before checking the deadline");
    const auto rootActionsAfterBaseSearch = zeroBudgetSearcher.root.edges.size();
    zeroBudgetSearcher.search(1, 0);
    check(zeroBudgetSearcher.root.edges.size() == rootActionsAfterBaseSearch,
          "continuing an inconclusive search must reuse root edges instead of duplicating actions");

    BattleContext reptomancerSpawnState;
    reptomancerSpawnState.monsters.arr[0].construct(
        reptomancerSpawnState, MonsterId::DAGGER, 0
    );
    reptomancerSpawnState.monsters.arr[2].construct(
        reptomancerSpawnState, MonsterId::REPTOMANCER, 2
    );
    reptomancerSpawnState.monsters.monsterCount = 3;
    reptomancerSpawnState.monsters.monstersAlive = 2;
    reptomancerSpawnState.monsters.arr[2].reptomancerSummon(
        reptomancerSpawnState, 1
    );
    check(reptomancerSpawnState.monsters.arr[4].isAlive(),
          "Reptomancer must summon into its first fixed open slot");
    check(reptomancerSpawnState.monsters.monsterCount == 5,
          "Reptomancer summons must extend monsterCount to include slot 4");
    check(reptomancerSpawnState.monsters.monstersAlive == 3,
          "Reptomancer summons must keep the alive count consistent");

    BattleContext selectState;
    selectState.outcome = Outcome::UNDECIDED;
    selectState.inputState = InputState::CARD_SELECT;
    selectState.cards.cardsInHand = 4;
    selectState.cardSelectInfo.cardSelectTask = CardSelectTask::EXHAUST_MANY;
    selectState.cardSelectInfo.pickCount = 2;
    auto actions = search::Action::enumerateCardSelectActions(selectState);
    check(actions.size() == 11, "EXHAUST_MANY must enumerate every subset up to pickCount");

    selectState.cardSelectInfo.cardSelectTask = CardSelectTask::GAMBLE;
    actions = search::Action::enumerateCardSelectActions(selectState);
    check(actions.size() == 16, "GAMBLE must enumerate every hand subset");
    selectState.inputState = InputState::PLAYER_NORMAL;
    check(!search::Action(search::ActionType::MULTI_CARD_SELECT, 0).isValidAction(selectState),
          "a stale multi-card selector must not be executable outside CARD_SELECT");
    selectState.inputState = InputState::CARD_SELECT;

    selectState.cards.cardsInHand = CardManager::MAX_HAND_SIZE;
    actions = search::Action::enumerateCardSelectActions(selectState);
    check(actions.size() == 1024,
          "a full-hand GAMBLE must enumerate all 2^10 subsets without a fixed-buffer overflow");

    BattleContext largePileState;
    for (int i = 0; i < Deck::MAX_SIZE; ++i) {
        largePileState.cards.discardPile.push_back(CardInstance(CardId::WOUND));
    }
    check(largePileState.cards.discardPile.size() == Deck::MAX_SIZE,
          "combat card piles must safely hold a full 96-card deck");

    selectState.cardSelectInfo.cardSelectTask = CardSelectTask::HOLOGRAM;
    selectState.cards.discardPile.push_back(CardInstance(CardId::STRIKE_BLUE));
    actions = search::Action::enumerateCardSelectActions(selectState);
    check(actions.size() == 1, "HOLOGRAM must enumerate discard-pile selections");

    if (failures == 0) {
        std::cout << "MCTS regression tests passed" << std::endl;
        return 0;
    }
    return 1;
}

int main(int argc, const char* argv[]) {

    if (argc < 2) {
        std::cout << "incorrect arguments" << std::endl;
        return 0;
    }

    const std::string command(argv[1]);

    if (command == "mcts_regression") {
        return mctsRegressionTests();
    }

    if (command == "replay") {
        const std::uint64_t seed = std::stoull(argv[2]);
        const int ascension = std::stoi(argv[3]);
        const std::string actionFile(argv[4]);
        replayActionFile(GameContext(sts::CharacterClass::IRONCLAD, seed, ascension), actionFile);

    } else if (command == "save") {
        playFromSaveFile(argv[2], argv[3]);

    } if (command == "agent_mt") { // actually doing tree search now
        const int threadCount(std::stoi(argv[2]));
        const int depthArg = std::stoi(argv[3]);
        const int ascensionIn = std::stoi(argv[4]);
        const std::uint64_t startSeedLong(std::stoull(argv[5]));
        const int playoutCount(std::stoi(argv[6]));
        const int printLevel = std::stoi(argv[7]);
        g_print_level = printLevel;
        g_searchAscension = ascensionIn;
        g_simulationCount = depthArg;

        agentMt(threadCount, startSeedLong, playoutCount);

    } if (command == "simple_agent_mt") { // actually doing tree search now
        const int threadCount(std::stoi(argv[2]));
        const std::uint64_t startSeedLong(std::stoull(argv[3]));
        const int playoutCount(std::stoi(argv[4]));

        bool print = false;
        if (argc > 5) {
            print = true;
        }

        search::SimpleAgent::runAgentsMt(threadCount, startSeedLong, playoutCount, print);

    } else if (command == "json") {
        const std::string saveFilePath(argv[2]);
        const std::string jsonOutPath(argv[3]);
        std::ofstream outFileStream(jsonOutPath);
        outFileStream << SaveFile::getJsonFromSaveFile(saveFilePath);
        outFileStream.close();

    } else if (command == "json_to_save") {
        const std::string jsonInPath(argv[2]);
        const std::string saveFileOutPath(argv[3]);

        std::ifstream jsonIfStream(jsonInPath);
        SaveFile::writeJsonToSaveFile(jsonIfStream, saveFileOutPath);

    }  else if (command == "scum_searcher") {
        const std::uint64_t startSeedLong(std::stoull(argv[2]));
        const int playoutCount(std::stoi(argv[3]));

        for (std::uint64_t seed = startSeedLong; seed < startSeedLong+playoutCount; ++seed) {
//            playRandom4(startSeedLong);
        }

    } else if (command == "mcts_save") {
        return mcts(argc, argv);
    }

    //    printSizes();
//    std::cout << SeedHelper::getString(77) << '\n';
//    playRandom();
//    std::cout << getSeedWithGuardian();
//    replayActionList(argv[1]);

    return 0;
}
