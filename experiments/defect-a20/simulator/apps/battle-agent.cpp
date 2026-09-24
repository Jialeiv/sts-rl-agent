#include <iostream>
#include <fstream>
#include <thread>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <iomanip>

#include "combat/BattleContext.h"
#include "convert/BattleConverter.h"
#include "sim/search/BattleScumSearcher2.h"
#include "sim/search/RootActionPolicy.h"
#include "constants/Potions.h"

#include <nlohmann/json.hpp>

using namespace sts;

void runSearch(search::BattleScumSearcher2 &searcher, int simulations) {
    searcher.search(simulations, 10000);
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

int getGameTargetIdx(const int *monsterIdxMap, int targetIdx) {
    if (targetIdx < 0 || targetIdx >= 5 || monsterIdxMap == nullptr) {
        return targetIdx;
    }
    return monsterIdxMap[targetIdx];
}

std::string describeAction(const search::Action &action, const BattleContext &bc, const int *monsterIdxMap) {
    if (!action.isValidAction(bc)) {
        return "{ INVALID ACTION }";
    }

    std::ostringstream os;
    switch (action.getActionType()) {
        case search::ActionType::CARD: {
            const auto &card = bc.cards.hand[action.getSourceIdx()];
            os << "play " << action.getSourceIdx() + 1;
            if (card.requiresTarget()) {
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
                os << " " << getGameTargetIdx(monsterIdxMap, action.getTargetIdx());
            }
            return os.str();
        }
        case search::ActionType::SINGLE_CARD_SELECT: {
            const auto task = bc.cardSelectInfo.cardSelectTask;
            const auto idx = action.getSelectIdx();
            os << "{ " << cardSelectTaskStrings[static_cast<int>(task)] << " (" << idx << ")";
            const auto name = selectedCardNameForTask(bc, task, idx);
            if (!name.empty()) {
                os << " " << name;
            }
            os << " }";
            return os.str();
        }
        case search::ActionType::MULTI_CARD_SELECT: {
            const auto task = bc.cardSelectInfo.cardSelectTask;
            const auto selected = action.getSelectedIdxs();
            os << "{ " << cardSelectTaskStrings[static_cast<int>(task)];
            if (selected.empty()) {
                os << " none";
            } else {
                for (int i = 0; i < selected.size(); ++i) {
                    const auto idx = selected[i];
                    os << " (" << idx << ") " << bc.cards.hand[idx].getName();
                    if (i + 1 < selected.size()) {
                        os << ",";
                    }
                }
            }
            os << " }";
            return os.str();
        }
        case search::ActionType::END_TURN:
            return "end";
        default:
            action.printDesc(os, bc);
            return os.str();
    }
}

int pickBestActionIndex(const std::vector<search::BattleScumSearcher2 *> &searchers, const BattleContext &bc, std::ofstream &ofs) {
    if (searchers.empty()) {
        return -1;
    }

    const auto &rootEdges = searchers[0]->root.edges;
    std::vector<search::RootActionCandidate> candidates;
    std::vector<int> edgeIndices;
    for (int j = 0; j < static_cast<int>(rootEdges.size()); ++j) {
        std::vector<search::RootActionThreadSample> threadSamples;
        threadSamples.reserve(searchers.size());
        for (const auto *searcher : searchers) {
            const auto matchingEdge = std::find_if(
                searcher->root.edges.begin(),
                searcher->root.edges.end(),
                [&rootEdges, j] (const auto &edge) { return edge.action == rootEdges[j].action; }
            );
            if (matchingEdge == searcher->root.edges.end()) {
                continue;
            }
            threadSamples.push_back(search::sampleFromEdge(*matchingEdge));
        }

        const auto aggregate = search::aggregateRootActionThreads(threadSamples);
        if (aggregate.rngWorlds == 0) {
            continue;
        }

        const double meanScore = aggregate.meanValue;
        const double maxScore = aggregate.meanBestValue;
        const auto averageVisits = aggregate.visits / aggregate.rngWorlds;
        const double effectiveMaxWeight = searchers[0]->getEffectiveMaxBackupWeight(averageVisits);
        const double score = (1.0 - effectiveMaxWeight) * meanScore
                             + effectiveMaxWeight * maxScore;
        ofs <<j <<":"<< aggregate.visits << " visits / " << std::fixed << std::setprecision(5)
            << meanScore << " mean / " << maxScore << " best / " << score
            << " tree value / " << aggregate.successUtility
            << " success utility / " << aggregate.meanBestWinUtility
            << " mean best-win utility +/- "
            << aggregate.bestWinUtilityStandardError << " SE / "
            << aggregate.winWorldRate
            << " win-world rate (max weight " << effectiveMaxWeight
            << ") for ";
        searchers[0]->root.edges[j].action.printDesc(ofs, bc);
        ofs << std::endl;

        // This app does not collect direct end-turn boundary evidence, so the
        // danger policy remains off. Stolen gold matters only on the first
        // action of a turn, when the search compares complete turn plans.
        search::RootActionCandidate candidate = search::candidateFromAggregate(
            aggregate,
            bc,
            rootEdges[j].action.getActionType() == search::ActionType::END_TURN
        );
        if (bc.player.cardsPlayedThisTurn == 0) {
            candidate.expectedEscapedStolenGoldOnWin =
                aggregate.expectedEscapedStolenGoldOnWin;
        }
        candidates.push_back(std::move(candidate));
        edgeIndices.push_back(j);
    }

    const int candidateIdx =
        search::selectRootActionWithEndTurnSafety(candidates);
    return candidateIdx >= 0 ? edgeIndices[candidateIdx] : -1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "usage: battle-agent <input.json> [simulations_per_thread] [thread_count] [forced_first_action_index]" << std::endl;
        return 1;
    }

    int simulationsPerThread = 100000;
    int threadCount = 8;
    int forcedFirstActionIdx = -1;

    if (argc >= 3) {
        simulationsPerThread = std::max(1, std::stoi(argv[2]));
    }
    if (argc >= 4) {
        threadCount = std::max(1, std::stoi(argv[3]));
    }
    if (argc >= 5) {
        forcedFirstActionIdx = std::stoi(argv[4]);
    }

    std::ifstream file(argv[1]);
    if (!file) {
        std::cerr << "failed to open input json: " << argv[1] << std::endl;
        return 1;
    }

    std::ostringstream ss;
    ss << file.rdbuf();

    BattleConverter converter;
    int monsterIdxMap[5];
    for (int i = 0; i < 5; ++i) {
        monsterIdxMap[i] = -1;
    }

    auto json = nlohmann::json::parse(ss.str());
    BattleContext bc = converter.convertFromJson(json, monsterIdxMap);

    nlohmann::json actionDescriptions = nlohmann::json::array();

    std::ofstream ofs;
    ofs.open("agent.log", std::ios_base::app);
    ofs << "=== begin battle ===\n";

    int decisionCount = 0;
    while (bc.outcome == Outcome::UNDECIDED) {
        std::vector<search::BattleScumSearcher2 *> searchers;
        std::vector<std::thread> threads;
        searchers.reserve(threadCount);
        threads.reserve(threadCount);

        ofs << bc << std::endl;
        for (int i = 0; i < threadCount; ++i) {
            BattleContext threadBc = bc;
            const int rngMod = i * 1000;
            threadBc.cardRandomRng.setCounter(rngMod);
            threadBc.aiRng.setCounter(rngMod);
            threadBc.shuffleRng.setCounter(rngMod);
            threadBc.miscRng.setCounter(rngMod);
            threadBc.potionRng.setCounter(rngMod);

            auto *searcher = new search::BattleScumSearcher2(threadBc);
            searchers.push_back(searcher);
            threads.emplace_back(runSearch, std::ref(*searcher), simulationsPerThread);
        }

        for (auto &thread : threads) {
            thread.join();
        }

        int bestActionIdx = pickBestActionIndex(searchers, bc, ofs);
        if (decisionCount == 0 && forcedFirstActionIdx >= 0) {
            bestActionIdx = forcedFirstActionIdx;
        }
        if (bestActionIdx < 0 || bestActionIdx >= static_cast<int>(searchers[0]->root.edges.size())) {
            for (auto *searcher : searchers) {
                delete searcher;
            }
            std::cerr << "search failed to produce a valid action" << std::endl;
            return 2;
        }

        auto action = searchers[0]->root.edges[bestActionIdx].action;
        actionDescriptions.push_back(describeAction(action, bc, monsterIdxMap));
        action.printDesc(ofs, bc);
        action.execute(bc);

        for (auto *searcher : searchers) {
            delete searcher;
        }
        ++decisionCount;
        ofs << "\n---\n";
    }

    nlohmann::json monsterIdxMapJson = nlohmann::json::array();
    for (int i = 0; i < 5; ++i) {
        monsterIdxMapJson[i] = monsterIdxMap[i];
    }

    nlohmann::json output = nlohmann::json::object();
    output["monsterIdxMap"] = monsterIdxMapJson;
    output["actions"] = actionDescriptions;
    output["outcome"] = static_cast<int>(bc.outcome);
    output["playerCurHp"] = bc.player.curHp;
    output["turn"] = bc.turn;

    std::cout << output.dump() << std::endl;
    return 0;
}
