//
// Created by keega on 9/18/2021.
//

#include "sim/search/BattleScumSearcher2.h"
#include "sim/search/ActionEnumerator.h"
#include "sim/search/CardKey.h"
#include "sim/search/ExpertKnowledge.h"

#include <utility>
#include <memory>
#include <algorithm>
#include <cmath>
#include <array>

using namespace sts;

thread_local std::int64_t simulationIdx = 0; // for debugging

namespace sts::search {
    thread_local search::BattleScumSearcher2 *g_debug_scum_search;
}

namespace {
    constexpr std::uint64_t kHashSeed = 0x9e3779b97f4a7c15ULL;

    // Covers the normal player-action hot path without heap allocation. Card
    // selection can enumerate many more actions (for example 2^10 Gamble
    // subsets), so callers must fall back to dynamic storage above this size.
    constexpr int kCommonActionCapacity = 80;

    bool hasReachableCard(const BattleContext &bc, CardId id) {
        for (int i = 0; i < bc.cards.cardsInHand; ++i) {
            if (bc.cards.hand[i].getId() == id) {
                return true;
            }
        }
        for (const auto &card : bc.cards.drawPile) {
            if (card.getId() == id) {
                return true;
            }
        }
        for (const auto &card : bc.cards.discardPile) {
            if (card.getId() == id) {
                return true;
            }
        }
        return false;
    }

    bool hasPersistentVictoryOpportunity(const BattleContext &bc) {
        if (
            bc.player.curHp < bc.player.maxHp
            && !bc.player.hasRelic<R::MARK_OF_THE_BLOOM>()
            && (
                hasReachableCard(bc, CardId::REAPER)
                || hasReachableCard(bc, CardId::SELF_REPAIR)
            )
        ) {
            return true;
        }
        return hasReachableCard(bc, CardId::FEED)
            || hasReachableCard(bc, CardId::RITUAL_DAGGER)
            || hasReachableCard(bc, CardId::LESSON_LEARNED);
    }

    void hashCombine(std::uint64_t &seed, std::uint64_t value) {
        seed ^= value + kHashSeed + (seed << 6) + (seed >> 2);
    }

    void hashCardKey(std::uint64_t &seed, const search::CardKey &card) {
        hashCombine(seed, static_cast<std::uint64_t>(card.id));
        hashCombine(seed, static_cast<std::uint64_t>(card.upgradeCount));
        hashCombine(seed, static_cast<std::uint64_t>(card.specialData));
        hashCombine(seed, static_cast<std::uint64_t>(card.cost));
        hashCombine(seed, static_cast<std::uint64_t>(card.costForTurn));
        hashCombine(seed, static_cast<std::uint64_t>(card.freeToPlayOnce));
        hashCombine(seed, static_cast<std::uint64_t>(card.retain));
    }

    void hashCardQueueItem(std::uint64_t &seed, const CardQueueItem &item) {
        hashCardKey(seed, search::toCardKey(item.card));
        hashCombine(seed, static_cast<std::uint64_t>(item.target));
        hashCombine(seed, static_cast<std::uint64_t>(item.isEndTurn));
        hashCombine(seed, static_cast<std::uint64_t>(item.triggerOnUse));
        hashCombine(seed, static_cast<std::uint64_t>(item.ignoreEnergyTotal));
        hashCombine(seed, static_cast<std::uint64_t>(item.energyOnUse));
        hashCombine(seed, static_cast<std::uint64_t>(item.freeToPlay));
        hashCombine(seed, static_cast<std::uint64_t>(item.randomTarget));
        hashCombine(seed, static_cast<std::uint64_t>(item.autoplay));
        hashCombine(seed, static_cast<std::uint64_t>(item.regretCardCount));
        hashCombine(seed, static_cast<std::uint64_t>(item.purgeOnUse));
        hashCombine(seed, static_cast<std::uint64_t>(item.exhaustOnUse));
    }

    void hashRngState(std::uint64_t &seed, const Random &rng) {
        hashCombine(seed, static_cast<std::uint64_t>(rng.counter));
        hashCombine(seed, rng.seed0);
        hashCombine(seed, rng.seed1);
    }

    void hashPlayerState(std::uint64_t &seed, const Player &p) {
        hashCombine(seed, static_cast<std::uint64_t>(p.gold));
        hashCombine(seed, static_cast<std::uint64_t>(p.curHp));
        hashCombine(seed, static_cast<std::uint64_t>(p.maxHp));
        hashCombine(seed, static_cast<std::uint64_t>(p.energy));
        hashCombine(seed, static_cast<std::uint64_t>(p.energyPerTurn));
        hashCombine(seed, static_cast<std::uint64_t>(p.cardDrawPerTurn));
        hashCombine(seed, static_cast<std::uint64_t>(p.stance));
        hashCombine(seed, static_cast<std::uint64_t>(p.orbSlots));
        for (int i = 0; i < p.orbSlots; ++i) {
            hashCombine(seed, static_cast<std::uint64_t>(p.orbTypes[i]));
            hashCombine(seed, static_cast<std::uint64_t>(p.orbData[i]));
        }
        if (p.cc == CharacterClass::DEFECT) {
            hashCombine(seed, static_cast<std::uint64_t>(p.lightningChanneled));
            hashCombine(seed, static_cast<std::uint64_t>(p.frostChanneled));
            hashCombine(seed, static_cast<std::uint64_t>(p.lastDamageTaken));
        }
        hashCombine(seed, static_cast<std::uint64_t>(p.lastTargetedMonster));

        hashCombine(seed, static_cast<std::uint64_t>(p.block));
        hashCombine(seed, static_cast<std::uint64_t>(p.artifact));
        hashCombine(seed, static_cast<std::uint64_t>(p.dexterity));
        hashCombine(seed, static_cast<std::uint64_t>(p.focus));
        hashCombine(seed, static_cast<std::uint64_t>(p.strength));
        hashCombine(seed, p.justAppliedBits);
        hashCombine(seed, p.statusBits0);
        hashCombine(seed, p.statusBits1);
        hashCombine(seed, static_cast<std::uint64_t>(p.statusMap.size()));
        for (const auto &[status, value] : p.statusMap) {
            hashCombine(seed, static_cast<std::uint64_t>(status));
            hashCombine(seed, static_cast<std::uint64_t>(value));
        }

        hashCombine(seed, p.relicBits0);
        hashCombine(seed, p.relicBits1);
        hashCombine(seed, p.relicBits2);
        hashCombine(seed, static_cast<std::uint64_t>(p.happyFlowerCounter));
        hashCombine(seed, static_cast<std::uint64_t>(p.incenseBurnerCounter));
        hashCombine(seed, static_cast<std::uint64_t>(p.inkBottleCounter));
        hashCombine(seed, static_cast<std::uint64_t>(p.inserterCounter));
        hashCombine(seed, static_cast<std::uint64_t>(p.nunchakuCounter));
        hashCombine(seed, static_cast<std::uint64_t>(p.penNibCounter));
        hashCombine(seed, static_cast<std::uint64_t>(p.sundialCounter));
        hashCombine(seed, static_cast<std::uint64_t>(p.haveUsedNecronomiconThisTurn));

        hashCombine(seed, static_cast<std::uint64_t>(p.combustHpLoss));
        hashCombine(seed, static_cast<std::uint64_t>(p.devaFormEnergyPerTurn));
        hashCombine(seed, static_cast<std::uint64_t>(p.echoFormCardsDoubled));
        hashCombine(seed, static_cast<std::uint64_t>(p.panacheCounter));
        hashCombine(seed, static_cast<std::uint64_t>(p.bomb1));
        hashCombine(seed, static_cast<std::uint64_t>(p.bomb2));
        hashCombine(seed, static_cast<std::uint64_t>(p.bomb3));

        hashCombine(seed, static_cast<std::uint64_t>(p.cardsPlayedThisTurn));
        hashCombine(seed, static_cast<std::uint64_t>(p.attacksPlayedThisTurn));
        hashCombine(seed, static_cast<std::uint64_t>(p.skillsPlayedThisTurn));
        hashCombine(seed, p.orangePelletsCardTypesPlayed.to_ullong());
        hashCombine(seed, static_cast<std::uint64_t>(p.cardsDiscardedThisTurn));
        hashCombine(seed, static_cast<std::uint64_t>(p.lastAttackUnblockedDamage));
        hashCombine(seed, static_cast<std::uint64_t>(p.timesDamagedThisCombat));
    }

