//
// Created by gamerpuppy on 7/4/2021.
//

#include <cassert>
#include <algorithm>

#include <constants/PlayerStatusEffects.h>
#include <combat/BattleContext.h>
#include <combat/Actions.h>
#include <game/Game.h>
#include "combat/Player.h"

using namespace sts;


void Player::setHasStatus(PlayerStatus s, bool value) {
//        static_assert(s != PlayerStatus::THE_BOMB);

    switch (s) {
        case PS::ARTIFACT:
        case PS::DEXTERITY:
        case PS::STRENGTH:
        case PS::FOCUS:
            return;
        default:
            break;
    }

    //static_assert(((int)s) < 64); // did we add too many status effects
    int idx = static_cast<int>(s);
    if (value) {
        if (idx < 64) {
            statusBits0 |= 1ULL << idx;
        } else {
            statusBits1 |= 1ULL << (idx-64);
        }
    } else {
        if (idx < 64) {
            statusBits0 &= ~(1ULL << idx);
        } else {
            statusBits1 &= ~(1ULL << (idx-64));
        }
    }
}

void Player::setStatusValueNoChecks(PlayerStatus s, int value) {
    switch (s) {
        case PS::ARTIFACT:
            artifact = value;
            break;

        case PS::DEXTERITY:
            dexterity = value;
            break;

        case PS::FOCUS:
            focus = value;
            break;

        case PS::STRENGTH:
            strength = value;
            break;

        default:
            statusMap[s] = value;
    }
}

bool Player::hasRelicRuntime(RelicId r) const {
    const int relicIdx = static_cast<int>(r);
    if (relicIdx < 64) {
        return relicBits0 & (1ULL << relicIdx);
    } else if (relicIdx < 128) {
        return relicBits1 & (1ULL << (relicIdx-64));
    } else {
        return relicBits2 & (1ULL << (relicIdx-128));
    }
}

bool Player::hasStatusRuntime(PlayerStatus s) const {
    switch (s) {
        case PS::ARTIFACT:
            return artifact;

        case PS::DEXTERITY:
            return dexterity;

        case PS::FOCUS:
            return focus;

        case PS::STRENGTH:
            return strength;

        default:
            break;
    }

    int idx = static_cast<int>(s);
    if (idx < 64) {
        return statusBits0 & (1ULL << idx);
    } else {
        return statusBits1 & (1ULL << (idx-64));
    }
}

int Player::getStatusRuntime(PlayerStatus s) const {
    switch (s) {
        case PS::ARTIFACT:
            return artifact;
        case PS::DEXTERITY:
            return dexterity;
        case PS::FOCUS:
            return focus;
        case PS::STRENGTH:
            return strength;
        default:
            if (hasStatusRuntime(s)) {
                return statusMap.at(s);
            } else {
                return 0;
            }
    }
}

void Player::setJustApplied(PlayerStatus s, bool value) {
    if (value) {
        justAppliedBits |= (1ULL << static_cast<int>(s));
    } else {
        justAppliedBits &= ~(1ULL << static_cast<int>(s));
    }
}

void Player::gainBlock(BattleContext &bc, int amount) {
    if (amount <= 0) {
        return;
    }

    block += amount;

    if (hasStatus<PS::JUGGERNAUT>()) {
        bc.addToBot(Actions::DamageRandomEnemy(getStatus<PS::JUGGERNAUT>()));
    }

    // todo watcher weak power
}

void Player::gainGold(BattleContext &, int amount) {
#ifdef sts_asserts
    assert(amount > 0);
#endif

    if (hasRelic<R::ECTOPLASM>()) {
        return;
    }

    gold += amount;
    if (hasRelic<R::BLOODY_IDOL>()) {
        heal(5);
    }
}

void Player::useEnergy(int amount) {
#ifdef sts_asserts
    assert(energy >= amount);
#endif

    energy -= amount;
}

void Player::gainEnergy(int amount) {
    energy += amount;
}

void Player::increaseOrbSlots(int amount) {
    orbSlots = static_cast<int8_t>(
        std::min(MAX_ORB_SLOTS, static_cast<int>(orbSlots) + amount)
    );
}

void Player::channelOrb(BattleContext &bc, Orb orb) {
    if (orbSlots <= 0) {
        return;
    }

    if (getOrbCount() >= orbSlots) {
        // no empty slot: the oldest (leftmost) orb is evoked first
        evokeOrb(bc, 0);
    }

    const int idx = getOrbCount(); // first empty slot, orbs are kept contiguous
    orbTypes[idx] = orb;
    orbData[idx] = (orb == Orb::DARK) ? 6 : 0;

    if (orb == Orb::LIGHTNING) {
        ++lightningChanneled;
    } else if (orb == Orb::FROST) {
        ++frostChanneled;
    }
}

