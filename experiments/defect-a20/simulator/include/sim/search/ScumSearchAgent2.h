//
// Created by keega on 9/19/2021.
//

#ifndef STS_LIGHTSPEED_SCUMSEARCHAGENT2_H
#define STS_LIGHTSPEED_SCUMSEARCHAGENT2_H

#include "game/GameContext.h"
#include "sim/search/Action.h"
#include "sim/search/GameAction.h"
#include "sim/search/BattleScumSearcher2.h"

#include <memory>
#include <random>

namespace sts::search {

    enum class SearchIntent : int;

    class BattleScumSearcher2;

    struct ScumSearchAgent2 {
        std::int64_t simulationCountTotal = 0;
        std::vector<int> gameActionHistory;

        int stepCount = 0;
        bool paused = false;
        bool pauseOnCardReward = false;
        bool pauseOnRewards = false;
        // pause and cede control to the caller (python) on these out-of-combat decision screens,
        // so an external policy (e.g. a learned model) can make the choice instead of the built-in heuristic.
        bool pauseOnMap = false;
        bool pauseOnRest = false;
        bool pauseOnShop = false;
        bool pauseOnEvent = false;
        bool pauseOnBattle = false;   // cede combat to caller (python drives BattleContext with a learned model)

        bool printActions = false;
        bool printLogs = false;

        int simulationCountBase = 50000;
        long searchTimeLimitMillis = 1000;   // per-decision wall-clock cap for BattleScumSearcher2::search
        double bossSimulationMultiplier = 3;
        int stepsNoSolution = 5;
        int stepsWithSolution = 15;
        bool allowPotions = true;
        SearchIntent intent = SearchIntent::SURVIVAL_FIRST;

        std::default_random_engine rng;


        // public interface
        void playout(GameContext &gc);

        // private methods
        void playoutBattle(BattleContext &bc);

        void takeAction(GameContext &gc, GameAction a);
        void takeAction(BattleContext &bc, Action a);

        void stepThroughSolution(BattleContext &bc, std::vector<search::Action> &actions);
        void stepThroughSearchTree(BattleContext &bc, const search::BattleScumSearcher2 &s);
        static const BattleScumSearcher2::Edge *selectMostVisitedEdge(
            const BattleScumSearcher2::Node &node);

        void stepOutOfCombatPolicy(GameContext &gc);
        void cardSelectPolicy(GameContext &gc);
        void stepEventPolicy(GameContext &gc);
        void stepRandom(GameContext &gc);
        void stepRewardsPolicy(GameContext &gc);
        void weightedCardRewardPolicy(GameContext &gc);
    };

}


#endif //STS_LIGHTSPEED_SCUMSEARCHAGENT2_H
