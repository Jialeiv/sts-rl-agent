#pragma once

#include "sim/search/BattleScumSearcher2.h"
#include "sim/search/RootActionPolicy.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sts::search {

// Use the fixed-horizon objective when terminal evidence is absent, or when
// sparse terminal evidence conflicts with a safer common boundary. Independent
// RNG-world coverage alone is insufficient: a few high-risk winning lines may
// coexist with an action that reliably reaches the next decision in better
// condition.
inline bool shouldUseRecoverySearch(
    const std::vector<RootActionCandidate> &candidates,
    int selectedCandidateIdx
) {
    if (candidates.empty()) {
        return false;
    }
    const bool allActionsEvaluated = std::all_of(
        candidates.begin(),
        candidates.end(),
        [] (const auto &candidate) {
            return candidate.visits > 0 && candidate.rngWorlds > 0;
        }
    );
    if (!allActionsEvaluated) {
        return false;
    }
    if (!hasCredibleWinEvidence(candidates)
        && !hasBroadSparseWinEvidence(candidates)) {
        return true;
    }

    if (selectedCandidateIdx < 0
        || selectedCandidateIdx >= static_cast<int>(candidates.size())) {
        // A missing selector result cannot safely validate terminal evidence.
        return true;
    }
    const auto &selected = candidates[selectedCandidateIdx];
    if (
        hasCredibleWinEvidence({selected})
        && selected.lowerQuartileWinSampleRate > 0.0
        && !hasStableWinBasin(selected)
    ) {
        return true;
    }

    // A non-zero lower tail means terminal success is not confined to the
    // favorable worlds; keep the complete-combat objective in that case.
    const bool hasLowerTailWin = std::any_of(
        candidates.begin(),
        candidates.end(),
        [] (const auto &candidate) {
            return candidate.lowerQuartileWinSampleRate > 0.0;
        }
    );
    if (hasLowerTailWin) {
        return false;
    }

    constexpr double tolerance = 1e-12;
    const bool sparseWinNeedsMoreActions = std::any_of(
        candidates.begin(),
        candidates.end(),
        [tolerance] (const auto &candidate) {
            return candidate.winningRngWorlds > 0
                && candidate.directEndBoundaryReachRate < 1.0 - tolerance;
        }
    );
    const bool zeroWinReachesNextDecision = std::any_of(
        candidates.begin(),
        candidates.end(),
        [tolerance] (const auto &candidate) {
            return candidate.winningRngWorlds == 0
                && candidate.directEndBoundaryReachRate
                   >= 1.0 - tolerance
                && candidate.meanDirectEndBoundaryHp > 0.0;
        }
    );
    if (sparseWinNeedsMoreActions && zeroWinReachesNextDecision) {
        return true;
    }

    double bestSparseBoundaryHp = -1.0;
    int bestWinningWorlds = 0;
    for (const auto &candidate : candidates) {
        bestWinningWorlds = std::max(
            bestWinningWorlds, candidate.winningRngWorlds
        );
    }
    double bestLowerCoverageBoundaryHp = -1.0;
    for (const auto &candidate : candidates) {
        if (candidate.directEndBoundaryReachRate < 1.0 - tolerance) {
            continue;
        }
        if (candidate.winningRngWorlds == bestWinningWorlds) {
            bestSparseBoundaryHp = std::max(
                bestSparseBoundaryHp,
                candidate.meanDirectEndBoundaryHp
            );
        }
        if (candidate.winningRngWorlds < bestWinningWorlds) {
            bestLowerCoverageBoundaryHp = std::max(
                bestLowerCoverageBoundaryHp,
                candidate.meanDirectEndBoundaryHp
            );
        }
    }
    return bestSparseBoundaryHp >= 0.0
        && bestLowerCoverageBoundaryHp > bestSparseBoundaryHp + tolerance;
}

// Fixed-horizon evidence is aggregated once per determinized RNG world. The
// lower tail protects against a line that looks strong only in lucky worlds;
// the mean then distinguishes equally robust recovery plans.
struct RecoveryRootCandidate {
    int rngWorlds = 0;
    double horizonReachRate = 0.0;
    double meanSurvivedTurns = 0.0;
    double lowerQuartileHp = 0.0;
    double lowerQuartileQuality = 0.0;
    double meanHp = 0.0;
    double meanEffectiveHp = 0.0;
    double meanEngineScore = 0.0;
    double meanEnemyProgress = 0.0;
    double meanPotionCount = 0.0;
    double meanQuality = 0.0;
    double meanActionPrior = 0.0;
    double immediateEnemyDamage = 0.0;
};

