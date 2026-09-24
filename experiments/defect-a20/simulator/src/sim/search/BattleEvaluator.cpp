#include "sim/search/BattleEvaluator.h"

#include <algorithm>

using namespace sts;

namespace {

    // HP inflation for splitting enemies: the total HP of the split-off parts
    // counts against the player, so a slime boss at 80/140 must not look
    // better than two large slimes at 60/60 (120 total).
    double getMonsterHpScale(const Monster &m) {
        switch (m.id) {
            case MonsterId::SLIME_BOSS:
                return 4.0;
            case MonsterId::ACID_SLIME_L:
            case MonsterId::SPIKE_SLIME_L:
                return 2.0;
            default:
                return 1.0;
        }
    }

    int getReaperHealingReserve(const BattleContext &bc) {
        int reserve = 0;
        // One-shot attack buffs belong to the current tactical choice.  Do not
        // count them as durable Reaper reserve, otherwise preserving Akabeko
        // can appear safer than using it on a stronger multi-hit attack.  A
        // lowered input base cancels calculateCardDamage's Vigor addition
        // without copying the whole battle state on every playout.
        const int vigor = bc.player.getStatus<PS::VIGOR>();
        const auto accountForReaper = [&bc, &reserve, vigor] (
            const CardInstance &card
        ) {
            if (card.getId() != CardId::REAPER) {
                return;
            }

            int healing = 0;
            const int baseDamage = (card.isUpgraded() ? 5 : 4) - vigor;
            for (int i = 0; i < bc.monsters.monsterCount; ++i) {
                const auto &monster = bc.monsters.arr[i];
                if (monster.isDeadOrEscaped()) {
                    continue;
                }
                const int damage = bc.calculateCardDamage(card, i, baseDamage);
                healing += std::min(
                    monster.curHp,
                    std::max(0, damage - monster.block)
                );
            }
            reserve += healing;
        };

        for (int i = 0; i < bc.cards.cardsInHand; ++i) {
            accountForReaper(bc.cards.hand[i]);
        }
        for (const auto &card : bc.cards.drawPile) {
            accountForReaper(card);
        }
        for (const auto &card : bc.cards.discardPile) {
            accountForReaper(card);
        }

        // This is option value, not current HP, so it may extend effective HP
        // above max HP.  Bound it to one health bar to prevent duplicated or
        // generated Reapers from dominating the generic survival fallback.
        return std::min(reserve, std::max(0, bc.player.maxHp));
    }

    double getPlayerScalingDeltaScore(const BattleContext &before, const BattleContext &after) {
        const auto &bp = before.player;
        const auto &ap = after.player;
        return static_cast<double>(ap.strength - bp.strength)
               + static_cast<double>(ap.dexterity - bp.dexterity)
               + static_cast<double>(ap.focus - bp.focus)
               + (0.5 * static_cast<double>(ap.artifact - bp.artifact))
               + (1.25 * static_cast<double>(ap.energyPerTurn - bp.energyPerTurn))
               + (0.75 * static_cast<double>(ap.devaFormEnergyPerTurn - bp.devaFormEnergyPerTurn));
    }

    double getRecoveryScalingDeltaScore(const BattleContext &before, const BattleContext &after) {
        return getPlayerScalingDeltaScore(before, after)
            + 0.75 * (after.player.getRecurringDrawPerTurn()
                      - before.player.getRecurringDrawPerTurn())
            + (after.player.getRecurringCardCopiesPerTurn()
               - before.player.getRecurringCardCopiesPerTurn())
            + 0.5 * (after.player.orbSlots - before.player.orbSlots);
    }

    double getEnemyDebuffDeltaScore(const BattleContext &before, const BattleContext &after) {
        double score = 0.0;
        for (int i = 0; i < before.monsters.monsterCount; ++i) {
            const auto &bm = before.monsters.arr[i];
            const auto &am = after.monsters.arr[i];
            score += 0.5 * static_cast<double>(am.vulnerable - bm.vulnerable);
            score += 0.7 * static_cast<double>(am.weak - bm.weak);
            score += 0.5 * static_cast<double>(bm.strength - am.strength);
        }
        return score;
    }