void Player::channelOrbWithData(BattleContext &bc, Orb orb, int data) {
    if (orbSlots <= 0) {
        return;
    }

    if (getOrbCount() >= orbSlots) {
        evokeOrb(bc, 0);
    }

    const int idx = getOrbCount();
    orbTypes[idx] = orb;
    orbData[idx] = static_cast<std::int16_t>(data);

    if (orb == Orb::LIGHTNING) {
        ++lightningChanneled;
    } else if (orb == Orb::FROST) {
        ++frostChanneled;
    }
}

void Player::evokeOrb(BattleContext &bc, int slot) {
    if (slot < 0 || slot >= getOrbCount()) {
        return;
    }
    triggerOrbEvoke(bc, slot);
    removeOrb(slot);
}

void Player::triggerOrbEvoke(BattleContext &bc, int slot) {
    if (slot < 0 || slot >= getOrbCount()) {
        return;
    }

    // 真实游戏里四种球的 onEvoke() 全部走 addToTop(引爆特效插队立刻结算),
    // 只有回合末被动 onEndOfTurn() 才是 addToBottom。这个区别不是特效先后的小事:
    // 闪电球引爆会调 AbstractDungeon.getRandomMonster() 消耗一次 cardRandomRng,
    // 用 addToBot 排到队尾就会和同一张牌触发的其它入队效果(如邪咒塞迷乱牌)抢 RNG 顺序,
    // 两边摇到的数互换 —— 表现为牌堆插入位置对不上,极难回溯。
    const Orb orb = orbTypes[slot];
    const int data = orbData[slot];

    switch (orb) {
        case Orb::LIGHTNING: {
            const int dmg = std::max(0, 8 + focus);
            if (hasStatus<PS::ELECTRO>()) {
                bc.addToTop(Actions::OrbLightningAttackAllEnemy(dmg));
            } else {
                bc.addToTop(Actions::OrbLightningDamageRandom(dmg));
            }
            break;
        }

        case Orb::FROST:
            bc.addToTop(Actions::GainBlock(std::max(0, 5 + focus)));
            break;

        case Orb::DARK:
            // Dark deals its stored damage to the living enemy with the lowest HP
            bc.addToTop(Actions::OrbDarkDamageLowestHp(std::max(0, data)));
            break;

        case Orb::PLASMA:
            bc.addToTop(Actions::GainEnergy(2));
            break;

        default:
            break;
    }
}

void Player::removeOrb(int slot) {
    const int count = getOrbCount();
    if (slot < 0 || slot >= count) {
        return;
    }
    for (int i = slot; i < count - 1; ++i) {
        orbTypes[i] = orbTypes[i + 1];
        orbData[i] = orbData[i + 1];
    }
    orbTypes[count - 1] = Orb::EMPTY;
    orbData[count - 1] = 0;
}

void Player::triggerOrbPassive(BattleContext &bc, int slot) {
    if (slot < 0 || slot >= getOrbCount()) {
        return;
    }

    switch (orbTypes[slot]) {
        case Orb::LIGHTNING: {
            const int dmg = std::max(0, 3 + focus);
            if (hasStatus<PS::ELECTRO>()) {
                bc.addToBot(Actions::OrbLightningAttackAllEnemy(dmg));
            } else {
                bc.addToBot(Actions::OrbLightningDamageRandom(dmg));
            }
            break;
        }

        case Orb::FROST:
            bc.addToBot(Actions::GainBlock(std::max(0, 2 + focus)));
            break;

        case Orb::DARK:
            // A Dark orb's passive amount cannot be negative.
            orbData[slot] = static_cast<std::int16_t>(
                orbData[slot] + std::max(0, 6 + focus)
            );
            break;

        case Orb::PLASMA:
            // Plasma's passive is triggered at the start of the turn
            bc.addToBot(Actions::GainEnergy(1));
            break;

        default:
            break;
    }
}

bool Player::hasEmptyOrb() const {
    return getOrbCount() < orbSlots;
}

int Player::getOrbCount() const {
    int count = 0;
    while (count < orbSlots && orbTypes[count] != Orb::EMPTY) {
        ++count;
    }
    return count;
}

int Player::getLastOrbIdx() const {
    const int count = getOrbCount();
    return count > 0 ? count - 1 : -1;
}

int Player::getFirstOrbIdx() const {
    return getOrbCount() > 0 ? 0 : -1;
}

void Player::removeDebuffs() {
    if (getStatus<PS::STRENGTH>() < 0) {
        setStatusValueNoChecks<PS::STRENGTH>(0);
    }
    if (getStatus<PS::DEXTERITY>() < 0) {
        setStatusValueNoChecks<PS::DEXTERITY>(0);
    }

    removeStatus<PS::BIAS>();
    removeStatus<PS::CONFUSED>();
    removeStatus<PS::CONSTRICTED>();

    if (hasStatus<PS::DRAW_REDUCTION>()) {
        ++cardDrawPerTurn;
        removeStatus<PS::DRAW_REDUCTION>();
    }

    removeStatus<PS::ENTANGLED>();
    removeStatus<PS::FASTING>();
    removeStatus<PS::FRAIL>();
    removeStatus<PS::HEX>();
    removeStatus<PS::LOSE_DEXTERITY>();
    removeStatus<PS::LOSE_STRENGTH>();
    removeStatus<PS::NO_BLOCK>();
    removeStatus<PS::NO_DRAW>();
    removeStatus<PS::VULNERABLE>();
    removeStatus<PS::WEAK>();
    removeStatus<PS::WRAITH_FORM>();
}

