#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <thread>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>


#include "combat/BattleContext.h"
#include "convert/BattleConverter.h"
#include "sim/ConsoleSimulator.h"
#include "sim/search/BattleScumSearcher2.h"
#include "sim/search/RecoveryRootPolicy.h"
#include "sim/search/RootActionPolicy.h"
#include "constants/Potions.h"

#include <nlohmann/json.hpp>

using namespace sts;
using namespace std::chrono;


int getMonotonicCardProgress(const BattleContext &bc) {
    int total = bc.powersPlayedThisCombat
        + bc.player.getStatus<PS::SELF_REPAIR>();
    const auto addCard = [&total] (const CardInstance &card) {
        if (card.usesSpecialData()) {
            const int value = std::max(0, static_cast<int>(card.specialData));
            total += card.getId() == CardId::STEAM_BARRIER ? -value : value;
        }
    };
    for (int i = 0; i < bc.cards.cardsInHand; ++i) {
        addCard(bc.cards.hand[i]);
    }
    for (const auto &card : bc.cards.drawPile) {
        addCard(card);
    }
    for (const auto &card : bc.cards.discardPile) {
        addCard(card);
    }
    for (const auto &card : bc.cards.exhaustPile) {
        addCard(card);
    }
    for (const auto &card : bc.cards.stasisCards) {
        addCard(card);
    }
    return total;
}

void search2(search::BattleScumSearcher2 &searcher, int simulations, long maxTimeMillis) {
    searcher.search(simulations, maxTimeMillis);
}

struct RecoverySearchResult {
    int selectedAction = -1;
    long elapsedMillis = 0;
    std::string stopReason;
    std::vector<search::RecoveryRootCandidate> candidates;
};

constexpr int kRecoveryHorizonTurns = 2;

RecoverySearchResult runRecoveryHorizonSearch(
    const BattleContext &baseBc,
    const std::vector<search::Action> &rootActions,
    int simulationsPerThread,
    int threadCount,
    long maxTimeMillis,
    std::uint32_t allowedPotionSlotMask
) {
    const auto start = steady_clock::now();
    std::vector<std::unique_ptr<search::BattleScumSearcher2>> searchers;
    std::vector<std::thread> threads;
    searchers.reserve(threadCount);
    threads.reserve(threadCount);
    for (int i = 0; i < threadCount; ++i) {
        BattleContext world = baseBc;
        world.cardRandomRng.setCounter(i * 1000);
        world.aiRng.setCounter(i * 1000);
        world.shuffleRng.setCounter(i * 1000);
        world.miscRng.setCounter(i * 1000);
        world.potionRng.setCounter(i * 1000);
        auto searcher = std::make_unique<search::BattleScumSearcher2>(world);
        searcher->objective = search::SearchObjective::RECOVERY_HORIZON;
        searcher->recoveryHorizonTurns = kRecoveryHorizonTurns;
        searcher->allowedPotionSlotMask = allowedPotionSlotMask;
        searcher->balanceRootActions = true;
        searchers.push_back(std::move(searcher));
    }
    for (auto &searcher : searchers) {
        threads.emplace_back(
            search2,
            std::ref(*searcher),
            simulationsPerThread,
            maxTimeMillis
        );
    }
    for (auto &thread : threads) {
        thread.join();
    }

    RecoverySearchResult result;
    result.elapsedMillis = duration_cast<milliseconds>(
        steady_clock::now() - start
    ).count();
    if (!searchers.empty()) {
        result.stopReason = searchers.front()->stopReason;
    }
    result.candidates.reserve(rootActions.size());
    for (const auto &rootAction : rootActions) {
        std::vector<search::RecoverySnapshot> worlds;
        double actionPriorSum = 0.0;
        int actionPriorWorlds = 0;
        worlds.reserve(searchers.size());
        for (const auto &searcher : searchers) {
            const auto edge = std::find_if(
                searcher->root.edges.begin(),
                searcher->root.edges.end(),
                [&rootAction] (const auto &candidate) {
                    return candidate.action == rootAction;
                }
            );
            if (edge != searcher->root.edges.end()) {
                worlds.push_back(edge->bestRecovery);
                actionPriorSum += edge->heuristicPrior;
                ++actionPriorWorlds;
            }
        }
        result.candidates.push_back(
            search::aggregateRecoveryWorlds(worlds)
        );
        if (actionPriorWorlds > 0) {
            result.candidates.back().meanActionPrior =
                actionPriorSum / static_cast<double>(actionPriorWorlds);
        }
        // Use irreversible root damage only after the fixed-horizon states
        // are otherwise equivalent.  This breaks the exact tie between an
        // attack and expiring block against a sleeping enemy without changing
        // exploration or overriding a healthier/setup-rich future.
        BattleContext afterRoot(baseBc);
        const double hpBefore = search::getNonMinionMonsterCurHpTotal(afterRoot);
        rootAction.execute(afterRoot);
        result.candidates.back().immediateEnemyDamage = std::max(
            0.0,
            hpBefore - search::getNonMinionMonsterCurHpTotal(afterRoot)
        );
    }
    result.selectedAction = search::selectRecoveryRootAction(
        result.candidates
    );
    return result;
}

std::string selectedCardNameForTask(const BattleContext &bc, CardSelectTask task, int idx) {
    switch (task) {
        case CardSelectTask::CODEX:
            if (idx == 3) {
                return "skip";
            }
            return CardInstance(bc.cardSelectInfo.codexCards()[idx]).getName();
        case CardSelectTask::DISCOVERY:
            return CardInstance(bc.cardSelectInfo.discovery_Cards()[idx]).getName();
        case CardSelectTask::TOOLBOX:
            return CardInstance(bc.cardSelectInfo.toolboxCards()[idx]).getName();
        case CardSelectTask::HOLOGRAM:
        case CardSelectTask::LIQUID_MEMORIES_POTION:
        case CardSelectTask::HEADBUTT:
        case CardSelectTask::MEDITATE:
            return bc.cards.discardPile[idx].getName();
        case CardSelectTask::EXHUME:
            return bc.cards.exhaustPile[idx].getName();
        case CardSelectTask::SECRET_TECHNIQUE:
        case CardSelectTask::SECRET_WEAPON:
        case CardSelectTask::SEEK:
            return bc.cards.drawPile[idx].getName();
        case CardSelectTask::EXHAUST_ONE:
        case CardSelectTask::FORETHOUGHT:
        case CardSelectTask::NIGHTMARE:
        case CardSelectTask::RECYCLE:
        case CardSelectTask::SETUP:
        case CardSelectTask::WARCRY:
        case CardSelectTask::ARMAMENTS:
        case CardSelectTask::DUAL_WIELD:
            return bc.cards.hand[idx].getName();
        default:
            return "";
    }
}

CardInstance selectedCardForTask(
    const BattleContext &bc,
    CardSelectTask task,
    int idx
) {
    switch (task) {
        case CardSelectTask::CODEX:
            return idx == 3
                ? CardInstance()
                : CardInstance(bc.cardSelectInfo.codexCards()[idx]);
        case CardSelectTask::DISCOVERY:
            return CardInstance(bc.cardSelectInfo.discovery_Cards()[idx]);
        case CardSelectTask::TOOLBOX:
            return CardInstance(bc.cardSelectInfo.toolboxCards()[idx]);
        case CardSelectTask::HOLOGRAM:
        case CardSelectTask::LIQUID_MEMORIES_POTION:
        case CardSelectTask::HEADBUTT:
        case CardSelectTask::MEDITATE:
            return bc.cards.discardPile[idx];
        case CardSelectTask::EXHUME:
            return bc.cards.exhaustPile[idx];
        case CardSelectTask::SECRET_TECHNIQUE:
        case CardSelectTask::SECRET_WEAPON:
        case CardSelectTask::SEEK:
            return bc.cards.drawPile[idx];
        case CardSelectTask::EXHAUST_ONE:
        case CardSelectTask::FORETHOUGHT:
        case CardSelectTask::NIGHTMARE:
        case CardSelectTask::RECYCLE:
        case CardSelectTask::SETUP:
        case CardSelectTask::WARCRY:
        case CardSelectTask::ARMAMENTS:
        case CardSelectTask::DUAL_WIELD:
            return bc.cards.hand[idx];
        default:
            return CardInstance();
    }
}

nlohmann::json selectionCardJson(const CardInstance &card, int sourceIndex) {
    if (card.getId() == CardId::INVALID) {
        return nullptr;
    }
    return {
        {"sourceIndex", sourceIndex},
        {"id", getCardStringId(card.getId())},
        {"name", card.getName()},
        {"upgrades", card.getUpgradeCount()},
        {"cost", card.costForTurn}
    };
}

nlohmann::json describeCardSelection(
    const search::Action &action,
    const BattleContext &bc
) {
    const auto actionType = action.getActionType();
    if (
        actionType != search::ActionType::SINGLE_CARD_SELECT
        && actionType != search::ActionType::MULTI_CARD_SELECT
    ) {
        return nullptr;
    }

    const auto task = bc.cardSelectInfo.cardSelectTask;
    nlohmann::json cards = nlohmann::json::array();
    std::string completionCommand;
    if (actionType == search::ActionType::SINGLE_CARD_SELECT) {
        const int idx = action.getSelectIdx();
        if (task == CardSelectTask::CODEX && idx == 3) {
            completionCommand = "skip";
        } else {
            cards.push_back(selectionCardJson(
                selectedCardForTask(bc, task, idx),
                idx
            ));
        }
    } else {
        for (const int idx : action.getSelectedIdxs()) {
            cards.push_back(selectionCardJson(bc.cards.hand[idx], idx));
        }
        completionCommand = "confirm";
    }

    return {
        {"kind", "card_selection"},
        {"task", cardSelectTaskStrings[static_cast<int>(task)]},
        {"cards", cards},
        {"completionCommand",
         completionCommand.empty()
             ? nlohmann::json(nullptr)
             : nlohmann::json(completionCommand)}
    };
}

int getGameTargetIdx(const int *monsterIdxMap, int targetIdx) {
    if (targetIdx < 0 || targetIdx >= 5 || monsterIdxMap == nullptr) {
        return targetIdx;
    }
    return monsterIdxMap[targetIdx];
}

std::string describeAction(
    const search::Action &action,
    const BattleContext &bc,
    const int *monsterIdxMap,
    bool executableCardSelect = false
) {
    if (!action.isValidAction(bc)) {
        return "{ INVALID ACTION }";
    }

    std::ostringstream os;
    switch (action.getActionType()) {
        case search::ActionType::CARD: {
            const auto &card = bc.cards.hand[action.getSourceIdx()];
            os << "play " << action.getSourceIdx() + 1;
            if (card.requiresTarget()) {
                // const auto &monster = bc.monsters.arr[action.getTargetIdx()];
                os << " " << getGameTargetIdx(monsterIdxMap, action.getTargetIdx());
            }
            return os.str();
        }
        case search::ActionType::POTION: {
            const auto potion = bc.potions[action.getSourceIdx()];
            if (action.getTargetIdx() == -1) {
                os << "potion discard ";
            } else {
                os << "potion use ";
            }
            os << action.getSourceIdx();
            if (potionRequiresTarget(potion)) {
                // const auto &monster = bc.monsters.arr[action.getTargetIdx()];
                os << " " << getGameTargetIdx(monsterIdxMap, action.getTargetIdx());
            }
            return os.str();
        }
        case search::ActionType::SINGLE_CARD_SELECT: {
            const auto task = bc.cardSelectInfo.cardSelectTask;
            const auto idx = action.getSelectIdx();
            if (executableCardSelect) {
                if (task == CardSelectTask::CODEX && idx == 3) {
                    return "skip";
                }
                os << "choose";
                os << " " << idx;
                return os.str();
            }
            os << "choose";
            // os << "{ " << cardSelectTaskStrings[static_cast<int>(task)] << " (" << idx << ")";
            const auto name = selectedCardNameForTask(bc, task, idx);
            if (!name.empty()) {
                os << " (" << name << ")";
            }
            // os << " }";
            return os.str();
        }
        case search::ActionType::MULTI_CARD_SELECT: {
            const auto selected = action.getSelectedIdxs();
            os << "choose";
            // os << "{ " << cardSelectTaskStrings[static_cast<int>(task)];
            if (selected.empty()) {
                os << " none";
            } else {
                for (int i = 0; i < selected.size(); ++i) {
                    const auto idx = selected[i];
                    os << " (" <<bc.cards.hand[idx].getName()<< ")";
                    if (i + 1 < selected.size()) {
                        os << ",";
                    }
                }
            }
            // os << " }";
            return os.str();
        }
        case search::ActionType::END_TURN: {
            os << "end";
            return os.str();
        }
        default:
            action.printDesc(os, bc);
            return os.str();
    }
}