    void hashMonsterState(std::uint64_t &seed, const Monster &m) {
        hashCombine(seed, static_cast<std::uint64_t>(m.idx));
        hashCombine(seed, static_cast<std::uint64_t>(m.id));
        hashCombine(seed, static_cast<std::uint64_t>(m.curHp));
        hashCombine(seed, static_cast<std::uint64_t>(m.maxHp));
        hashCombine(seed, static_cast<std::uint64_t>(m.block));

        hashCombine(seed, static_cast<std::uint64_t>(m.isEscapingB));
        hashCombine(seed, static_cast<std::uint64_t>(m.halfDead));
        hashCombine(seed, static_cast<std::uint64_t>(m.escapeNext));
        hashCombine(seed, static_cast<std::uint64_t>(m.moveHistory[0]));
        hashCombine(seed, static_cast<std::uint64_t>(m.moveHistory[1]));

        hashCombine(seed, m.statusBits);
        hashCombine(seed, static_cast<std::uint64_t>(m.artifact));
        hashCombine(seed, static_cast<std::uint64_t>(m.blockReturn));
        hashCombine(seed, static_cast<std::uint64_t>(m.choked));
        hashCombine(seed, static_cast<std::uint64_t>(m.corpseExplosion));
        hashCombine(seed, static_cast<std::uint64_t>(m.lockOn));
        hashCombine(seed, static_cast<std::uint64_t>(m.mark));
        hashCombine(seed, static_cast<std::uint64_t>(m.metallicize));
        hashCombine(seed, static_cast<std::uint64_t>(m.platedArmor));
        hashCombine(seed, static_cast<std::uint64_t>(m.poison));
        hashCombine(seed, static_cast<std::uint64_t>(m.regen));
        hashCombine(seed, static_cast<std::uint64_t>(m.shackled));
        hashCombine(seed, static_cast<std::uint64_t>(m.strength));
        hashCombine(seed, static_cast<std::uint64_t>(m.vulnerable));
        hashCombine(seed, static_cast<std::uint64_t>(m.weak));
        hashCombine(seed, static_cast<std::uint64_t>(m.uniquePower0));
        hashCombine(seed, static_cast<std::uint64_t>(m.uniquePower1));
        hashCombine(seed, static_cast<std::uint64_t>(m.miscInfo));
    }

    void hashMonsterGroupState(std::uint64_t &seed, const MonsterGroup &g) {
        hashCombine(seed, static_cast<std::uint64_t>(g.monsterCount));
        hashCombine(seed, static_cast<std::uint64_t>(g.monstersAlive));
        hashCombine(seed, static_cast<std::uint64_t>(g.extraRollMoveOnTurn.to_ullong()));
        hashCombine(seed, static_cast<std::uint64_t>(g.skipTurn.to_ullong()));

        for (int i = 0; i < g.monsterCount; ++i) {
            hashMonsterState(seed, g.arr[i]);
        }
    }

    template <typename Iterator>
    void hashCardGroupInOrder(std::uint64_t &seed, Iterator begin, Iterator end) {
        for (auto it = begin; it != end; ++it) {
            hashCardKey(seed, search::toCardKey(*it));
        }
    }

    // Shared normalization helpers for the boundary-state keys below.

    void sortSpentPiles(CardManager &cards) {
        // Two orders of the same cards may append those cards to spent piles
        // in opposite order.  That order has the same shuffled distribution
        // and is not a reason to preserve an otherwise inferior sequence.
        std::sort(cards.discardPile.begin(), cards.discardPile.end(), search::cardLess);
        std::sort(cards.exhaustPile.begin(), cards.exhaustPile.end(), search::cardLess);
    }

    void resetEmptyExecutorQueues(BattleContext &bc) {
        // Empty ring-buffer cursors and the last processed queue item are
        // executor history, not pending future work.  Different-length action
        // sequences leave different cursors even when they arrive at the same
        // stable input state.
        if (bc.actionQueue.size == 0) {
            bc.actionQueue = {};
        }
        if (bc.cardQueue.size == 0) {
            bc.cardQueue = {};
            bc.curCardQueueItem = {};
        }
    }

    bool hasLivingWrithingMass(const BattleContext &bc) {
        for (int i = 0; i < bc.monsters.monsterCount; ++i) {
            if (bc.monsters.arr[i].id == MonsterId::WRITHING_MASS
                && bc.monsters.arr[i].isAlive()) {
                return true;
            }
        }
        return false;
    }

    // Search bookkeeping that is shared by every rollout update lives in
    // PlayoutStats::record (see the header); Node and Edge inherit it.

    bool rootIsConvergedForcedWin(const search::BattleScumSearcher2::Node &root) {
        if (root.edges.empty()) {
            return false;
        }

        constexpr std::int64_t kMinVisitsPerAction = 2048;
        constexpr double kMaxVariance = 1e-4;
        constexpr double kMaxMeanRange = 0.0025;
        double minMean = std::numeric_limits<double>::max();
        double maxMean = std::numeric_limits<double>::lowest();
        for (const auto &edge : root.edges) {
            if (edge.simulationCount < kMinVisitsPerAction
                || edge.lossSamples != 0
                || edge.cutoffSamples != 0
                || edge.winSamples != edge.simulationCount) {
                return false;
            }
            const double mean = edge.evaluationSum
                                / static_cast<double>(edge.simulationCount);
            const double variance = std::max(
                0.0,
                edge.evaluationSquaredSum
                    / static_cast<double>(edge.simulationCount)
                    - mean * mean
            );
            if (variance > kMaxVariance) {
                return false;
            }
            minMean = std::min(minMean, mean);
            maxMean = std::max(maxMean, mean);
        }
        return maxMean - minMean <= kMaxMeanRange;
    }

    // Index of the highest score, breaking ties uniformly at random within a
    // small tolerance.  Used for leaf expansion and rollout action selection.
    // The common combat path uses a fixed tie list; large card-selection
    // screens use dynamic storage instead of relying on a false hard bound.
    int pickBestTiedIndex(const double *scores, int count,
                          std::default_random_engine &rng) {
        std::array<int, kCommonActionCapacity> fixedBestIdxs;
        std::vector<int> dynamicBestIdxs;
        int *bestIdxs = fixedBestIdxs.data();
        if (count > kCommonActionCapacity) {
            dynamicBestIdxs.resize(count);
            bestIdxs = dynamicBestIdxs.data();
        }
        int bestCount = 0;
        double bestScore = std::numeric_limits<double>::lowest();
        for (int i = 0; i < count; ++i) {
            const double score = scores[i];
            if (score > bestScore) {
                bestScore = score;
                bestCount = 0;
                bestIdxs[bestCount++] = i;
            } else if (std::abs(score - bestScore) < 0.01) {
                bestIdxs[bestCount++] = i;
            }
        }
        auto dist = std::uniform_int_distribution<int>(0, bestCount - 1);
        return bestIdxs[dist(rng)];
    }