void Player::increaseMaxHp(int amount) {
    maxHp += amount;
    heal(amount);
}

void Player::heal(int amount) {
    if (hasRelic<RelicId::MARK_OF_THE_BLOOM>()) {
        return;
    }

    if (hasRelic<RelicId::MAGIC_FLOWER>()) {
        amount = amount * 3 / 2;
    }

    bool wasBloodied = curHp <= maxHp/2;

    curHp = std::min(static_cast<int>(maxHp), curHp + amount);

    if (wasBloodied && curHp > maxHp/2 && hasRelic<RelicId::RED_SKULL>()) {
        debuff<PS::STRENGTH>(-3);
    }
}

void Player::damage(BattleContext &bc, const int calculatedDamage, const bool selfDamage) {
#ifdef sts_asserts
    assert(calculatedDamage >= 0);
#endif

    int damage = calculatedDamage;

    if (damage > 0 && hasStatus<PS::INTANGIBLE>()) {
        damage = 1;
    }

    const auto savedBlock = block;
    block = std::max(0, block-damage);
    damage -= savedBlock;



    damage -= block;
    if (damage > 0) {
        block = 0;
    }

    if (damage > 0 && hasStatus<PS::BUFFER>()) {
        decrementStatus<PS::BUFFER>();
        damage = 0;
    }

    if (damage > 0 && hasRelic<RelicId::TUNGSTEN_ROD>()) {
        damage -= 1;
    }

    if (damage > 0) {
        if (damage <= 0) {
            std::cerr << "tried to lose less than 0 hp in damage" << std::endl;
            std::cerr << bc << std::endl;
        }
        hpWasLost(bc, damage, selfDamage);
    }
}

void Player::attacked(BattleContext &bc, int enemyIdx, int calculatedDamage) {
    assert(calculatedDamage >= 0);

    // assume intangible is already handled

    int damage = calculatedDamage;
    const auto savedBlock = block;
    block = std::max(0, block-damage);
    damage -= savedBlock;

    // buffer triggers before tungsten rod in the game's implementation
    // cases where tungsten rod would prevent damage // todo check if this is true
    if (damage > 0 && hasStatus<PS::BUFFER>()) {
        decrementStatus<PS::BUFFER>();
        damage = 0;
    }

    if (hasStatus<PS::THORNS>()) {
        bc.addToTop( Actions::DamageEnemy(enemyIdx, getStatus<PS::THORNS>()) );
    }

    if (hasStatus<PS::FLAME_BARRIER>()) {
        bc.addToTop( Actions::DamageEnemy(enemyIdx, getStatus<PS::FLAME_BARRIER>()) );
    }

    if (damage > 0 && damage <=5 && hasRelic<RelicId::TORII>()) {
        damage = 1;
    }

    if (damage > 0 && hasRelic<RelicId::TUNGSTEN_ROD>()) {
        damage -= 1;
    }

    if (damage > 0) {
        lastAttackUnblockedDamage = damage;

        if (hasStatus<PS::PLATED_ARMOR>()) {
            decrementStatus<PS::PLATED_ARMOR>();
        }

        if (bc.monsters.arr[enemyIdx].hasStatus<MS::PAINFUL_STABS>()) {
            bc.addToBot( Actions::MakeTempCardInDiscard({CardId::WOUND}) );
        }

        // Static Discharge only triggers on unblocked attack damage
        if (hasStatus<PS::STATIC_DISCHARGE>()) {
            bc.addToTop( Actions::ChannelOrb(Orb::LIGHTNING, getStatus<PS::STATIC_DISCHARGE>()) );
        }

        if (damage <= 0) {
            std::cerr << "tried to lose less than 0 hp in attacked" << std::endl;
            std::cerr << bc << std::endl;
        }
        hpWasLost(bc, damage, false);

    } else {
        lastAttackUnblockedDamage = 0;
    }
}

void Player::loseHp(BattleContext &bc, int amount, bool selfDamage) {
    if (hasStatus<PS::INTANGIBLE>()) {
        amount = 1;
    }

    if (amount > 0 && hasStatus<PS::BUFFER>()) {
        decrementStatus<PS::BUFFER>();
        return;
    }

    if (amount > 0 && hasRelic<RelicId::TUNGSTEN_ROD>()) {
        amount -= 1;
        if (amount == 0) {
            return;
        }
    }

    if (amount <= 0) {
        std::cerr << "tried to lose less than 0 hp in loseHp" << std::endl;
        std::cerr << bc << std::endl;
    }
    hpWasLost(bc, amount, selfDamage);
}