const search::BattleScumSearcher2::Edge *bestMandatoryContinuationEdge(
    const search::BattleScumSearcher2::Node &node,
    const BattleContext &state
) {
    const search::BattleScumSearcher2::Edge *best = nullptr;
    const auto meanValue = [] (const auto &edge) {
        return edge.simulationCount > 0
            ? edge.evaluationSum / static_cast<double>(edge.simulationCount)
            : std::numeric_limits<double>::lowest();
    };
    const auto better = [&meanValue] (const auto &candidate, const auto &incumbent) {
        if (candidate.foundWinningLine != incumbent.foundWinningLine) {
            return candidate.foundWinningLine;
        }
        if (
            candidate.foundWinningLine
            && candidate.bestWinUtility != incumbent.bestWinUtility
        ) {
            return candidate.bestWinUtility > incumbent.bestWinUtility;
        }
        if (candidate.maxEvaluation != incumbent.maxEvaluation) {
            return candidate.maxEvaluation > incumbent.maxEvaluation;
        }
        if (meanValue(candidate) != meanValue(incumbent)) {
            return meanValue(candidate) > meanValue(incumbent);
        }
        return candidate.simulationCount > incumbent.simulationCount;
    };
    for (const auto &edge : node.edges) {
        if (!edge.action.isValidAction(state)) {
            continue;
        }
        if (best == nullptr || better(edge, *best)) {
            best = &edge;
        }
    }
    return best;
}

// sampleFromEdge / candidateFromAggregate live in sim/search/RootActionPolicy.h
// so battle-agent and battle-sim aggregate root evidence identically.

struct ControlledContinuationEvaluation {
    search::Action action;
    search::RootActionAggregate aggregate;
    std::vector<std::pair<
        search::Action,
        search::RootActionAggregate
    >> candidates;
};

struct AdaptiveSearchAssessment {
    bool extend = false;
    std::string reason;
    bool credibleWins = false;
    int winningRootActions = 0;
    int bestWinningWorlds = 0;
    int rngWorlds = 0;
    double meanTranspositionStates = 0.0;
};

void annotateCertifiedImmediateWin(
    search::RootActionCandidate &candidate,
    const search::Action &action,
    const std::vector<search::BattleScumSearcher2 *> &searchers
) {
    bool wins = action.getActionType() != search::ActionType::END_TURN;
    bool losesResources = false;
    double utilitySum = 0.0;
    for (const auto *searcher : searchers) {
        BattleContext immediate(*searcher->rootState);
        action.execute(immediate);
        if (immediate.outcome != Outcome::PLAYER_VICTORY) {
            wins = false;
            break;
        }
        utilitySum += search::BattleScumSearcher2::evaluateEndState(
            *searcher->rootState,
            immediate
        );
        BattleContext neutral(*searcher->rootState);
        neutral.outcome = Outcome::PLAYER_VICTORY;
        losesResources = losesResources
            || search::BattleScumSearcher2::evaluateEndState(
                *searcher->rootState,
                immediate
            ) + 1e-12 < search::BattleScumSearcher2::evaluateEndState(
                *searcher->rootState,
                neutral
            );
    }
    candidate.certifiedImmediateWin = wins && losesResources;
    candidate.certifiedImmediateWinUtility = wins
        ? utilitySum / static_cast<double>(searchers.size())
        : 0.0;
}

AdaptiveSearchAssessment assessAdaptiveSearch(
    const std::vector<search::BattleScumSearcher2 *> &searchers,
    const BattleContext &rootState
) {
    AdaptiveSearchAssessment assessment;
    if (searchers.empty() || searchers.front()->root.edges.size() < 2) {
        return assessment;
    }

    std::vector<search::RootActionCandidate> candidates;
    candidates.reserve(searchers.front()->root.edges.size());
    for (const auto &referenceEdge : searchers.front()->root.edges) {
        std::vector<search::RootActionThreadSample> samples;
        samples.reserve(searchers.size());
        for (const auto *searcher : searchers) {
            const auto edge = std::find_if(
                searcher->root.edges.begin(),
                searcher->root.edges.end(),
                [&referenceEdge] (const auto &candidate) {
                    return candidate.action == referenceEdge.action;
                }
            );
            if (edge != searcher->root.edges.end()) {
                samples.push_back(search::sampleFromEdge(*edge));
            }
        }
        const auto aggregate = search::aggregateRootActionThreads(samples);
        if (aggregate.rngWorlds == 0) {
            continue;
        }
        candidates.push_back(search::candidateFromAggregate(
            aggregate,
            rootState,
            referenceEdge.action.getActionType()
                == search::ActionType::END_TURN,
            search::RootCandidateOptions{true, true}
        ));
        annotateCertifiedImmediateWin(
            candidates.back(), referenceEdge.action, searchers
        );
        assessment.rngWorlds = std::max(
            assessment.rngWorlds,
            aggregate.rngWorlds
        );
        assessment.bestWinningWorlds = std::max(
            assessment.bestWinningWorlds,
            aggregate.winningRngWorlds
        );
        if (aggregate.winningRngWorlds > 0) {
            ++assessment.winningRootActions;
        }
    }

    for (const auto *searcher : searchers) {
        assessment.meanTranspositionStates += static_cast<double>(
            searcher->transpositionTable.size()
        );
    }
    assessment.meanTranspositionStates /= static_cast<double>(
        searchers.size()
    );
    assessment.credibleWins = search::hasCredibleWinEvidence(candidates);
    const auto certified = std::find_if(
        candidates.begin(),
        candidates.end(),
        [] (const auto &candidate) {
            return candidate.certifiedImmediateWin;
        }
    );
    if (certified != candidates.end()) {
        const bool unresolvedRiskGate = std::any_of(
            candidates.begin(),
            candidates.end(),
            [&certified] (const auto &candidate) {
                const double conditionalUtility = candidate.winSampleRate > 0.0
                    ? candidate.successUtility / candidate.winSampleRate
                    : 0.0;
                return !candidate.certifiedImmediateWin
                    && candidate.winWorldRate >= 1.0
                    && candidate.winSampleRate
                       >= search::kCertifiedWinAlternativeMinRate
                    && candidate.lowerQuartileWinSampleRate
                       < search::kCertifiedWinAlternativeMinRate
                    && conditionalUtility
                       > certified->certifiedImmediateWinUtility;
            }
        );
        if (unresolvedRiskGate) {
            assessment.extend = true;
            assessment.reason = "certified_win_risk_gate_near_threshold";
            return assessment;
        }
    }
    if (assessment.credibleWins || assessment.bestWinningWorlds == 0) {
        return assessment;
    }

    // A single lucky leaf is not enough reason to triple every difficult
    // combat's budget. Multiple winning roots, or one root that reaches at
    // least a third of the independently searched RNG worlds, shows that a
    // real complete-combat basin exists but has not converged yet.
    const bool broadSparseEvidence = std::any_of(
        candidates.begin(),
        candidates.end(),
        [] (const search::RootActionCandidate &candidate) {
            return candidate.rngWorlds > 0
                && candidate.winningRngWorlds * 3 >= candidate.rngWorlds;
        }
    );
    if (assessment.winningRootActions >= 2 || broadSparseEvidence) {
        assessment.extend = true;
        assessment.reason = "sparse_complete_combat_wins";
    }
    return assessment;
}

std::optional<BattleContext> executeControlledRootAction(
    const BattleContext &rootState,
    const search::Action &rootAction,
    const std::optional<ControlledContinuationEvaluation> &continuation
) {
    if (!rootAction.isValidAction(rootState)) {
        return std::nullopt;
    }
    BattleContext state(rootState);
    rootAction.execute(state);
    if (state.inputState == InputState::CARD_SELECT) {
        if (
            !continuation
            || !continuation->action.isValidAction(state)
        ) {
            return std::nullopt;
        }
        continuation->action.execute(state);
    }
    if (
        state.outcome != Outcome::UNDECIDED
        || state.inputState != InputState::PLAYER_NORMAL
        || state.turn != rootState.turn
    ) {
        return std::nullopt;
    }
    return state;
}

CardInstance *findCardByUniqueId(BattleContext &state, int uniqueId) {
    for (int i = 0; i < state.cards.cardsInHand; ++i) {
        if (state.cards.hand[i].uniqueId == uniqueId) {
            return &state.cards.hand[i];
        }
    }
    for (auto &card : state.cards.drawPile) {
        if (card.uniqueId == uniqueId) {
            return &card;
        }
    }
    for (auto &card : state.cards.discardPile) {
        if (card.uniqueId == uniqueId) {
            return &card;
        }
    }
    for (auto &card : state.cards.exhaustPile) {
        if (card.uniqueId == uniqueId) {
            return &card;
        }
    }
    return nullptr;
}

bool normalizeNoWorseCardUpgrades(
    BattleContext &candidate,
    const BattleContext &baseline,
    bool &strictlyUpgraded
) {
    const auto compareCard = [
        &candidate,
        &strictlyUpgraded
    ] (const CardInstance &baselineCard) {
        auto *candidateCard = findCardByUniqueId(
            candidate,
            baselineCard.uniqueId
        );
        if (
            candidateCard == nullptr
            || candidateCard->getUpgradeCount()
               < baselineCard.getUpgradeCount()
        ) {
            return false;
        }
        strictlyUpgraded = strictlyUpgraded
            || candidateCard->getUpgradeCount()
               > baselineCard.getUpgradeCount();
        candidateCard->upgraded = baselineCard.upgraded;
        candidateCard->cost = baselineCard.cost;
        candidateCard->costForTurn = baselineCard.costForTurn;
        return true;
    };
    for (int i = 0; i < baseline.cards.cardsInHand; ++i) {
        if (!compareCard(baseline.cards.hand[i])) {
            return false;
        }
    }
    for (const auto &card : baseline.cards.drawPile) {
        if (!compareCard(card)) {
            return false;
        }
    }
    for (const auto &card : baseline.cards.discardPile) {
        if (!compareCard(card)) {
            return false;
        }
    }
    for (const auto &card : baseline.cards.exhaustPile) {
        if (!compareCard(card)) {
            return false;
        }
    }
    return true;
}