    // Normalizes scores to [-1, 1] and blends in a per-action LLM prior.
    template <typename ScoreContainer, typename GetPrior>
    void blendActionPriors(ScoreContainer &scores, int count,
                           double llmGuidanceWeight,
                           GetPrior &&getPrior) {
        if (count <= 0) {
            return;
        }
        // Normalize against the original min/max, not the in-place updated
        // values: re-reading the extrema after scores[i] was overwritten
        // would corrupt every later prior.
        double minScore = scores[0];
        double maxScore = scores[0];
        for (int i = 1; i < count; ++i) {
            minScore = std::min(minScore, scores[i]);
            maxScore = std::max(maxScore, scores[i]);
        }
        const double range = maxScore - minScore;
        for (int i = 0; i < count; ++i) {
            const double enginePrior = range < 1e-9
                                       ? 0.0
                                       : (2.0 * (scores[i] - minScore) / range) - 1.0;
            scores[i] = enginePrior + llmGuidanceWeight * getPrior(i);
        }
    }
}

search::BattleScumSearcher2::BattleScumSearcher2(const BattleContext &bc, search::EvalFnc _evalFnc)
    : rootState(new BattleContext(bc)), evalFnc(std::move(_evalFnc)), randGen(bc.seed+bc.floorNum) {
    transpositionTable.emplace(buildStateKey(*rootState), std::shared_ptr<Node>(&root, [] (Node*) {}));
}

void search::BattleScumSearcher2::setActionGuidance(
    int turn,
    std::vector<std::string> prefer,
    std::vector<std::string> discourage
) {
    guidanceTurn = turn;
    preferredCardNames = std::move(prefer);
    discouragedCardNames = std::move(discourage);
}

bool search::BattleScumSearcher2::hasActiveActionGuidance(
    const BattleContext &bc
) const {
    return bc.turn == guidanceTurn
           && (!preferredCardNames.empty() || !discouragedCardNames.empty());
}

double search::BattleScumSearcher2::getActionGuidancePrior(
    const BattleContext &bc,
    const Action &action
) const {
    if (!hasActiveActionGuidance(bc)) {
        return 0.0;
    }

    std::string guidanceName;
    if (action.getActionType() == ActionType::END_TURN) {
        guidanceName = "END_TURN";
    } else if (action.getActionType() == ActionType::CARD) {
        const int sourceIdx = action.getSourceIdx();
        if (sourceIdx < 0 || sourceIdx >= bc.cards.cardsInHand) {
            return 0.0;
        }
        guidanceName = bc.cards.hand[sourceIdx].getName();
    } else {
        return 0.0;
    }

    const auto preferred = std::find(
        preferredCardNames.begin(), preferredCardNames.end(), guidanceName
    );
    if (preferred != preferredCardNames.end()) {
        const auto rank = static_cast<double>(
            std::distance(preferredCardNames.begin(), preferred)
        );
        const auto denominator = static_cast<double>(
            std::max<std::size_t>(1, preferredCardNames.size() - 1)
        );
        // Preserve list order while keeping every preferred item positive.
        return 1.0 - 0.5 * rank / denominator;
    }

    if (std::find(
            discouragedCardNames.begin(),
            discouragedCardNames.end(),
            guidanceName
        ) != discouragedCardNames.end()) {
        return -1.0;
    }
    return 0.0;
}

void search::BattleScumSearcher2::search(int64_t simulations, long maxTimeMillis) {
    g_debug_scum_search = this;
    startTime = std::chrono::steady_clock::now();

    if (isTerminalState(*rootState)) {
        const auto evaluation = evaluateState(*rootState);
        outcomePlayerHp = rootState->outcome == Outcome::PLAYER_VICTORY
                          ? getPostCombatPlayerHp(*rootState)
                          : 0;
        bestActionSequence = {};
        bestActionValue = evaluation;
        minActionValue = evaluation;

        root.reset();
        const double winUtility = rootState->outcome == Outcome::PLAYER_VICTORY
            ? evaluateEndState(*rootState, *rootState)
            : 0.0;
        root.record(
            evaluation,
            *rootState,
            winUtility,
            false,
            search::getEscapedStolenGold(*rootState)
        );
        stopReason = "terminal_root";
        return;
    }

    // A legal action that ends the fight immediately dominates every longer
    // line under the survival/remaining-HP objective.  Detect it exactly before
    // spending hundreds of thousands of rollouts rediscovering the same fact.
    // Counterfactual mode deliberately evaluates every root action, so it opts
    // out of this gameplay-only shortcut.
    bool mustCompareWinningLines = false;
    if (!allowRootPotions && canUseImmediateVictoryShortcut(*rootState)) {
        // search() may be called again to extend an inconclusive tree.  Root
        // edges and their statistics are persistent; enumerating them twice
        // would split visits across duplicate actions and corrupt aggregation.
        if (root.edges.empty()) {
            enumerateActionsForNode(root, *rootState);
            expandLeafNode(root, *rootState);
        }
        int lethalEdgeIdx = -1;
        double lethalValue = std::numeric_limits<double>::lowest();
        BattleContext lethalState;
        for (int i = 0; i < static_cast<int>(root.edges.size()); ++i) {
            BattleContext after = *rootState;
            root.edges[i].action.execute(after);
            if (after.outcome != Outcome::PLAYER_VICTORY) {
                continue;
            }
            // END_TURN can reach victory only after processing enemy attacks,
            // start-of-turn draws, powers, and other queued effects.  That is
            // not an immediate lethal: a longer card sequence may win before
            // those effects and preserve substantially more HP.  Keep END_TURN
            // in the ordinary root search so it is compared with those lines.
            if (
                root.edges[i].action.getActionType()
                == ActionType::END_TURN
            ) {
                continue;
            }
            const double value = evaluateState(after);
            if (lethalEdgeIdx == -1 || value > lethalValue) {
                lethalEdgeIdx = i;
                lethalValue = value;
                lethalState = after;
            }
        }
        if (lethalEdgeIdx >= 0) {
            BattleContext resourceNeutralVictory(*rootState);
            resourceNeutralVictory.outcome = Outcome::PLAYER_VICTORY;
            const bool losesRunResources =
                evaluateEndState(*rootState, lethalState) + 1e-12
                < evaluateEndState(*rootState, resourceNeutralVictory);
            if (
                losesRunResources
                || hasPersistentVictoryOpportunity(*rootState)
            ) {
                // Keep the exact lethal as an ordinary root action. Search for
                // a sufficiently reliable win which preserves more HP or a
                // reachable Reaper/Feed-style persistent payoff.
                mustCompareWinningLines = true;
            }
        }
        if (lethalEdgeIdx >= 0 && !mustCompareWinningLines) {
            auto &edge = root.edges[lethalEdgeIdx];
            const bool consumedLizardTail =
                search::getLizardTailReserveHp(*rootState)
                > search::getLizardTailReserveHp(lethalState);
            const double winUtility =
                evaluateEndState(*rootState, lethalState);
            const int escapedStolenGold = search::getEscapedStolenGold(lethalState);
            edge.reset();
            edge.record(
                lethalValue, lethalState, winUtility,
                consumedLizardTail, escapedStolenGold
            );
            root.reset();
            root.record(
                lethalValue, lethalState, winUtility,
                consumedLizardTail, escapedStolenGold
            );
            bestActionSequence = {edge.action};
            bestActionValue = lethalValue;
            minActionValue = lethalValue;
            outcomePlayerHp = getPostCombatPlayerHp(lethalState);
            stopReason = "immediate_lethal";
            return;
        }
    }

    if (stopOnForcedRootAction && root.edges.empty()) {
        enumerateActionsForNode(root, *rootState);
        expandLeafNode(root, *rootState);
    }
    const bool forcedRootAction =
        stopOnForcedRootAction && root.edges.size() == 1;

    for (std::int64_t simCount = 0; simCount < simulations; ++simCount) {
        // Always complete one simulation so callers receive a usable root tree,
        // even under a zero or extremely small wall-clock budget.
        if (simCount > 0 && (simCount == 1 || simCount % 100 == 0)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime
            );
            if (elapsed.count() >= maxTimeMillis) {
                stopReason = "time_budget";
                return;
            }
        }
        step();
        if (forcedRootAction) {
            stopReason = "forced_root_action";
            return;
        }
        if (
            !allowRootPotions
            && !mustCompareWinningLines
            && simCount > 0
            && simCount % 100 == 0
            // A nominal victory is not necessarily converged while a thief
            // can still escape with stolen gold.  The easier escape leaves
            // every root looking identical before the deeper kill line has
            // been explored.
            && canUseImmediateVictoryShortcut(*rootState)
            && rootIsConvergedForcedWin(root)
        ) {
            stopReason = "converged_forced_win";
            return;
        }
    }
    stopReason = "simulation_budget";
}

