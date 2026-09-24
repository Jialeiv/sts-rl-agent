#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <vector>

#include "combat/BattleContext.h"
#include "sim/search/Action.h"
#include "sim/search/BattleScumSearcher2.h"

using namespace sts;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

BattleContext battle(int monsterCount = 1) {
    BattleContext bc;
    bc.player.cc = CharacterClass::DEFECT;
    bc.player.curHp = 75;
    bc.player.maxHp = 75;
    bc.player.energy = 3;
    bc.player.energyPerTurn = 3;
    bc.player.cardDrawPerTurn = 5;
    bc.player.orbSlots = 10;
    bc.turn = 1;
    for (int i = 0; i < monsterCount; ++i) {
        bc.monsters.createMonster(bc, MonsterId::CULTIST);
        bc.monsters.arr[i].curHp = 999;
        bc.monsters.arr[i].maxHp = 999;
    }
    bc.monsterTurnIdx = bc.monsters.monsterCount;
    bc.inputState = InputState::PLAYER_NORMAL;
    return bc;
}

CardInstance makeCard(BattleContext &bc, CardId id, bool upgraded = false) {
    CardInstance card(id, upgraded);
    card.uniqueId = static_cast<std::int16_t>(bc.cards.nextUniqueCardId++);
    bc.cards.notifyAddCardToCombat(card);
    return card;
}

void putInHand(BattleContext &bc, CardInstance card) {
    bc.cards.moveToHand(card);
}

void putInDraw(BattleContext &bc, CardInstance card) {
    bc.cards.moveToDrawPileTop(card);
}

void putInDiscard(BattleContext &bc, CardInstance card) {
    bc.cards.moveToDiscardPile(card);
}

void runAction(BattleContext &bc, Action action) {
    bc.addToBot(action);
    bc.inputState = InputState::EXECUTING_ACTIONS;
    bc.executeActions();
}

void play(BattleContext &bc, int handIdx = 0, int targetIdx = 0) {
    search::Action(search::ActionType::CARD, handIdx, targetIdx).execute(bc);
}

int countCard(const std::vector<CardInstance> &cards, CardId id) {
    return static_cast<int>(std::count_if(
        cards.begin(), cards.end(), [=](const CardInstance &c) {
            return c.getId() == id;
        }
    ));
}

bool isDefectCommon(CardId id) {
    static constexpr std::array<CardId, 18> cards {{
        CardId::GO_FOR_THE_EYES, CardId::BALL_LIGHTNING,
        CardId::STREAMLINE, CardId::RECURSION, CardId::COMPILE_DRIVER,
        CardId::BARRAGE, CardId::STACK, CardId::REBOUND, CardId::CLAW,
        CardId::COOLHEADED, CardId::TURBO, CardId::SWEEPING_BEAM,
        CardId::CHARGE_BATTERY, CardId::HOLOGRAM, CardId::BEAM_CELL,
        CardId::LEAP, CardId::COLD_SNAP, CardId::STEAM_BARRIER,
    }};
    return std::find(cards.begin(), cards.end(), id) != cards.end();
}

void testScrapeRemovesDiscardedCards() {
    auto bc = battle();
    putInHand(bc, makeCard(bc, CardId::CLAW));
    putInDraw(bc, makeCard(bc, CardId::CLAW));
    putInDraw(bc, makeCard(bc, CardId::DEFEND_BLUE));
    putInDraw(bc, makeCard(bc, CardId::REINFORCED_BODY));
    auto freeDefend = makeCard(bc, CardId::DEFEND_BLUE);
    freeDefend.freeToPlayOnce = true;
    putInDraw(bc, freeDefend);

    runAction(bc, Actions::ScrapeAction(4));

    check(bc.cards.cardsInHand == 3,
          "Scrape must keep old, zero-cost, and freeToPlayOnce cards in hand");
    check(countCard(bc.cards.discardPile, CardId::DEFEND_BLUE) == 1,
          "Scrape must remove a nonzero-cost drawn card before discarding it");
    check(countCard(bc.cards.discardPile, CardId::REINFORCED_BODY) == 1,
          "Scrape must discard an X-cost card because its cost is not zero");
}