    int getProjectedIncomingHpLoss(const BattleContext &bc) {
        int incomingDamage = 0;
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            const auto &m = bc.monsters.arr[i];
            if (m.isDeadOrEscaped() || m.isHalfDead()) {
                continue;
            }

            auto damageInfo = m.getMoveBaseDamage(bc);
            if (damageInfo.attackCount <= 0 || damageInfo.damage <= 0) {
                continue;
            }

            damageInfo.damage = m.calculateDamageToPlayer(bc, damageInfo.damage);
            incomingDamage += damageInfo.damage * damageInfo.attackCount;
        }

        return std::max(0, incomingDamage - bc.player.block);
    }

    double getNonMinionMonsterBlockTotal(const BattleContext &bc) {
        int blockTotal = 0;

        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            const auto &m = bc.monsters.arr[i];
            if (!m.hasStatus<MS::MINION>() && m.id != sts::MonsterId::INVALID) {
                blockTotal += m.block * getMonsterHpScale(m);
            }
        }

        return blockTotal;
    }

    double getNonMinionMonsterMaxHpTotal(const BattleContext &bc) {
        int maxHpTotal = 0;

        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            const auto &m = bc.monsters.arr[i];
            if (!m.hasStatus<MS::MINION>() && m.id != sts::MonsterId::INVALID) {
                maxHpTotal += m.maxHp * getMonsterHpScale(m);
                if (m.id == sts::MonsterId::AWAKENED_ONE) {
                    maxHpTotal += m.maxHp * getMonsterHpScale(m);
                }
            }
        }

        return maxHpTotal;
    }

    double getMinionMonsterCurHpTotal(const BattleContext &bc) {
        int curHpTotal = 0;
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            const auto &m = bc.monsters.arr[i];
            if (m.hasStatus<MS::MINION>() && m.id != MonsterId::INVALID) {
                curHpTotal += m.curHp * getMonsterHpScale(m);
            }
        }
        return curHpTotal;
    }

    double getMinionMonsterMaxHpTotal(const BattleContext &bc) {
        int maxHpTotal = 0;
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            const auto &m = bc.monsters.arr[i];
            if (m.hasStatus<MS::MINION>() && m.id != MonsterId::INVALID) {
                maxHpTotal += m.maxHp * getMonsterHpScale(m);
            }
        }
        return maxHpTotal;
    }

    int getLivingMinionMonsterCount(const BattleContext &bc) {
        int count = 0;
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            const auto &m = bc.monsters.arr[i];
            if (m.hasStatus<MS::MINION>() && m.id != MonsterId::INVALID && m.curHp > 0
                && !m.isEscapingB) {
                ++count;
            }
        }
        return count;
    }

    double getRecoveryActiveEngineScore(const BattleContext &bc) {
        const auto &p = bc.player;
        return
            2.0 * p.getStatus<PS::INTANGIBLE>()
            + 1.5 * p.getStatus<PS::BUFFER>()
            + (p.hasStatus<PS::BARRICADE>() ? 1.25 : 0.0)
            + (p.hasStatus<PS::CORRUPTION>() ? 1.25 : 0.0)
            + 1.0 * p.getStatus<PS::DARK_EMBRACE>()
            + 1.0 * p.getStatus<PS::FEEL_NO_PAIN>()
            // Status-driven draw is durable engine value even when its first
            // payoff lies beyond the short recovery horizon.  Without this,
            // Evolve looked identical to a generic unplayed Power in long
            // status-heavy fights such as Hexaghost.
            + 0.75 * p.getStatus<PS::EVOLVE>()
            + 0.75 * p.getStatus<PS::DEMON_FORM>()
            + 0.5 * p.getStatus<PS::METALLICIZE>()
            + 0.5 * p.getStatus<PS::PLATED_ARMOR>()
            + 0.5 * p.getStatus<PS::RITUAL>();
    }

    double getRecoveryCardOptionScore(const BattleContext &bc) {
        double score = 0.0;
        const auto accountForCard = [&score] (const CardInstance &card) {
            // Preserve future setup that the short recovery horizon has not
            // reached yet. Active effects must contribute more than this
            // reserve through their generic player-state capabilities.
            if (card.getType() == CardType::POWER) {
                score += 0.35;
            }
            switch (card.getId()) {
                case CardId::APPARITION:
                    score += 0.75;
                    break;
                case CardId::IMPERVIOUS:
                case CardId::OFFERING:
                    score += 0.5;
                    break;
                case CardId::FEED:
                case CardId::LESSON_LEARNED:
                case CardId::RITUAL_DAGGER:
                    score += 0.35;
                    break;
                default:
                    break;
            }
        };
        for (int i = 0; i < bc.cards.cardsInHand; ++i) {
            accountForCard(bc.cards.hand[i]);
        }
        for (const auto &card : bc.cards.drawPile) {
            accountForCard(card);
        }
        for (const auto &card : bc.cards.discardPile) {
            accountForCard(card);
        }
        return score;
    }

    double getRecoverySetupScore(const BattleContext &bc) {
        return getRecoveryActiveEngineScore(bc)
            + getRecoveryCardOptionScore(bc);
    }

    struct HeuristicWeights {
        double hp = 5.0;
        double block = 0.5;
        double energy = 0.45;
        double enemyHp = 1.0;
        double enemyBlock = 0.4;
        double scaling = 0.65;
        double enemyDebuff = 0.5;
    };

    HeuristicWeights getHeuristicWeightsForIntent(search::SearchIntent intent) {
        switch (intent) {
            case search::SearchIntent::AGGRESSIVE:
                return {5.0, 0.2, 0.25, 2.4, 0.7, 0.35, 0.35};

            case search::SearchIntent::SCALING_FIRST:
                return {5.0, 0.4, 0.6, 0.7, 0.25, 2.1, 1.15};

            case search::SearchIntent::SURVIVAL_FIRST:
                return {6.5, 1.2, 0.9, 0.8, 0.3, 0.5, 0.6};

            default:
                return {6.5, 1.2, 0.9, 0.8, 0.3, 0.5, 0.6};
        }
    }

}