bool search::BattleScumSearcher2::canUseImmediateVictoryShortcut(
    const BattleContext &bc
) {
    for (int i = 0; i < bc.monsters.monsterCount; ++i) {
        const auto &monster = bc.monsters.arr[i];
        if (monster.id == MonsterId::TRANSIENT && monster.isAlive()) {
            // Transient can expire after END_TURN while still dealing its
            // attack.  A card played first may reduce that damage through
            // Shifting, so "ends combat now" does not imply dominance.
            return false;
        }
        if ((monster.id == MonsterId::LOOTER
             || monster.id == MonsterId::MUGGER)
            && monster.isAlive()
            && monster.miscInfo > 0) {
            // END_TURN may produce a nominal victory by letting a thief flee.
            // That is not guaranteed to dominate a longer line which kills it
            // and recovers the stolen gold.
            return false;
        }
    }
    return true;
}

void search::BattleScumSearcher2::step() {
    searchStack = {&root};
    actionStack.clear();
    edgeIdxStack.clear();
    BattleContext curState;
    curState = *rootState;
    ReplanCheckpoint checkpoint;

    while (true) {
        auto &curNode = *searchStack.back();

        if (isTerminalState(curState)) {
            updateFromPlayout(
                searchStack, edgeIdxStack, actionStack, curState, checkpoint
            );
            return;
        }

        if (static_cast<int>(searchStack.size()) >= maxTreeDepth) {
            playoutRandom(curState, actionStack, checkpoint);
            updateFromPlayout(
                searchStack, edgeIdxStack, actionStack, curState, checkpoint
            );
            return;
        }

        // A transposition node may be shared by states whose omitted identity
        // details make a previously enumerated edge illegal. This must remain
        // a release-build check: assertions alone would permit invalid actions
        // and undefined simulation state in production.
        pruneInvalidEdgesForState(curNode, curState);

        const bool isLeaf = curNode.edges.empty();
        if (isLeaf) {

            ++simulationIdx;
            enumerateActionsForNode(curNode, curState);
            expandLeafNode(curNode, curState);
            if (curNode.edges.empty()) {
                updateFromPlayout(
                    searchStack, edgeIdxStack, actionStack, curState, checkpoint
                );
                return;
            }
            const auto selectIdx = selectFirstActionForLeafNode(curNode, curState);
            auto &edgeTaken = curNode.edges[selectIdx];

#ifdef sts_asserts
            if (!edgeTaken.action.isValidAction(curState)) {
                std::cerr << "step: selected an invalid leaf action" << std::endl;
                assert(false);
            }
#endif
            edgeTaken.action.execute(curState);
            observeReplanCheckpoint(curState, checkpoint);
            actionStack.push_back(edgeTaken.action);
            edgeIdxStack.push_back(selectIdx);
            searchStack.push_back(edgeTaken.node.get());

            playoutRandom(curState, actionStack, checkpoint);
            updateFromPlayout(
                searchStack, edgeIdxStack, actionStack, curState, checkpoint
            );
            return;

        } else {
            const auto selectIdx = selectBestEdgeToSearch(curNode);
            auto &edgeTaken = curNode.edges[selectIdx];

#ifdef sts_asserts
            if (!edgeTaken.action.isValidAction(curState)) {
                std::cerr << "step: selected an invalid edge" << std::endl;
                assert(false);
            }
#endif
            edgeTaken.action.execute(curState);
            observeReplanCheckpoint(curState, checkpoint);
            actionStack.push_back(edgeTaken.action);
            edgeIdxStack.push_back(selectIdx);
            searchStack.push_back(edgeTaken.node.get());
        }
    }
}

void search::BattleScumSearcher2::pruneInvalidEdgesForState(
    search::BattleScumSearcher2::Node &node,
    const BattleContext &bc
) {
    node.edges.erase(
        std::remove_if(
            node.edges.begin(), node.edges.end(), [&] (const Edge &edge) {
                return !edge.action.isValidAction(bc);
            }
        ),
        node.edges.end()
    );
}

std::uint64_t search::BattleScumSearcher2::buildStateKey(const BattleContext &bc) const {
    std::uint64_t seed = 0;
    hashCombine(seed, static_cast<std::uint64_t>(bc.energyWasted));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cardsDrawn));
    if (bc.player.cc == CharacterClass::DEFECT) {
        hashCombine(seed, static_cast<std::uint64_t>(bc.powersPlayedThisCombat));
    }
    hashCombine(seed, static_cast<std::uint64_t>(bc.outcome));
    hashCombine(seed, static_cast<std::uint64_t>(bc.inputState));
    hashCombine(seed, static_cast<std::uint64_t>(bc.monsterTurnIdx));
    hashCombine(seed, static_cast<std::uint64_t>(bc.turn));
    hashCombine(seed, static_cast<std::uint64_t>(bc.isBattleOver));
    hashCombine(seed, static_cast<std::uint64_t>(bc.endTurnQueued));
    hashCombine(seed, static_cast<std::uint64_t>(bc.turnHasEnded));
    hashCombine(seed, static_cast<std::uint64_t>(bc.skipMonsterTurn));
    hashCombine(seed, static_cast<std::uint64_t>(bc.miscBits.to_ullong()));
    hashCombine(seed, static_cast<std::uint64_t>(bc.actionQueue.size));
    hashCombine(seed, static_cast<std::uint64_t>(bc.actionQueue.front));
    hashCombine(seed, static_cast<std::uint64_t>(bc.actionQueue.back));
    hashCombine(seed, static_cast<std::uint64_t>(bc.actionQueue.bits.to_ullong()));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cardQueue.size));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cardQueue.frontIdx));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cardQueue.backIdx));

    hashCombine(seed, static_cast<std::uint64_t>(bc.cardSelectInfo.cardSelectTask));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cardSelectInfo.canPickZero));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cardSelectInfo.canPickAnyNumber));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cardSelectInfo.pickCount));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cardSelectInfo.data0));
    for (const auto &cardId : bc.cardSelectInfo.cards) {
        hashCombine(seed, static_cast<std::uint64_t>(cardId));
    }

    hashCombine(seed, static_cast<std::uint64_t>(bc.potionCount));
    hashCombine(seed, static_cast<std::uint64_t>(bc.potionCapacity));
    for (int i = 0; i < bc.potionCapacity; ++i) {
        hashCombine(seed, static_cast<std::uint64_t>(bc.potions[i]));
    }

    hashRngState(seed, bc.aiRng);
    hashRngState(seed, bc.cardRandomRng);
    hashRngState(seed, bc.miscRng);
    hashRngState(seed, bc.monsterHpRng);
    hashRngState(seed, bc.potionRng);
    hashRngState(seed, bc.shuffleRng);

    if (bc.cardQueue.size > 0) {
        int idx = bc.cardQueue.frontIdx;
        for (int i = 0; i < bc.cardQueue.size; ++i) {
            if (idx >= CardQueue::capacity) {
                idx = 0;
            }
            hashCardQueueItem(seed, bc.cardQueue.arr[idx]);
            ++idx;
        }
    }
    hashCardQueueItem(seed, bc.curCardQueueItem);

    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.nextUniqueCardId));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.handNormalityCount));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.handPainCount));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.strikeCount));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.handBloodCardCount));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.drawPileBloodCardCount));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.discardPileBloodCardCount));

    // Card contents alone are ambiguous across group boundaries. For example,
    // hand=[Strike], draw=[Defend] otherwise hashes exactly like
    // hand=[Strike, Defend], draw=[] and would merge states with different legal
    // actions and future draws.
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.cardsInHand));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.drawPile.size()));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.discardPile.size()));
    hashCombine(seed, static_cast<std::uint64_t>(bc.cards.exhaustPile.size()));

    hashCardGroupInOrder(seed, bc.cards.hand.begin(), bc.cards.hand.begin() + bc.cards.cardsInHand);
    hashCardGroupInOrder(seed, bc.cards.drawPile.begin(), bc.cards.drawPile.end());
    hashCardGroupInOrder(seed, bc.cards.discardPile.begin(), bc.cards.discardPile.end());
    hashCardGroupInOrder(seed, bc.cards.exhaustPile.begin(), bc.cards.exhaustPile.end());
    hashCardGroupInOrder(seed, bc.cards.limbo.begin(), bc.cards.limbo.end());
    hashCardGroupInOrder(seed, bc.cards.stasisCards.begin(), bc.cards.stasisCards.end());

    hashMonsterGroupState(seed, bc.monsters);

    hashPlayerState(seed, bc.player);

    return seed;
}