void testRebootMovesEveryCardExactlyOnce() {
    auto bc = battle();
    putInHand(bc, makeCard(bc, CardId::STRIKE_BLUE));
    putInHand(bc, makeCard(bc, CardId::DEFEND_BLUE));
    putInDraw(bc, makeCard(bc, CardId::ZAP));
    putInDiscard(bc, makeCard(bc, CardId::CLAW));
    bc.player.setHasRelic<RelicId::SUNDIAL>(true);
    bc.player.sundialCounter = 0;
    const auto cardRandomCounterBefore = bc.cardRandomRng.counter;

    runAction(bc, Actions::RebootAction(0));

    check(bc.cards.cardsInHand == 0, "Reboot must empty the hand");
    check(bc.cards.discardPile.empty(), "Reboot must empty the discard pile");
    check(bc.cards.drawPile.size() == 4,
          "Reboot must preserve every hand/draw/discard card exactly once");
    check(bc.player.sundialCounter == 1,
          "Reboot must trigger onShuffle relics exactly once");
    check(bc.cardRandomRng.counter == cardRandomCounterBefore + 2,
          "Reboot must consume cardRandomRng once per returned hand card");
}

void testXCostCardsSpendEnergyAndUseChemicalX() {
    {
        auto bc = battle();
        putInHand(bc, makeCard(bc, CardId::REINFORCED_BODY));
        play(bc);
        check(bc.player.energy == 0, "Reinforced Body must spend all energy");
        check(bc.player.block == 21, "Reinforced Body must block once per energy");
    }
    {
        auto bc = battle();
        bc.player.setHasRelic<RelicId::CHEMICAL_X>(true);
        putInHand(bc, makeCard(bc, CardId::REINFORCED_BODY));
        play(bc);
        check(bc.player.energy == 0,
              "Chemical X must not prevent Reinforced Body from spending energy");
        check(bc.player.block == 35,
              "Chemical X must add two Reinforced Body repetitions");
    }
    {
        auto bc = battle();
        putInHand(bc, makeCard(bc, CardId::TEMPEST));
        play(bc);
        check(bc.player.energy == 0, "Tempest must spend all energy");
        check(bc.player.getOrbCount() == 3,
              "Tempest must channel one Lightning per energy");
    }
    {
        auto bc = battle();
        bc.player.channelOrb(bc, Orb::DARK);
        putInHand(bc, makeCard(bc, CardId::MULTI_CAST));
        const int hpBefore = bc.monsters.arr[0].curHp;
        play(bc);
        check(bc.player.energy == 0, "Multi-Cast must spend all energy");
        check(bc.player.getOrbCount() == 0, "Multi-Cast must evoke the selected orb");
        check(bc.monsters.arr[0].curHp == hpBefore - 18,
              "Multi-Cast must evoke a base Dark orb once per energy");
    }
}

void testActionQueueExpandsWithoutChangingOrder() {
    BattleContext bc;
    std::vector<int> order;
    const auto action = [&] (int value, bool clear) {
        return Action{[&, value] (BattleContext &) { order.push_back(value); }, clear};
    };
    ActionQueue<2> queue;
    queue.pushBack(action(1, false));
    queue.pushBack(action(2, true));
    queue.pushFront(action(0, false));
    queue.pushBack(action(3, false));
    queue.clearOnCombatVictory();
    while (!queue.isEmpty()) {
        queue.popFront()(bc);
    }
    check(order == std::vector<int>({0, 1, 3}),
          "an expanded queue must preserve order and combat-victory actions");
}

void testLargeXCostEffectsDoNotOverflowActionQueue() {
    {
        auto bc = battle();
        bc.player.energy = 64;
        putInHand(bc, makeCard(bc, CardId::REINFORCED_BODY));
        play(bc);
        check(bc.player.energy == 0 && bc.player.block == 64 * 7,
              "large Reinforced Body must resolve without filling the action queue");
    }
    {
        auto bc = battle();
        bc.player.energy = 64;
        const int hpBefore = bc.monsters.arr[0].curHp;
        putInHand(bc, makeCard(bc, CardId::TEMPEST));
        play(bc);
        check(bc.player.energy == 0
                  && bc.player.getOrbCount() == 10
                  && bc.player.lightningChanneled == 64
                  && bc.monsters.arr[0].curHp == hpBefore - (64 - 10) * 8,
              "large Tempest must resolve without filling the action queue");
    }
    {
        auto bc = battle();
        bc.player.energy = 64;
        bc.player.channelOrb(bc, Orb::DARK);
        const int hpBefore = bc.monsters.arr[0].curHp;
        putInHand(bc, makeCard(bc, CardId::MULTI_CAST));
        play(bc);
        check(bc.player.energy == 0
                  && bc.player.getOrbCount() == 0
                  && bc.monsters.arr[0].curHp == hpBefore - 64 * 6,
              "large Multi-Cast must resolve without filling the action queue");
    }
}