void Player::hpWasLost(BattleContext &bc, int amount, bool selfDamage) {
    if (amount <= 0) {
        std::cerr << "tried to lose less than 0 hp in hpWasLost" << std::endl;
        std::cerr << bc << std::endl;
    }
    assert(amount > 0);

    // Emotion Chip listens to wasHPLost, including self-damage and direct
    // HP-loss effects, not only unblocked enemy attacks.
    if (hasRelic<RelicId::EMOTION_CHIP>()) {
        lastDamageTaken += amount;
    }

    bool wasBloodied = curHp <= maxHp/2;

    curHp = std::max(0, curHp-amount);

    if (selfDamage && hasStatus<PS::RUPTURE>()) {
        buff<PS::STRENGTH>(getStatus<PS::RUPTURE>());
    }

    // todo - does order acquired matter with centennial/runic?
    // relics wasHpLost
    // -centennial-puzzle
    // -emotion chip
    // -runic cube
    // -self forming clay

    if (hasRelic<RelicId::CENTENNIAL_PUZZLE>()) {
        setHasRelic<RelicId::CENTENNIAL_PUZZLE>(false);
        bc.addToTop( Actions::DrawCards(3) );
    }

    if (hasRelic<RelicId::SELF_FORMING_CLAY>()) {
        buff<PS::NEXT_TURN_BLOCK>(3);
    }

    if (hasRelic<RelicId::RUNIC_CUBE>()) {
        bc.addToTop( Actions::DrawCards(1) );
    }

    if (hasRelic<RelicId::RED_SKULL>() && !wasBloodied && curHp <= maxHp/2) {
        buff<PS::STRENGTH>(3);
    }

    bc.cards.onTookDamage();
    ++timesDamagedThisCombat;

    if (curHp <= 0) {
        wouldDie(bc);
    }
}

void Player::wouldDie(BattleContext &bc) {
    // assume fairy and lizard tail heal for greater than zero - max hp is not less than ~8
    curHp = 0;
    if  (!hasRelic<RelicId::MARK_OF_THE_BLOOM>()) {
        for (int i = 0; i < bc.potionCapacity; i++) {
            if (bc.potions[i] == Potion::FAIRY_POTION) {
                bc.discardPotion(i);
                const int healAmount = std::max(
                        1,
                        static_cast<int>(static_cast<float>(maxHp) * (hasRelic<R::SACRED_BARK>() ? 0.6f : 0.3f))
                );
                heal(healAmount);
                return;
            }
        }

        if (hasRelic<RelicId::LIZARD_TAIL>()) {
            setHasRelic<RelicId::LIZARD_TAIL>(false);
            heal(maxHp/2);
            return;
        }
    }

    bc.outcome = Outcome::PLAYER_LOSS;
}

void Player::applyEndOfTurnPowers(BattleContext &bc) {
    if (bomb1) {
        bc.addToBot( Actions::DamageAllEnemy(bomb1) );
    }
    bomb1 = bomb2;
    bomb2 = bomb3;
    bomb3 = 0;

    for (auto pair : statusMap) {
        if (!hasStatusRuntime(pair.first)) {
            continue;
        }

        switch (pair.first) {
            case PS::AMPLIFY:
                bc.addToBot(Actions::RemoveStatus<PS::AMPLIFY>());
                break;

            case PS::BURST:
                bc.addToBot(Actions::RemoveStatus<PS::BURST>());
                break;

            case PS::COMBUST:
                if (!bc.monsters.areMonstersBasicallyDead()) {
                    bc.addToBot(Actions::PlayerLoseHp(combustHpLoss, true)); // todo combust doesnt stack hp loss correctly
                    bc.addToBot(Actions::DamageAllEnemy(pair.second));
                }
                break;

            case PS::CONSTRICTED:
                bc.addToBot(Actions::DamagePlayer(pair.second));
                break;

            case PS::DOUBLE_TAP:
                bc.addToBot(Actions::RemoveStatus<PS::DOUBLE_TAP>());
                break;

            case PS::ENTANGLED:
                bc.addToBot(Actions::RemoveStatus<PS::ENTANGLED>());
                break;

            case PS::EQUILIBRIUM:
                // todo if card is ethereal set to retain
                break;

            case PS::ESTABLISHMENT:
                // todo addToBot establishmentPowerAction
                break;

            case PS::LOSE_DEXTERITY:
                bc.addToBot(Actions::DebuffPlayer<PS::DEXTERITY>(-pair.second));
                bc.addToBot(Actions::RemoveStatus<PS::LOSE_DEXTERITY>());
                break;

            case PS::LOSE_STRENGTH:
                bc.addToBot(Actions::DebuffPlayer<PS::STRENGTH>(-pair.second));
                bc.addToBot(Actions::RemoveStatus<PS::LOSE_STRENGTH>());
                break;

            case PS::NO_DRAW:
                bc.addToBot(Actions::RemoveStatus<PS::NO_DRAW>());
                break;

            case PS::OMEGA:
                bc.addToBot(Actions::DamageAllEnemy(pair.second));
                break;

            case PS::RAGE:
                removeStatus<PS::RAGE>();
                break;

            case PS::REBOUND:
                bc.addToBot(Actions::RemoveStatus<PS::REBOUND>());
                break;

            case PS::REGEN:
                bc.addToTop(Actions::HealPlayer(pair.second));
                bc.addToTop(Actions::DecrementStatus<PS::REGEN>());
                break;

                //case RetainCardPower -> if not has relic runic pyramid and not has power equilibrium, addToBot retain cards action

            case PS::RITUAL:
                bc.addToBot(Actions::BuffPlayer<PS::STRENGTH>(pair.second));
                break;
                // case TheBomb

            case PS::WRAITH_FORM: // todo does this debuff or just decrement?
                bc.addToBot(Actions::DecrementStatus<PS::DEXTERITY>(pair.second));
                break;

            default:
                break;
        }
    }
}