bool search::BattleScumSearcher2::shouldDedupState(const BattleContext &bc) const {
    return bc.actionQueue.size == 0
           && bc.cardQueue.size == 0
           // CARD_SELECT states are index-sensitive and can vary by transient selection context.
           // Restrict transposition merges to normal player states to avoid cross-state edge reuse.
           && bc.inputState == InputState::PLAYER_NORMAL;
}

std::uint64_t search::BattleScumSearcher2::buildComparableReplanStateKey(
    const BattleContext &bc,
    bool normalizeMonotonicProgress
) const {
    BattleContext normalized(bc);
    // Search-only cumulative diagnostics do not affect future game behavior.
    normalized.energyWasted = 0;
    normalized.cardsDrawn = 0;
    resetEmptyExecutorQueues(normalized);
    if (normalized.inputState == InputState::PLAYER_NORMAL) {
        normalized.cardSelectInfo = {};
    }
    normalized.player.curHp = 0;
    // This field only affects Writhing Mass's reactive targeting. Against all
    // other encounters it is UI/history residue, not future game state.
    if (!hasLivingWrithingMass(normalized)) {
        normalized.player.lastTargetedMonster = 0;
    }
    for (int i = 0; i < normalized.monsters.monsterCount; ++i) {
        auto &monster = normalized.monsters.arr[i];
        monster.curHp = 0;
        monster.block = 0;
        // Damage, Weak, and Vulnerable are all monotonic combat progress at
        // the same next-turn boundary: a player action cannot make the enemy
        // better by applying them.  Keep Artifact and every reactive/phase
        // field in the key, so a debuff that was blocked or triggered another
        // mechanic is still not treated as the same future state.
        monster.setStatus(MonsterStatus::WEAK, 0);
        monster.setStatus(MonsterStatus::VULNERABLE, 0);
        monster.setJustApplied(MonsterStatus::WEAK, false);
        monster.setJustApplied(MonsterStatus::VULNERABLE, false);
    }

    if (normalizeMonotonicProgress) {
        // At the same next-turn boundary, a player action cannot lower
        // permanent Strength. Temporary Strength (for example Flex) has
        // already expired, while Shuriken/Inflame gains remain.
        normalized.player.strength = 0;
        // Playing Self Repair trades one reachable card and one Power play for
        // deterministic post-combat healing.  Canonicalize that trade here;
        // the caller compares the gained healing and Power progress
        // separately, while every adverse trigger remains in the state key.
        normalized.player.statusMap.erase(PS::SELF_REPAIR);
        normalized.player.setHasStatus<PS::SELF_REPAIR>(false);
        normalized.powersPlayedThisCombat = 0;
        for (int i = normalized.cards.cardsInHand - 1; i >= 0; --i) {
            if (normalized.cards.hand[i].getId() == CardId::SELF_REPAIR) {
                normalized.cards.removeFromHandAtIdx(i);
            }
        }
        for (int i = static_cast<int>(normalized.cards.drawPile.size()) - 1;
             i >= 0; --i) {
            if (normalized.cards.drawPile[i].getId() == CardId::SELF_REPAIR) {
                normalized.cards.removeFromDrawPileAtIdx(i);
            }
        }
        for (int i = static_cast<int>(normalized.cards.discardPile.size()) - 1;
             i >= 0; --i) {
            if (normalized.cards.discardPile[i].getId() == CardId::SELF_REPAIR) {
                normalized.cards.removeFromDiscard(i);
            }
        }
        for (int i = static_cast<int>(normalized.cards.exhaustPile.size()) - 1;
             i >= 0; --i) {
            if (normalized.cards.exhaustPile[i].getId() == CardId::SELF_REPAIR) {
                normalized.cards.removeFromExhaustPile(i);
            }
        }
        const auto normalizeCard = [] (CardInstance &card) {
            if (card.usesSpecialData()) {
                card.specialData = 0;
            }
        };
        for (int i = 0; i < normalized.cards.cardsInHand; ++i) {
            normalizeCard(normalized.cards.hand[i]);
        }
        for (auto &card : normalized.cards.drawPile) {
            normalizeCard(card);
        }
        for (auto &card : normalized.cards.discardPile) {
            normalizeCard(card);
        }
        for (auto &card : normalized.cards.exhaustPile) {
            normalizeCard(card);
        }
        for (auto &card : normalized.cards.limbo) {
            normalizeCard(card);
        }
        for (auto &card : normalized.cards.stasisCards) {
            normalizeCard(card);
        }
    }

    sortSpentPiles(normalized.cards);
    return buildStateKey(normalized);
}

std::uint64_t search::BattleScumSearcher2::buildLocalActionOrderingStateKey(
    const BattleContext &bc
) const {
    BattleContext normalized(bc);
    resetEmptyExecutorQueues(normalized);
    sortSpentPiles(normalized.cards);

    // Enemy HP is the one field compared separately by the caller.  Every
    // other future-relevant field remains exact, including Block, debuffs,
    // player resources, relic counters, draw order, and RNG state.
    for (int i = 0; i < normalized.monsters.monsterCount; ++i) {
        normalized.monsters.arr[i].curHp = 0;
    }
    return buildStateKey(normalized);
}