// A CARD_SELECT opened by a card or potion is controlled by the player, not
// chance.  Evaluate the parent action through its best legal selection child;
// averaging exploratory visits to deliberately inferior choices otherwise
// penalizes actions merely for offering more choices.
std::optional<ControlledContinuationEvaluation>
evaluateControlledCardSelection(
    const std::vector<search::BattleScumSearcher2 *> &searchers,
    const search::Action &rootAction
) {
    if (searchers.empty()) {
        return std::nullopt;
    }
    const auto referenceParent = std::find_if(
        searchers[0]->root.edges.begin(),
        searchers[0]->root.edges.end(),
        [&rootAction] (const auto &edge) {
            return edge.action == rootAction;
        }
    );
    if (
        referenceParent == searchers[0]->root.edges.end()
        || referenceParent->node == nullptr
    ) {
        return std::nullopt;
    }

    BattleContext selectionState(*searchers[0]->rootState);
    rootAction.execute(selectionState);
    if (selectionState.inputState != InputState::CARD_SELECT) {
        return std::nullopt;
    }

    std::vector<search::Action> actions;
    std::vector<search::RootActionAggregate> aggregates;
    std::vector<search::RootActionCandidate> candidates;
    for (const auto &referenceChild : referenceParent->node->edges) {
        std::vector<search::RootActionThreadSample> samples;
        samples.reserve(searchers.size());
        for (const auto *searcher : searchers) {
            const auto parent = std::find_if(
                searcher->root.edges.begin(),
                searcher->root.edges.end(),
                [&rootAction] (const auto &edge) {
                    return edge.action == rootAction;
                }
            );
            if (
                parent == searcher->root.edges.end()
                || parent->node == nullptr
            ) {
                continue;
            }
            const auto child = std::find_if(
                parent->node->edges.begin(),
                parent->node->edges.end(),
                [&referenceChild] (const auto &edge) {
                    return edge.action == referenceChild.action;
                }
            );
            if (child != parent->node->edges.end()) {
                samples.push_back(search::sampleFromEdge(*child));
            }
        }
        const auto aggregate = search::aggregateRootActionThreads(samples);
        if (aggregate.rngWorlds == 0) {
            continue;
        }
        actions.push_back(referenceChild.action);
        aggregates.push_back(aggregate);
        candidates.push_back(search::candidateFromAggregate(
            aggregate,
            selectionState,
            false,
            search::RootCandidateOptions{true, true}
        ));
    }
    const int selected = search::selectRootActionByWorldBest(candidates);
    if (selected < 0) {
        return std::nullopt;
    }
    ControlledContinuationEvaluation result{
        actions[selected],
        aggregates[selected],
        {},
    };
    result.candidates.reserve(actions.size());
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        result.candidates.emplace_back(actions[i], aggregates[i]);
    }
    return result;
}

bool endCurrentTurn(BattleContext &state, int rootTurn) {
    if (
        state.outcome != Outcome::UNDECIDED
        || state.inputState != InputState::PLAYER_NORMAL
        || state.turn != rootTurn
    ) {
        return false;
    }
    search::Action(search::ActionType::END_TURN).execute(state);
    return state.outcome == Outcome::PLAYER_VICTORY
        || (
            state.outcome == Outcome::UNDECIDED
            && state.inputState == InputState::PLAYER_NORMAL
            && state.turn > rootTurn
        );
}

bool freeContinuationDominatesInWorld(
    const search::BattleScumSearcher2 &searcher,
    const search::Action &baselineAction,
    const std::optional<ControlledContinuationEvaluation>
        &baselineContinuation,
    const search::Action &challengerAction,
    const std::optional<ControlledContinuationEvaluation>
        &challengerContinuation,
    int freeCardUniqueId,
    int targetIdx
) {
    auto baseline = executeControlledRootAction(
        *searcher.rootState,
        baselineAction,
        baselineContinuation
    );
    auto challenger = executeControlledRootAction(
        *searcher.rootState,
        challengerAction,
        challengerContinuation
    );
    if (!baseline || !challenger) {
        return false;
    }

    int freeCardIdx = -1;
    for (int i = 0; i < challenger->cards.cardsInHand; ++i) {
        if (challenger->cards.hand[i].uniqueId == freeCardUniqueId) {
            freeCardIdx = i;
            break;
        }
    }
    if (freeCardIdx < 0) {
        return false;
    }
    const search::Action freeAction(
        search::ActionType::CARD,
        freeCardIdx,
        targetIdx
    );
    if (
        !freeAction.isValidAction(*challenger)
        || (
            challenger->cards.hand[freeCardIdx].costForTurn != 0
            && !challenger->cards.hand[freeCardIdx].isFreeToPlay(*challenger)
        )
    ) {
        return false;
    }
    freeAction.execute(*challenger);
    if (
        !endCurrentTurn(*baseline, searcher.rootState->turn)
        || !endCurrentTurn(*challenger, searcher.rootState->turn)
    ) {
        return false;
    }
    if (
        challenger->outcome == Outcome::PLAYER_VICTORY
        && baseline->outcome != Outcome::PLAYER_VICTORY
    ) {
        return true;
    }
    if (
        challenger->outcome != Outcome::UNDECIDED
        || baseline->outcome != Outcome::UNDECIDED
        || challenger->player.curHp < baseline->player.curHp
        || challenger->player.block < baseline->player.block
    ) {
        return false;
    }

    bool dealsStrictlyMoreDamage = false;
    for (int i = 0; i < baseline->monsters.monsterCount; ++i) {
        const int baselineHp = std::max(
            0,
            baseline->monsters.arr[i].curHp
        );
        const int challengerHp = std::max(
            0,
            challenger->monsters.arr[i].curHp
        );
        if (
            challengerHp > baselineHp
            || challenger->monsters.arr[i].block
               > baseline->monsters.arr[i].block
            || challenger->monsters.arr[i].weak
               < baseline->monsters.arr[i].weak
            || challenger->monsters.arr[i].vulnerable
               < baseline->monsters.arr[i].vulnerable
        ) {
            return false;
        }
        dealsStrictlyMoreDamage = dealsStrictlyMoreDamage
            || challengerHp < baselineHp;
    }

    bool strictlyUpgraded = false;
    BattleContext normalizedChallenger(*challenger);
    if (!normalizeNoWorseCardUpgrades(
            normalizedChallenger,
            *baseline,
            strictlyUpgraded
        )) {
        return false;
    }
    const auto challengerKey = searcher.buildComparableReplanStateKey(
        normalizedChallenger
    );
    const auto baselineKey = searcher.buildComparableReplanStateKey(
        *baseline
    );
    return (dealsStrictlyMoreDamage || strictlyUpgraded)
        && challengerKey == baselineKey;
}

int applyControlledFreeContinuationDominance(
    const std::vector<search::BattleScumSearcher2 *> &searchers,
    const std::vector<search::RootActionCandidate> &candidates,
    const std::vector<int> &edgeIndices,
    const std::vector<std::optional<ControlledContinuationEvaluation>>
        &continuations,
    int baselineIdx
) {
    if (
        searchers.empty()
        || baselineIdx < 0
        || baselineIdx >= static_cast<int>(candidates.size())
        || baselineIdx >= static_cast<int>(edgeIndices.size())
        || continuations.size() != candidates.size()
    ) {
        return baselineIdx;
    }
    const auto &baselineAction = searchers[0]->root.edges[
        edgeIndices[baselineIdx]
    ].action;
    std::vector<search::RootActionCandidate> dominantCandidates;
    std::vector<int> dominantIndices;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (i == baselineIdx || !continuations[i]) {
            continue;
        }
        const auto &challengerAction = searchers[0]->root.edges[
            edgeIndices[i]
        ].action;
        auto challenger = executeControlledRootAction(
            *searchers[0]->rootState,
            challengerAction,
            continuations[i]
        );
        if (!challenger) {
            continue;
        }

        bool foundDominatingContinuation = false;
        for (int cardIdx = 0;
             cardIdx < challenger->cards.cardsInHand
             && !foundDominatingContinuation;
             ++cardIdx) {
            if (
                challenger->cards.hand[cardIdx].costForTurn != 0
                && !challenger->cards.hand[cardIdx].isFreeToPlay(*challenger)
            ) {
                continue;
            }
            const int uniqueId = challenger->cards.hand[cardIdx].uniqueId;
            for (int targetIdx = 0; targetIdx < 5; ++targetIdx) {
                const search::Action freeAction(
                    search::ActionType::CARD,
                    cardIdx,
                    targetIdx
                );
                if (!freeAction.isValidAction(*challenger)) {
                    continue;
                }
                bool dominatesEveryWorld = true;
                for (const auto *searcher : searchers) {
                    if (!freeContinuationDominatesInWorld(
                            *searcher,
                            baselineAction,
                            continuations[baselineIdx],
                            challengerAction,
                            continuations[i],
                            uniqueId,
                            targetIdx
                        )) {
                        dominatesEveryWorld = false;
                        break;
                    }
                }
                if (dominatesEveryWorld) {
                    foundDominatingContinuation = true;
                    break;
                }
            }
        }
        if (foundDominatingContinuation) {
            dominantCandidates.push_back(candidates[i]);
            dominantIndices.push_back(i);
        }
    }
    if (dominantCandidates.empty()) {
        return baselineIdx;
    }
    const int selected = search::selectRootActionByWorldBest(
        dominantCandidates
    );
    return selected < 0 ? baselineIdx : dominantIndices[selected];
}

// A card or potion is one game action even when it opens a mandatory card
// selector.  Complete only those mandatory selectors before ending the turn,
// so every root action is measured at the same next-turn replanning boundary.
// The continuation comes from the already-searched subtree; this adds neither
// encounter-specific rules nor a second search.
bool executeRootActionToNextTurnBoundary(
    BattleContext &state,
    const BattleContext &rootState,
    const search::BattleScumSearcher2::Edge &rootEdge
) {
    rootEdge.action.execute(state);
    const search::BattleScumSearcher2::Node *node = rootEdge.node.get();
    for (int mandatoryDepth = 0;
         state.outcome == Outcome::UNDECIDED
         && state.inputState == InputState::CARD_SELECT
         && mandatoryDepth < 8;
         ++mandatoryDepth) {
        if (node == nullptr) {
            return false;
        }
        const auto *continuation = bestMandatoryContinuationEdge(*node, state);
        if (continuation == nullptr) {
            return false;
        }
        continuation->action.execute(state);
        node = continuation->node.get();
    }
    if (
        state.outcome == Outcome::UNDECIDED
        && state.inputState == InputState::PLAYER_NORMAL
        && state.turn == rootState.turn
    ) {
        search::Action(search::ActionType::END_TURN).execute(state);
    }
    return state.outcome == Outcome::PLAYER_VICTORY
        || (
            state.outcome == Outcome::UNDECIDED
            && state.inputState == InputState::PLAYER_NORMAL
            && state.turn > rootState.turn
        );
}

std::optional<BattleContext> executeCardPair(
    const BattleContext &rootState,
    const search::Action &first,
    const search::Action &second
) {
    if (
        first.getActionType() != search::ActionType::CARD
        || second.getActionType() != search::ActionType::CARD
        || !first.isValidAction(rootState)
        || !second.isValidAction(rootState)
    ) {
        return std::nullopt;
    }

    const int secondSourceIdx = second.getSourceIdx();
    if (
        secondSourceIdx < 0
        || secondSourceIdx >= rootState.cards.cardsInHand
    ) {
        return std::nullopt;
    }
    const auto secondUniqueId =
        rootState.cards.hand[secondSourceIdx].uniqueId;

    BattleContext state(rootState);
    first.execute(state);
    if (
        state.outcome != Outcome::UNDECIDED
        || state.inputState != InputState::PLAYER_NORMAL
        || state.turn != rootState.turn
    ) {
        return std::nullopt;
    }

    int remappedSourceIdx = -1;
    for (int i = 0; i < state.cards.cardsInHand; ++i) {
        if (state.cards.hand[i].uniqueId == secondUniqueId) {
            remappedSourceIdx = i;
            break;
        }
    }
    if (remappedSourceIdx < 0) {
        return std::nullopt;
    }

    const search::Action remappedSecond(
        search::ActionType::CARD,
        remappedSourceIdx,
        second.getTargetIdx()
    );
    if (!remappedSecond.isValidAction(state)) {
        return std::nullopt;
    }
    remappedSecond.execute(state);
    if (
        state.outcome != Outcome::UNDECIDED
        || state.inputState != InputState::PLAYER_NORMAL
        || state.turn != rootState.turn
    ) {
        return std::nullopt;
    }
    return state;
}

std::optional<int> cardsDrawnByCardAction(
    const BattleContext &rootState,
    const search::Action &action
) {
    if (
        action.getActionType() != search::ActionType::CARD
        || !action.isValidAction(rootState)
    ) {
        return std::nullopt;
    }

    BattleContext state(rootState);
    const int cardsDrawnBefore = state.cardsDrawn;
    action.execute(state);
    if (
        state.outcome != Outcome::UNDECIDED
        || state.inputState != InputState::PLAYER_NORMAL
        || state.turn != rootState.turn
    ) {
        return std::nullopt;
    }
    return state.cardsDrawn - cardsDrawnBefore;
}