namespace sts::search {

double getLizardTailReserveHp(const BattleContext &bc) {
    return bc.player.hasRelic<R::LIZARD_TAIL>()
        ? static_cast<double>(bc.player.maxHp / 2)
        : 0.0;
}

int getEscapedStolenGold(const BattleContext &bc) {
    int stolenGold = 0;
    for (int i = 0; i < bc.monsters.monsterCount; ++i) {
        const auto &monster = bc.monsters.arr[i];
        const bool isThief = monster.id == MonsterId::LOOTER
                             || monster.id == MonsterId::MUGGER;
        if (isThief && monster.isEscapingB) {
            stolenGold += std::max(0, monster.miscInfo);
        }
    }
    return stolenGold;
}

int getEffectiveReplanHp(const BattleContext &bc) {
    return std::max(0, bc.player.curHp) + getReaperHealingReserve(bc);
}

int getPostCombatPlayerHp(const BattleContext &bc) {
    const int hp = std::max(0, bc.player.curHp);
    if (bc.player.hasRelic<R::MARK_OF_THE_BLOOM>()) {
        return hp;
    }
    return std::min(
        bc.player.maxHp,
        hp + bc.player.getStatus<PS::SELF_REPAIR>()
    );
}

double getNonMinionMonsterCurHpTotal(const BattleContext &bc) {
    int curHpTotal = 0;

    for (int i = 0; i < bc.monsters.monsterCount; ++i) {
        const auto &m = bc.monsters.arr[i];
        if (!m.hasStatus<MS::MINION>() && m.id != sts::MonsterId::INVALID) {
            curHpTotal += m.curHp * getMonsterHpScale(m);
            if (m.id == sts::MonsterId::AWAKENED_ONE && !m.miscInfo) { // is awakened one stage 1 // todo change to status
                curHpTotal += m.maxHp * getMonsterHpScale(m);
            }
        }
    }

    return curHpTotal;
}

double evaluateActionDelta(
    const BattleContext &before,
    const BattleContext &after,
    SearchIntent intent
) {
    const double hpDelta = static_cast<double>(after.player.curHp - before.player.curHp);
    const double blockDelta = static_cast<double>(after.player.block - before.player.block);
    const double energyDelta = static_cast<double>(after.player.energy - before.player.energy);
    const double enemyHpDelta = static_cast<double>(getNonMinionMonsterCurHpTotal(before)
                                                  - getNonMinionMonsterCurHpTotal(after));
    const double enemyBlockDelta = static_cast<double>(getNonMinionMonsterBlockTotal(before)
                                                     - getNonMinionMonsterBlockTotal(after));
    const double scalingDelta = getPlayerScalingDeltaScore(before, after);
    const double enemyDebuffDelta = getEnemyDebuffDeltaScore(before, after);
    const double projectedHpLossDelta = static_cast<double>(getProjectedIncomingHpLoss(before)
                                                          - getProjectedIncomingHpLoss(after));

    const auto weights = getHeuristicWeightsForIntent(intent);

    return (hpDelta * weights.hp)
           + (blockDelta * weights.block)
           + (energyDelta * weights.energy)
           + (enemyHpDelta * weights.enemyHp)
           + (enemyBlockDelta * weights.enemyBlock)
           + (scalingDelta * weights.scaling)
           + (enemyDebuffDelta * weights.enemyDebuff)
           + (projectedHpLossDelta * weights.hp);
}

double evaluateActionHeuristic(
    const BattleContext &before,
    const Action &action,
    SearchIntent intent
) {
    BattleContext after(before);
    action.execute(after);
    return evaluateActionDelta(before, after, intent);
}

double evaluateRecoveryActionDelta(
    const BattleContext &before,
    const BattleContext &after
) {
    const double hpDelta = after.player.curHp - before.player.curHp;
    const double blockDelta = after.player.block - before.player.block;
    const double energyDelta = after.player.energy - before.player.energy;
    const double handDelta = after.cards.cardsInHand
        - before.cards.cardsInHand;
    const double enemyHpDelta =
        getNonMinionMonsterCurHpTotal(before)
        - getNonMinionMonsterCurHpTotal(after);
    const double projectedHpLossDelta =
        getProjectedIncomingHpLoss(before)
        - getProjectedIncomingHpLoss(after);

    // This prior only guides expansion inside the fixed-horizon recovery
    // tree.  HP is judged at the horizon itself, so an immediate, bounded
    // setup cost must not hide generic draw/energy actions from the tree.
    // No card identity is involved.
    return (0.5 * hpDelta)
        + (0.7 * blockDelta)
        + (1.2 * energyDelta)
        + handDelta
        + (0.5 * enemyHpDelta)
        + (1.4 * getRecoveryScalingDeltaScore(before, after))
        + getEnemyDebuffDeltaScore(before, after)
        + (1.6 * projectedHpLossDelta);
}

double evaluateRecoveryActionHeuristic(
    const BattleContext &before,
    const Action &action
) {
    BattleContext after(before);
    action.execute(after);
    return evaluateRecoveryActionDelta(before, after);
}

RecoverySnapshot recoverySnapshot(
    const BattleContext &rootBc,
    const BattleContext &bc,
    int recoveryHorizonTurns
) {
    RecoverySnapshot snapshot;
    snapshot.observed = true;
    snapshot.survivedTurns = std::clamp(
        bc.turn - rootBc.turn,
        0,
        std::max(1, recoveryHorizonTurns)
    );
    snapshot.reachedHorizon = bc.outcome == Outcome::PLAYER_VICTORY
        || (
            bc.outcome == Outcome::UNDECIDED
            && bc.inputState == InputState::PLAYER_NORMAL
            && snapshot.survivedTurns >= recoveryHorizonTurns
        );
    snapshot.hp = bc.outcome == Outcome::PLAYER_VICTORY
        ? getPostCombatPlayerHp(bc)
        : std::max(0, bc.player.curHp);
    snapshot.effectiveHp = bc.outcome == Outcome::PLAYER_VICTORY
        ? snapshot.hp
        : getEffectiveReplanHp(bc);
    snapshot.potionCount = bc.potionCount;
    snapshot.engineScore = getRecoveryScalingDeltaScore(rootBc, bc)
        + getEnemyDebuffDeltaScore(rootBc, bc)
        + getRecoverySetupScore(bc)
        - getRecoverySetupScore(rootBc)
        + static_cast<double>(std::max(0, bc.player.block))
            / static_cast<double>(std::max(1, rootBc.player.maxHp));
    snapshot.enemyProgress = std::clamp(
        1.0 + evaluateEndState(rootBc, bc),
        0.0,
        1.0
    );
    const double maxHp = static_cast<double>(
        std::max(1, rootBc.player.maxHp)
    );
    snapshot.quality =
        1.25 * std::clamp(
            static_cast<double>(snapshot.effectiveHp) / maxHp,
            0.0,
            2.0
        )
        + 2.0 * snapshot.enemyProgress
        + 0.25 * std::clamp(snapshot.engineScore, -4.0, 4.0)
        + 0.02 * static_cast<double>(snapshot.potionCount);
    return snapshot;
}

double evaluateEndState(const BattleContext &rootBc, const BattleContext &bc) {
    if (bc.outcome == Outcome::PLAYER_VICTORY) {
        // UCT assumes a bounded reward. Dividing by the HP at the decision point
        // can produce values far above 1 after healing or Lizard Tail, causing an
        // early lucky branch to dominate exploration. Max HP keeps the objective
        // monotonic in remaining HP and the reward in [0, 1].
        const auto rootMaxHp = std::max(1, rootBc.player.maxHp);
        // Lizard Tail is a one-shot future HP reserve, not free healing.  Charge
        // its exact revival amount when a rollout spends it, while leaving every
        // branch that preserves the relic on the existing HP scale.  A victory
        // remains slightly positive even when the consumed reserve exceeds the
        // final HP, so it still ranks above a losing rollout.
        const double consumedLizardReserve = std::max(
            0.0,
            getLizardTailReserveHp(rootBc) - getLizardTailReserveHp(bc)
        );
        const int newlyEscapedStolenGold = std::max(
            0,
            getEscapedStolenGold(bc) - getEscapedStolenGold(rootBc)
        );
        // Preserve the existing HP-first objective while accounting for the
        // only combat outcome that irreversibly changes run gold. Ten stolen
        // gold is charged as one effective HP: enough to prefer a cheap lethal,
        // but not enough to sacrifice large amounts of health for a small purse.
        constexpr double kStolenGoldPerEffectiveHp = 10.0;
        const double stolenGoldPenalty =
            static_cast<double>(newlyEscapedStolenGold)
            / kStolenGoldPerEffectiveHp;
        const double effectiveHp = std::max(
            1.0,
            static_cast<double>(getPostCombatPlayerHp(bc))
                - consumedLizardReserve
                - stolenGoldPenalty
        );
        return std::clamp(
            effectiveHp / static_cast<double>(rootMaxHp),
            0.0,
            1.0
        );
    }

    // A flat -1 for every loss removes all learning signal until a rollout happens
    // to win the entire combat. Preserve the win/loss boundary at zero, but rank
    // losing and rollout-cutoff states by how close they came to defeating the
    // non-minion enemies. This keeps hard combats searchable under a fixed budget.
    const double nonMinionMaxHp = getNonMinionMonsterMaxHpTotal(rootBc);
    const double minionMaxHp = getMinionMonsterMaxHpTotal(rootBc);
    if (nonMinionMaxHp <= 0.0 && minionMaxHp <= 0.0) {
        return -1.0;
    }

    const double nonMinionRatio = nonMinionMaxHp > 0.0
        ? std::clamp(getNonMinionMonsterCurHpTotal(bc) / nonMinionMaxHp, 0.0, 1.0)
        : 0.0;
    if (minionMaxHp <= 0.0) {
        return -nonMinionRatio;
    }

    const double minionHpRatio = std::clamp(
        getMinionMonsterCurHpTotal(bc) / minionMaxHp,
        0.0,
        1.0
    );
    const int rootLivingMinions = getLivingMinionMonsterCount(rootBc);
    const double minionAliveRatio = rootLivingMinions > 0
        ? std::clamp(
            static_cast<double>(getLivingMinionMonsterCount(bc))
                / static_cast<double>(rootLivingMinions),
            0.0,
            1.0
        )
        : minionHpRatio;
    // Preserve the dense HP-progress signal while every minion is alive, then
    // add a bounded discrete bonus only when an action actually removes one.
    // Averaging HP and alive ratios would weaken early focus-fire guidance and
    // made low-budget search fall back to attacking the encounter leader.
    constexpr double kMinionKillBonus = 0.25;
    const double minionRatio = std::clamp(
        minionHpRatio - (kMinionKillBonus * (1.0 - minionAliveRatio)),
        0.0,
        1.0
    );
    if (nonMinionMaxHp <= 0.0) {
        return -minionRatio;
    }

    // The encounter leader remains the primary objective, but defeating an
    // already-present minion must provide a learning signal in losing rollouts.
    // Without it, MCTS systematically ignores Bronze Orbs, Gremlin minions and
    // Daggers until a complete winning rollout happens by chance.
    constexpr double kExistingMinionWeight = 0.30;
    const double hpRatio = ((1.0 - kExistingMinionWeight) * nonMinionRatio)
                           + (kExistingMinionWeight * minionRatio);
    return -std::clamp(hpRatio, 0.0, 1.0);
}

}