inline RecoveryRootCandidate aggregateRecoveryWorlds(
    const std::vector<RecoverySnapshot> &worlds
) {
    RecoveryRootCandidate result;
    std::vector<double> hpValues;
    std::vector<double> qualityValues;
    for (const auto &world : worlds) {
        if (!world.observed) {
            continue;
        }
        ++result.rngWorlds;
        result.horizonReachRate += world.reachedHorizon ? 1.0 : 0.0;
        result.meanSurvivedTurns += world.survivedTurns;
        result.meanHp += world.hp;
        result.meanEffectiveHp += world.effectiveHp;
        result.meanEngineScore += world.engineScore;
        result.meanEnemyProgress += world.enemyProgress;
        result.meanPotionCount += world.potionCount;
        result.meanQuality += world.quality;
        hpValues.push_back(static_cast<double>(world.hp));
        qualityValues.push_back(world.quality);
    }
    if (result.rngWorlds == 0) {
        return result;
    }
    const double inverse = 1.0 / static_cast<double>(result.rngWorlds);
    result.horizonReachRate *= inverse;
    result.meanSurvivedTurns *= inverse;
    result.meanHp *= inverse;
    result.meanEffectiveHp *= inverse;
    result.meanEngineScore *= inverse;
    result.meanEnemyProgress *= inverse;
    result.meanPotionCount *= inverse;
    result.meanQuality *= inverse;

    std::sort(hpValues.begin(), hpValues.end());
    const auto count = std::max<std::size_t>(
        1,
        (hpValues.size() + 3) / 4
    );
    for (std::size_t i = 0; i < count; ++i) {
        result.lowerQuartileHp += hpValues[i];
    }
    result.lowerQuartileHp /= static_cast<double>(count);
    std::sort(qualityValues.begin(), qualityValues.end());
    for (std::size_t i = 0; i < count; ++i) {
        result.lowerQuartileQuality += qualityValues[i];
    }
    result.lowerQuartileQuality /= static_cast<double>(count);
    return result;
}

inline int selectRecoveryRootAction(
    const std::vector<RecoveryRootCandidate> &candidates
) {
    constexpr double tolerance = 1e-12;
    const auto compare = [tolerance] (double left, double right) {
        if (left > right + tolerance) {
            return 1;
        }
        if (left + tolerance < right) {
            return -1;
        }
        return 0;
    };
    const bool anyReachedHorizon = std::any_of(
        candidates.begin(),
        candidates.end(),
        [] (const auto &candidate) {
            return candidate.rngWorlds > 0
                && candidate.horizonReachRate > 0.0;
        }
    );

    int best = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (candidates[i].rngWorlds <= 0) {
            continue;
        }
        if (best < 0) {
            best = i;
            continue;
        }
        const auto &candidate = candidates[i];
        const auto &incumbent = candidates[best];
        std::vector<int> comparisons;
        if (anyReachedHorizon) {
            comparisons = {
                compare(
                    candidate.horizonReachRate,
                    incumbent.horizonReachRate
                ),
                compare(
                    candidate.lowerQuartileQuality,
                    incumbent.lowerQuartileQuality
                ),
                compare(candidate.meanQuality, incumbent.meanQuality),
                compare(
                    candidate.immediateEnemyDamage,
                    incumbent.immediateEnemyDamage
                ),
                compare(candidate.meanActionPrior, incumbent.meanActionPrior),
                compare(candidate.lowerQuartileHp, incumbent.lowerQuartileHp),
                compare(candidate.meanHp, incumbent.meanHp),
                compare(candidate.meanEffectiveHp, incumbent.meanEffectiveHp),
                compare(candidate.meanEngineScore, incumbent.meanEngineScore),
                compare(candidate.meanEnemyProgress, incumbent.meanEnemyProgress),
                compare(candidate.meanPotionCount, incumbent.meanPotionCount),
            };
        } else {
            comparisons = {
                compare(
                    candidate.meanSurvivedTurns,
                    incumbent.meanSurvivedTurns
                ),
                compare(
                    candidate.lowerQuartileQuality,
                    incumbent.lowerQuartileQuality
                ),
                compare(candidate.meanQuality, incumbent.meanQuality),
                compare(
                    candidate.immediateEnemyDamage,
                    incumbent.immediateEnemyDamage
                ),
                compare(candidate.meanActionPrior, incumbent.meanActionPrior),
                compare(candidate.lowerQuartileHp, incumbent.lowerQuartileHp),
                compare(candidate.meanHp, incumbent.meanHp),
                compare(candidate.meanEffectiveHp, incumbent.meanEffectiveHp),
                compare(candidate.meanEngineScore, incumbent.meanEngineScore),
                compare(candidate.meanEnemyProgress, incumbent.meanEnemyProgress),
                compare(candidate.meanPotionCount, incumbent.meanPotionCount),
            };
        }
        for (const int decision : comparisons) {
            if (decision == 0) {
                continue;
            }
            if (decision > 0) {
                best = i;
            }
            break;
        }
    }
    return best;
}

} // namespace sts::search