void Player::applyAtEndOfRoundPowers() {
    if (hasStatus<PS::FRAIL>()) {
        decrementIfNotJustApplied<PS::FRAIL>();
    }

    if (hasStatus<PS::VULNERABLE>()) {
        decrementIfNotJustApplied<PS::VULNERABLE>();
    }

    if (hasStatus<PS::WEAK>()) {
        decrementIfNotJustApplied<PS::WEAK>();
    }

    // handling this later so it is not removed before block check
//    if (hasStatusInternal<PS::BLUR>()) {
//        decrementStatus<PS::BLUR>();
//    }

    if (hasStatus<PS::DOUBLE_DAMAGE>()) {
        decrementIfNotJustApplied<PS::DOUBLE_DAMAGE>();
    }

    // handle elsewhere
//    if (hasStatusInternal<PS::DRAW_REDUCTION>()) {
//        decrementStatus<PS::DRAW_REDUCTION>();
//    }

    if (hasStatus<PS::DUPLICATION>()) {
        decrementStatus<PS::DUPLICATION>();
    }

    if (hasStatus<PS::EQUILIBRIUM>()) {
        decrementStatus<PS::EQUILIBRIUM>();
    }

    if (hasStatus<PS::INTANGIBLE>()) {
        decrementStatus<PS::INTANGIBLE>();
    }

    if (hasStatus<PS::NO_BLOCK>()) {
        decrementStatus<PS::NO_BLOCK>();
    }

    if (hasStatus<PS::WAVE_OF_THE_HAND>()) {
        decrementStatus<PS::WAVE_OF_THE_HAND>();
    }
}

void Player::applyStartOfTurnRelics(BattleContext &bc) {
    //****** Player relics atTurnStart ******
    if (hasRelic<R::ART_OF_WAR>()) {
        if (attacksPlayedThisTurn == 0) {
            bc.addToBot(Actions::GainEnergy(1));
        }
    }

    if (hasRelic<R::BRIMSTONE>()) {
        buff<PS::STRENGTH>(2);
        for (int i = 0; i < bc.monsters.monsterCount; i++) {
            if (bc.monsters.arr[i].isTargetable()) {
                bc.monsters.arr[i].buff<MS::STRENGTH>(1);
            }
        }
    }

    if (hasRelic<R::CAPTAINS_WHEEL>()) {
        if (bc.turn == 2) {
            bc.addToBot( Actions::GainBlock(18) );
        }
    }

    if (hasRelic<R::DAMARU>()) {
        bc.addToBot( Actions::BuffPlayer<PS::MANTRA>(1) );
        // todo handle mantra change stance
    }

    if (hasRelic<R::EMOTION_CHIP>()) {
        // if we lost HP last turn, all orbs trigger their passive at the
        // start of this turn
        if (lastDamageTaken > 0) {
            const int orbCount = bc.player.getOrbCount();
            for (int i = 0; i < orbCount; ++i) {
                bc.addToBot(Actions::TriggerOrbPassive(i, 1));
            }
        }
        lastDamageTaken = 0;
    }

    if (hasRelic<R::HAPPY_FLOWER>()) {
        if (++happyFlowerCounter == 3) {
            happyFlowerCounter = 0;
            bc.addToBot( Actions::GainEnergy(1) );
        }
    }

    if (hasRelic<R::HORN_CLEAT>()) {
        if (bc.turn == 1) {
            bc.addToBot( Actions::GainBlock(14) );
        }
    }

    if (hasRelic<R::INCENSE_BURNER>()) {
        if (++incenseBurnerCounter == 6) {
            incenseBurnerCounter = 0;
            bc.addToBot( Actions::BuffPlayer<PS::INTANGIBLE>(1) );
        }
    }

    if (hasRelic<R::INSERTER>()) {
        if (++inserterCounter == 2) {
            inserterCounter = 0; // todo
            bc.addToBot( {[=](BattleContext &bc) {
                bc.player.increaseOrbSlots(1);
            }});
        }
    }

    if (hasRelic<R::MERCURY_HOURGLASS>()) {
        bc.addToBot( Actions::DamageAllEnemy(3) );
    }

    if (hasRelic<R::NECRONOMICON>()) {
        haveUsedNecronomiconThisTurn = false;
    }

    if (hasRelic<R::ORANGE_PELLETS>()) {
        orangePelletsCardTypesPlayed.reset();
    }

}

