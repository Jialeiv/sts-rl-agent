#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "convert/ProspectiveBattle.h"
#include "sim/search/BattleScumSearcher2.h"
#include "sim/search/RootActionPolicy.h"

using namespace sts;

namespace {

struct SelectedWorld {
    search::RootActionThreadSample sample;
    nlohmann::json audit;
};

std::string baseCardName(std::string name) {
    const auto plus = name.find('+');
    if (plus != std::string::npos) {
        name.resize(plus);
    }
    return name;
}

std::vector<std::string> trackedStartupCards(
    const evaluation::ProspectiveBattleSpec &spec
) {
    std::vector<std::string> result;
    const auto diagnostics = spec.source.find("diagnostics");
    if (diagnostics == spec.source.end() || !diagnostics->is_object()) {
        return result;
    }
    const auto cards = diagnostics->find("tracked_startup_cards");
    if (cards == diagnostics->end() || !cards->is_array()) {
        return result;
    }
    for (const auto &raw : *cards) {
        if (!raw.is_string()) {
            throw std::runtime_error(
                "diagnostics.tracked_startup_cards must contain strings"
            );
        }
        const auto name = baseCardName(raw.get<std::string>());
        if (
            !name.empty()
            && std::find(result.begin(), result.end(), name) == result.end()
        ) {
            result.push_back(name);
        }
    }
    return result;
}

struct StartupTracker {
    explicit StartupTracker(const std::vector<std::string> &names) {
        for (const auto &name : names) {
            firstSeen.emplace(name, -1);
            firstPlayed.emplace(name, -1);
        }
    }

    void observeHand(const BattleContext &battle) {
        for (int index = 0; index < battle.cards.cardsInHand; ++index) {
            const auto name = baseCardName(battle.cards.hand[index].getName());
            const auto found = firstSeen.find(name);
            if (found != firstSeen.end() && found->second < 0) {
                found->second = battle.turn;
            }
        }
    }

    void observeAction(
        const search::Action &action,
        const BattleContext &battle
    ) {
        if (action.getActionType() != search::ActionType::CARD) {
            return;
        }
        const auto index = action.getSourceIdx();
        if (index < 0 || index >= battle.cards.cardsInHand) {
            return;
        }
        const auto name = baseCardName(battle.cards.hand[index].getName());
        const auto found = firstPlayed.find(name);
        if (found != firstPlayed.end() && found->second < 0) {
            found->second = battle.turn;
        }
    }

    nlohmann::json json() const {
        nlohmann::json result = nlohmann::json::object();
        for (const auto &[name, seenTurn] : firstSeen) {
            const auto playedTurn = firstPlayed.at(name);
            result[name] = {
                {"first_seen_turn", seenTurn >= 0
                    ? nlohmann::json(seenTurn)
                    : nlohmann::json(nullptr)},
                {"first_played_turn", playedTurn >= 0
                    ? nlohmann::json(playedTurn)
                    : nlohmann::json(nullptr)},
            };
        }
        return result;
    }