bool cardActionDrawsUsableCard(
    const BattleContext &rootState,
    const search::Action &action
) {
    std::vector<int> originalHandIds;
    originalHandIds.reserve(rootState.cards.cardsInHand);
    for (int i = 0; i < rootState.cards.cardsInHand; ++i) {
        originalHandIds.push_back(rootState.cards.hand[i].uniqueId);
    }

    BattleContext state(rootState);
    const int cardsDrawnBefore = state.cardsDrawn;
    action.execute(state);
    if (
        state.outcome != Outcome::UNDECIDED
        || state.inputState != InputState::PLAYER_NORMAL
        || state.turn != rootState.turn
        || state.cardsDrawn <= cardsDrawnBefore
    ) {
        return false;
    }

    for (int i = 0; i < state.cards.cardsInHand; ++i) {
        const auto uniqueId = state.cards.hand[i].uniqueId;
        if (
            std::find(
                originalHandIds.begin(),
                originalHandIds.end(),
                uniqueId
            ) != originalHandIds.end()
        ) {
            continue;
        }
        for (int target = 0; target < 5; ++target) {
            if (
                search::Action(
                    search::ActionType::CARD,
                    i,
                    target
                ).isValidAction(state)
            ) {
                return true;
            }
        }
    }
    return false;
}

bool searchedBestTurnPlanUsesCardAfterSelected(
    const search::BattleScumSearcher2 &searcher,
    const search::Action &selected,
    const search::Action &laterCard
) {
    if (
        laterCard.getActionType() != search::ActionType::CARD
        || laterCard.getSourceIdx() < 0
        || laterCard.getSourceIdx()
           >= searcher.rootState->cards.cardsInHand
    ) {
        return false;
    }
    const int laterCardUniqueId = searcher.rootState->cards.hand[
        laterCard.getSourceIdx()
    ].uniqueId;
    const auto rootEdge = std::find_if(
        searcher.root.edges.begin(),
        searcher.root.edges.end(),
        [&selected] (const auto &edge) {
            return edge.action == selected;
        }
    );
    if (rootEdge == searcher.root.edges.end()) {
        return false;
    }

    BattleContext state(*searcher.rootState);
    selected.execute(state);
    const auto *node = rootEdge->node.get();
    const int rootTurn = searcher.rootState->turn;
    for (int depth = 0;
         depth < 32
         && node != nullptr
         && state.outcome == Outcome::UNDECIDED
         && state.turn == rootTurn;
         ++depth) {
        const auto *bestEdge = bestMandatoryContinuationEdge(*node, state);
        if (bestEdge == nullptr) {
            return false;
        }
        if (bestEdge->action.getActionType() == search::ActionType::END_TURN) {
            return false;
        }
        if (bestEdge->action.getActionType() == search::ActionType::CARD) {
            const int sourceIdx = bestEdge->action.getSourceIdx();
            if (
                sourceIdx >= 0
                && sourceIdx < state.cards.cardsInHand
                && state.cards.hand[sourceIdx].uniqueId == laterCardUniqueId
            ) {
                return true;
            }
        }
        bestEdge->action.execute(state);
        node = bestEdge->node.get();
    }
    return false;
}

bool utilityIntervalsOverlap(
    const search::RootActionCandidate &left,
    const search::RootActionCandidate &right
) {
    if (
        left.winningRngWorlds <= 0
        || left.winningRngWorlds != right.winningRngWorlds
        || left.rngWorlds != right.rngWorlds
        || !std::isfinite(left.meanBestWinUtility)
        || !std::isfinite(right.meanBestWinUtility)
        || !std::isfinite(left.bestWinUtilityStandardError)
        || !std::isfinite(right.bestWinUtilityStandardError)
    ) {
        return false;
    }

    constexpr double confidenceMultiplier = 1.96;
    const double leftMargin = confidenceMultiplier
        * std::max(0.0, left.bestWinUtilityStandardError);
    const double rightMargin = confidenceMultiplier
        * std::max(0.0, right.bestWinUtilityStandardError);
    return left.meanBestWinUtility + leftMargin
               >= right.meanBestWinUtility - rightMargin
           && right.meanBestWinUtility + rightMargin
               >= left.meanBestWinUtility - leftMargin;
}

enum class LocalOrderingDominance {
    NONE,
    DAMAGE,
    DRAW,
};

LocalOrderingDominance secondCardFirstLocallyDominates(
    const search::BattleScumSearcher2 &searcher,
    const search::Action &selected,
    const search::Action &challenger
) {
    const auto selectedThenChallenger = executeCardPair(
        *searcher.rootState,
        selected,
        challenger
    );
    const auto challengerThenSelected = executeCardPair(
        *searcher.rootState,
        challenger,
        selected
    );
    if (!selectedThenChallenger || !challengerThenSelected) {
        return LocalOrderingDominance::NONE;
    }
    const auto selectedFirstKey = searcher.buildLocalActionOrderingStateKey(
        *selectedThenChallenger
    );
    const auto challengerFirstKey = searcher.buildLocalActionOrderingStateKey(
        *challengerThenSelected
    );
    if (selectedFirstKey != challengerFirstKey) {
        return LocalOrderingDominance::NONE;
    }

    bool dealsStrictlyMoreDamage = false;
    for (
        int i = 0;
        i < selectedThenChallenger->monsters.monsterCount;
        ++i
    ) {
        const int selectedFirstHp = std::max(
            0,
            selectedThenChallenger->monsters.arr[i].curHp
        );
        const int challengerFirstHp = std::max(
            0,
            challengerThenSelected->monsters.arr[i].curHp
        );
        if (challengerFirstHp > selectedFirstHp) {
            return LocalOrderingDominance::NONE;
        }
        dealsStrictlyMoreDamage = dealsStrictlyMoreDamage
            || challengerFirstHp < selectedFirstHp;
    }
    if (dealsStrictlyMoreDamage) {
        return LocalOrderingDominance::DAMAGE;
    }

    const auto selectedDraw = cardsDrawnByCardAction(
        *searcher.rootState,
        selected
    );
    const auto challengerDraw = cardsDrawnByCardAction(
        *searcher.rootState,
        challenger
    );
    if (!selectedDraw || !challengerDraw) {
        return LocalOrderingDominance::NONE;
    }

    // Do not introduce a draw that the searched line meant to preserve for a
    // later turn. Reorder only when the selected root's own best complete line
    // already plays this draw card before ending the current turn, and the
    // revealed information can affect a legal action now.
    return *challengerDraw > 0
        && *selectedDraw == 0
        && cardActionDrawsUsableCard(*searcher.rootState, challenger)
        && searchedBestTurnPlanUsesCardAfterSelected(
            searcher,
            selected,
            challenger
        )
        ? LocalOrderingDominance::DRAW
        : LocalOrderingDominance::NONE;
}

bool cardActionDealsNoMonsterDamage(
    const BattleContext &rootState,
    const search::Action &action
) {
    if (
        action.getActionType() != search::ActionType::CARD
        || !action.isValidAction(rootState)
    ) {
        return false;
    }
    BattleContext state(rootState);
    action.execute(state);
    for (int i = 0; i < rootState.monsters.monsterCount; ++i) {
        if (
            std::max(0, state.monsters.arr[i].curHp)
            < std::max(0, rootState.monsters.arr[i].curHp)
        ) {
            return false;
        }
    }
    return true;
}