std::uint64_t search::BattleScumSearcher2::buildStrictBlockBoundaryStateKey(
    const BattleContext &bc,
    bool canonicalizeShuffledDrawPile
) const {
    BattleContext normalized(bc);
    // These values describe how the executor reached the boundary, not the
    // future game state.  They must not make a harmless extra card action look
    // different from END_TURN.
    normalized.energyWasted = 0;
    normalized.cardsDrawn = 0;
    resetEmptyExecutorQueues(normalized);
    if (normalized.inputState == InputState::PLAYER_NORMAL) {
        normalized.cardSelectInfo = {};
    }

    // Block is the only gameplay field this key intentionally ignores.  The
    // caller compares it separately in every RNG world. HP, monster progress,
    // powers, relic counters, piles, draw order, and RNG state remain exact.
    normalized.player.block = 0;
    // Limbo is scratch storage used while discarding/retaining at end of turn.
    // Its stale contents are not part of a stable PLAYER_NORMAL future state.
    normalized.cards.limbo = {};
    if (
        std::all_of(
            normalized.cards.stasisCards.begin(),
            normalized.cards.stasisCards.end(),
            [] (const CardInstance &card) {
                return card.getId() == CardId::INVALID;
            }
        )
    ) {
        normalized.cards.stasisCards = {
            CardId::INVALID,
            CardId::INVALID,
        };
    }
    if (!hasLivingWrithingMass(normalized)) {
        normalized.player.lastTargetedMonster = 0;
    }

    sortSpentPiles(normalized.cards);
    if (canonicalizeShuffledDrawPile) {
        std::vector<CardInstance> shuffledCards;
        shuffledCards.reserve(
            normalized.cards.cardsInHand
            + normalized.cards.drawPile.size()
        );
        shuffledCards.insert(
            shuffledCards.end(),
            normalized.cards.hand.begin(),
            normalized.cards.hand.begin() + normalized.cards.cardsInHand
        );
        shuffledCards.insert(
            shuffledCards.end(),
            normalized.cards.drawPile.begin(),
            normalized.cards.drawPile.end()
        );
        std::sort(shuffledCards.begin(), shuffledCards.end(), search::cardLess);
        for (int i = 0; i < normalized.cards.cardsInHand; ++i) {
            normalized.cards.hand[i] = shuffledCards[i];
        }
        normalized.cards.drawPile.clear();
        for (auto it = shuffledCards.begin() + normalized.cards.cardsInHand;
             it != shuffledCards.end(); ++it) {
            normalized.cards.drawPile.push_back(*it);
        }
    }
    return buildStateKey(normalized);
}

void search::BattleScumSearcher2::expandLeafNode(search::BattleScumSearcher2::Node &node,
                                                 const BattleContext &bc) {
    if (node.edges.empty()) {
        return;
    }

    const bool dedup = shouldDedupState(bc);
    std::vector<double> scores;
    scores.reserve(node.edges.size());
    for (auto &edge : node.edges) {
        // Execute the action once and reuse the resulting state for both the
        // transposition lookup and the heuristic delta.
        BattleContext nextState(bc);
        edge.action.execute(nextState);

        if (dedup && shouldDedupState(nextState)) {
            const auto key = buildStateKey(nextState);
            const auto [it, inserted] = transpositionTable.emplace(key, edge.node);
            if (!inserted) {
                edge.node = it->second;
            }
        }

        scores.push_back(
            objective == SearchObjective::RECOVERY_HORIZON
                ? search::evaluateRecoveryActionDelta(bc, nextState)
                : search::evaluateActionDelta(bc, nextState, intent)
        );
    }

    blendActionPriors(
        scores, static_cast<int>(scores.size()), llmGuidanceWeight, [&] (int i) {
            return getActionGuidancePrior(bc, node.edges[i].action);
        }
    );
    for (int i = 0; i < static_cast<int>(node.edges.size()); ++i) {
        node.edges[i].heuristicPrior = scores[i];
    }
}

void search::BattleScumSearcher2::updateFromPlayout(const std::vector<Node *> &stack,
                                                  const std::vector<int> &edgeIdxStack,
                                                  const std::vector<Action> &actionStack,
                                                  const BattleContext &endState,
                                                  const ReplanCheckpoint &checkpoint) {
    const auto evaluation = evaluateState(endState);
    const double winUtility = endState.outcome == Outcome::PLAYER_VICTORY
        ? evaluateEndState(*rootState, endState)
        : 0.0;
    const bool consumedLizardTail =
        endState.outcome == Outcome::PLAYER_VICTORY
        && search::getLizardTailReserveHp(*rootState)
           > search::getLizardTailReserveHp(endState);
    // Stolen gold is the same for every node on the path, so compute it once
    // instead of once per visited depth.
    const int escapedStolenGold = endState.outcome == Outcome::PLAYER_VICTORY
        ? search::getEscapedStolenGold(endState)
        : 0;

    if (evaluation > bestActionValue) {
        bestActionSequence = actionStack;
        bestActionValue = evaluation;
        outcomePlayerHp = endState.outcome == Outcome::PLAYER_VICTORY
                          ? getPostCombatPlayerHp(endState)
                          : 0;
    }

    if (evaluation < minActionValue) {
        minActionValue = evaluation;
    }

    for (int depth = static_cast<int>(stack.size()) - 1; depth >= 0; --depth) {
        auto &node = *stack[depth];
        node.record(
            evaluation, endState, winUtility,
            consumedLizardTail, escapedStolenGold
        );

        if (depth > 0) {
            const auto edgeStackIdx = static_cast<std::size_t>(depth - 1);
            if (edgeStackIdx >= edgeIdxStack.size()) {
                continue;
            }

            auto &parent = *stack[depth - 1];
            const auto edgeIdx = edgeIdxStack[edgeStackIdx];
            if (edgeIdx < 0 || edgeIdx >= static_cast<int>(parent.edges.size())) {
                continue;
            }

            auto &edge = parent.edges[edgeIdx];
            edge.record(
                evaluation, endState, winUtility,
                consumedLizardTail, escapedStolenGold
            );
        }
    }

    if (!edgeIdxStack.empty()) {
        const int rootEdgeIdx = edgeIdxStack.front();
        if (rootEdgeIdx >= 0
            && rootEdgeIdx < static_cast<int>(root.edges.size())
            && checkpoint.reached) {
            auto &rootEdge = root.edges[rootEdgeIdx];
            rootEdge.bestReplanHorizonTurns = std::max(
                rootEdge.bestReplanHorizonTurns,
                checkpoint.horizonTurns
            );
            if (
                !rootEdge.reachedNextDecision
                || checkpoint.effectiveHp
                   > rootEdge.bestNextDecisionEffectiveHp
                || (
                    checkpoint.effectiveHp
                        == rootEdge.bestNextDecisionEffectiveHp
                    && checkpoint.potionCount
                       > rootEdge.bestNextDecisionPotionCount
                )
                || (
                    checkpoint.effectiveHp
                        == rootEdge.bestNextDecisionEffectiveHp
                    && checkpoint.potionCount
                        == rootEdge.bestNextDecisionPotionCount
                    && checkpoint.value
                       > rootEdge.bestNextDecisionValue
                )
            ) {
                rootEdge.reachedNextDecision = true;
                rootEdge.bestNextDecisionHp = checkpoint.hp;
                rootEdge.bestNextDecisionEffectiveHp = checkpoint.effectiveHp;
                rootEdge.bestNextDecisionPotionCount = checkpoint.potionCount;
                rootEdge.bestNextDecisionValue = checkpoint.value;
            }
        }

        if (
            objective == SearchObjective::RECOVERY_HORIZON
            && rootEdgeIdx >= 0
            && rootEdgeIdx < static_cast<int>(root.edges.size())
        ) {
            auto &rootEdge = root.edges[rootEdgeIdx];
            const auto candidate = recoverySnapshot(endState);
            if (search::preferRecoverySnapshot(candidate, rootEdge.bestRecovery)) {
                rootEdge.bestRecovery = candidate;
            }
        }
    }
}

