#include "combat/BattleContext.h"
#include "game/GameContext.h"
#include "sim/search/Action.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace sts;

namespace {
void expect(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void addCard(BattleContext &bc, CardId id, bool upgraded = false) {
    CardInstance card(id, upgraded);
    card.uniqueId = bc.cards.nextUniqueCardId++;
    bc.cards.notifyAddCardToCombat(card);
    bc.cards.notifyAddToHand(card);
    bc.cards.hand[bc.cards.cardsInHand++] = card;
}

BattleContext fixture(CardId card = CardId::STRIKE_RED, bool upgraded = false) {
    GameContext gc(CharacterClass::IRONCLAD, 123, 20);
    BattleContext bc;
    bc.init(gc, MonsterEncounter::CULTIST);
    bc.player.relicBits0 = 0;
    bc.player.relicBits1 = 0;
    bc.player.maxHp = 100;
    bc.player.curHp = 10;
    bc.player.block = 0;
    bc.player.energy = 3;
    bc.cards = CardManager();
    addCard(bc, card, upgraded);
    bc.potions.fill(Potion::EMPTY_POTION_SLOT);
    bc.potionCapacity = 2;
    bc.potionCount = 0;
    bc.monsters.arr[0].setMove(MonsterMoveId::CULTIST_DARK_STRIKE);
    return bc;
}

void givePotion(BattleContext &bc, Potion potion) {
    bc.potions[0] = potion;
    bc.potionCount = 1;
}

void drinkPotion(BattleContext &bc) {
    search::Action action(search::ActionType::POTION, 0, 0);
    expect(action.isValidAction(bc), "potion action must be legal before execution");
    action.execute(bc);
}

void blood(bool bark, int maxHp, int startingHp, int expectedHp) {
    auto bc = fixture();
    bc.player.maxHp = maxHp;
    bc.player.curHp = startingHp;
    bc.player.setHasRelic<RelicId::SACRED_BARK>(bark);
    givePotion(bc, Potion::BLOOD_POTION);
    drinkPotion(bc);
    expect(bc.player.curHp == expectedHp,
           "Blood Potion: HP=" + std::to_string(bc.player.curHp) +
           ", expected=" + std::to_string(expectedHp));
    expect(bc.potionCount == 0, "Blood Potion must be consumed");
    expect(bc.outcome == Outcome::UNDECIDED, "healing must not end this battle");
}

void ironWave(bool upgraded, int dexterity, bool frail, int expectedBlock) {
    auto bc = fixture(CardId::IRON_WAVE, upgraded);
    bc.player.dexterity = dexterity;
    if (frail) bc.player.debuff<PlayerStatus::FRAIL>(2, false);
    const int enemyHp = bc.monsters.arr[0].curHp;
    search::Action action(search::ActionType::CARD, 0, 0);
    expect(action.isValidAction(bc), "Iron Wave must be playable");
    action.execute(bc);
    expect(bc.player.block == expectedBlock,
           "Iron Wave: block=" + std::to_string(bc.player.block) +
           ", expected=" + std::to_string(expectedBlock));
    expect(bc.monsters.arr[0].curHp == enemyHp - (upgraded ? 7 : 5),
           "block fix must preserve Iron Wave damage");
}

BattleContext exhaustLastCards(Potion potion) {
    // A reachable Ironclad sequence: Fiend Fire exhausts the only other card
    // (Wound), deals 7 damage, then exhausts itself. Potions remain available.
    auto bc = fixture(CardId::FIEND_FIRE);
    addCard(bc, CardId::WOUND);
    bc.monsters.arr[0].curHp = 12;
    if (potion != Potion::EMPTY_POTION_SLOT) givePotion(bc, potion);
    search::Action action(search::ActionType::CARD, 0, 0);
    expect(action.isValidAction(bc), "Fiend Fire must be playable");
    action.execute(bc);
    expect(bc.cards.cardsInHand == 0 && bc.cards.drawPile.empty() && bc.cards.discardPile.empty(),
           "Fiend Fire must leave no playable piles");
    expect(bc.cards.exhaustPile.size() == 2 && bc.monsters.arr[0].curHp == 5,
           "Fiend Fire must exhaust both cards and leave the Cultist at 5 HP");
    expect(bc.outcome == Outcome::UNDECIDED,
           "living Ironclad must retain control after exhausting the last cards");
    return bc;
}

void cardlessPoison() {
    auto bc = exhaustLastCards(Potion::POISON_POTION);
    drinkPotion(bc);
    expect(bc.monsters.arr[0].getStatus<MonsterStatus::POISON>() == 6,
           "Poison Potion must apply 6 Poison");
    expect(bc.outcome == Outcome::UNDECIDED, "poison must wait for the enemy turn");
    search::Action(search::ActionType::END_TURN).execute(bc);
    expect(bc.outcome == Outcome::PLAYER_VICTORY && bc.monsters.arr[0].curHp <= 0,
           "Poison Potion must win after Fiend Fire exhausts the deck");
    expect(bc.player.curHp == 10, "lethal poison must resolve before the Cultist attack");
}

void cardlessFire() {
    auto bc = exhaustLastCards(Potion::FIRE_POTION);
    drinkPotion(bc);
    expect(bc.outcome == Outcome::PLAYER_VICTORY && bc.monsters.arr[0].curHp <= 0,
           "Fire Potion must remain a winning action after exhausting the deck");
}

void cardlessDeath() {
    auto bc = exhaustLastCards(Potion::EMPTY_POTION_SLOT);
    bc.player.curHp = 1;
    search::Action(search::ActionType::END_TURN).execute(bc);
    expect(bc.outcome == Outcome::PLAYER_LOSS && bc.player.curHp <= 0,
           "removing the shortcut must preserve defeat from actual enemy damage");
}

void poisonWithCards() {
    auto bc = fixture();
    bc.monsters.arr[0].curHp = 5;
    givePotion(bc, Potion::POISON_POTION);
    drinkPotion(bc);
    search::Action(search::ActionType::END_TURN).execute(bc);
    expect(bc.outcome == Outcome::PLAYER_VICTORY && bc.player.curHp == 10,
           "control: lethal poison with cards remaining must still win before the attack");
}

void poisonModifiers(const std::string &name) {
    auto bc = fixture();
    bc.player.curHp = 100;
    auto &monster = bc.monsters.arr[0];
    monster.curHp = 50;
    if (name == "poison_block") {
        monster.block = 20;
        monster.setHasStatus<MonsterStatus::BARRICADE>(true);
    }
    if (name == "poison_intangible") monster.buff<MonsterStatus::INTANGIBLE>(2);
    if (name == "poison_artifact") monster.buff<MonsterStatus::ARTIFACT>(1);
    if (name == "poison_expiration") {
        monster.addDebuff<MonsterStatus::POISON>(1, false);
    } else {
        givePotion(bc, Potion::POISON_POTION);
        drinkPotion(bc);
    }
    search::Action(search::ActionType::END_TURN).execute(bc);
    const int expectedDamage = name == "poison_artifact" ? 0 :
            (name == "poison_expiration" || name == "poison_intangible" ? 1 : 6);
    const int expectedPoison = name == "poison_artifact" || name == "poison_expiration" ? 0 : 5;
    expect(monster.curHp == 50 - expectedDamage, name + ": incorrect HP loss");
    expect(monster.getStatus<MonsterStatus::POISON>() == expectedPoison,
           name + ": poison must decrement once, or be blocked by Artifact");
    expect(monster.hasStatus<MonsterStatus::POISON>() == (expectedPoison > 0),
           name + ": expired poison must remove its status flag");
    if (name == "poison_block") expect(monster.block == 20, "poison must bypass block");
    if (name == "poison_artifact") expect(monster.artifact == 0, "Artifact must be consumed");
}

void poisonTargets() {
    auto bc = fixture();
    bc.player.curHp = 100;
    bc.monsters.arr[1].construct(bc, MonsterId::CULTIST, 1);
    bc.monsters.arr[2].construct(bc, MonsterId::CULTIST, 2);
    bc.monsters.monsterCount = 3;
    bc.monsters.monstersAlive = 3;
    for (int i = 0; i < 3; ++i) {
        bc.monsters.arr[i].curHp = 50;
        bc.monsters.arr[i].setMove(MonsterMoveId::CULTIST_DARK_STRIKE);
    }
    bc.monsters.arr[0].addDebuff<MonsterStatus::POISON>(6, false);
    bc.monsters.arr[1].addDebuff<MonsterStatus::POISON>(2, false);
    search::Action(search::ActionType::END_TURN).execute(bc);
    expect(bc.monsters.arr[0].curHp == 44 && bc.monsters.arr[0].poison == 5,
           "first poisoned enemy must tick once for 6");
    expect(bc.monsters.arr[1].curHp == 48 && bc.monsters.arr[1].poison == 1,
           "second poisoned enemy must tick once for 2");
    expect(bc.monsters.arr[2].curHp == 50 && !bc.monsters.arr[2].hasStatus<MonsterStatus::POISON>(),
           "unpoisoned enemy must be unaffected");
}
}

int main(int argc, char **argv) {
    try {
        const std::string name = argc > 1 ? argv[1] : "";
        if (name == "blood_normal") blood(false, 100, 10, 30);
        else if (name == "blood_bark") blood(true, 100, 10, 50);
        else if (name == "blood_rounding") blood(true, 99, 10, 49);
        else if (name == "blood_cap") blood(false, 100, 95, 100);
        else if (name == "iron_wave_base") ironWave(false, 0, false, 5);
        else if (name == "iron_wave_dex") ironWave(false, 2, false, 7);
        else if (name == "iron_wave_frail") ironWave(true, 0, true, 5);
        else if (name == "iron_wave_negative_dex") ironWave(false, -2, false, 3);
        else if (name == "cardless_poison") cardlessPoison();
        else if (name == "cardless_fire") cardlessFire();
        else if (name == "cardless_death") cardlessDeath();
        else if (name == "poison_with_cards") poisonWithCards();
        else if (name == "poison_block" || name == "poison_intangible" ||
                 name == "poison_expiration" || name == "poison_artifact") poisonModifiers(name);
        else if (name == "poison_targets") poisonTargets();
        else throw std::runtime_error("unknown test case: " + name);
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