void testRecycleSelectionAndXCostEnergy() {
    auto bc = battle();
    auto xCost = makeCard(bc, CardId::REINFORCED_BODY);
    putInHand(bc, xCost);
    putInHand(bc, makeCard(bc, CardId::STRIKE_BLUE));
    bc.player.energy = 2;
    bc.openSimpleCardSelectScreen(CardSelectTask::RECYCLE, 1);

    const auto choices = search::Action::enumerateCardSelectActions(bc);
    check(choices.size() == 2, "Recycle must expose every hand card to MCTS");
    choices[0].execute(bc);

    check(bc.player.energy == 4,
          "Recycling an X-cost card must gain energy equal to current energy");
    check(countCard(bc.cards.exhaustPile, CardId::REINFORCED_BODY) == 1,
          "Recycle must exhaust the selected card");
}

void testUpgradedSeekSelectsTwoCards() {
    auto bc = battle();
    putInDraw(bc, makeCard(bc, CardId::STRIKE_BLUE));
    putInDraw(bc, makeCard(bc, CardId::DEFEND_BLUE));
    bc.openSimpleCardSelectScreen(CardSelectTask::SEEK, 2);

    search::Action(search::ActionType::SINGLE_CARD_SELECT, 0).execute(bc);
    check(bc.inputState == InputState::CARD_SELECT,
          "Seek+ must keep selection open after the first card");
    check(bc.cardSelectInfo.pickCount == 1,
          "Seek+ must request exactly one more card");

    search::Action(search::ActionType::SINGLE_CARD_SELECT, 0).execute(bc);
    check(bc.inputState != InputState::CARD_SELECT,
          "Seek+ must close selection after the second card");
    check(bc.cards.cardsInHand == 2,
          "Seek+ must move two distinct draw-pile cards into hand");
}

void testSelfRepairCountsAsPostCombatHp() {
    auto root = battle();
    root.player.curHp = 40;
    auto won = root;
    won.player.buff<PS::SELF_REPAIR>(7);
    won.outcome = Outcome::PLAYER_VICTORY;

    check(search::getPostCombatPlayerHp(won) == 47,
          "Self Repair must count toward HP carried out of a victory");
    check(search::evaluateEndState(root, won) == 47.0 / 75.0,
          "terminal MCTS utility must include pending Self Repair healing");

    search::PlayoutStats stats;
    stats.record(0.0, won, search::evaluateEndState(root, won), false, 0);
    check(stats.winEndHpSum == 47.0 && stats.bestWinEndHp == 47,
          "MCTS victory HP statistics must include Self Repair healing");

    won.player.setHasRelic<RelicId::MARK_OF_THE_BLOOM>(true);
    check(search::getPostCombatPlayerHp(won) == 40,
          "Mark of the Bloom must prevent Self Repair healing");
}

void testClawUsesPerCardState() {
    auto bc = battle();
    auto played = makeCard(bc, CardId::CLAW);
    auto other = makeCard(bc, CardId::CLAW);
    putInHand(bc, played);
    putInHand(bc, other);
    const int hpBefore = bc.monsters.arr[0].curHp;

    play(bc);

    check(bc.monsters.arr[0].curHp == hpBefore - 3,
          "an unscaled Claw must initially deal 3 damage");
    check(bc.cards.cardsInHand == 1 && bc.cards.hand[0].specialData == 2,
          "Claw must increase other existing Claws by 2");
    check(countCard(bc.cards.discardPile, CardId::CLAW) == 1
              && bc.cards.discardPile.back().specialData == 2,
          "the played Claw must retain its own +2 increase");

    const auto future = makeCard(bc, CardId::CLAW);
    check(future.specialData == 0,
          "Claw must not grant a global bonus to future generated Claws");
}