void Player::applyStartOfTurnPowers(BattleContext &bc) {
    // ****** Player powers atStartOfTurn ******
    for (auto pair : statusMap) {
        if (!hasStatusRuntime(pair.first)) {
            continue;
        }

        switch (pair.first) {
            case PS::BATTLE_HYMN:
                bc.addToBot(Actions::MakeTempCardInHand(CardId::SMITE, hasStatus<PS::MASTER_REALITY>(), pair.second) );
                break;

            case PS::BIAS:
                bc.addToBot( Actions::DecrementStatus<PS::FOCUS>(pair.second) );
                break;

            case PS::CREATIVE_AI: {
                // add a random Power card into the hand for each stack
                for (int i = 0; i < pair.second; ++i) {
                    bc.addToBot(Actions::MakeTempCardInHand(
                        getTrulyRandomCardInCombat(bc.cardRandomRng, bc.player.cc, CardType::POWER)
                    ));
                }
                break;
            }

            case PS::ECHO_FORM:
                echoFormCardsDoubled = 0;
                break;

            case PS::BLASPHEMER:
                removeStatus<PS::BLASPHEMER>();
                bc.addToBot( Actions::PlayerLoseHp(9999) );
                break;

            case PS::FASTING:
                //  add energy dont need? just change energy per turn instead
                break;

            case PS::FORESIGHT:
                if (bc.cards.drawPile.empty()) {
                    bc.addToTop( Actions::SetState(InputState::SHUFFLE_DISCARD_TO_DRAW) );
                }
//                bc.addToBot( Actions::SetState(InputState::SCRY, pair.second) ); // tood
                break;

            case PS::FLAME_BARRIER:
                removeStatus<PS::FLAME_BARRIER>();
                break;

            case PS::HELLO_WORLD: {
                // Each stack independently creates a random Common card.
                for (int i = 0; i < pair.second; ++i) {
                    const auto cardId = getRandomClassCardOfRarity(
                        bc.cardRandomRng,
                        bc.player.cc,
                        CardRarity::COMMON
                    );
                    bc.addToBot(Actions::MakeTempCardInHand(cardId));
                }
                break;
            }

            case PS::INFINITE_BLADES:
                bc.addToBot(Actions::MakeTempCardInHand(CardId::SHIV, hasStatus<PS::MASTER_REALITY>(), pair.second) );
                break;

            case PS::LOOP: {
                // At the start of the turn the leftmost orb (your next Orb)
                // triggers its passive, once per stack of Loop.
                const int firstIdx = bc.player.getFirstOrbIdx();
                if (firstIdx >= 0) {
                    // 必须**立即**触发, 不能 addToBot 一个 TriggerOrbPassive 包装动作:
                    // 包装动作执行时才 addToBot(OrbLightningDamageRandom), 那次会排到
                    // afterMonsterTurns() 里的 DrawCards 之后, 导致闪电球的随机目标
                    // 比真机晚消耗一次 cardRandomRng —— 在 Snecko Eye(混乱) 下直接表现为
                    // 整手牌费用相对真机右移一位。真机 LoopPower.atStartOfTurn() 是在
                    // DrawCardAction 入队前就把 DamageRandomEnemyAction 排进去的。
                    for (int i = 0; i < pair.second; ++i) {
                        bc.player.triggerOrbPassive(bc, firstIdx);
                    }
                }
                break;
            }

            case PS::MAGNETISM:
//                bc.addToBot( Actions::SetState(InputState::CREATE_RANDOM_CARD_IN_HAND_COLORLESS, pair.second) );
                break;

            case PS::MAYHEM:
                for (int i = 0; i < pair.second; i++) {
                    bc.addToBot( Actions::PlayTopCard(bc.monsters.getRandomMonsterIdx(bc.cardRandomRng), false) ); // todo fix target
                }
                break;

            case PS::NEXT_TURN_BLOCK:
                bc.addToBot( Actions::GainBlock(pair.second) );
                removeStatus<PS::NEXT_TURN_BLOCK>();
                break;

                // todo  case NightMarePower: this.bc.addToBot(new MakeTempCardInHandAction(this.card, this.amount)); bc.addToBot remove power

            case PS::PANACHE:
                panacheCounter = 5;
                break;

            case PS::PHANTASMAL:
                decrementStatus<PS::PHANTASMAL>();
                bc.addToBot( Actions::BuffPlayer<PS::DOUBLE_DAMAGE>() );
                break;

                // time maze not used in standard modes

            case PS::WRATH_NEXT_TURN:
                removeStatus<PS::WRATH_NEXT_TURN>();
                bc.addToBot( Actions::ChangeStance(Stance::WRATH) );
                break;

            default:
                break;
        }

    }
}