    std::unordered_map<std::string, int> firstSeen;
    std::unordered_map<std::string, int> firstPlayed;
};

nlohmann::json aggregateStartupAccess(
    const nlohmann::json &trials,
    const std::vector<std::string> &trackedCards
) {
    nlohmann::json result = nlohmann::json::object();
    const auto attempts = static_cast<int>(trials.size());
    for (const auto &name : trackedCards) {
        int seenBy[4] {0, 0, 0, 0};
        int playedBy[4] {0, 0, 0, 0};
        int seen = 0;
        int played = 0;
        for (const auto &trial : trials) {
            const auto startup = trial.find("startup_cards");
            if (
                startup == trial.end()
                || !startup->is_object()
                || !startup->contains(name)
            ) {
                continue;
            }
            const auto &row = startup->at(name);
            if (row["first_seen_turn"].is_number_integer()) {
                const auto turn = row["first_seen_turn"].get<int>();
                ++seen;
                for (int deadline = 1; deadline <= 3; ++deadline) {
                    seenBy[deadline] += turn <= deadline;
                }
            }
            if (row["first_played_turn"].is_number_integer()) {
                const auto turn = row["first_played_turn"].get<int>();
                ++played;
                for (int deadline = 1; deadline <= 3; ++deadline) {
                    playedBy[deadline] += turn <= deadline;
                }
            }
        }
        auto rate = [attempts](int count) {
            return attempts > 0
                ? static_cast<double>(count) / static_cast<double>(attempts)
                : 0.0;
        };
        result[name] = {
            {"seen_by_turn", {
                {"1", rate(seenBy[1])},
                {"2", rate(seenBy[2])},
                {"3", rate(seenBy[3])},
            }},
            {"played_by_turn", {
                {"1", rate(playedBy[1])},
                {"2", rate(playedBy[2])},
                {"3", rate(playedBy[3])},
            }},
            {"never_seen_rate", rate(attempts - seen)},
            {"seen_but_not_played_rate", rate(seen - played)},
        };
    }
    return result;
}

int selectEvaluationAction(
    const search::BattleScumSearcher2 &searcher,
    const BattleContext &battle
) {
    std::vector<search::RootActionCandidate> candidates;
    std::vector<int> edgeIndices;
    candidates.reserve(searcher.root.edges.size());
    edgeIndices.reserve(searcher.root.edges.size());
    for (int index = 0;
         index < static_cast<int>(searcher.root.edges.size());
         ++index) {
        const auto &edge = searcher.root.edges[index];
        if (edge.simulationCount <= 0) {
            continue;
        }
        const auto inverseVisits =
            1.0 / static_cast<double>(edge.simulationCount);
        const auto winRate =
            static_cast<double>(edge.winSamples) * inverseVisits;
        const auto successUtility = edge.winUtilitySum * inverseVisits;
        candidates.push_back({
            edge.evaluationSum * inverseVisits,
            successUtility,
            winRate,
            edge.winSamples > 0
                ? edge.winEndHpSum / static_cast<double>(edge.winSamples)
                : 0.0,
            edge.simulationCount,
            winRate,
            successUtility,
            edge.action.getActionType() == search::ActionType::END_TURN,
            edge.maxEvaluation,
            edge.reachedNextDecision ? 1.0 : 0.0,
            static_cast<double>(edge.bestNextDecisionHp),
            static_cast<double>(edge.bestNextDecisionEffectiveHp),
            static_cast<double>(edge.bestNextDecisionPotionCount),
            static_cast<double>(battle.player.maxHp),
            edge.bestNextDecisionValue,
        });
        candidates.back().expectedEscapedStolenGoldOnWin =
            edge.winSamples > 0
                ? edge.escapedStolenGoldWinSum
                    / static_cast<double>(edge.winSamples)
                : 0.0;
        candidates.back().winningRngWorlds =
            edge.foundWinningLine ? 1 : 0;
        candidates.back().rngWorlds = 1;
        candidates.back().winWorldRate = edge.foundWinningLine ? 1.0 : 0.0;
        candidates.back().lowerQuartileBestWinUtility = edge.bestWinUtility;
        candidates.back().meanBestWinUtility = edge.bestWinUtility;
        candidates.back().meanBestWinEndHp = edge.bestWinEndHp;
        candidates.back().meanBestWinPotionCount = edge.bestWinPotionCount;
        edgeIndices.push_back(index);
    }
    if (candidates.empty()) {
        return -1;
    }
    const auto selected = battle.inputState == InputState::CARD_SELECT
        ? search::selectRootActionByWinRateBandThenEndHp(candidates)
        : search::selectRootActionWithEndTurnSafety(candidates);
    return selected >= 0
        && selected < static_cast<int>(edgeIndices.size())
        ? edgeIndices[selected]
        : -1;
}

nlohmann::json evaluateIndependentBattle(
    const GameContext &game,
    MonsterEncounter encounter,
    int world,
    std::int64_t simulationsPerDecision,
    long maxTimeMillisPerDecision,
    int maxDecisions,
    const std::vector<std::string> &trackedCards
) {
    auto battle = evaluation::buildProspectiveBattle(game, encounter, world);
    const auto startingHp = battle.player.curHp;
    const auto startingMaxHp = battle.player.maxHp;
    int decisions = 0;
    std::int64_t totalVisits = 0;
    std::string stopReason = "terminal";
    StartupTracker startup(trackedCards);
    while (
        battle.outcome == Outcome::UNDECIDED
        && decisions < maxDecisions
    ) {
        startup.observeHand(battle);
        search::BattleScumSearcher2 searcher(battle);
        searcher.allowPotions = false;
        searcher.search(simulationsPerDecision, maxTimeMillisPerDecision);
        totalVisits += searcher.root.simulationCount;
        const auto selected = selectEvaluationAction(searcher, battle);
        if (
            selected < 0
            || selected >= static_cast<int>(searcher.root.edges.size())
        ) {
            stopReason = "no_usable_action";
            break;
        }
        const auto action = searcher.root.edges[selected].action;
        startup.observeAction(action, battle);
        action.execute(battle);
        ++decisions;
    }
    if (
        battle.outcome == Outcome::UNDECIDED
        && decisions >= maxDecisions
    ) {
        stopReason = "decision_limit";
    }
    const bool won = battle.outcome == Outcome::PLAYER_VICTORY;
    const bool completed = battle.outcome != Outcome::UNDECIDED;
    return {
        {"world", world},
        {"entry_hp", game.curHp},
        {"opening_hp", startingHp},
        {"max_hp", startingMaxHp},
        {"won", won},
        {"completed", completed},
        {"ending_hp", won ? battle.player.curHp : 0},
        {"turns", battle.turn},
        {"decisions", decisions},
        {"search_visits", totalVisits},
        {"stop_reason", stopReason},
        {"startup_cards", startup.json()},
    };
}

nlohmann::json evaluateIndependentBattles(
    const evaluation::ProspectiveBattleSpec &spec,
    int worldStart,
    int worldCount,
    std::int64_t simulationsPerDecision,
    long maxTimeMillisPerDecision,
    int maxDecisions
) {
    const auto trackedCards = trackedStartupCards(spec);
    nlohmann::json targets = nlohmann::json::array();
    for (const auto encounter : spec.targets) {
        auto evaluateOne = [&](
            const std::optional<evaluation::CandidateSpec> &candidate
        ) {
            const auto game = evaluation::buildProspectiveGame(spec, candidate);
            nlohmann::json trials = nlohmann::json::array();
            int wins = 0;
            int completed = 0;
            double winningHpSum = 0.0;
            for (int offset = 0; offset < worldCount; ++offset) {
                const auto world = worldStart + offset;
                auto trial = evaluateIndependentBattle(
                    game,
                    encounter,
                    world,
                    simulationsPerDecision,
                    maxTimeMillisPerDecision,
                    maxDecisions,
                    trackedCards
                );
                if (trial["won"].get<bool>()) {
                    ++wins;
                    winningHpSum += trial["ending_hp"].get<double>();
                }
                if (trial["completed"].get<bool>()) {
                    ++completed;
                }
                trials.push_back(std::move(trial));
            }
            return nlohmann::json {
                {"action", candidate.has_value()
                    ? "choose " + std::to_string(candidate->choiceId)
                    : "skip"},
                {"card", candidate.has_value()
                    ? candidate->card.card.getName()
                    : "SKIP"},
                {"aggregate", {
                    {"attempts", worldCount},
                    {"wins", wins},
                    {"non_wins", worldCount - wins},
                    {"completed", completed},
                    {"incomplete", worldCount - completed},
                    {"win_rate", worldCount > 0
                        ? static_cast<double>(wins)
                            / static_cast<double>(worldCount)
                        : 0.0},
                    {"expected_end_hp_on_win", wins > 0
                        ? winningHpSum / static_cast<double>(wins)
                        : 0.0},
                }},
                {"startup_access", aggregateStartupAccess(
                    trials,
                    trackedCards
                )},
                {"trials", trials},
            };
        };

        auto baseline = evaluateOne(std::nullopt);
        auto targetResult = baseline;
        targetResult["target"] =
            monsterEncouterNames[static_cast<int>(encounter)];
        if (!spec.candidates.empty()) {
            nlohmann::json candidateResults = nlohmann::json::array();
            candidateResults.push_back(std::move(baseline));
            for (const auto &candidate : spec.candidates) {
                candidateResults.push_back(evaluateOne(candidate));
            }
            targetResult["variants"] = std::move(candidateResults);
        }
        targets.push_back(std::move(targetResult));
    }
    return {
        {"schema_version", 1},
        {"mode", "independent_battle_evaluation"},
        {"assumptions", {
            {"snapshot", "fight_now"},
            {"potions_allowed_by_mcts", false},
            {"one_binary_outcome_per_rng_world", true},
            {"tree_rollouts_are_not_counted_as_probability_trials", true},
        }},
        {"policy", {
            {"world_start", worldStart},
            {"rng_worlds", worldCount},
            {"simulations_per_decision", simulationsPerDecision},
            {"max_time_ms_per_decision", maxTimeMillisPerDecision},
            {"max_decisions_per_battle", maxDecisions},
            {"root_action_policy",
             "single_world_online_mcts_with_runtime_root_policy"},
        }},
        {"targets", targets},
    };
}

std::string actionDescription(
    const search::Action &action,
    const BattleContext &bc
) {
    std::ostringstream stream;
    action.printDesc(stream, bc);
    return stream.str();
}

nlohmann::json openingAudit(const BattleContext &bc) {
    nlohmann::json hand = nlohmann::json::array();
    for (int index = 0; index < bc.cards.cardsInHand; ++index) {
        hand.push_back(bc.cards.hand[index].getName());
    }
    nlohmann::json monsters = nlohmann::json::array();
    for (int index = 0; index < bc.monsters.monsterCount; ++index) {
        const auto &monster = bc.monsters.arr[index];
        if (monster.idx < 0) {
            continue;
        }
        monsters.push_back({
            {"name", monster.getName()},
            {"current_hp", monster.curHp},
            {"max_hp", monster.maxHp},
        });
    }
    return {
        {"hand", hand},
        {"monsters", monsters},
        {"player_hp", bc.player.curHp},
        {"player_max_hp", bc.player.maxHp},
        {"energy", bc.player.energy},
    };
}

SelectedWorld evaluateWorld(
    const GameContext &gc,
    MonsterEncounter encounter,
    int world,
    std::int64_t simulations,
    long maxTimeMillis
) {
    auto battle = evaluation::buildProspectiveBattle(gc, encounter, world);
    const auto opening = openingAudit(battle);
    search::BattleScumSearcher2 searcher(battle);
    searcher.search(simulations, maxTimeMillis);

    std::vector<search::RootActionCandidate> candidates;
    candidates.reserve(searcher.root.edges.size());
    for (const auto &edge : searcher.root.edges) {
        if (edge.simulationCount <= 0) {
            candidates.push_back({});
            continue;
        }
        const auto inverseVisits =
            1.0 / static_cast<double>(edge.simulationCount);
        const auto winRate =
            static_cast<double>(edge.winSamples) * inverseVisits;
        candidates.push_back({
            edge.evaluationSum * inverseVisits,
            edge.winUtilitySum * inverseVisits,
            winRate,
            edge.winSamples > 0
                ? edge.winEndHpSum / static_cast<double>(edge.winSamples)
                : 0.0,
            edge.simulationCount,
            0.0,
            0.0,
            edge.action.getActionType() == search::ActionType::END_TURN,
            edge.maxEvaluation,
            edge.reachedNextDecision ? 1.0 : 0.0,
            static_cast<double>(edge.bestNextDecisionHp),
            static_cast<double>(edge.bestNextDecisionEffectiveHp),
            static_cast<double>(edge.bestNextDecisionPotionCount),
            static_cast<double>(battle.player.maxHp),
            edge.bestNextDecisionValue,
        });
        candidates.back().winningRngWorlds =
            edge.foundWinningLine ? 1 : 0;
        candidates.back().rngWorlds = 1;
        candidates.back().winWorldRate = edge.foundWinningLine ? 1.0 : 0.0;
        candidates.back().lowerQuartileBestWinUtility = edge.bestWinUtility;
        candidates.back().meanBestWinUtility = edge.bestWinUtility;
        candidates.back().meanBestWinEndHp = edge.bestWinEndHp;
        candidates.back().meanBestWinPotionCount = edge.bestWinPotionCount;
    }
    const auto selectedIdx =
        search::selectRootActionBySuccessUtility(candidates);
    if (
        selectedIdx < 0
        || selectedIdx >= static_cast<int>(searcher.root.edges.size())
    ) {
        throw std::runtime_error("MCTS did not evaluate a usable root action");
    }
    const auto &edge = searcher.root.edges[selectedIdx];
    search::RootActionThreadSample sample {
        edge.simulationCount,
        edge.evaluationSum,
        edge.evaluationSquaredSum,
        edge.maxEvaluation,
        edge.winSamples,
        edge.lossSamples,
        edge.cutoffSamples,
        edge.winEndHpSum,
        edge.winUtilitySum,
        edge.lizardTailConsumedWinSamples,
        edge.reachedNextDecision,
        static_cast<double>(edge.bestNextDecisionHp),
        static_cast<double>(edge.bestNextDecisionEffectiveHp),
        static_cast<double>(edge.bestNextDecisionPotionCount),
        edge.bestNextDecisionValue,
        edge.escapedStolenGoldWinSum,
        edge.foundWinningLine,
        edge.bestWinUtility,
        static_cast<double>(edge.bestWinEndHp),
        static_cast<double>(edge.bestWinPotionCount),
    };
    const auto inverseVisits =
        1.0 / static_cast<double>(std::max<std::int64_t>(1, edge.simulationCount));
    return {
        sample,
        {
            {"world", world},
            {"opening", opening},
            {"selected_action", actionDescription(edge.action, battle)},
            {"visits", edge.simulationCount},
            {"win_rate", static_cast<double>(edge.winSamples) * inverseVisits},
            {"loss_rate", static_cast<double>(edge.lossSamples) * inverseVisits},
            {"cutoff_rate", static_cast<double>(edge.cutoffSamples) * inverseVisits},
            {"expected_end_hp_on_win",
             edge.winSamples > 0
                 ? edge.winEndHpSum / static_cast<double>(edge.winSamples)
                 : 0.0},
            {"search_stop_reason", searcher.stopReason},
        },
    };
}

nlohmann::json aggregateWorlds(const std::vector<SelectedWorld> &worlds) {
    std::vector<search::RootActionThreadSample> samples;
    nlohmann::json audit = nlohmann::json::array();
    samples.reserve(worlds.size());
    for (const auto &world : worlds) {
        samples.push_back(world.sample);
        audit.push_back(world.audit);
    }
    const auto aggregate = search::aggregateRootActionThreads(samples);
    return {
        {"aggregate", {
            {"rng_worlds", aggregate.rngWorlds},
            {"visits", aggregate.visits},
            {"value", aggregate.meanValue},
            {"variance", aggregate.variance},
            {"win_rate", aggregate.winSampleRate},
            {"loss_rate", aggregate.lossSampleRate},
            {"cutoff_rate", aggregate.cutoffSampleRate},
            {"expected_end_hp_on_win", aggregate.expectedEndHpOnWin},
            {"success_utility", aggregate.successUtility},
            {"lizard_tail_consumed_rate_on_win",
             aggregate.lizardTailConsumedRateOnWin},
        }},
        {"worlds", audit},
    };
}

nlohmann::json evaluateVariant(
    const evaluation::ProspectiveBattleSpec &spec,
    const std::optional<evaluation::CandidateSpec> &candidate,
    MonsterEncounter target,
    int worldCount,
    std::int64_t simulations,
    long maxTimeMillis
) {
    const auto game = evaluation::buildProspectiveGame(spec, candidate);
    std::vector<SelectedWorld> worlds;
    worlds.reserve(worldCount);
    for (int world = 0; world < worldCount; ++world) {
        worlds.push_back(evaluateWorld(
            game,
            target,
            world,
            simulations,
            maxTimeMillis
        ));
    }
    auto result = aggregateWorlds(worlds);
    result["action"] = candidate.has_value()
        ? "choose " + std::to_string(candidate->choiceId)
        : "skip";
    result["card"] = candidate.has_value()
        ? candidate->card.card.getName()
        : "SKIP";
    result["deck_size"] = game.deck.size();
    return result;
}

nlohmann::json evaluate(
    const evaluation::ProspectiveBattleSpec &spec,
    int worldCount,
    std::int64_t simulations,
    long maxTimeMillis
) {
    nlohmann::json targetResults = nlohmann::json::array();
    for (const auto target : spec.targets) {
        nlohmann::json variants = nlohmann::json::array();
        variants.push_back(evaluateVariant(
            spec,
            std::nullopt,
            target,
            worldCount,
            simulations,
            maxTimeMillis
        ));
        for (const auto &candidate : spec.candidates) {
            variants.push_back(evaluateVariant(
                spec,
                candidate,
                target,
                worldCount,
                simulations,
                maxTimeMillis
            ));
        }
        const auto skipWinRate =
            variants[0]["aggregate"]["win_rate"].get<double>();
        const auto skipEndHp =
            variants[0]["aggregate"]["expected_end_hp_on_win"].get<double>();
        for (auto &variant : variants) {
            variant["delta_win_rate_vs_skip"] =
                variant["aggregate"]["win_rate"].get<double>() - skipWinRate;
            variant["delta_end_hp_vs_skip"] =
                variant["aggregate"]["expected_end_hp_on_win"].get<double>()
                - skipEndHp;
        }
        targetResults.push_back({
            {"target", monsterEncouterNames[static_cast<int>(target)]},
            {"variants", variants},
        });
    }
    return {
        {"schema_version", 1},
        {"mode", "validation_only"},
        {"assumptions", {
            {"snapshot", "fight_now"},
            {"future_cards_relics_upgrades", false},
            {"potions_allowed_by_mcts", false},
            {"same_rng_worlds_for_each_variant", true},
        }},
        {"budget", {
            {"rng_worlds", worldCount},
            {"simulations_per_world", simulations},
            {"max_time_ms_per_world", maxTimeMillis},
        }},
        {"base_deck_size", spec.deck.size()},
        {"targets", targetResults},
    };
}

nlohmann::json selfTestInput() {
    nlohmann::json deck = nlohmann::json::array();
    for (int index = 0; index < 5; ++index) {
        deck.push_back({{"id", "Strike_R"}, {"upgrades", 0}});
    }
    for (int index = 0; index < 4; ++index) {
        deck.push_back({{"id", "Defend_R"}, {"upgrades", 0}});
    }
    deck.push_back({{"id", "Bash"}, {"upgrades", 0}});
    deck.push_back({{"id", "AscendersBane"}, {"upgrades", 0}});
    return {
        {"game_state", {
            {"seed", 12345},
            {"ascension_level", 20},
            {"act", 1},
            {"floor", 10},
            {"current_hp", 50},
            {"max_hp", 75},
            {"gold", 100},
            {"class", "IRONCLAD"},
            {"deck", deck},
            {"relics", nlohmann::json::array({
                {{"id", "Burning Blood"}, {"counter", -1}},
                {{"id", "Pantograph"}, {"counter", -1}},
            })},
            {"potions", nlohmann::json::array({
                {{"id", "Potion Slot"}},
                {{"id", "Potion Slot"}},
            })},
        }},
        {"candidates", nlohmann::json::array({
            {{"choice_id", 0}, {"id", "Inflame"}, {"upgrades", 1}},
        })},
        {"targets", nlohmann::json::array({"The Guardian"})},
    };
}

int runSelfTest() {
    const auto input = selfTestInput();
    const auto spec = evaluation::parseProspectiveBattleSpec(input);
    const auto skipGame =
        evaluation::buildProspectiveGame(spec, std::nullopt);
    const auto candidateGame =
        evaluation::buildProspectiveGame(spec, spec.candidates[0]);
    if (skipGame.deck.size() != 11 || candidateGame.deck.size() != 12) {
        throw std::runtime_error("candidate deck differs by more than one card");
    }
    if (
        skipGame.deck.cardTypeCounts[static_cast<int>(CardType::ATTACK)] != 6
        || skipGame.deck.cardTypeCounts[static_cast<int>(CardType::SKILL)] != 4
        || skipGame.deck.cardTypeCounts[static_cast<int>(CardType::CURSE)] != 1
        || candidateGame.deck.cardTypeCounts[
            static_cast<int>(CardType::POWER)
        ] != 1
    ) {
        throw std::runtime_error("raw deck card type metadata was counted incorrectly");
    }
    const auto &added = candidateGame.deck.cards.back();
    if (added.getId() != CardId::INFLAME || !added.isUpgraded()) {
        throw std::runtime_error("candidate card id or upgrade was not preserved");
    }
    const auto skipBattle =
        evaluation::buildProspectiveBattle(skipGame, spec.targets[0], 0);
    const auto candidateBattle =
        evaluation::buildProspectiveBattle(candidateGame, spec.targets[0], 0);
    if (skipBattle.encounter != MonsterEncounter::THE_GUARDIAN) {
        throw std::runtime_error("target encounter was not preserved");
    }
    if (skipBattle.player.curHp != 75 || candidateBattle.player.curHp != 75) {
        throw std::runtime_error("boss-room Pantograph trigger was not applied");
    }
    if (
        skipBattle.monsters.arr[0].maxHp
        != candidateBattle.monsters.arr[0].maxHp
    ) {
        throw std::runtime_error("paired variants did not share monster HP RNG");
    }

    auto normalInput = input;
    normalInput["targets"] = nlohmann::json::array({"Jaw Worm"});
    const auto normalSpec = evaluation::parseProspectiveBattleSpec(normalInput);
    const auto normalBattle = evaluation::buildProspectiveBattle(
        skipGame,
        normalSpec.targets[0],
        0
    );
    if (normalBattle.encounter != MonsterEncounter::JAW_WORM) {
        throw std::runtime_error("non-boss encounter was not preserved");
    }
    if (normalBattle.player.curHp != 50) {
        throw std::runtime_error("non-boss encounter incorrectly triggered Pantograph");
    }

    auto invalid = input;
    invalid["candidates"][0]["upgrades"] = 2;
    bool rejectedMultiUpgrade = false;
    try {
        evaluation::parseProspectiveBattleSpec(invalid);
    } catch (const std::runtime_error &) {
        rejectedMultiUpgrade = true;
    }
    if (!rejectedMultiUpgrade) {
        throw std::runtime_error("multi-upgrade card was silently accepted");
    }

    auto missingBottle = input;
    missingBottle["game_state"]["relics"].push_back({
        {"id", "Bottled Flame"},
        {"counter", -1},
    });
    bool rejectedMissingBottle = false;
    try {
        const auto bottleSpec =
            evaluation::parseProspectiveBattleSpec(missingBottle);
        evaluation::buildProspectiveGame(bottleSpec, std::nullopt);
    } catch (const std::runtime_error &) {
        rejectedMissingBottle = true;
    }
    if (!rejectedMissingBottle) {
        throw std::runtime_error("missing bottled card identity was silently accepted");
    }

    auto defectInput = input;
    defectInput["game_state"]["class"] = "DEFECT";
    defectInput["game_state"]["deck"] = nlohmann::json::array({
        {{"id", "Strike_B"}}, {{"id", "Strike_B"}},
        {{"id", "Strike_B"}}, {{"id", "Strike_B"}},
        {{"id", "Defend_B"}}, {{"id", "Defend_B"}},
        {{"id", "Defend_B"}}, {{"id", "Defend_B"}},
        {{"id", "Zap"}}, {{"id", "Dualcast"}},
        {{"id", "AscendersBane"}},
    });
    defectInput["game_state"]["relics"] = nlohmann::json::array({
        {{"id", "Cracked Core"}, {"counter", -1}},
    });
    defectInput["candidates"] = nlohmann::json::array({
        {{"choice_id", 0}, {"id", "Glacier"}},
    });
    defectInput["targets"] = nlohmann::json::array({"Jaw Worm"});
    const auto defectSpec =
        evaluation::parseProspectiveBattleSpec(defectInput);
    const auto defectGame =
        evaluation::buildProspectiveGame(defectSpec, defectSpec.candidates[0]);
    if (
        defectGame.cc != CharacterClass::DEFECT
        || defectGame.deck.cards.back().getId() != CardId::GLACIER
    ) {
        throw std::runtime_error("Defect prospective deck was not preserved");
    }
    const auto defectBattle = evaluation::buildProspectiveBattle(
        defectGame,
        defectSpec.targets[0],
        0
    );
    if (
        defectBattle.player.cc != CharacterClass::DEFECT
        || defectBattle.player.getOrbCount() != 1
    ) {
        throw std::runtime_error("Defect prospective battle did not initialize");
    }

    const auto startup = aggregateStartupAccess(
        nlohmann::json::array({
            {{"startup_cards", {
                {"Demon Form", {
                    {"first_seen_turn", 1},
                    {"first_played_turn", 2},
                }},
            }}},
            {{"startup_cards", {
                {"Demon Form", {
                    {"first_seen_turn", 3},
                    {"first_played_turn", nullptr},
                }},
            }}},
        }),
        {"Demon Form"}
    );
    if (
        startup["Demon Form"]["seen_by_turn"]["1"].get<double>() != 0.5
        || startup["Demon Form"]["played_by_turn"]["2"].get<double>() != 0.5
        || startup["Demon Form"]["seen_but_not_played_rate"].get<double>() != 0.5
    ) {
        throw std::runtime_error("startup access aggregation is incorrect");
    }

    std::cout << nlohmann::json({
        {"status", "ok"},
        {"checks", 12},
    }).dump() << std::endl;
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            return runSelfTest();
        }
        if (argc >= 3 && std::string(argv[1]) == "--battle-eval") {
            if (argc > 8) {
                std::cerr
                    << "usage: card-reward-eval --battle-eval <input.json> "
                    << "[simulations_per_decision] [world_start] [world_count] "
                    << "[max_time_ms_per_decision] [max_decisions]"
                    << std::endl;
                return 1;
            }
            const auto simulationsPerDecision = argc >= 4
                ? std::max<std::int64_t>(1, std::stoll(argv[3]))
                : 2000;
            const auto worldStart = argc >= 5
                ? std::max(0, std::stoi(argv[4]))
                : 0;
            const auto worldCount = argc >= 6
                ? std::max(1, std::stoi(argv[5]))
                : 16;
            const auto maxTimeMillisPerDecision = argc >= 7
                ? std::max(1L, std::stol(argv[6]))
                : 250L;
            const auto maxDecisions = argc >= 8
                ? std::max(1, std::stoi(argv[7]))
                : 300;
            std::ifstream stream(argv[2]);
            if (!stream) {
                throw std::runtime_error(
                    "could not open input file: " + std::string(argv[2])
                );
            }
            nlohmann::json input;
            stream >> input;
            const auto spec =
                evaluation::parseProspectiveBattleSpec(input);
            std::cout << evaluateIndependentBattles(
                spec,
                worldStart,
                worldCount,
                simulationsPerDecision,
                maxTimeMillisPerDecision,
                maxDecisions
            ).dump(2) << std::endl;
            return 0;
        }
        if (argc < 2 || argc > 5) {
            std::cerr
                << "usage: card-reward-eval <input.json> "
                << "[simulations_per_world] [rng_worlds] [max_time_ms_per_world]"
                << std::endl;
            return 1;
        }
        const auto simulations = argc >= 3
            ? std::max<std::int64_t>(1, std::stoll(argv[2]))
            : 5000;
        const auto worldCount = argc >= 4
            ? std::max(1, std::stoi(argv[3]))
            : 4;
        const auto maxTimeMillis = argc >= 5
            ? std::max(1L, std::stol(argv[4]))
            : 60000L;

        std::ifstream stream(argv[1]);
        if (!stream) {
            throw std::runtime_error(
                "could not open input file: " + std::string(argv[1])
            );
        }
        nlohmann::json input;
        stream >> input;
        const auto spec =
            evaluation::parseProspectiveBattleSpec(input);
        std::cout << evaluate(
            spec,
            worldCount,
            simulations,
            maxTimeMillis
        ).dump(2) << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "card-reward-eval: " << error.what() << std::endl;
        return 2;
    }
}