void search::BattleScumSearcher2::observeReplanCheckpoint(
    const BattleContext &state,
    ReplanCheckpoint &checkpoint
) const {
    if (state.outcome == Outcome::PLAYER_VICTORY) {
        checkpoint.horizonTurns = 2;
        if (!checkpoint.reached) {
            checkpoint.reached = true;
            checkpoint.hp = search::getPostCombatPlayerHp(state);
            checkpoint.effectiveHp = checkpoint.hp;
            checkpoint.potionCount = state.potionCount;
            checkpoint.value = evaluateState(state);
        }
        return;
    }

    if (
        state.outcome == Outcome::UNDECIDED
        && state.inputState == InputState::PLAYER_NORMAL
        && state.turn > rootState->turn
    ) {
        const int horizonTurns = std::min(2, state.turn - rootState->turn);
        // Record the first player decision in each future turn.  Later card
        // plays in that same turn are descendants of the replanning point,
        // not additional lookahead horizon.
        if (horizonTurns <= checkpoint.horizonTurns) {
            return;
        }
        checkpoint.horizonTurns = horizonTurns;
        if (!checkpoint.reached) {
            checkpoint.reached = true;
            checkpoint.hp = std::max(0, state.player.curHp);
            checkpoint.effectiveHp = search::getEffectiveReplanHp(state);
            checkpoint.potionCount = state.potionCount;
            checkpoint.value = evaluateState(state);
        }
    }
}

bool search::BattleScumSearcher2::isTerminalState(const BattleContext &bc) const { // maybe can optimize by making this evaluate directly if score cannot possibly be higher than best
    if (bc.outcome != Outcome::UNDECIDED) {
        return true;
    }
    return objective == SearchObjective::RECOVERY_HORIZON
        && bc.inputState == InputState::PLAYER_NORMAL
        && bc.turn - rootState->turn >= recoveryHorizonTurns;
}

double search::BattleScumSearcher2::evaluateState(const BattleContext &bc) const {
    if (objective == SearchObjective::RECOVERY_HORIZON) {
        const auto snapshot = recoverySnapshot(bc);
        if (bc.outcome == Outcome::PLAYER_VICTORY) {
            return 1.0;
        }
        if (snapshot.reachedHorizon) {
            // The fixed-horizon search learns from survival and setup instead
            // of the complete-combat loss value. Keep the reward bounded for
            // UCT. Final cross-world selection compares the lower tail and
            // mean of the same recovery-state quality across RNG worlds.
            return 0.5 + 0.499999 * std::tanh(snapshot.quality / 3.0);
        }
        const double survivedRatio = std::clamp(
            static_cast<double>(snapshot.survivedTurns)
                / static_cast<double>(std::max(1, recoveryHorizonTurns)),
            0.0,
            1.0
        );
        return -1.0
            + 0.55 * survivedRatio
            + 0.25 * snapshot.enemyProgress;
    }
    return evalFnc(*rootState, bc);
}

search::RecoverySnapshot search::BattleScumSearcher2::recoverySnapshot(
    const BattleContext &bc
) const {
    return search::recoverySnapshot(*rootState, bc, recoveryHorizonTurns);
}

double search::BattleScumSearcher2::getEffectiveMaxBackupWeight(std::int64_t visits) const {
    if (visits <= 0 || maxBackupWeight <= 0.0) {
        return 0.0;
    }

    const auto warmup = std::max<std::int64_t>(0, maxBackupWarmupVisits);
    if (warmup == 0) {
        return maxBackupWeight;
    }

    // A maximum is extremely optimistic after only one or two playouts. Let its
    // influence grow with evidence, while retaining mean backup as the stable
    // signal in newly expanded/deep parts of the tree.
    return maxBackupWeight * static_cast<double>(visits)
           / static_cast<double>(visits + warmup);
}

double search::BattleScumSearcher2::evaluateEdge(const search::BattleScumSearcher2::Node &parent, int edgeIdx) {

    const auto &edge = parent.edges[edgeIdx];

    // unexplored edges must be assigned a sufficiently large value
    // that they are explored at a priority over any other edge
    if (edge.simulationCount == 0) {
        return unexploredNodeValueParameter;
    }

    const double meanValue = edge.evaluationSum / static_cast<double>(edge.simulationCount);
    // Expected rollout value is the stable UCT signal. An optional max component
    // is retained for experiments, but is disabled by default because a large
    // weight made decisions change as the simulation budget increased.
    const double effectiveMaxWeight = getEffectiveMaxBackupWeight(edge.simulationCount);
    const double qualityValue = (1.0 - effectiveMaxWeight) * meanValue
                                + effectiveMaxWeight * edge.maxEvaluation;

    const double explorationValue = explorationParameter *
            std::sqrt(std::log(parent.simulationCount + 1.0) / static_cast<double>(edge.simulationCount));

    const double progressiveBias = progressiveBiasParameter * edge.heuristicPrior
                                   / static_cast<double>(edge.simulationCount + 1);

    return qualityValue + explorationValue + progressiveBias;
}

int search::BattleScumSearcher2::selectBestEdgeToSearch(const search::BattleScumSearcher2::Node &cur) {
    if (cur.edges.size() == 1) {
        return 0;
    }

    if (&cur == &root) {
        int leastVisited = 0;
        for (int i = 1; i < static_cast<int>(cur.edges.size()); ++i) {
            const auto visits = cur.edges[i].simulationCount;
            const auto leastVisits =
                cur.edges[leastVisited].simulationCount;
            if (
                visits < leastVisits
                || (
                    visits == leastVisits
                    && getActionGuidancePrior(
                        *rootState, cur.edges[i].action
                    ) > getActionGuidancePrior(
                        *rootState, cur.edges[leastVisited].action
                    )
                )
            ) {
                leastVisited = i;
            }
        }

        // Root actions are compared after the search, so balanced mode keeps
        // their sample counts comparable. An explicit opt-out uses only a
        // short fair-sampling prefix before returning to normal UCT.
        if (balanceRootActions
            || cur.edges[leastVisited].simulationCount < minRootActionVisits) {
            return leastVisited;
        }

        // Progressive bias decays after only a few visits.  Without this
        // second, bounded sampling phase, a preferred action with unlucky
        // opening rollouts could receive only the ordinary 256 samples while
        // another action received hundreds of thousands.  This floor is
        // generic for every positive prior and does not alter backed-up values.
        if (hasActiveActionGuidance(*rootState)) {
            int leastVisitedPreferred = -1;
            for (int i = 0; i < static_cast<int>(cur.edges.size()); ++i) {
                if (getActionGuidancePrior(*rootState, cur.edges[i].action) <= 0.0
                    || cur.edges[i].simulationCount >= preferredRootActionVisits) {
                    continue;
                }
                if (leastVisitedPreferred == -1
                    || cur.edges[i].simulationCount
                       < cur.edges[leastVisitedPreferred].simulationCount) {
                    leastVisitedPreferred = i;
                }
            }
            if (leastVisitedPreferred != -1) {
                return leastVisitedPreferred;
            }
        }
    }

    auto bestEdge = 0;
    auto bestEdgeValue = evaluateEdge(cur, bestEdge);

    for (int i = 1; i < static_cast<int>(cur.edges.size()); ++i) {
        const auto value = evaluateEdge(cur, i);
        if (value > bestEdgeValue) {
            bestEdge = i;
            bestEdgeValue = value;
        }
    }
    return bestEdge;
}

void search::BattleScumSearcher2::initializeEdgeHeuristics(search::BattleScumSearcher2::Node &node,
                                                           const BattleContext &bc) {
    if (node.edges.empty()) {
        return;
    }

    std::vector<double> scores;
    scores.reserve(node.edges.size());
    for (const auto &edge : node.edges) {
        scores.push_back(
            objective == SearchObjective::RECOVERY_HORIZON
                ? search::evaluateRecoveryActionHeuristic(bc, edge.action)
                : search::evaluateActionHeuristic(bc, edge.action, intent)
        );
    }

    blendActionPriors(
        scores, static_cast<int>(scores.size()), llmGuidanceWeight, [&] (int i) {
            return getActionGuidancePrior(bc, node.edges[i].action);
        }
    );
    for (int i = 0; i < static_cast<int>(node.edges.size()); ++i) {
        node.edges[i].heuristicPrior = scores[i];
    }
}