void testStackedEchoForm() {
    auto bc = battle();
    bc.player.energy = 10;
    bc.player.buff<PS::ECHO_FORM>(2);
    putInHand(bc, makeCard(bc, CardId::STRIKE_BLUE));
    putInHand(bc, makeCard(bc, CardId::STRIKE_BLUE));
    putInHand(bc, makeCard(bc, CardId::STRIKE_BLUE));
    const int hpBefore = bc.monsters.arr[0].curHp;

    play(bc);
    play(bc);
    play(bc);

    check(bc.monsters.arr[0].curHp == hpBefore - 30,
          "Echo Form 2 must duplicate the first two original cards");
    check(bc.player.echoFormCardsDoubled == 2,
          "Echo Form must track two duplicated original cards");
}

void testBiasedCognitionArtifact() {
    auto bc = battle();
    bc.player.artifact = 1;
    putInHand(bc, makeCard(bc, CardId::BIASED_COGNITION));
    play(bc);

    check(bc.player.focus == 4, "Biased Cognition must immediately grant Focus");
    check(bc.player.artifact == 0, "Artifact must block Biased Cognition's Bias");
    check(!bc.player.hasStatus<PS::BIAS>(),
          "blocked Bias must not be installed as a power");
}

void testDoubleEnergyUsesPostCostEnergy() {
    auto bc = battle();
    putInHand(bc, makeCard(bc, CardId::DOUBLE_ENERGY));
    play(bc);
    check(bc.player.energy == 4,
          "unupgraded Double Energy at 3 energy must finish at 4, not 5");
}

void testEmotionChipTracksAllHpLoss() {
    auto bc = battle();
    bc.player.setHasRelic<RelicId::EMOTION_CHIP>(true);
    bc.player.loseHp(bc, 2, true);
    check(bc.player.lastDamageTaken == 2,
          "Emotion Chip must remember self-inflicted HP loss");
}

void testHelloWorldStacks() {
    auto bc = battle();
    bc.player.buff<PS::HELLO_WORLD>(2);
    bc.player.applyStartOfTurnPowers(bc);
    bc.inputState = InputState::EXECUTING_ACTIONS;
    bc.executeActions();
    check(bc.cards.cardsInHand == 2,
          "Hello World must create one Common card per stack");
    for (int i = 0; i < bc.cards.cardsInHand; ++i) {
        check(isDefectCommon(bc.cards.hand[i].getId()),
              "Hello World created non-Defect card: "
                  + bc.cards.hand[i].getName());
    }
}

void testDarkPassiveCannotReduceCharge() {
    auto bc = battle();
    bc.player.focus = -10;
    bc.player.orbTypes[0] = Orb::DARK;
    bc.player.orbData[0] = 6;
    bc.player.triggerOrbPassive(bc, 0);
    check(bc.player.orbData[0] == 6,
          "negative Focus must not reduce a Dark orb's stored damage");
}

void testDualcastCanEvokeLastOrb() {
    auto bc = battle();
    bc.player.orbSlots = 3;
    bc.player.channelOrbWithData(bc, Orb::DARK, 37);
    putInHand(bc, makeCard(bc, CardId::DUALCAST));

    play(bc);

    check(bc.player.getOrbCount() == 0,
          "Dualcast must remove the last orb");
    check(bc.player.orbTypes[0] == Orb::EMPTY && bc.player.orbData[0] == 0,
          "removing the last orb must clear both its type and stored data");
}

void testUpgradedDualcastStillEvokesTwice() {
    auto bc = battle();
    bc.player.orbSlots = 3;
    bc.player.orbTypes[0] = Orb::DARK;
    bc.player.orbData[0] = 37;
    bc.player.orbTypes[1] = Orb::FROST;
    bc.player.orbTypes[2] = Orb::DARK;
    bc.player.orbData[2] = 6;
    bc.monsters.arr[0].curHp = 86;
    bc.monsters.arr[0].maxHp = 125;
    putInHand(bc, makeCard(bc, CardId::DUALCAST, true));

    play(bc);

    check(bc.monsters.arr[0].curHp == 12,
          "Dualcast+ must evoke a 37-damage Dark orb exactly twice");
    check(bc.outcome != Outcome::PLAYER_VICTORY,
          "Dualcast+ must not report lethal when two evokes leave 12 HP");
    check(bc.player.getOrbCount() == 2,
          "Dualcast+ must remove only the orb it evokes twice");
}

