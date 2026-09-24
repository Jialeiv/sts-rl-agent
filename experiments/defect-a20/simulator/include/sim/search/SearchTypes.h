#pragma once

#include <tuple>

namespace sts::search {

    // How the search weights immediate survival vs. combat progress vs.
    // long-term scaling when choosing which continuations to expand first.
    enum class SearchIntent {
        SURVIVAL_FIRST,
        AGGRESSIVE,
        SCALING_FIRST,
    };

    // What the search considers a "finished" rollout.
    enum class SearchObjective {
        COMPLETE_COMBAT,
        RECOVERY_HORIZON,
    };

    // State quality at (or past) the fixed recovery horizon, used by the
    // RECOVERY_HORIZON objective and by the recovery root-selection policy.
    struct RecoverySnapshot {
        bool observed = false;
        bool reachedHorizon = false;
        int survivedTurns = 0;
        int hp = 0;
        int effectiveHp = 0;
        int potionCount = 0;
        double engineScore = 0.0;
        double enemyProgress = 0.0;
        double quality = 0.0;
    };

    // Lexicographic comparison of two recovery snapshots: prefer the one that
    // reached the horizon, survived longer, then higher quality and better
    // resources.  Used to keep the best recovery checkpoint seen for a root
    // action.
    inline bool preferRecoverySnapshot(
        const RecoverySnapshot &candidate,
        const RecoverySnapshot &incumbent
    ) {
        if (!candidate.observed) {
            return false;
        }
        if (!incumbent.observed) {
            return true;
        }
        return std::tie(
            candidate.reachedHorizon,
            candidate.survivedTurns,
            candidate.quality,
            candidate.engineScore,
            candidate.effectiveHp,
            candidate.hp,
            candidate.enemyProgress,
            candidate.potionCount
        ) > std::tie(
            incumbent.reachedHorizon,
            incumbent.survivedTurns,
            incumbent.quality,
            incumbent.engineScore,
            incumbent.effectiveHp,
            incumbent.hp,
            incumbent.enemyProgress,
            incumbent.potionCount
        );
    }

}