int search::BattleScumSearcher2::selectFirstActionForLeafNode(const search::BattleScumSearcher2::Node &leafNode,
                                                              const BattleContext &) {
    if (leafNode.edges.size() == 1) {
        return 0;
    }

    std::vector<double> scores;
    scores.reserve(leafNode.edges.size());
    for (const auto &edge : leafNode.edges) {
        scores.push_back(edge.heuristicPrior);
    }
    return pickBestTiedIndex(scores.data(), static_cast<int>(scores.size()), randGen);
}

void search::BattleScumSearcher2::playoutRandom(
    BattleContext &state,
    std::vector<Action> &actionStack,
    ReplanCheckpoint &checkpoint
) {
    const ActionEnumerationOptions opts{
        allowPotions, false, allowedPotionSlotMask
    };
    std::vector<Action> actions;
    int rolloutActionCount = 0;
    while (!isTerminalState(state) && rolloutActionCount < maxRolloutActions) {
        ++simulationIdx;
        ++rolloutActionCount;

        enumerateActions(state, opts, false, actions);
        if (actions.empty()) {
            std::cerr << state.seed << " " << simulationIdx << std::endl;
            std::cerr << state.monsters.arr[0].getName() << " " << state.floorNum << " " << monsterEncouterNames[static_cast<int>(state.encounter)] << std::endl;
            assert(false);
        }

        constexpr double kRandomRolloutChance = 0.12;
        auto distChance = std::uniform_real_distribution<double>(0.0, 1.0);
        const int actionCount = static_cast<int>(actions.size());
        std::array<double, kCommonActionCapacity> fixedActionScores;
        std::vector<double> dynamicActionScores;
        double *actionScores = fixedActionScores.data();
        if (actionCount > kCommonActionCapacity) {
            dynamicActionScores.resize(actionCount);
            actionScores = dynamicActionScores.data();
        }
        for (int i = 0; i < actionCount; ++i) {
            const auto &action = actions[i];
            actionScores[i] =
                objective == SearchObjective::RECOVERY_HORIZON
                    ? search::evaluateRecoveryActionHeuristic(state, action)
                    : search::evaluateActionHeuristic(state, action, intent);
        }
        if (hasActiveActionGuidance(state) && actionCount > 0) {
            blendActionPriors(actionScores, actionCount, llmGuidanceWeight, [&] (int i) {
                return getActionGuidancePrior(state, actions[i]);
            });
        }

        int nonEndTurnCount = 0;
        for (int i = 0; i < actionCount; ++i) {
            if (actions[i].getActionType() != ActionType::END_TURN) {
                ++nonEndTurnCount;
            }
        }

        int selectedIdx;
        if (distChance(randGen) < kRandomRolloutChance && nonEndTurnCount > 0) {
            auto dist = std::uniform_int_distribution<int>(0, nonEndTurnCount - 1);
            int selectedRank = dist(randGen);
            selectedIdx = 0;
            for (; selectedIdx < actionCount; ++selectedIdx) {
                if (actions[selectedIdx].getActionType() == ActionType::END_TURN) {
                    continue;
                }
                if (selectedRank-- == 0) {
                    break;
                }
            }
        } else {
            selectedIdx = pickBestTiedIndex(
                actionScores, actionCount, randGen
            );
        }

        const auto action = actions[selectedIdx];
        actionStack.push_back(action);
        action.execute(state);
        observeReplanCheckpoint(state, checkpoint);
    }
}

void search::BattleScumSearcher2::enumerateActionsForNode(search::BattleScumSearcher2::Node &node,
                                                               const BattleContext &bc) {
    const ActionEnumerationOptions opts{
        allowPotions, allowRootPotions, allowedPotionSlotMask
    };
    std::vector<Action> actions;
    enumerateActions(bc, opts, &node == &root, actions);
    for (auto &action : actions) {
        node.edges.push_back({std::move(action)});
    }

#ifdef sts_print_debug
    std::cout << "{ (" << node.edges.size() << ") ";
    for (int i = 0; i < node.edges.size(); ++i) {
        node.edges[i].action.printDesc(std::cout, bc) << ", ";
    }
    std::cout << " }" << std::endl;
#endif
}

void search::BattleScumSearcher2::enumerateActionsForRollout(search::BattleScumSearcher2::Node &node,
                                                             const BattleContext &bc) {
    node.edges.clear();
    const ActionEnumerationOptions opts{
        allowPotions, false, allowedPotionSlotMask
    };
    std::vector<Action> actions;
    enumerateActions(bc, opts, false, actions);
    for (auto &action : actions) {
        node.edges.push_back({std::move(action)});
    }
}

// Kept as a static member so existing callers (including tests and the
// counterfactual evaluator) can keep using BattleScumSearcher2::evaluateEndState.
double search::BattleScumSearcher2::evaluateEndState(const BattleContext &rootBc, const BattleContext &bc) {
    return search::evaluateEndState(rootBc, bc);
}

// ---------------------------------------------------------------------------
// Debugging utilities: layer walkers used by printSearchTree / printSearchStack.
// ---------------------------------------------------------------------------

struct LayerStruct {
    const search::BattleScumSearcher2::Node *node;
    std::unique_ptr<BattleContext> bc;
    int edgeIdx;
};

typedef std::pair<search::BattleScumSearcher2::Edge, std::unique_ptr<const BattleContext>> EdgeInfo;

std::vector<EdgeInfo> getEdgesForLayer(const search::BattleScumSearcher2 &s, int layerNum) {
    if (layerNum <= 0) {
        return {};
    }

    std::vector<EdgeInfo> layerEdges;

    std::vector<LayerStruct> curStack;
    curStack.push_back({
        &s.root, std::make_unique<BattleContext>(*s.rootState), 0
    });

    while (!curStack.empty()) {
        if (curStack.size() == static_cast<std::size_t>(layerNum)) {
            for (const auto &edge : curStack.back().node->edges) {
                layerEdges.emplace_back(
                    edge,
                    std::make_unique<BattleContext>(*curStack.back().bc)
                );
            }
        }

        // curStack size less than layerNum
        const bool visitedAll = curStack.back().edgeIdx
                                >= static_cast<int>(curStack.back().node->edges.size());
        if (visitedAll || curStack.size() == static_cast<std::size_t>(layerNum)) {
            curStack.pop_back();
            continue;
        }

        // visit next edge
        auto &nextIdx = curStack.back().edgeIdx;
        const auto action = curStack.back().node->edges[nextIdx].action;

        BattleContext bc(*curStack.back().bc);
        action.execute(bc);

        curStack.push_back({
            curStack.back().node->edges[nextIdx++].node.get(),
            std::make_unique<BattleContext>(bc),
            0
        });
    }

    return layerEdges;
}

void search::BattleScumSearcher2::printSearchTree(std::ostream &os, int levels) {
    std::vector<std::vector<EdgeInfo>> layerEdges;
    for (int depth = 1; depth <= levels; ++depth) {
        layerEdges.push_back(getEdgesForLayer(*this, depth));
    }

    for (int depth = 0; depth < levels; ++depth) {
        for (const auto &x : layerEdges[depth]) {
            os << "(" << x.first.node->simulationCount << ")";
            x.first.action.printDesc(os, *x.second) << "\t";
        }
        std::cout << '\n';
    }
}

void search::BattleScumSearcher2::printSearchStack(std::ostream &os) {
    for (std::size_t i = 0; i < actionStack.size(); ++i) {
        os << std::hex << actionStack[i].bits << '\n';
    }
    os.flush();
}