void testChillCountsLivingEnemies() {
    auto bc = battle(2);
    bc.monsters.arr[1].curHp = 0;
    bc.monsters.monstersAlive = 1;
    putInHand(bc, makeCard(bc, CardId::CHILL));
    play(bc);
    check(bc.player.getOrbCount() == 1
              && bc.player.orbTypes[0] == Orb::FROST,
          "Chill must channel Frost only for living enemies");
}

void testAllForOneIncludesFreeCards() {
    auto bc = battle();
    auto freeDefend = makeCard(bc, CardId::DEFEND_BLUE);
    freeDefend.freeToPlayOnce = true;
    putInDiscard(bc, freeDefend);
    putInDiscard(bc, makeCard(bc, CardId::CLAW));
    putInDraw(bc, makeCard(bc, CardId::STRIKE_BLUE));

    runAction(bc, Actions::AllForOneAction());

    check(bc.cards.cardsInHand == 2,
          "All for One must return base-cost-zero and freeToPlayOnce cards");
}

void testForceFieldTracksCombatPowerHistory() {
    {
        auto bc = battle();
        bc.powersPlayedThisCombat = 3;
        runAction(bc, Actions::MakeTempCardInHand(CardId::FORCE_FIELD));
        check(bc.cards.cardsInHand == 1
                  && bc.cards.hand[0].cost == 1
                  && bc.cards.hand[0].costForTurn == 1,
              "a newly generated Force Field must include prior Power plays");
    }
    {
        auto bc = battle();
        putInHand(bc, makeCard(bc, CardId::FORCE_FIELD));
        putInHand(bc, makeCard(bc, CardId::DEFRAGMENT));
        play(bc, 1);
        check(bc.powersPlayedThisCombat == 1,
              "playing a Power must advance Force Field combat history");
        check(bc.cards.cardsInHand == 1
                  && bc.cards.hand[0].cost == 3
                  && bc.cards.hand[0].specialData == 1,
              "an existing Force Field must lose one cost per Power played");
    }
}

void testAmplify() {
    {
        auto bc = battle();
        bc.player.energy = 10;
        putInHand(bc, makeCard(bc, CardId::AMPLIFY));
        putInHand(bc, makeCard(bc, CardId::DEFRAGMENT));

        play(bc);
        check(bc.player.getStatus<PS::AMPLIFY>() == 1,
              "Amplify must install one same-turn Power duplication");
        play(bc);
        check(bc.player.focus == 2,
              "Amplify must play the next Power twice");
        check(!bc.player.hasStatus<PS::AMPLIFY>(),
              "playing a Power must consume Amplify");
    }
    {
        auto bc = battle();
        putInHand(bc, makeCard(bc, CardId::AMPLIFY, true));
        play(bc);
        check(bc.player.getStatus<PS::AMPLIFY>() == 2,
              "Amplify+ must install two Power duplications");

        bc.player.applyEndOfTurnPowers(bc);
        bc.inputState = InputState::EXECUTING_ACTIONS;
        bc.executeActions();
        check(!bc.player.hasStatus<PS::AMPLIFY>(),
              "unused Amplify must expire at end of turn");
    }
}

} // namespace

int main() {
    testScrapeRemovesDiscardedCards();
    testRebootMovesEveryCardExactlyOnce();
    testXCostCardsSpendEnergyAndUseChemicalX();
    testActionQueueExpandsWithoutChangingOrder();
    testLargeXCostEffectsDoNotOverflowActionQueue();
    testRecycleSelectionAndXCostEnergy();
    testUpgradedSeekSelectsTwoCards();
    testSelfRepairCountsAsPostCombatHp();
    testClawUsesPerCardState();
    testStackedEchoForm();
    testBiasedCognitionArtifact();
    testDoubleEnergyUsesPostCostEnergy();
    testEmotionChipTracksAllHpLoss();
    testHelloWorldStacks();
    testDarkPassiveCannotReduceCharge();
    testDualcastCanEvokeLastOrb();
    testUpgradedDualcastStillEvokesTwice();
    testChillCountsLivingEnemies();
    testAllForOneIncludesFreeCards();
    testForceFieldTracksCombatPowerHistory();
    testAmplify();

    if (failures != 0) {
        return 1;
    }
    std::cout << "Defect combat tests passed\n";
    return 0;
}