void Player::applyStartOfTurnPostDrawRelics(BattleContext &bc) {
    // ****** Player Relics AtTurnStartPostDraw ******
    if (hasRelic<R::POCKETWATCH>()) {
        if (cardsPlayedThisTurn <= 3) {
            bc.addToBot(Actions::DrawCards(3));
        }
    }

    if (hasRelic<R::WARPED_TONGS>()) {
        bc.addToBot(Actions::UpgradeRandomCardAction());
    }
}

void Player::applyStartOfTurnPostDrawPowers(BattleContext &bc) {
    // ****** Player Powers AtStartOfTurnPostDraw ******
    for (auto pair :statusMap) {
        if (!hasStatusRuntime(pair.first)) {
            continue;
        }

        switch (pair.first) {
            case PS::BRUTALITY:
                bc.addToBot( Actions::DrawCards(pair.second) );
                bc.addToBot( Actions::PlayerLoseHp(pair.second, true) );
                break;

            case PS::DEMON_FORM:
                bc.addToBot( Actions::BuffPlayer<PS::STRENGTH>(pair.second) );
                break;

            case PS::DEVOTION: // the implementation of this is really weird in the game code
                bc.addToBot( Actions::BuffPlayer<PS::MANTRA>(pair.second) ); // todo make buffing mantra switch stance
                break;

            case PS::DRAW_CARD_NEXT_TURN:
                bc.addToBot( Actions::DrawCards(pair.second) );
                removeStatus<PS::DRAW_CARD_NEXT_TURN>();
                break;

            case PS::NOXIOUS_FUMES:
                bc.addToBot( Actions::DebuffAllEnemy<MS::POISON>(pair.second) );
                break;

            case PS::MACHINE_LEARNING:
                bc.addToBot( Actions::DrawCards(pair.second) );
                break;

            case PS::TOOLS_OF_THE_TRADE:
                bc.addToBot( Actions::DrawCards(pair.second) );
//                bc.addToBot( Actions::SetState(InputState::CHOOSE_DISCARD_CARDS, pair.second) );
                break;

            default:
                break;
        }
    }
}

void Player::rechargeEnergy(BattleContext &bc) {
    if (hasRelic<R::ICE_CREAM>()) {
        gainEnergy(energyPerTurn);
    } else {
        energy = energyPerTurn;
    }

    // ****** Player powers onEnergyRecharge ******
    if (hasStatus<PS::COLLECT>()) {
        decrementStatus<PS::COLLECT>();
        bc.addToBot(Actions::MakeTempCardInHand(CardId::MIRACLE) );
    }

    if (hasStatus<PS::DEVA>()) {
        gainEnergy(devaFormEnergyPerTurn);
        devaFormEnergyPerTurn += getStatus<PS::DEVA>();
    }

    if (hasStatus<PS::ENERGIZED>()) {
        gainEnergy(getStatus<PS::ENERGIZED>());
        removeStatus<PS::ENERGIZED>();
    }
}

std::ostream& printIfHaveStatus(const Player &p, std::ostream &os, PlayerStatus s) {
    if (!p.hasStatusRuntime(s)){
        return os;
    }

    std::string desc = playerStatusStrings[(int)s];
    return os << "(" << desc << "," << p.getStatusRuntime(s) << ")" << ", ";
}

namespace sts {

//    void printRelicCounters(std::ostream &os, const Player &p) {
//        const std::string s = " ";
//
//        if (p.hasRelic<R::INK_BOTTLE>()) {
//            os << s << "inkBottleCounter: " << p.inkBottleCounter;
//        }
//
//        if (p.hasRelic<R::INSERTER>()) {
//            os << "inserterCounter: " << p.inserterCounter << " ";
//        }
//
//        if (p.hasRelic<R::NUNCHAKU>()) {
//            os << "nunchakuCounter: " << p.nunchakuCounter << " ";
//        }
//
//        if (p.hasRelic<R::LETTER_OPENER>()) {
//            os << "skillsPlayedThisTurn: " << p.skillsPlayedThisTurn << " ";
//        }
//
//    }
//
//    void printStatusCounters(std::ostream &os, const Player &p) {
//
//        if (p.hasStatusInternal<PS::COMBUST>()) {
//            os << "combustHpLoss: " << p.combustHpLoss << " ";
//        }
//
//        if (p.hasStatusInternal<PS::DEVA>()) {
//            os << "devaFormEnergyPerTurn: " << p.devaFormEnergyPerTurn << " ";
//        }
//
//        if (p.hasStatusInternal<PS::ECHO_FORM>()) {
//            os << "panacheCounter: " << p.panacheCounter << " ";
//        }
//
//
//        if (p.hasStatusInternal<PS::PANACHE>()) {
//            os << "panacheCounter: " << p.panacheCounter << " ";
//        }
//    }

