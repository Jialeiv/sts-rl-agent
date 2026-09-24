//
// Created by keega on 9/17/2021.
//

#ifndef STS_LIGHTSPEED_BATTLESCUMSEARCHER2_H
#define STS_LIGHTSPEED_BATTLESCUMSEARCHER2_H

#include "sim/search/Action.h"
#include "sim/search/BattleEvaluator.h"
#include "sim/search/SearchTypes.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <random>
#include <iostream>
#include <limits>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sts::search {

    // SearchIntent, SearchObjective and RecoverySnapshot live in SearchTypes.h;
    // state evaluation helpers live in BattleEvaluator.h.

    typedef std::function<double (const BattleContext&, const BattleContext&)> EvalFnc;

    // Accumulates the outcome of one playout for a node or an edge.  Node and
    // Edge track exactly the same rollout statistics; keeping them in one type
    // guarantees the backup logic in updateFromPlayout and the terminal-root
    // shortcuts in search() stay consistent with each other.
    struct PlayoutStats {
        std::int64_t simulationCount = 0;
        double evaluationSum = 0;
        double evaluationSquaredSum = 0;
        double maxEvaluation = std::numeric_limits<double>::lowest();
        std::int64_t winSamples = 0;
        std::int64_t lossSamples = 0;
        std::int64_t cutoffSamples = 0;
        double winEndHpSum = 0;
        double winEndHpSquaredSum = 0;
        double winUtilitySum = 0;
        bool foundWinningLine = false;
        double bestWinUtility = 0;
        int bestWinEndHp = 0;
        int bestWinPotionCount = 0;
        std::int64_t lizardTailConsumedWinSamples = 0;
        double escapedStolenGoldWinSum = 0;

        void reset() {
            *this = PlayoutStats{};
        }

        // Records one finished playout: increments the visit/evaluation
        // counters and folds the terminal outcome into the win/loss/cutoff
        // sample distributions.  escapedStolenGold is the gold a thief took
        // in this playout, computed once per playout by the caller.
        void record(
            double evaluation,
            const BattleContext &endState,
            double winUtility,
            bool consumedLizardTail,
            int escapedStolenGold
        ) {
            ++simulationCount;
            evaluationSum += evaluation;
            evaluationSquaredSum += evaluation * evaluation;
            maxEvaluation = std::max(maxEvaluation, evaluation);

            switch (endState.outcome) {
                case Outcome::PLAYER_VICTORY: {
                    ++winSamples;
                    const int postCombatHp = getPostCombatPlayerHp(endState);
                    const double endHp = static_cast<double>(postCombatHp);
                    winEndHpSum += endHp;
                    winEndHpSquaredSum += endHp * endHp;
                    const double boundedWinUtility = std::max(0.0, winUtility);
                    winUtilitySum += boundedWinUtility;
                    if (
                        !foundWinningLine
                        || boundedWinUtility > bestWinUtility
                        || (
                            boundedWinUtility == bestWinUtility
                            && postCombatHp > bestWinEndHp
                        )
                        || (
                            boundedWinUtility == bestWinUtility
                            && postCombatHp == bestWinEndHp
                            && endState.potionCount > bestWinPotionCount
                        )
                    ) {
                        foundWinningLine = true;
                        bestWinUtility = boundedWinUtility;
                        bestWinEndHp = postCombatHp;
                        bestWinPotionCount = endState.potionCount;
                    }
                    escapedStolenGoldWinSum += static_cast<double>(escapedStolenGold);
                    if (consumedLizardTail) {
                        ++lizardTailConsumedWinSamples;
                    }
                    break;
                }
                case Outcome::PLAYER_LOSS:
                    ++lossSamples;
                    break;
                case Outcome::UNDECIDED:
                    ++cutoffSamples;
                    break;
            }
        }
    };

    // to find a solution to a battle with tree pruning
    struct BattleScumSearcher2 {
        struct Edge;

        // Node is declared first because it owns a vector of Edges; Edge then
        // references the child node through a shared pointer.  Both inherit
        // the shared rollout statistics from PlayoutStats.
        struct Node : public PlayoutStats {
            std::vector<Edge> edges;
        };

        struct Edge : public PlayoutStats {
            Edge() = default;
            Edge(Action action) : action(action) {}

            Action action;
            std::shared_ptr<Node> node = std::make_shared<Node>();
            // Final root selection compares the best complete plan found in
            // each equally weighted RNG world. Rollout hit counts remain UCT
            // diagnostics; they are not a calibrated probability because UCT
            // deliberately visits continuations unevenly.
            // Best safe replanning checkpoint discovered below this root
            // action.  This is intentionally a maximum within one fixed RNG
            // world; callers average those maxima across worlds.
            bool reachedNextDecision = false;
            int bestReplanHorizonTurns = 0;
            int bestNextDecisionHp = 0;
            int bestNextDecisionEffectiveHp = 0;
            int bestNextDecisionPotionCount = 0;
            double bestNextDecisionValue =
                std::numeric_limits<double>::lowest();
            RecoverySnapshot bestRecovery;
            double heuristicPrior = 0;
        };

        struct ReplanCheckpoint {
            bool reached = false;
            int horizonTurns = 0;
            int hp = 0;
            int effectiveHp = 0;
            int potionCount = 0;
            double value = std::numeric_limits<double>::lowest();
        };

        std::unique_ptr<const BattleContext> rootState;
        Node root;
        std::chrono::steady_clock::time_point startTime;

        // --- UCT tuning ---
        EvalFnc evalFnc;
        double unexploredNodeValueParameter = 100.0; // only needs to be large enough to be larger than any realistic value of the quality term + the exploration term
        double explorationParameter = sqrt(2);
        double progressiveBiasParameter = 0.35;
        // Standard UCT backs up expected rollout value. Max backup remains an
        // opt-in experiment because it made root choices budget-sensitive.
        double maxBackupWeight = 0.0;
        std::int64_t maxBackupWarmupVisits = 64;

        // --- Search results (written by search()) ---
        double bestActionValue = std::numeric_limits<double>::lowest();
        double minActionValue = std::numeric_limits<double>::max();
        int outcomePlayerHp = 0;
        std::string stopReason;

        // --- Potion control ---
        bool allowPotions = false;
        bool allowRootPotions = false;
        // A non-zero mask enables only the selected inventory slots at every
        // search and rollout node. This is the harness-facing potion mode.
        std::uint32_t allowedPotionSlotMask = 0;

        // --- Root balancing ---
        // Root actions are the alternatives compared by the caller, so give
        // them equal evidence. Descendants still use ordinary UCT to focus the
        // search on promising continuations. Without a balanced root, early
        // stochastic results made action estimates differ by orders of
        // magnitude in visit count and change as the budget increased.
        bool balanceRootActions = true;
        // Callers may skip rollout evaluation when legal-action enumeration
        // proves that the current decision has only one possible root action.
        bool stopOnForcedRootAction = false;
        // If a caller explicitly opts out of full root balancing, still give
        // every root action enough evidence in every RNG world before UCT is
        // allowed to concentrate the remaining budget.
        std::int64_t minRootActionVisits = 2048;
        // A positive LLM prior is a request to evaluate an action, not to force
        // it.  Guarantee enough root samples for that evidence to become
        // meaningful, then let the same value-based UCT policy decide.
        std::int64_t preferredRootActionVisits = 8192;

        // --- Objective / search budget ---
        SearchIntent intent = SearchIntent::SURVIVAL_FIRST;
        SearchObjective objective = SearchObjective::COMPLETE_COMBAT;
        int recoveryHorizonTurns = 2;
        int maxTreeDepth = 500;
        int maxRolloutActions = 500;

        // --- Optional LLM action guidance ---
        // These apply only to the player turn on which they were authored,
        // influence exploration/rollouts, and never alter backed-up rewards
        // or final root-action statistics.
        int guidanceTurn = -1;
        double llmGuidanceWeight = 1.25;
        std::vector<std::string> preferredCardNames;
        std::vector<std::string> discouragedCardNames;

        // --- Per-search scratch state ---
        std::vector<Action> bestActionSequence;
        std::default_random_engine randGen;

        std::vector<Node*> searchStack;
        std::vector<int> edgeIdxStack;
        std::vector<Action> actionStack;
        std::unordered_map<std::uint64_t, std::shared_ptr<Node>> transpositionTable;

        explicit BattleScumSearcher2(const BattleContext &bc, EvalFnc evalFnc=&evaluateEndState);

        // public methods
        void search(int64_t simulations, long maxTimeMillis);
        void step();
        void setActionGuidance(
            int turn,
            std::vector<std::string> prefer,
            std::vector<std::string> discourage
        );
        [[nodiscard]] bool hasActiveActionGuidance(const BattleContext &bc) const;
        [[nodiscard]] double getActionGuidancePrior(
            const BattleContext &bc,
            const Action &action
        ) const;
        [[nodiscard]] static bool canUseImmediateVictoryShortcut(
            const BattleContext &bc
        );

        // private helpers
        void updateFromPlayout(const std::vector<Node*> &stack,
                              const std::vector<int> &edgeIdxStack,
                              const std::vector<Action> &actionStack,
                              const BattleContext &endState,
                              const ReplanCheckpoint &checkpoint);
        void updateFromPlayout(const std::vector<Node*> &stack,
                              const std::vector<int> &edgeIdxStack,
                              const std::vector<Action> &actionStack,
                              const BattleContext &endState) {
            updateFromPlayout(
                stack, edgeIdxStack, actionStack, endState, ReplanCheckpoint{}
            );
        }
        [[nodiscard]] bool isTerminalState(const BattleContext &bc) const;
        [[nodiscard]] double evaluateState(const BattleContext &bc) const;
        [[nodiscard]] RecoverySnapshot recoverySnapshot(
            const BattleContext &bc
        ) const;
        [[nodiscard]] double getEffectiveMaxBackupWeight(std::int64_t visits) const;

        double evaluateEdge(const Node &parent, int edgeIdx);
        int selectBestEdgeToSearch(const Node &cur);
        int selectFirstActionForLeafNode(const Node &leafNode, const BattleContext &state);

        void playoutRandom(BattleContext &state,
                           std::vector<Action> &actionStack,
                           ReplanCheckpoint &checkpoint);
        void observeReplanCheckpoint(const BattleContext &state,
                                     ReplanCheckpoint &checkpoint) const;

        // Thin wrappers around sts::search::enumerateActions that apply this
        // searcher's potion flags and wrap the resulting actions into edges.
        void enumerateActionsForNode(Node &node, const BattleContext &bc);
        void enumerateActionsForRollout(Node &node, const BattleContext &bc);
        // One-pass leaf expansion: execute each action on a scratch state once,
        // use it for both transposition dedup and the heuristic prior.
        void expandLeafNode(Node &node, const BattleContext &bc);
        // Heuristic priors only (no state execution); used by tests.
        void initializeEdgeHeuristics(Node &node, const BattleContext &bc);
        // Transposition nodes can be reached from states with different legal
        // actions. Keep this release-build guard; it is intentionally in-place
        // to avoid the old per-visit vector allocation.
        static void pruneInvalidEdgesForState(Node &node, const BattleContext &bc);
        [[nodiscard]] std::uint64_t buildStateKey(const BattleContext &bc) const;
        [[nodiscard]] std::uint64_t buildComparableReplanStateKey(
            const BattleContext &bc,
            bool normalizeMonotonicProgress = false
        ) const;
        [[nodiscard]] std::uint64_t buildLocalActionOrderingStateKey(
            const BattleContext &bc
        ) const;
        [[nodiscard]] std::uint64_t buildStrictBlockBoundaryStateKey(
            const BattleContext &bc,
            bool canonicalizeShuffledDrawPile = false
        ) const;
        [[nodiscard]] bool shouldDedupState(const BattleContext &bc) const;
        static double evaluateEndState(const BattleContext &rootBc, const BattleContext &bc);

        void printSearchTree(std::ostream &os, int levels);
        void printSearchStack(std::ostream &os);
    };

    extern thread_local BattleScumSearcher2 *g_debug_scum_search;

}


#endif //STS_LIGHTSPEED_BATTLESCUMSEARCHER2_H