int applyLocalTwoCardOrderingTieBreak(
    const std::vector<search::BattleScumSearcher2 *> &searchers,
    const std::vector<search::RootActionCandidate> &candidates,
    const std::vector<int> &edgeIndices,
    int baselineIdx
) {
    if (
        searchers.empty()
        || baselineIdx < 0
        || baselineIdx >= static_cast<int>(candidates.size())
        || baselineIdx >= static_cast<int>(edgeIndices.size())
    ) {
        return baselineIdx;
    }
    const auto &baselineAction = searchers[0]->root.edges[
        edgeIndices[baselineIdx]
    ].action;
    if (baselineAction.getActionType() != search::ActionType::CARD) {
        return baselineIdx;
    }

    std::vector<search::RootActionCandidate> dominantCandidates;
    std::vector<int> dominantIndices;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (
            i == baselineIdx
            || i >= static_cast<int>(edgeIndices.size())
        ) {
            continue;
        }
        const auto &challengerAction = searchers[0]->root.edges[
            edgeIndices[i]
        ].action;
        if (challengerAction.getActionType() != search::ActionType::CARD) {
            continue;
        }

        LocalOrderingDominance dominance = LocalOrderingDominance::NONE;
        bool dominatesEveryWorld = true;
        for (const auto *searcher : searchers) {
            const auto worldDominance = secondCardFirstLocallyDominates(
                *searcher,
                baselineAction,
                challengerAction
            );
            if (
                worldDominance == LocalOrderingDominance::NONE
                || (
                    dominance != LocalOrderingDominance::NONE
                    && dominance != worldDominance
                )
            ) {
                dominatesEveryWorld = false;
                break;
            }
            dominance = worldDominance;
        }
        bool selectedDealsNoDamageInEveryWorld =
            dominance == LocalOrderingDominance::DAMAGE;
        if (selectedDealsNoDamageInEveryWorld) {
            for (const auto *searcher : searchers) {
                if (!cardActionDealsNoMonsterDamage(
                        *searcher->rootState,
                        baselineAction
                    )) {
                    selectedDealsNoDamageInEveryWorld = false;
                    break;
                }
            }
        }
        if (
            !selectedDealsNoDamageInEveryWorld
            && (
                !utilityIntervalsOverlap(
                    candidates[i],
                    candidates[baselineIdx]
                )
                || search::completeCombatParetoDominates(
                    candidates[baselineIdx],
                    candidates[i]
                )
            )
        ) {
            dominatesEveryWorld = false;
        }
        if (dominatesEveryWorld) {
            dominantCandidates.push_back(candidates[i]);
            dominantIndices.push_back(i);
        }
    }
    if (dominantCandidates.empty()) {
        return baselineIdx;
    }
    const int selected = search::selectRootActionByWorldBest(
        dominantCandidates
    );
    return selected < 0 ? baselineIdx : dominantIndices[selected];
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "usage: battle-sim <input.json> [simulations_per_thread] [thread_count] [max_time_ms] [max_backup_weight] [root_potions|potion_slots=0,1|adaptive_max_time_ms=N|adaptive_max_simulations=N]" << std::endl;
        return 1;
    }

    const int simulationsPerThread = argc >= 3 ? std::max(1, std::stoi(argv[2])) : 100000;
    const int thread_count = argc >= 4 ? std::max(1, std::stoi(argv[3])) : 12;
    const long maxTimeMillis = argc >= 5 ? std::max(1L, std::stol(argv[4])) : 10000L;
    double maxBackupWeight = 0.0;
    bool rootPotions = false;
    std::uint32_t allowedPotionSlotMask = 0;
    long adaptiveMaxTimeMillis = maxTimeMillis;
    std::int64_t adaptiveMaxSimulations = simulationsPerThread;
    const auto parseMode = [&](const std::string &option) {
        if (option == "root_potions") {
            rootPotions = true;
            return true;
        }
        constexpr std::string_view prefix = "potion_slots=";
        if (option.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        const std::string slots = option.substr(prefix.size());
        std::stringstream stream(slots);
        std::string token;
        while (std::getline(stream, token, ',')) {
            if (token.empty()) {
                continue;
            }
            const int slot = std::stoi(token);
            if (slot < 0 || slot >= 32) {
                throw std::out_of_range("potion slot must be between 0 and 31");
            }
            allowedPotionSlotMask |= std::uint32_t{1} << slot;
        }
        if (allowedPotionSlotMask == 0) {
            throw std::invalid_argument("potion_slots requires at least one slot");
        }
        return true;
    };
    const auto parseSearchLimit = [
        &adaptiveMaxTimeMillis,
        &adaptiveMaxSimulations
    ] (const std::string &option) {
        constexpr std::string_view timePrefix = "adaptive_max_time_ms=";
        constexpr std::string_view simulationPrefix =
            "adaptive_max_simulations=";
        if (option.compare(0, timePrefix.size(), timePrefix) == 0) {
            adaptiveMaxTimeMillis = std::max(
                0L,
                std::stol(option.substr(timePrefix.size()))
            );
            return true;
        }
        if (option.compare(0, simulationPrefix.size(), simulationPrefix) == 0) {
            adaptiveMaxSimulations = std::max<std::int64_t>(
                0,
                std::stoll(option.substr(simulationPrefix.size()))
            );
            return true;
        }
        return false;
    };
    if (argc >= 6) {
        const std::string option(argv[5]);
        if (!parseMode(option) && !parseSearchLimit(option)) {
            maxBackupWeight = std::clamp(std::stod(option), 0.0, 1.0);
        }
    }
    for (int i = 6; i < argc; ++i) {
        const std::string option(argv[i]);
        if (!parseMode(option) && !parseSearchLimit(option)) {
            std::cerr << "unknown battle-sim mode: " << option << std::endl;
            return 1;
        }
    }
    adaptiveMaxTimeMillis = std::max(
        maxTimeMillis,
        adaptiveMaxTimeMillis
    );
    adaptiveMaxSimulations = std::max<std::int64_t>(
        simulationsPerThread,
        adaptiveMaxSimulations
    );
    std::string jsonString;
    std::ifstream file(argv[1]);
    if (file) {
        std::ostringstream ss;
        ss << file.rdbuf();
        jsonString = ss.str();
    }

    nlohmann::json json = nlohmann::json::parse(jsonString);
    BattleConverter converter;
    int monsterIdxMap[5];
    for (int i = 0; i < 5; ++i) {
        monsterIdxMap[i] = -1;
    }
    BattleContext baseBc = converter.convertFromJson(json, monsterIdxMap);
    const nlohmann::json importedTurnCounters = {
        {"cardsPlayed", baseBc.player.cardsPlayedThisTurn},
        {"attacksPlayed", baseBc.player.attacksPlayedThisTurn},
        {"skillsPlayed", baseBc.player.skillsPlayedThisTurn}
    };

    int externalGuidanceTurn = -1;
    int guidanceTurn = -1;
    std::vector<std::string> preferredCardNames;
    std::vector<std::string> discouragedCardNames;
    const auto guidanceIt = json.find("mcts_guidance");
    if (guidanceIt != json.end() && guidanceIt->is_object()) {
        externalGuidanceTurn = guidanceIt->value("turn", -1);
        // Combat guidance uses CommunicationMod's 1-based turn number.  MCTS
        // compares it with BattleContext::turn, which is 0-based.
        guidanceTurn = externalGuidanceTurn > 0
            ? externalGuidanceTurn - 1
            : -1;
        const auto readNames = [] (
            const nlohmann::json &guidance,
            const char *field
        ) {
            std::vector<std::string> names;
            const auto it = guidance.find(field);
            if (it == guidance.end() || !it->is_array()) {
                return names;
            }
            for (const auto &value : *it) {
                if (value.is_string()) {
                    names.push_back(value.get<std::string>());
                }
            }
            return names;
        };
        preferredCardNames = readNames(*guidanceIt, "prefer");
        discouragedCardNames = readNames(*guidanceIt, "discourage");
        if (guidanceTurn != baseBc.turn) {
            preferredCardNames.clear();
            discouragedCardNames.clear();
        }
    }
    // Snapshot this before executing the selected root action. END_TURN can
    // advance baseBc.turn while formatting a follow-up, which previously made
    // telemetry claim that guidance had not been applied to the completed
    // search.
    const bool guidanceApplied = guidanceTurn == baseBc.turn
                                 && (!preferredCardNames.empty()
                                     || !discouragedCardNames.empty());

    milliseconds ms1 = duration_cast< milliseconds >(
        system_clock::now().time_since_epoch()
    );
    // std::cout << bc << std::endl;
    std::vector<search::BattleScumSearcher2 *> searchers(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        int rngMod = i;
        BattleContext threadBc = baseBc;
        threadBc.cardRandomRng.setCounter(rngMod * 1000);
        threadBc.aiRng.setCounter(rngMod * 1000);
        threadBc.shuffleRng.setCounter(rngMod * 1000);
        threadBc.miscRng.setCounter(rngMod * 1000);
        threadBc.potionRng.setCounter(rngMod * 1000);
        searchers[i] = new search::BattleScumSearcher2(threadBc);
        searchers[i]->setActionGuidance(
            guidanceTurn,
            preferredCardNames,
            discouragedCardNames
        );
        searchers[i]->maxBackupWeight = maxBackupWeight;
        searchers[i]->allowRootPotions = rootPotions;
        searchers[i]->allowedPotionSlotMask = allowedPotionSlotMask;
        searchers[i]->balanceRootActions = true;
        // A baseline search still supplies the cross-combat potion gate with
        // risk evidence. It is safe to skip that evidence when no potion is
        // held, or when every permitted potion is already a root alternative.
        searchers[i]->stopOnForcedRootAction =
            baseBc.potionCount == 0
            || rootPotions
            || allowedPotionSlotMask != 0;
    }
    const auto runSearchPhase = [
        &searchers,
        thread_count
    ] (std::int64_t simulations, long timeMillis) {
        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back(
                search2,
                std::ref(*searchers[i]),
                simulations,
                timeMillis
            );
        }
        for (auto &thread : threads) {
            thread.join();
        }
    };
    const auto adaptiveStart = steady_clock::now();
    runSearchPhase(simulationsPerThread, maxTimeMillis);
    const auto baseElapsedMillis = duration_cast<milliseconds>(
        steady_clock::now() - adaptiveStart
    ).count();
    const auto baseStopReason = searchers.front()->stopReason;
    const auto adaptiveAssessment = assessAdaptiveSearch(searchers, baseBc);
    bool adaptiveSearchTriggered = false;
    if (
        adaptiveAssessment.extend
        && (
            baseStopReason == "time_budget"
            || baseStopReason == "simulation_budget"
        )
        && adaptiveMaxTimeMillis > baseElapsedMillis
        && adaptiveMaxSimulations > simulationsPerThread
    ) {
        adaptiveSearchTriggered = true;
        runSearchPhase(
            adaptiveMaxSimulations - simulationsPerThread,
            adaptiveMaxTimeMillis - baseElapsedMillis
        );
    }
    const auto adaptiveElapsedMillis = duration_cast<milliseconds>(
        steady_clock::now() - adaptiveStart
    ).count();
    milliseconds ms2 = duration_cast< milliseconds >(
        system_clock::now().time_since_epoch()
    );
    // std::cout << "took " << (ms2 - ms1).count() << "ms\n";

    // std::cout << searcher.bestActionValue << '\n';
    // std::cout << searcher.root.simulationCount << '\n';
    // searcher.bestActionSequence[0].printDesc(std::cout, bc);
    // std::cout << '\n' << bc << '\n';
    // searcher.printSearchTree(std::cout, 1);

    std::ofstream outfile;
    outfile.open("test.log", std::ios_base::app);
    outfile << "====" << std::endl;
    outfile << baseBc << std::endl;

    int bestActionIdx = -1;
    double bestScore = std::numeric_limits<double>::lowest();
    double bestOptimisticScore = std::numeric_limits<double>::lowest();
    nlohmann::json rootActionStats = nlohmann::json::array();
    std::vector<search::RootActionCandidate> rootActionCandidates;
    std::vector<int> rootActionEdgeIndices;
    std::vector<std::optional<ControlledContinuationEvaluation>>
        rootControlledContinuations;
    const bool rootIsCardSelect =
        baseBc.inputState == InputState::CARD_SELECT;
    for (int j = 0; j < static_cast<int>(searchers[0]->root.edges.size()); ++j) {
        const auto &rootAction = searchers[0]->root.edges[j].action;
        std::vector<search::RootActionThreadSample> threadSamples;
        threadSamples.reserve(thread_count);
        for (int i = 0; i < thread_count; ++i) {
            const auto matchingEdge = std::find_if(
                searchers[i]->root.edges.begin(),
                searchers[i]->root.edges.end(),
                [&searchers, j] (const auto &edge) {
                    return edge.action == searchers[0]->root.edges[j].action;
                }
            );
            if (matchingEdge == searchers[i]->root.edges.end()) {
                continue;
            }
            threadSamples.push_back({
                matchingEdge->simulationCount,
                matchingEdge->evaluationSum,
                matchingEdge->evaluationSquaredSum,
                matchingEdge->maxEvaluation,
                matchingEdge->winSamples,
                matchingEdge->lossSamples,
                matchingEdge->cutoffSamples,
                matchingEdge->winEndHpSum,
                matchingEdge->winUtilitySum,
                matchingEdge->lizardTailConsumedWinSamples,
                matchingEdge->reachedNextDecision,
                static_cast<double>(matchingEdge->bestNextDecisionHp),
                static_cast<double>(
                    matchingEdge->bestNextDecisionEffectiveHp
                ),
                static_cast<double>(
                    matchingEdge->bestNextDecisionPotionCount
                ),
                matchingEdge->bestNextDecisionValue,
                matchingEdge->escapedStolenGoldWinSum,
                matchingEdge->foundWinningLine,
                matchingEdge->bestWinUtility,
                static_cast<double>(matchingEdge->bestWinEndHp),
                static_cast<double>(matchingEdge->bestWinPotionCount),
            });
            threadSamples.back().bestReplanHorizonTurns =
                static_cast<double>(matchingEdge->bestReplanHorizonTurns);
        }
        auto aggregate = search::aggregateRootActionThreads(threadSamples);
        if (aggregate.rngWorlds == 0) {
            continue;
        }
        std::optional<ControlledContinuationEvaluation>
            controlledContinuation;
        if (!rootIsCardSelect) {
            controlledContinuation = evaluateControlledCardSelection(
                searchers,
                rootAction
            );
        }
        const double score = aggregate.meanValue;
        const double bestValue = aggregate.meanBestValue;
        const auto averageVisits = aggregate.visits / aggregate.rngWorlds;
        const double effectiveMaxWeight = searchers[0]->getEffectiveMaxBackupWeight(averageVisits);
        const double optimisticScore = (1.0 - effectiveMaxWeight) * score
                                       + effectiveMaxWeight * bestValue;
        const double expectedHpLossOnWin = aggregate.winSampleRate > 0.0
                                           ? static_cast<double>(baseBc.player.curHp)
                                             - aggregate.expectedEndHpOnWin
                                           : 0.0;
        outfile <<j <<":"<< aggregate.visits << " visits / " << std::fixed << std::setprecision(5) << score << " value for ";

        searchers[0]->root.edges[j].action.printDesc(outfile, baseBc);
        outfile << std::endl;

        nlohmann::json stat;
        stat["edgeIndex"] = j;
        stat["action"] = describeAction(
            rootAction,
            baseBc,
            monsterIdxMap,
            rootIsCardSelect
        );
        if (rootIsCardSelect
            && rootAction.getActionType() == search::ActionType::SINGLE_CARD_SELECT) {
            stat["choice"] = selectedCardNameForTask(
                baseBc,
                baseBc.cardSelectInfo.cardSelectTask,
                rootAction.getSelectIdx()
            );
        }
        if (controlledContinuation) {
            BattleContext selectionState(baseBc);
            rootAction.execute(selectionState);
            stat["controlledContinuation"] = describeAction(
                controlledContinuation->action,
                selectionState,
                monsterIdxMap
            );
            stat["controlledContinuationCandidates"] =
                nlohmann::json::array();
            for (const auto &[continuationAction, continuationAggregate]
                 : controlledContinuation->candidates) {
                stat["controlledContinuationCandidates"].push_back({
                    {"action", describeAction(
                        continuationAction,
                        selectionState,
                        monsterIdxMap
                    )},
                    {"successUtility",
                     continuationAggregate.successUtility},
                    {"winSampleRate",
                     continuationAggregate.winSampleRate},
                    {"meanBestWinUtility",
                     continuationAggregate.meanBestWinUtility},
                    {"bestWinUtilityStandardError",
                     continuationAggregate.bestWinUtilityStandardError},
                });
            }
        }
        stat["visits"] = aggregate.visits;
        stat["rngWorlds"] = aggregate.rngWorlds;
        stat["value"] = score;
        stat["variance"] = aggregate.variance;
        stat["bestValue"] = bestValue;
        stat["maxBackupWeight"] = effectiveMaxWeight;
        stat["optimisticValue"] = optimisticScore;
        stat["winSamples"] = aggregate.winSampleRate * aggregate.visits;
        stat["lossSamples"] = aggregate.lossSampleRate * aggregate.visits;
        stat["cutoffSamples"] = aggregate.cutoffSampleRate * aggregate.visits;
        stat["winSampleRate"] = aggregate.winSampleRate;
        stat["lowerQuartileWinSampleRate"] =
            aggregate.lowerQuartileWinSampleRate;
        stat["lowerQuartileSuccessUtility"] =
            aggregate.lowerQuartileSuccessUtility;
        stat["expectedEndHpOnWin"] = aggregate.expectedEndHpOnWin;
        stat["expectedHpLossOnWin"] = expectedHpLossOnWin;
        stat["successUtility"] = aggregate.successUtility;
        stat["expectedWinUtilityOnWin"] =
            aggregate.expectedWinUtilityOnWin;
        stat["expectedEffectiveEndHpOnWin"] =
            aggregate.expectedWinUtilityOnWin
            * static_cast<double>(std::max(1, baseBc.player.maxHp));
        stat["lizardTailConsumedRateOnWin"] =
            aggregate.lizardTailConsumedRateOnWin;
        stat["expectedEscapedStolenGoldOnWin"] =
            aggregate.expectedEscapedStolenGoldOnWin;
        stat["nextDecisionReachRate"] =
            aggregate.nextDecisionReachRate;
        stat["meanReplanHorizonTurns"] =
            aggregate.meanReplanHorizonTurns;
        stat["meanBestNextDecisionHp"] =
            aggregate.meanBestNextDecisionHp;
        stat["meanBestNextDecisionEffectiveHp"] =
            aggregate.meanBestNextDecisionEffectiveHp;
        stat["meanBestNextDecisionPotionCount"] =
            aggregate.meanBestNextDecisionPotionCount;
        stat["meanBestNextDecisionValue"] =
            aggregate.meanBestNextDecisionValue;
        stat["safeReplanUtility"] =
            aggregate.meanValue
            + aggregate.meanBestNextDecisionEffectiveHp
              / static_cast<double>(std::max(1, baseBc.player.maxHp));
        stat["winningRngWorlds"] = aggregate.winningRngWorlds;
        stat["winWorldRate"] = aggregate.winWorldRate;
        stat["lowerQuartileBestWinUtility"] =
            aggregate.lowerQuartileBestWinUtility;
        stat["meanBestWinUtility"] = aggregate.meanBestWinUtility;
        stat["bestWinUtilityStandardError"] =
            aggregate.bestWinUtilityStandardError;
        stat["meanBestWinEndHp"] = aggregate.meanBestWinEndHp;
        stat["meanBestWinPotionCount"] =
            aggregate.meanBestWinPotionCount;
        rootActionStats.push_back(std::move(stat));
        rootActionCandidates.push_back(search::candidateFromAggregate(
            aggregate,
            baseBc,
            rootAction.getActionType() == search::ActionType::END_TURN,
            search::RootCandidateOptions{true, true}
        ));
        auto &candidate = rootActionCandidates.back();
        if (rootAction.getActionType() == search::ActionType::CARD) {
            const auto &card = baseBc.cards.hand[rootAction.getSourceIdx()];
            candidate.zeroEnergyRootAction =
                card.costForTurn == 0 || card.isFreeToPlay(baseBc);
        }
        annotateCertifiedImmediateWin(candidate, rootAction, searchers);
        rootActionStats.back()["certifiedImmediateWin"] =
            candidate.certifiedImmediateWin;
        rootActionStats.back()["certifiedImmediateWinUtility"] =
            candidate.certifiedImmediateWinUtility;
        // Use the persistent-resource tie-break only while choosing the first
        // action of a turn, when the search can compare complete turn plans.
        // Reapplying it after every atomic action creates receding-horizon
        // greed (for example attacking instead of blocking after committing
        // to a thief-kill line).
        if (baseBc.player.cardsPlayedThisTurn == 0) {
            candidate.expectedEscapedStolenGoldOnWin =
                aggregate.expectedEscapedStolenGoldOnWin;
        } else {
            candidate.expectedEscapedStolenGoldOnWin =
                std::numeric_limits<double>::quiet_NaN();
        }
        double directHpSum = 0.0;
        double directBlockSum = 0.0;
        double directPotionSum = 0.0;
        double directValueSum = 0.0;
        int directReachWorlds = 0;
        candidate.directEndBoundaryStateKeys.reserve(thread_count);
        candidate.monotonicBoundaryStateKeys.reserve(thread_count);
        candidate.directEndBoundaryMonotonicCardProgress.reserve(thread_count);
        candidate.directEndBoundaryStrength.reserve(thread_count);
        candidate.directEndBoundaryReached.reserve(thread_count);
        candidate.directEndBoundaryHps.reserve(thread_count);
        candidate.directEndBoundaryPotionCounts.reserve(thread_count);
        candidate.directEndBoundaryValues.reserve(thread_count);
        candidate.strictBlockBoundaryStateKeys.reserve(thread_count);
        candidate.shuffledBlockBoundaryStateKeys.reserve(thread_count);
        candidate.directEndBoundaryShuffleAdvances.reserve(thread_count);
        candidate.directEndBoundaryBlocks.reserve(thread_count);
        const auto appendMissingBoundary = [&candidate] {
            candidate.directEndBoundaryStateKeys.push_back(0);
            candidate.monotonicBoundaryStateKeys.push_back(0);
            candidate.directEndBoundaryMonotonicCardProgress.push_back(0);
            candidate.directEndBoundaryStrength.push_back(0);
            candidate.directEndBoundaryReached.push_back(0);
            candidate.directEndBoundaryHps.push_back(0.0);
            candidate.directEndBoundaryPotionCounts.push_back(0.0);
            candidate.directEndBoundaryValues.push_back(0.0);
            candidate.strictBlockBoundaryStateKeys.push_back(0);
            candidate.shuffledBlockBoundaryStateKeys.push_back(0);
            candidate.directEndBoundaryShuffleAdvances.push_back(0);
            candidate.directEndBoundaryBlocks.push_back(0);
        };
        for (int i = 0; i < thread_count; ++i) {
            const auto matchingEdge = std::find_if(
                searchers[i]->root.edges.begin(),
                searchers[i]->root.edges.end(),
                [&rootAction] (const auto &edge) {
                    return edge.action == rootAction;
                }
            );
            if (matchingEdge == searchers[i]->root.edges.end()) {
                appendMissingBoundary();
                continue;
            }

            BattleContext boundary = *searchers[i]->rootState;
            const bool reached = executeRootActionToNextTurnBoundary(
                boundary,
                *searchers[i]->rootState,
                *matchingEdge
            );
            if (!reached) {
                appendMissingBoundary();
                continue;
            }
            ++directReachWorlds;
            const double boundaryHp =
                boundary.outcome == Outcome::PLAYER_VICTORY
                ? search::getPostCombatPlayerHp(boundary)
                : std::max(0, boundary.player.curHp);
            const double boundaryValue = searchers[i]->evaluateState(boundary);
            directHpSum += boundaryHp;
            directBlockSum += std::max(0, boundary.player.block);
            directPotionSum += boundary.potionCount;
            directValueSum += boundaryValue;
            candidate.directEndBoundaryReached.push_back(1);
            candidate.directEndBoundaryHps.push_back(boundaryHp);
            candidate.directEndBoundaryPotionCounts.push_back(
                boundary.potionCount
            );
            candidate.directEndBoundaryValues.push_back(boundaryValue);
            candidate.directEndBoundaryStateKeys.push_back(
                searchers[i]->buildComparableReplanStateKey(boundary)
            );
            candidate.monotonicBoundaryStateKeys.push_back(
                searchers[i]->buildComparableReplanStateKey(boundary, true)
            );
            candidate.directEndBoundaryMonotonicCardProgress.push_back(
                getMonotonicCardProgress(boundary)
            );
            candidate.directEndBoundaryStrength.push_back(
                boundary.player.strength
            );
            candidate.strictBlockBoundaryStateKeys.push_back(
                searchers[i]->buildStrictBlockBoundaryStateKey(boundary)
            );
            const int shuffleAdvance =
                boundary.shuffleRng.counter
                - searchers[i]->rootState->shuffleRng.counter;
            const bool retainedHandCanChangeShuffleDistribution =
                searchers[i]->rootState->player.hasRelic<R::RUNIC_PYRAMID>()
                || searchers[i]->rootState->player.hasStatus<PS::EQUILIBRIUM>()
                || std::any_of(
                    searchers[i]->rootState->cards.hand.begin(),
                    searchers[i]->rootState->cards.hand.begin()
                        + searchers[i]->rootState->cards.cardsInHand,
                    [] (const CardInstance &card) {
                        return card.retain || card.hasSelfRetain();
                    }
                );
            candidate.shuffledBlockBoundaryStateKeys.push_back(
                shuffleAdvance > 0
                    && boundary.cards.discardPile.empty()
                    && !retainedHandCanChangeShuffleDistribution
                ? searchers[i]->buildStrictBlockBoundaryStateKey(
                    boundary,
                    true
                )
                : 0
            );
            candidate.directEndBoundaryShuffleAdvances.push_back(
                shuffleAdvance
            );
            candidate.directEndBoundaryBlocks.push_back(
                std::max(0, boundary.player.block)
            );
        }
        const double inverseWorlds = 1.0 / static_cast<double>(thread_count);
        candidate.directEndBoundaryReachRate =
            static_cast<double>(directReachWorlds) * inverseWorlds;
        candidate.meanDirectEndBoundaryHp = directHpSum * inverseWorlds;
        candidate.meanDirectEndBoundaryBlock =
            directBlockSum * inverseWorlds;
        candidate.meanDirectEndBoundaryPotionCount =
            directPotionSum * inverseWorlds;
        candidate.meanDirectEndBoundaryValue = directValueSum * inverseWorlds;
        rootActionStats.back()["directEndBoundaryReachRate"] =
            candidate.directEndBoundaryReachRate;
        rootActionStats.back()["meanDirectEndBoundaryHp"] =
            candidate.meanDirectEndBoundaryHp;
        rootActionStats.back()["meanDirectEndBoundaryBlock"] =
            candidate.meanDirectEndBoundaryBlock;
        rootActionStats.back()["meanDirectEndBoundaryPotionCount"] =
            candidate.meanDirectEndBoundaryPotionCount;
        rootActionStats.back()["meanDirectEndBoundaryValue"] =
            candidate.meanDirectEndBoundaryValue;
        rootActionStats.back()["directEndBoundaryReached"] =
            candidate.directEndBoundaryReached;
        rootActionStats.back()["directEndBoundaryHps"] =
            candidate.directEndBoundaryHps;
        rootActionStats.back()["directEndBoundaryPotionCounts"] =
            candidate.directEndBoundaryPotionCounts;
        rootActionStats.back()["directEndBoundaryValues"] =
            candidate.directEndBoundaryValues;
        rootActionStats.back()["zeroEnergyRootAction"] =
            candidate.zeroEnergyRootAction;
        rootActionStats.back()["monotonicCardComparableWorlds"] =
            static_cast<int>(std::count_if(
                candidate.monotonicBoundaryStateKeys.begin(),
                candidate.monotonicBoundaryStateKeys.end(),
                [] (std::uint64_t key) { return key != 0; }
            ));
        rootActionStats.back()["strictBlockComparableWorlds"] =
            static_cast<int>(std::count_if(
                candidate.strictBlockBoundaryStateKeys.begin(),
                candidate.strictBlockBoundaryStateKeys.end(),
                [] (std::uint64_t key) { return key != 0; }
            ));
        rootActionEdgeIndices.push_back(j);
        rootControlledContinuations.push_back(
            std::move(controlledContinuation)
        );
    }
    const bool hasSuccessUtility =
        search::hasCredibleWinEvidence(rootActionCandidates);
    const bool hasBroadSparseWins =
        search::hasBroadSparseWinEvidence(rootActionCandidates);
    int baselineCandidateIdx = -1;
    int commonBoundaryCandidateIdx = -1;
    int strictBlockCandidateIdx = -1;
    int recoveryReferenceCandidateIdx = -1;
    if (!rootPotions && !rootIsCardSelect) {
        baselineCandidateIdx = allowedPotionSlotMask != 0
            ? search::selectRootPotionAction(rootActionCandidates, false)
            : search::selectRootActionByWorldBest(
                rootActionCandidates,
                false
            );
        commonBoundaryCandidateIdx =
            search::applyCommonBoundaryEndTurnDominanceVeto(
                rootActionCandidates,
                baselineCandidateIdx
            );
        strictBlockCandidateIdx =
            search::applyStrictBlockEndTurnDominanceVeto(
                rootActionCandidates,
                commonBoundaryCandidateIdx
            );
        recoveryReferenceCandidateIdx = strictBlockCandidateIdx;
    } else if (rootIsCardSelect) {
        recoveryReferenceCandidateIdx =
            search::selectRootActionByWinRateBandThenEndHp(
                rootActionCandidates
            );
    }
    const bool exactBoundaryProof =
        !rootPotions
        && !rootIsCardSelect
        && (
            commonBoundaryCandidateIdx != baselineCandidateIdx
            || strictBlockCandidateIdx != commonBoundaryCandidateIdx
        );
    const bool recoverySearchRequired =
        !rootPotions
        && !exactBoundaryProof
        && search::shouldUseRecoverySearch(
               rootActionCandidates,
               recoveryReferenceCandidateIdx
           );
    RecoverySearchResult recoverySearch;
    bool recoverySearchUsed = false;
    if (
        recoverySearchRequired
        && !guidanceApplied
    ) {
        std::vector<search::Action> rootActions;
        rootActions.reserve(rootActionEdgeIndices.size());
        for (const int edgeIdx : rootActionEdgeIndices) {
            rootActions.push_back(searchers[0]->root.edges[edgeIdx].action);
        }
        recoverySearch = runRecoveryHorizonSearch(
            baseBc,
            rootActions,
            simulationsPerThread,
            thread_count,
            maxTimeMillis,
            allowedPotionSlotMask
        );
        recoverySearchUsed = recoverySearch.selectedAction >= 0;
        for (
            int i = 0;
            i < static_cast<int>(recoverySearch.candidates.size())
                && i < static_cast<int>(rootActionStats.size());
            ++i
        ) {
            const auto &candidate = recoverySearch.candidates[i];
            rootActionStats[i]["recoveryHorizonReachRate"] =
                candidate.horizonReachRate;
            rootActionStats[i]["recoveryMeanSurvivedTurns"] =
                candidate.meanSurvivedTurns;
            rootActionStats[i]["recoveryLowerQuartileHp"] =
                candidate.lowerQuartileHp;
            rootActionStats[i]["recoveryLowerQuartileQuality"] =
                candidate.lowerQuartileQuality;
            rootActionStats[i]["recoveryMeanHp"] = candidate.meanHp;
            rootActionStats[i]["recoveryMeanEffectiveHp"] =
                candidate.meanEffectiveHp;
            rootActionStats[i]["recoveryMeanEngineScore"] =
                candidate.meanEngineScore;
            rootActionStats[i]["recoveryMeanEnemyProgress"] =
                candidate.meanEnemyProgress;
            rootActionStats[i]["recoveryMeanPotionCount"] =
                candidate.meanPotionCount;
            rootActionStats[i]["recoveryMeanQuality"] =
                candidate.meanQuality;
            rootActionStats[i]["recoveryMeanActionPrior"] =
                candidate.meanActionPrior;
            rootActionStats[i]["recoveryImmediateEnemyDamage"] =
                candidate.immediateEnemyDamage;
        }
    }

    // Recovery adds no decision evidence when terminal evidence exists and it
    // agrees with the ordinary selector.  Keep that case on the established
    // path so diagnostics continue to name the policy that selected it.
    if (recoverySearchUsed && !rootIsCardSelect) {
        if (
            (hasSuccessUtility || hasBroadSparseWins)
            && recoverySearch.selectedAction == strictBlockCandidateIdx
        ) {
            recoverySearchUsed = false;
        }
    }
    int bestCandidateIdx = -1;
    int recoveryBaselineCandidateIdx = -1;
    bool recoveryCompleteCombatParetoSelected = false;
    bool dangerPolicySelected = false;
    bool reliableActionEndTurnSelected = false;
    // `root_potions` is the legacy root-only counterfactual mode and keeps its
    // dedicated selector. `potion_slots=...` merely makes those potion actions
    // legal throughout the normal tree; it must retain the ordinary root
    // policy and END_TURN safety corrections.
    if (recoverySearchUsed) {
        recoveryBaselineCandidateIdx = recoverySearch.selectedAction;
        bestCandidateIdx = search::applyCompleteCombatParetoVeto(
            rootActionCandidates, recoveryBaselineCandidateIdx
        );
        recoveryCompleteCombatParetoSelected =
            bestCandidateIdx != recoveryBaselineCandidateIdx;
    } else if (rootPotions) {
        bestCandidateIdx = search::selectRootPotionAction(
            rootActionCandidates
        );
    } else if (rootIsCardSelect) {
        bestCandidateIdx =
            search::selectRootActionByWinRateBandThenEndHp(
                rootActionCandidates
            );
    } else {
        // In an all-loss potion-enabled tree, keep the potion-aware survival
        // ordering. With credible wins this is identical to the ordinary
        // complete-combat selector. In both cases the result still passes
        // through the normal END_TURN dominance checks below.
        bestCandidateIdx = strictBlockCandidateIdx;
        // Exact common-boundary dominance is stronger evidence than rollout
        // rates. Only when no such proof changed the normal selector may an
        // immediate-survival reliability inversion activate the danger policy.
        if (
            bestCandidateIdx == baselineCandidateIdx
            && search::shouldPreferDangerWinRatePolicy(
                rootActionCandidates,
                bestCandidateIdx
            )
        ) {
            bestCandidateIdx =
                search::selectRootActionByWinRateBandThenEndHp(
                    rootActionCandidates
                );
            dangerPolicySelected = true;
        }
        if (
            bestCandidateIdx == baselineCandidateIdx
            && !dangerPolicySelected
        ) {
            const int reliableActionIdx =
                search::applyReliableActionEndTurnVeto(
                    rootActionCandidates,
                    bestCandidateIdx
                );
            reliableActionEndTurnSelected =
                reliableActionIdx != bestCandidateIdx;
            bestCandidateIdx = reliableActionIdx;
        }
    }
    if (bestCandidateIdx < 0) {
        std::cerr << "search failed to evaluate any root action" << std::endl;
        return 2;
    }
    const bool commonBoundaryDominanceSelected =
        !recoverySearchUsed
        && !rootPotions
        && !rootIsCardSelect
        && commonBoundaryCandidateIdx != baselineCandidateIdx;
    const bool strictBlockDominanceSelected =
        !recoverySearchUsed
        && !rootPotions
        && !rootIsCardSelect
        && strictBlockCandidateIdx != commonBoundaryCandidateIdx;
    const bool endTurnDominanceSelected =
        commonBoundaryDominanceSelected
        || strictBlockDominanceSelected;
    // A common-boundary proof is a hard safety result, not another heuristic
    // preference.  Later ordering/continuation tie-breaks must not silently
    // replace it; doing so made the diagnostics name the proven Block action
    // while the emitted command was an attack.
    const bool rootSelectionLocked = endTurnDominanceSelected;
    const int localOrderingBaselineIdx = bestCandidateIdx;
    if (
        !rootSelectionLocked
        && !recoverySearchUsed
        && !rootPotions
        && !rootIsCardSelect
    ) {
        bestCandidateIdx = applyLocalTwoCardOrderingTieBreak(
            searchers,
            rootActionCandidates,
            rootActionEdgeIndices,
            bestCandidateIdx
        );
    }
    const bool localOrderingSelected =
        bestCandidateIdx != localOrderingBaselineIdx;
    const int controlledContinuationBaselineIdx = bestCandidateIdx;
    if (
        !rootSelectionLocked
        && !recoverySearchUsed
        && !rootPotions
        && !rootIsCardSelect
    ) {
        bestCandidateIdx = applyControlledFreeContinuationDominance(
            searchers,
            rootActionCandidates,
            rootActionEdgeIndices,
            rootControlledContinuations,
            bestCandidateIdx
        );
    }
    const bool controlledContinuationDominanceSelected =
        bestCandidateIdx != controlledContinuationBaselineIdx;
    const int robustContinuationBaselineIdx = bestCandidateIdx;
    if (
        !recoverySearchUsed
        && !rootPotions
        && !rootIsCardSelect
        && !endTurnDominanceSelected
        && !dangerPolicySelected
        && !reliableActionEndTurnSelected
        && !localOrderingSelected
        && !controlledContinuationDominanceSelected
    ) {
        bestCandidateIdx = search::applyRobustContinuationDominance(
            rootActionCandidates,
            bestCandidateIdx
        );
    }
    const bool robustContinuationDominanceSelected =
        bestCandidateIdx != robustContinuationBaselineIdx;
    const int pairedBoundaryBaselineIdx = bestCandidateIdx;
    if (
        !recoverySearchUsed
        && !rootPotions
        && !rootIsCardSelect
        && !endTurnDominanceSelected
        && !dangerPolicySelected
        && !reliableActionEndTurnSelected
        && !localOrderingSelected
        && !controlledContinuationDominanceSelected
        && !robustContinuationDominanceSelected
    ) {
        bestCandidateIdx = search::applyPairedBoundaryEndTurnDominanceVeto(
            rootActionCandidates,
            bestCandidateIdx
        );
    }
    const bool pairedBoundaryDominanceSelected =
        bestCandidateIdx != pairedBoundaryBaselineIdx;
    const int certifiedWinRiskBaselineIdx = bestCandidateIdx;
    bestCandidateIdx = search::applyCertifiedWinRiskGate(
        rootActionCandidates,
        bestCandidateIdx
    );
    const bool certifiedWinRiskVetoSelected =
        bestCandidateIdx != certifiedWinRiskBaselineIdx;
    const bool certifiedWinAlternativeAccepted =
        std::any_of(
            rootActionCandidates.begin(),
            rootActionCandidates.end(),
            [] (const auto &candidate) {
                return candidate.certifiedImmediateWin;
            }
        )
        && !rootActionCandidates[bestCandidateIdx].certifiedImmediateWin;
    for (int i = 0; i < static_cast<int>(rootActionStats.size()); ++i) {
        rootActionStats[i]["selectionValue"] =
            certifiedWinRiskVetoSelected
            ? rootActionCandidates[i].certifiedImmediateWinUtility
            : recoveryCompleteCombatParetoSelected
            ? rootActionCandidates[i].winSampleRate
            : recoverySearchUsed
            ? recoverySearch.candidates[i].meanQuality
            : strictBlockDominanceSelected
            ? rootActionCandidates[i].meanDirectEndBoundaryBlock
            : pairedBoundaryDominanceSelected
            ? rootActionCandidates[i].meanDirectEndBoundaryValue
            : commonBoundaryDominanceSelected
            ? rootActionCandidates[i].meanDirectEndBoundaryValue
            : dangerPolicySelected
                    ? rootActionCandidates[i].winSampleRate
            : reliableActionEndTurnSelected
                    ? rootActionCandidates[i].winSampleRate
            : robustContinuationDominanceSelected
                    ? rootActionCandidates[i].lowerQuartileBestWinUtility
            : hasSuccessUtility
                    ? rootActionCandidates[i].successUtility
                    : hasBroadSparseWins
                    ? rootActionCandidates[i].meanBestWinUtility
                    : rootActionCandidates[i]
                          .meanBestNextDecisionEffectiveHp
            ;
    }
    bestActionIdx = rootActionEdgeIndices[bestCandidateIdx];
    bestScore = rootActionCandidates[bestCandidateIdx].meanValue;
    bestOptimisticScore = rootActionStats[bestCandidateIdx]["optimisticValue"];
    nlohmann::json monsterIdxMapJson = nlohmann::json::array();
    for (int i = 0; i < 5; ++i) {
        monsterIdxMapJson[i] = monsterIdxMap[i];
    }

    nlohmann::json actionDescriptions = nlohmann::json::array();
    auto action = searchers[0]->root.edges[bestActionIdx].action;
    const auto rootAction = action;
    const auto selectedActionDescription = describeAction(
        rootAction,
        baseBc,
        monsterIdxMap,
        baseBc.inputState == InputState::CARD_SELECT
    );
    if (
        bestCandidateIdx >= static_cast<int>(rootActionStats.size())
        || rootActionStats[bestCandidateIdx]["action"]
           != selectedActionDescription
    ) {
        std::cerr
            << "selected root candidate does not match emitted action"
            << std::endl;
        return 2;
    }
    outfile <<"choose:\t"<<bestActionIdx<<"\n";
    action.printDesc(outfile, baseBc);
    actionDescriptions.push_back(selectedActionDescription);
    const auto controlledContinuation = evaluateControlledCardSelection(
        searchers,
        rootAction
    );
    action.execute(baseBc);

    // A mandatory player-controlled selector is part of the same game action.
    // Use the same selected child for parent evaluation and for execution.
    nlohmann::json followUp = nullptr;
    bool reachedNextSelector = false;
    if (
        baseBc.inputState == InputState::CARD_SELECT
        && controlledContinuation
    ) {
        auto *node = searchers[0]->root.edges[bestActionIdx].node.get();
        action = controlledContinuation->action;
        for (int depth = 0;
             baseBc.inputState == InputState::CARD_SELECT && depth < 8;
             ++depth) {
            if (node == nullptr) {
                std::cerr << "mandatory card selection has no search node" << std::endl;
                return 2;
            }
            const auto edge = std::find_if(
                node->edges.begin(),
                node->edges.end(),
                [&action] (const auto &candidate) {
                    return candidate.action == action;
                }
            );
            if (edge == node->edges.end()) {
                std::cerr << "mandatory card selection is missing from search tree"
                          << std::endl;
                return 2;
            }
            const auto step = describeCardSelection(action, baseBc);
            if (followUp.is_null()) {
                followUp = step;
            } else {
                if (followUp["task"] != step["task"]) {
                    std::cerr << "mandatory card selection changed task" << std::endl;
                    return 2;
                }
                for (const auto &card : step["cards"]) {
                    followUp["cards"].push_back(card);
                }
                if (!step["completionCommand"].is_null()) {
                    followUp["completionCommand"] = step["completionCommand"];
                }
            }
            outfile << "\n---select--\nchoose:\n";
            action.printDesc(outfile, baseBc);
            actionDescriptions.push_back(
                describeAction(action, baseBc, monsterIdxMap)
            );
            const auto selectionTask = baseBc.cardSelectInfo.cardSelectTask;
            const int selectionCount = baseBc.cardSelectInfo.pickCount;
            action.execute(baseBc);
            node = edge->node.get();
            if (baseBc.inputState == InputState::CARD_SELECT) {
                // A non-decreasing pick count is a new game selector (for
                // example, Echo Form playing Recycle twice), not another pick
                // on the current screen.  Let the caller complete this screen
                // and replan from the next one.
                if (
                    baseBc.cardSelectInfo.cardSelectTask != selectionTask
                    || baseBc.cardSelectInfo.pickCount >= selectionCount
                ) {
                    reachedNextSelector = true;
                    break;
                }
                const auto *next = node == nullptr
                    ? nullptr
                    : bestMandatoryContinuationEdge(*node, baseBc);
                if (next == nullptr) {
                    std::cerr << "mandatory card selection plan is incomplete"
                              << std::endl;
                    return 2;
                }
                action = next->action;
            }
        }
        if (baseBc.inputState == InputState::CARD_SELECT && !reachedNextSelector) {
            std::cerr << "mandatory card selection exceeded depth limit" << std::endl;
            return 2;
        }
    }

    nlohmann::json output = nlohmann::json::object();
    output["protocolVersion"] = 1;
    output["rootCommand"] = selectedActionDescription;
    output["followUp"] = followUp;
    output["monsterIdxMap"] = monsterIdxMapJson;
    output["actions"] = actionDescriptions;
    output["score"] = bestScore;
    output["selectionScore"] = certifiedWinRiskVetoSelected
        ? rootActionCandidates[bestCandidateIdx].certifiedImmediateWinUtility
        : recoveryCompleteCombatParetoSelected
        ? rootActionCandidates[bestCandidateIdx].winSampleRate
        : recoverySearchUsed
        ? recoverySearch.candidates[bestCandidateIdx].meanQuality
        : strictBlockDominanceSelected
        ? rootActionCandidates[bestCandidateIdx].meanDirectEndBoundaryBlock
        : pairedBoundaryDominanceSelected
        ? rootActionCandidates[bestCandidateIdx].meanDirectEndBoundaryValue
        : commonBoundaryDominanceSelected
        ? rootActionCandidates[bestCandidateIdx].meanDirectEndBoundaryValue
        : dangerPolicySelected
        ? rootActionCandidates[bestCandidateIdx].winSampleRate
        : reliableActionEndTurnSelected
        ? rootActionCandidates[bestCandidateIdx].winSampleRate
        : robustContinuationDominanceSelected
        ? rootActionCandidates[bestCandidateIdx].lowerQuartileBestWinUtility
        : hasSuccessUtility
                ? rootActionCandidates[bestCandidateIdx].successUtility
                : hasBroadSparseWins
                ? rootActionCandidates[bestCandidateIdx].meanBestWinUtility
                : rootActionCandidates[bestCandidateIdx]
                      .meanBestNextDecisionEffectiveHp
        ;
    output["optimisticSelectionScore"] = bestOptimisticScore;
    output["rootSelectionPolicy"] = certifiedWinRiskVetoSelected
        ? "certified_immediate_win_risk_floor"
        : recoveryCompleteCombatParetoSelected
        ? "complete_combat_pareto_over_recovery_horizon"
        : recoverySearchUsed
        ? "recovery_horizon_survival"
        : controlledContinuationDominanceSelected
        ? "controlled_free_continuation_dominance"
        : localOrderingSelected
        ? "local_two_card_ordering_dominance"
        : strictBlockDominanceSelected
        ? "strict_block_end_turn_dominance"
        : pairedBoundaryDominanceSelected
        ? "paired_rng_boundary_end_turn_dominance"
        : commonBoundaryDominanceSelected
        ? "common_boundary_end_turn_dominance"
        : dangerPolicySelected
        ? "danger_complete_combat_win_rate_then_end_hp"
        : reliableActionEndTurnSelected
        ? "reliable_action_over_survivor_biased_end_turn"
        : robustContinuationDominanceSelected
        ? "robust_complete_plan_dominance"
        : hasSuccessUtility
            ? "complete_combat_rng_stability_then_success_utility"
            : hasBroadSparseWins
            ? "best_complete_plan_by_rng_world"
            : "safe_replan_no_win_fallback";
    output["selectedCandidateIndex"] = bestCandidateIdx;
    output["certifiedWinAlternativeMinRate"] =
        search::kCertifiedWinAlternativeMinRate;
    output["certifiedWinRiskVeto"] = certifiedWinRiskVetoSelected;
    output["certifiedWinAlternativeAccepted"] =
        certifiedWinAlternativeAccepted;
    output["endTurnDominanceOverride"] =
        endTurnDominanceSelected || pairedBoundaryDominanceSelected;
    output["reliableActionEndTurnOverride"] =
        reliableActionEndTurnSelected;
    output["strictBlockEndTurnDominanceOverride"] =
        strictBlockDominanceSelected;
    output["pairedBoundaryEndTurnDominanceOverride"] =
        pairedBoundaryDominanceSelected;
    output["localActionOrderingOverride"] = localOrderingSelected;
    if (localOrderingSelected) {
        output["localActionOrderingBaseline"] = rootActionStats[
            localOrderingBaselineIdx
        ]["action"];
    }
    output["controlledContinuationDominanceOverride"] =
        controlledContinuationDominanceSelected;
    if (controlledContinuationDominanceSelected) {
        output["controlledContinuationDominanceBaseline"] =
            rootActionStats[controlledContinuationBaselineIdx]["action"];
    }
    output["rootAggregationPolicy"] = recoverySearchUsed
        ? "equal_rng_world_recovery_horizon"
        : "equal_rng_world_complete_combat";
    output["recoveryCompleteCombatParetoOverride"] =
        recoveryCompleteCombatParetoSelected;
    if (recoveryCompleteCombatParetoSelected) {
        output["recoveryCompleteCombatParetoBaseline"] = rootActionStats[
            recoveryBaselineCandidateIdx
        ]["action"];
    }
    output["credibleWinEvidence"] = hasSuccessUtility;
    output["searchStopReason"] = searchers[0]->stopReason;
    output["adaptiveSearch"] = {
        {"enabled",
         adaptiveMaxTimeMillis > maxTimeMillis
             && adaptiveMaxSimulations > simulationsPerThread},
        {"triggered", adaptiveSearchTriggered},
        {"reason", adaptiveAssessment.reason},
        {"baseElapsedMillis", baseElapsedMillis},
        {"totalElapsedMillis", adaptiveElapsedMillis},
        {"baseStopReason", baseStopReason},
        {"baseCredibleWinEvidence", adaptiveAssessment.credibleWins},
        {"baseWinningRootActions", adaptiveAssessment.winningRootActions},
        {"baseBestWinningRngWorlds", adaptiveAssessment.bestWinningWorlds},
        {"rngWorlds", adaptiveAssessment.rngWorlds},
        {"meanTranspositionStates",
         adaptiveAssessment.meanTranspositionStates},
        {"baseMaxTimeMillis", maxTimeMillis},
        {"adaptiveMaxTimeMillis", adaptiveMaxTimeMillis},
        {"baseSimulationsPerThread", simulationsPerThread},
        {"adaptiveMaxSimulationsPerThread", adaptiveMaxSimulations}
    };
    output["recoverySearch"] = {
        {"used", recoverySearchUsed},
        {"horizonTurns", kRecoveryHorizonTurns},
        {"elapsedMillis", recoverySearch.elapsedMillis},
        {"stopReason", recoverySearch.stopReason}
    };
    output["maxBackupWeight"] = maxBackupWeight;
    output["importedTurnCounters"] = importedTurnCounters;
    output["rootActions"] = rootActionStats;
    output["mctsGuidance"] = {
        {"turn", externalGuidanceTurn},
        {"prefer", preferredCardNames},
        {"discourage", discouragedCardNames},
        {"applied", guidanceApplied}
    };
    std::cout << output.dump() << std::endl;

    for (int i = 0; i < thread_count; ++i) {
        delete searchers[i];
    }
    return 0;
}