    void printRelics(std::ostream &os, const Player &p) {
        os << "\t" << "Relics: ";

        bool firstRelic = true;
        for (int i = static_cast<int>(R::AKABEKO); i < static_cast<int>(R::INVALID); ++i) {
            const auto r = static_cast<RelicId>(i);
            if (!p.hasRelicRuntime(r)) {
                continue;
            }

            if (!firstRelic) {
                os << ", ";
            }
            firstRelic = false;
            os << getRelicName(r);
        }
        os << "\n";
    }

    void printAllInfos(std::ostream &os, const Player &p) {
        const std::string s = ", ";

        os << "\t" << "Relic Counters: "
            << "" << "happyFlowerCounter: " << static_cast<int>(p.happyFlowerCounter)
            << s << "incenseBurnerCounter: " << static_cast<int>(p.incenseBurnerCounter)
            << s << "inkBottleCounter: " << static_cast<int>(p.inkBottleCounter)
            << s << "inserterCounter: " << static_cast<int>(p.inserterCounter)
            << s << "nunchakuCounter: " << static_cast<int>(p.nunchakuCounter)
            << s << "penNibCounter: " << static_cast<int>(p.penNibCounter)
            << s << "sundialCounter: " << static_cast<int>(p.sundialCounter)

            << "\n\t" << "Status Counters: "
            << "" << "combustHpLoss: " << static_cast<int>(p.combustHpLoss)
            << s << "devaFormEnergyPerTurn: " << static_cast<int>(p.devaFormEnergyPerTurn)
            << s << "echoFormCardsDoubled: " << static_cast<int>(p.echoFormCardsDoubled)
            << s << "panacheCounter: " << static_cast<int>(p.panacheCounter)
            << s << "TheBomb: { 1=" << static_cast<int>(p.bomb1)
                << ", 2=" << static_cast<int>(p.bomb2)
                << ", 3=" << static_cast<int>(p.bomb3)
                << " }"

            << "\n\t" << "Misc: "
            << "" << "cardsPlayedThisTurn: " << static_cast<int>(p.cardsPlayedThisTurn)
            << s << "attacksPlayedThisTurn: " << static_cast<int>(p.attacksPlayedThisTurn)
            << s << "skillsPlayedThisTurn: " << static_cast<int>(p.skillsPlayedThisTurn);


            os << s << "orangePelletsCardTypesPlayed: {"
                << (p.orangePelletsCardTypesPlayed.test(0) ? "Attack," : "")
                << (p.orangePelletsCardTypesPlayed.test(1) ? "Skill," : "")
                << (p.orangePelletsCardTypesPlayed.test(2) ? "Power," : "")
                << "}";

            os << s << "cardsDiscardedThisTurn: " << static_cast<int>(p.cardsDiscardedThisTurn);
            os << s << "gold: " << static_cast<int>(p.gold) << '\n';
    }


    void printStatusEffects(std::ostream &os, const Player &p) {
        os << "\tStatusEffects: {";
        if (p.hasStatus<PS::CONFUSED>()) {
            os << "(Confused), ";
        }
        if (p.hasStatus<PS::CORRUPTION>()) {
            os << "(Corruption), ";
        }
        if (p.hasStatus<PS::BARRICADE>()) {
            os << "(Barricade), ";
        }

        printIfHaveStatus(p, os, PS::ARTIFACT);
        printIfHaveStatus(p, os, PS::DEXTERITY);
        printIfHaveStatus(p, os, PS::FOCUS);
        printIfHaveStatus(p, os, PS::STRENGTH);
        for (auto pair : p.statusMap) {
            printIfHaveStatus(p, os, pair.first);
        }
        os << "}\n";
    }

    std::ostream& operator<<(std::ostream &os, const Player &p) {
        os << "Player: {\n";

        os << "\t" << "hp:(" << p.curHp << "/" << p.maxHp << ")"
            << " energy:(" << p.energy << "/" << static_cast<int>(p.energyPerTurn)
            << ") block:(" << p.block << ")\n";

        os << "\t" << "Orbs: (slots " << static_cast<int>(p.orbSlots) << ") [";
        const int orbCount = p.getOrbCount();
        for (int i = 0; i < p.orbSlots; ++i) {
            if (i > 0) {
                os << ", ";
            }
            switch (p.orbTypes[i]) {
                case Orb::LIGHTNING:
                    os << "L";
                    break;
                case Orb::FROST:
                    os << "F";
                    break;
                case Orb::DARK:
                    os << "D(" << p.orbData[i] << ")";
                    break;
                case Orb::PLASMA:
                    os << "P";
                    break;
                default:
                    os << "-";
                    break;
            }
        }
        os << "] (" << orbCount << " channeled, L:" << p.lightningChanneled
           << " F:" << p.frostChanneled << ")\n";

        printStatusEffects(os, p);
        printRelics(os, p);
        printAllInfos(os, p);

        os << "}\n";
        return os;
    }



}
