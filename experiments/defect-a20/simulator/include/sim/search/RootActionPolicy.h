#pragma once

#include "sim/search/BattleScumSearcher2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace sts::search {

// Optimistic values are useful while exploring the tree, but the action sent
// to the game should maximize the expected result observed across rollouts.
inline bool preferRootActionByMean(
    double candidateMean,
    std::int64_t candidateVisits,
    double incumbentMean,
    std::int64_t incumbentVisits
) {
    constexpr double tieTolerance = 1e-12;
    if (candidateMean > incumbentMean + tieTolerance) {
        return true;
    }
    if (candidateMean + tieTolerance < incumbentMean) {
        return false;
    }
    return candidateVisits > incumbentVisits;
}

struct RootActionCandidate {
    double meanValue = -1.0;
    double successUtility = 0.0;
    double winSampleRate = 0.0;
    double expectedEndHpOnWin = 0.0;
    std::int64_t visits = 0;
    double lowerQuartileWinSampleRate = 0.0;
    double lowerQuartileSuccessUtility = 0.0;
    bool isEndTurn = false;
    // Mean, across equally weighted RNG worlds, of the best continuation
    // reached below this root action.  Unlike a single global maximum this
    // cannot be supplied by one lucky world.  It is used only when the search
    // found no credible victory at all: in that regime meanValue mostly
    // measures the quality of random continuations, while this value answers
    // the planning question of which action still has the strongest reachable
    // line.
    double meanBestValue = std::numeric_limits<double>::quiet_NaN();
    // When no complete victory has been found, a live agent will search again
    // after the next monster phase.  These fields describe the best state the
    // search actually reached at that next decision point in each equally
    // weighted RNG world.  Keeping this separate from the dense enemy-HP
    // reward prevents an all-loss tree from treating player HP as worthless.
    double nextDecisionReachRate = std::numeric_limits<double>::quiet_NaN();
    double meanBestNextDecisionHp = std::numeric_limits<double>::quiet_NaN();
    double meanBestNextDecisionEffectiveHp =
        std::numeric_limits<double>::quiet_NaN();
    double meanBestNextDecisionPotionCount =
        std::numeric_limits<double>::quiet_NaN();
    // Used only by the all-loss replanning fallback.  Enemy progress is
    // already normalized by encounter HP; normalizing the checkpoint HP by
    // the player's max HP puts survival on the same scale without a
    // fight-specific threshold.
    double rootMaxHp = std::numeric_limits<double>::quiet_NaN();
    // Value of the best state reached at the same replanning boundary for
    // every root action.  This is deliberately separate from terminal rollout
    // value: atomic actions can require different numbers of decisions before
    // reaching that boundary, which otherwise gives END_TURN extra lookahead.
    double meanBestNextDecisionValue =
        std::numeric_limits<double>::quiet_NaN();
    // Deterministically execute only this root action and then END_TURN. This
    // provides a common boundary without relying on a deeper random rollout.
    // Keys normalize player/enemy HP and unordered spent-card piles, but retain
    // draw order, powers, relic counters, monster phase, RNG state, and card
    // exhaust/consumption.
    double directEndBoundaryReachRate =
        std::numeric_limits<double>::quiet_NaN();
    double meanDirectEndBoundaryHp =
        std::numeric_limits<double>::quiet_NaN();
    double meanDirectEndBoundaryPotionCount =
        std::numeric_limits<double>::quiet_NaN();
    double meanDirectEndBoundaryValue =
        std::numeric_limits<double>::quiet_NaN();
    std::vector<std::uint64_t> directEndBoundaryStateKeys {};
    // Some next-turn values can only improve through a player action:
    // permanent Strength and combat-local card growth (Rampage damage,
    // Genetic Algorithm Block, Ritual Dagger damage). Keep a second boundary
    // key with only those values normalized, plus their actual values per RNG
    // world. This does not hide card consumption, draw order, other powers,
    // relic counters, or enemy reactions.
    std::vector<std::uint64_t> monotonicBoundaryStateKeys {};
    std::vector<int> directEndBoundaryMonotonicCardProgress {};
    std::vector<int> directEndBoundaryStrength {};
    // Persistent run resource lost when a Looter or Mugger escapes. NaN means
    // that the caller did not provide this diagnostic.
    double expectedEscapedStolenGoldOnWin =
        std::numeric_limits<double>::quiet_NaN();
    // Search-result evidence, aggregated once per equally likely RNG world.
    // These fields, rather than rollout hit frequency, drive gameplay choice.
    int winningRngWorlds = 0;
    int rngWorlds = 0;
    double winWorldRate = 0.0;
    double lowerQuartileBestWinUtility = 0.0;
    double meanBestWinUtility = 0.0;
    double bestWinUtilityStandardError = 0.0;
    double meanBestWinEndHp = 0.0;
    double meanBestWinPotionCount = 0.0;
    // Per-RNG-world boundary evidence for the narrow case where playing one
    // card and then ending the turn produces exactly the same future state as
    // ending immediately, except for retained player Block.  Keeping these
    // samples per world makes the override a proof of Pareto dominance rather
    // than a comparison of averages.
    double meanDirectEndBoundaryBlock =
        std::numeric_limits<double>::quiet_NaN();
    std::vector<std::uint64_t> strictBlockBoundaryStateKeys {};
    std::vector<std::uint64_t> shuffledBlockBoundaryStateKeys {};
    std::vector<int> directEndBoundaryShuffleAdvances {};
    std::vector<int> directEndBoundaryBlocks {};
    // Legacy callers which only provide the original next-decision fields
    // represent a one-turn horizon.
    double meanReplanHorizonTurns = 1.0;
    // Exact one-action victory which irreversibly loses run resources. When it
    // exists, a sampled alternative may replace it only through the reviewed
    // high-confidence risk gate below.
    bool certifiedImmediateWin = false;
    double certifiedImmediateWinUtility = 0.0;
    // Paired samples retain the actual result in each independent RNG world.
    // Means are useful diagnostics but cannot prove dominance: [26, 26] and
    // [50, 2] have the same mean while trading away one world. Keep these at
    // the end so legacy aggregate initializers retain their field mapping.
    std::vector<int> directEndBoundaryReached {};
    std::vector<double> directEndBoundaryHps {};
    std::vector<double> directEndBoundaryPotionCounts {};
    std::vector<double> directEndBoundaryValues {};
    bool zeroEnergyRootAction = false;
};

inline constexpr double kCertifiedWinAlternativeMinRate = 0.995;

inline int applyCertifiedWinRiskGate(
    const std::vector<RootActionCandidate> &candidates,
    int selectedIdx,
    double minimumWinRate = kCertifiedWinAlternativeMinRate
) {
    if (
        selectedIdx < 0
        || selectedIdx >= static_cast<int>(candidates.size())
    ) {
        return selectedIdx;
    }
    constexpr double tieTolerance = 1e-12;
    int certifiedIdx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (
            candidates[i].certifiedImmediateWin
            && (
                certifiedIdx < 0
                || candidates[i].certifiedImmediateWinUtility
                   > candidates[certifiedIdx].certifiedImmediateWinUtility
            )
        ) {
            certifiedIdx = i;
        }
    }
    if (certifiedIdx < 0) {
        return selectedIdx;
    }
    const auto &certified = candidates[certifiedIdx];
    int alternativeIdx = -1;
    double alternativeUtility = certified.certifiedImmediateWinUtility;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const auto &candidate = candidates[i];
        const double conservativeWinRate = std::min(
            candidate.winSampleRate,
            candidate.lowerQuartileWinSampleRate
        );
        const double conditionalWinUtility = candidate.winSampleRate > 0.0
            ? candidate.successUtility / candidate.winSampleRate
            : 0.0;
        if (
            !candidate.certifiedImmediateWin
            && candidate.winWorldRate + tieTolerance >= 1.0
            && conservativeWinRate + tieTolerance >= minimumWinRate
            && conditionalWinUtility > alternativeUtility + tieTolerance
        ) {
            alternativeIdx = i;
            alternativeUtility = conditionalWinUtility;
        }
    }
    return alternativeIdx >= 0 ? alternativeIdx : certifiedIdx;
}

struct RootActionThreadSample {
    std::int64_t visits = 0;
    double evaluationSum = 0.0;
    double evaluationSquaredSum = 0.0;
    double maxEvaluation = std::numeric_limits<double>::lowest();
    std::int64_t winSamples = 0;
    std::int64_t lossSamples = 0;
    std::int64_t cutoffSamples = 0;
    double winEndHpSum = 0.0;
    double winUtilitySum = 0.0;
    std::int64_t lizardTailConsumedWinSamples = 0;
    bool reachedNextDecision = false;
    double bestNextDecisionHp = 0.0;
    double bestNextDecisionEffectiveHp = 0.0;
    double bestNextDecisionPotionCount = 0.0;
    double bestNextDecisionValue = std::numeric_limits<double>::lowest();
    double escapedStolenGoldWinSum = 0.0;
    bool foundWinningLine = false;
    double bestWinUtility = 0.0;
    double bestWinEndHp = 0.0;
    double bestWinPotionCount = 0.0;
    double bestReplanHorizonTurns = 0.0;
};

struct RootActionAggregate {
    int rngWorlds = 0;
    std::int64_t visits = 0;
    double meanValue = -1.0;
    double variance = 0.0;
    double meanBestValue = -1.0;
    double winSampleRate = 0.0;
    double lossSampleRate = 0.0;
    double cutoffSampleRate = 0.0;
    double expectedEndHpOnWin = 0.0;
    double successUtility = 0.0;
    double lowerQuartileWinSampleRate = 0.0;
    double lowerQuartileSuccessUtility = 0.0;
    double expectedWinUtilityOnWin = 0.0;
    double lizardTailConsumedRateOnWin = 0.0;
    double nextDecisionReachRate = 0.0;
    double meanReplanHorizonTurns = 0.0;
    double meanBestNextDecisionHp = 0.0;
    double meanBestNextDecisionEffectiveHp = 0.0;
    double meanBestNextDecisionPotionCount = 0.0;
    double meanBestNextDecisionValue = 0.0;
    double expectedEscapedStolenGoldOnWin = 0.0;
    int winningRngWorlds = 0;
    double winWorldRate = 0.0;
    double lowerQuartileBestWinUtility = 0.0;
    double meanBestWinUtility = 0.0;
    double bestWinUtilityStandardError = 0.0;
    double meanBestWinEndHp = 0.0;
    double meanBestWinPotionCount = 0.0;
};

// The optional candidate fields below feed app-specific policies.  They are
// off by default so legacy callers keep their exact previous behavior:
//   * fillReplanHorizon   -> candidate.meanReplanHorizonTurns;
//   * fillEscapedStolenGold -> candidate.expectedEscapedStolenGoldOnWin
//                              (thief gold; callers that only want it on the
//                              first action of a turn override it afterwards).
struct RootCandidateOptions {
    bool fillReplanHorizon = false;
    bool fillEscapedStolenGold = false;
};

// Converts one search thread's edge statistics into the world sample that
// aggregateRootActionThreads consumes.
inline RootActionThreadSample sampleFromEdge(
    const BattleScumSearcher2::Edge &edge
) {
    RootActionThreadSample sample{
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
    sample.bestReplanHorizonTurns = static_cast<double>(
        edge.bestReplanHorizonTurns
    );
    return sample;
}

// Converts the per-world aggregate of one root action into the candidate that
// the root selectors compare.
inline RootActionCandidate candidateFromAggregate(
    const RootActionAggregate &aggregate,
    const BattleContext &rootState,
    bool isEndTurn = false,
    const RootCandidateOptions &opts = {}
) {
    RootActionCandidate candidate;
    candidate.meanValue = aggregate.meanValue;
    candidate.successUtility = aggregate.successUtility;
    candidate.winSampleRate = aggregate.winSampleRate;
    candidate.expectedEndHpOnWin = aggregate.expectedEndHpOnWin;
    candidate.visits = aggregate.visits;
    candidate.lowerQuartileWinSampleRate = aggregate.lowerQuartileWinSampleRate;
    candidate.lowerQuartileSuccessUtility = aggregate.lowerQuartileSuccessUtility;
    candidate.isEndTurn = isEndTurn;
    candidate.meanBestValue = aggregate.meanBestValue;
    candidate.nextDecisionReachRate = aggregate.nextDecisionReachRate;
    candidate.meanBestNextDecisionHp = aggregate.meanBestNextDecisionHp;
    candidate.meanBestNextDecisionEffectiveHp = aggregate.meanBestNextDecisionEffectiveHp;
    candidate.meanBestNextDecisionPotionCount = aggregate.meanBestNextDecisionPotionCount;
    candidate.rootMaxHp = static_cast<double>(rootState.player.maxHp);
    candidate.meanBestNextDecisionValue = aggregate.meanBestNextDecisionValue;
    if (opts.fillReplanHorizon) {
        candidate.meanReplanHorizonTurns = aggregate.meanReplanHorizonTurns;
    }
    if (opts.fillEscapedStolenGold) {
        candidate.expectedEscapedStolenGoldOnWin =
            aggregate.expectedEscapedStolenGoldOnWin;
    }
    candidate.winningRngWorlds = aggregate.winningRngWorlds;
    candidate.rngWorlds = aggregate.rngWorlds;
    candidate.winWorldRate = aggregate.winWorldRate;
    candidate.lowerQuartileBestWinUtility =
        aggregate.lowerQuartileBestWinUtility;
    candidate.meanBestWinUtility = aggregate.meanBestWinUtility;
    candidate.bestWinUtilityStandardError =
        aggregate.bestWinUtilityStandardError;
    candidate.meanBestWinEndHp = aggregate.meanBestWinEndHp;
    candidate.meanBestWinPotionCount = aggregate.meanBestWinPotionCount;
    return candidate;
}

struct BinomialConfidenceInterval {
    double lower = 0.0;
    double upper = 0.0;
};

// Root rollouts are correlated and their rare victories can be concentrated
// in one determinized RNG world, so use a deliberately conservative 99%
// Wilson interval.  This is not presented as a calibrated game win rate; it
// only prevents one or two lucky terminal paths from changing the root
// objective from dense progress to lexicographic survival.
inline BinomialConfidenceInterval rootWinRateConfidenceInterval(
    double winSampleRate,
    std::int64_t visits
) {
    if (visits <= 0) {
        return {};
    }
    constexpr double z = 2.5758293035489004; // two-sided 99%
    const double n = static_cast<double>(visits);
    const double p = std::clamp(winSampleRate, 0.0, 1.0);
    const double zSquared = z * z;
    const double denominator = 1.0 + zSquared / n;
    const double center =
        (p + zSquared / (2.0 * n)) / denominator;
    const double radius =
        z
        * std::sqrt(
            p * (1.0 - p) / n
            + zSquared / (4.0 * n * n)
        )
        / denominator;
    return {
        std::max(0.0, center - radius),
        std::min(1.0, center + radius),
    };
}

inline bool hasStableWinBasin(const RootActionCandidate &candidate) {
    if (
        candidate.rngWorlds <= 0
        || candidate.visits <= 0
        || candidate.lowerQuartileWinSampleRate <= 0.0
    ) {
        return false;
    }
    // Rollouts are correlated, so this is not a confidence interval.  It is
    // only a convergence guard: at least the lower quartile of independent
    // worlds must contain a non-vanishing basin as the search budget grows.
    const double visitsPerWorld =
        static_cast<double>(candidate.visits)
        / static_cast<double>(candidate.rngWorlds);
    return candidate.lowerQuartileWinSampleRate
        >= 1.0 / std::sqrt(visitsPerWorld);
}

inline bool hasCredibleWinEvidence(
    const std::vector<RootActionCandidate> &candidates
) {
    // Repeated UCT rollouts inside one determinized RNG world are correlated.
    // When equal-world aggregation is available, decide whether terminal
    // evidence is credible from those independent worlds instead of treating
    // every rollout hit as an independent Bernoulli sample.  Otherwise a few
    // descendants rediscovered thousands of times can switch the whole root
    // objective even though most worlds have no winning continuation.
    const RootActionCandidate *bestWorldCandidate = nullptr;
    for (const auto &candidate : candidates) {
        if (
            candidate.visits > 0
            && candidate.rngWorlds > 0
            && candidate.winningRngWorlds > 0
            && (
                bestWorldCandidate == nullptr
                || candidate.winWorldRate
                   > bestWorldCandidate->winWorldRate
            )
        ) {
            bestWorldCandidate = &candidate;
        }
    }
    if (bestWorldCandidate != nullptr) {
        const auto observed = rootWinRateConfidenceInterval(
            bestWorldCandidate->winWorldRate,
            bestWorldCandidate->rngWorlds
        );
        const auto zeroWinBaseline = rootWinRateConfidenceInterval(
            0.0,
            bestWorldCandidate->rngWorlds
        );
        return observed.lower > zeroWinBaseline.upper;
    }

    // Keep the policy usable for callers that only provide pooled samples.
    const RootActionCandidate *best = nullptr;
    for (const auto &candidate : candidates) {
        if (
            candidate.visits > 0
            && candidate.winSampleRate > 0.0
            && (
                best == nullptr
                || candidate.winSampleRate > best->winSampleRate
            )
        ) {
            best = &candidate;
        }
    }
    if (best == nullptr) {
        return false;
    }

    const auto observed = rootWinRateConfidenceInterval(
        best->winSampleRate,
        best->visits
    );
    const auto zeroWinBaseline = rootWinRateConfidenceInterval(
        0.0,
        best->visits
    );
    return observed.lower > zeroWinBaseline.upper;
}

// Each search thread represents one equally likely RNG determinization. UCT
// deliberately gives different actions different visit counts inside a thread,
// so pooling all visits would over-weight the worlds where an action happened
// to look good. Average each world's estimate instead.
inline RootActionAggregate aggregateRootActionThreads(
    const std::vector<RootActionThreadSample> &samples
) {
    RootActionAggregate result;
    double meanSum = 0.0;
    double secondMomentSum = 0.0;
    double bestValueSum = 0.0;
    double winRateSum = 0.0;
    double lossRateSum = 0.0;
    double cutoffRateSum = 0.0;
    double winningHpMassSum = 0.0;
    double successUtilitySum = 0.0;
    double consumedLizardWinRateSum = 0.0;
    double nextDecisionReachSum = 0.0;
    double replanHorizonTurnsSum = 0.0;
    double bestNextDecisionHpSum = 0.0;
    double bestNextDecisionEffectiveHpSum = 0.0;
    double bestNextDecisionPotionCountSum = 0.0;
    double bestNextDecisionValueSum = 0.0;
    double escapedStolenGoldWinningMassSum = 0.0;
    double bestWinUtilitySum = 0.0;
    double bestWinUtilitySquaredSum = 0.0;
    double bestWinEndHpSum = 0.0;
    double bestWinPotionCountSum = 0.0;
    std::vector<double> worldWinRates;
    std::vector<double> worldSuccessUtilities;
    std::vector<double> worldBestWinUtilities;

    for (const auto &sample : samples) {
        if (sample.visits <= 0) {
            continue;
        }
        const double inverseVisits = 1.0 / static_cast<double>(sample.visits);
        ++result.rngWorlds;
        result.visits += sample.visits;
        meanSum += sample.evaluationSum * inverseVisits;
        secondMomentSum += sample.evaluationSquaredSum * inverseVisits;
        bestValueSum += sample.maxEvaluation;
        winRateSum += static_cast<double>(sample.winSamples) * inverseVisits;
        lossRateSum += static_cast<double>(sample.lossSamples) * inverseVisits;
        cutoffRateSum += static_cast<double>(sample.cutoffSamples) * inverseVisits;
        winningHpMassSum += sample.winEndHpSum * inverseVisits;
        successUtilitySum += sample.winUtilitySum * inverseVisits;
        consumedLizardWinRateSum +=
            static_cast<double>(sample.lizardTailConsumedWinSamples)
            * inverseVisits;
        escapedStolenGoldWinningMassSum +=
            sample.escapedStolenGoldWinSum * inverseVisits;
        if (sample.reachedNextDecision) {
            nextDecisionReachSum += 1.0;
            replanHorizonTurnsSum += sample.bestReplanHorizonTurns;
            bestNextDecisionHpSum += sample.bestNextDecisionHp;
            bestNextDecisionEffectiveHpSum +=
                sample.bestNextDecisionEffectiveHp;
            bestNextDecisionPotionCountSum +=
                sample.bestNextDecisionPotionCount;
            bestNextDecisionValueSum += sample.bestNextDecisionValue;
        }
        worldWinRates.push_back(
            static_cast<double>(sample.winSamples) * inverseVisits
        );
        worldSuccessUtilities.push_back(
            sample.winUtilitySum * inverseVisits
        );
        if (sample.foundWinningLine) {
            ++result.winningRngWorlds;
            bestWinUtilitySum += sample.bestWinUtility;
            bestWinUtilitySquaredSum +=
                sample.bestWinUtility * sample.bestWinUtility;
            bestWinEndHpSum += sample.bestWinEndHp;
            bestWinPotionCountSum += sample.bestWinPotionCount;
            worldBestWinUtilities.push_back(sample.bestWinUtility);
        } else {
            // A world without a discovered victory contributes zero. This
            // makes the lower tail reflect both robustness and ending HP.
            worldBestWinUtilities.push_back(0.0);
        }
    }

    if (result.rngWorlds == 0) {
        return result;
    }
    const double inverseWorlds = 1.0 / static_cast<double>(result.rngWorlds);
    result.meanValue = meanSum * inverseWorlds;
    result.variance = std::max(
        0.0,
        secondMomentSum * inverseWorlds - result.meanValue * result.meanValue
    );
    result.meanBestValue = bestValueSum * inverseWorlds;
    result.winSampleRate = winRateSum * inverseWorlds;
    result.lossSampleRate = lossRateSum * inverseWorlds;
    result.cutoffSampleRate = cutoffRateSum * inverseWorlds;
    result.successUtility = successUtilitySum * inverseWorlds;
    result.nextDecisionReachRate = nextDecisionReachSum * inverseWorlds;
    result.meanReplanHorizonTurns =
        replanHorizonTurnsSum * inverseWorlds;
    // Unreached worlds contribute zero HP.  Reach rate is compared first, so
    // this value then ranks the health of equally reachable replanning states.
    result.meanBestNextDecisionHp =
        bestNextDecisionHpSum * inverseWorlds;
    result.meanBestNextDecisionEffectiveHp =
        bestNextDecisionEffectiveHpSum * inverseWorlds;
    result.meanBestNextDecisionPotionCount =
        bestNextDecisionPotionCountSum * inverseWorlds;
    result.meanBestNextDecisionValue =
        bestNextDecisionValueSum * inverseWorlds;
    const auto lowerQuartileMean = [] (std::vector<double> values) {
        std::sort(values.begin(), values.end());
        const auto count = std::max<std::size_t>(1, (values.size() + 3) / 4);
        double sum = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            sum += values[i];
        }
        return sum / static_cast<double>(count);
    };
    result.lowerQuartileWinSampleRate =
        lowerQuartileMean(std::move(worldWinRates));
    result.lowerQuartileSuccessUtility =
        lowerQuartileMean(std::move(worldSuccessUtilities));
    result.winWorldRate =
        static_cast<double>(result.winningRngWorlds) * inverseWorlds;
    result.lowerQuartileBestWinUtility =
        lowerQuartileMean(std::move(worldBestWinUtilities));
    result.meanBestWinUtility = bestWinUtilitySum * inverseWorlds;
    const double bestWinUtilityVariance = std::max(
        0.0,
        bestWinUtilitySquaredSum * inverseWorlds
            - result.meanBestWinUtility * result.meanBestWinUtility
    );
    result.bestWinUtilityStandardError = std::sqrt(
        bestWinUtilityVariance * inverseWorlds
    );
    if (result.winningRngWorlds > 0) {
        const double inverseWinningWorlds =
            1.0 / static_cast<double>(result.winningRngWorlds);
        result.meanBestWinEndHp = bestWinEndHpSum * inverseWinningWorlds;
        result.meanBestWinPotionCount =
            bestWinPotionCountSum * inverseWinningWorlds;
    }
    if (winRateSum > 0.0) {
        result.expectedEndHpOnWin = winningHpMassSum / winRateSum;
        result.expectedWinUtilityOnWin = successUtilitySum / winRateSum;
        result.lizardTailConsumedRateOnWin =
            consumedLizardWinRateSum / winRateSum;
        result.expectedEscapedStolenGoldOnWin =
            escapedStolenGoldWinningMassSum / winRateSum;
    }
    return result;
}

inline bool hasWinningWorldEvidence(
    const std::vector<RootActionCandidate> &candidates
) {
    return std::any_of(
        candidates.begin(),
        candidates.end(),
        [] (const RootActionCandidate &candidate) {
            return candidate.visits > 0 && candidate.winningRngWorlds > 0;
        }
    );
}

inline bool hasBroadSparseWinEvidence(
    const std::vector<RootActionCandidate> &candidates
) {
    return std::any_of(
        candidates.begin(),
        candidates.end(),
        [] (const RootActionCandidate &candidate) {
            // A rare rollout can still be useful when independently found
            // across at least one third of determinized RNG worlds. Isolated
            // lucky worlds stay with survival replanning, while distributed
            // evidence may still guide a difficult combat.
            return candidate.visits > 0
                && candidate.rngWorlds > 0
                && candidate.winningRngWorlds * 3 >= candidate.rngWorlds;
        }
    );
}

// A root action is judged by the whole combat, not by HP at the next turn.
// Rank complete victories in stages.  With sparse evidence, retain the
// actions that reach the widest set of equally weighted RNG worlds.  Once
// wins are credible, first discard actions that are Pareto-dominated in RNG
// coverage, lower-tail win rate, and overall win rate when their terminal
// utility intervals overlap.  Rank the remaining stability frontier by
// probability-weighted terminal utility.  This keeps a small ending-HP
// advantage from rescuing a uniformly less reliable action, without replacing
// the proven complete-plan objective with noisy raw rates.
inline int selectRootActionByWorldBest(
    const std::vector<RootActionCandidate> &candidates,
    bool allowSparseWinningWorlds = true
) {
    if (candidates.empty()) {
        return -1;
    }

    constexpr double tieTolerance = 1e-12;
    const bool hasCredibleWins = hasCredibleWinEvidence(candidates);
    const bool hasBroadSparseWins = hasBroadSparseWinEvidence(candidates);
    const bool hasWinningWorld = hasWinningWorldEvidence(candidates)
        && (
            allowSparseWinningWorlds
            || hasCredibleWins
            || hasBroadSparseWins
        );
    int bestWinningWorlds = 0;
    if (hasWinningWorld) {
        for (const auto &candidate : candidates) {
            if (candidate.visits > 0) {
                bestWinningWorlds = std::max(
                    bestWinningWorlds,
                    candidate.winningRngWorlds
                );
            }
        }
    }
    const auto hasCompetitiveCompletePlanEvidence = [
        &candidates,
        hasWinningWorld,
        hasCredibleWins,
        bestWinningWorlds
    ] (const RootActionCandidate &candidate) {
        if (!hasWinningWorld) {
            return true;
        }
        if (!hasCredibleWins) {
            return candidate.winningRngWorlds >= bestWinningWorlds;
        }
        const double candidateUpperUtility =
            candidate.meanBestWinUtility
            + std::max(0.0, candidate.bestWinUtilityStandardError);
        for (const auto &challenger : candidates) {
            if (
                challenger.visits <= 0
                || challenger.winningRngWorlds
                   <= candidate.winningRngWorlds
            ) {
                continue;
            }
            const double challengerLowerUtility =
                challenger.meanBestWinUtility
                - std::max(
                    0.0,
                    challenger.bestWinUtilityStandardError
                );
            if (challengerLowerUtility > candidateUpperUtility) {
                return false;
            }
        }
        return true;
    };
    const bool hasSafeReplanValues = !hasWinningWorld && std::all_of(
        candidates.begin(),
        candidates.end(),
        [] (const RootActionCandidate &candidate) {
            return candidate.visits <= 0
                || (
                    std::isfinite(candidate.nextDecisionReachRate)
                    && std::isfinite(candidate.meanReplanHorizonTurns)
                    && std::isfinite(candidate.meanBestNextDecisionEffectiveHp)
                    && std::isfinite(candidate.meanBestNextDecisionPotionCount)
                    && std::isfinite(candidate.rootMaxHp)
                    && candidate.rootMaxHp > 0.0
                );
        }
    );
    const bool hasComparableBestValues = !hasWinningWorld && std::all_of(
        candidates.begin(),
        candidates.end(),
        [] (const RootActionCandidate &candidate) {
            return candidate.visits <= 0
                || std::isfinite(candidate.meanBestValue);
        }
    );

    const auto compareDouble = [tieTolerance] (double left, double right) {
        if (left > right + tieTolerance) {
            return 1;
        }
        if (left + tieTolerance < right) {
            return -1;
        }
        return 0;
    };

    const auto isDominatedAtReplan = [
        &candidates,
        tieTolerance,
        hasSafeReplanValues
    ] (int candidateIdx) {
        if (!hasSafeReplanValues) {
            return false;
        }
        const auto &candidate = candidates[candidateIdx];
        for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
            if (i == candidateIdx || candidates[i].visits <= 0) {
                continue;
            }
            const auto &challenger = candidates[i];
            const bool noWorse =
                challenger.nextDecisionReachRate + tieTolerance
                    >= candidate.nextDecisionReachRate
                && challenger.meanReplanHorizonTurns + tieTolerance
                    >= candidate.meanReplanHorizonTurns
                && challenger.meanBestNextDecisionEffectiveHp + tieTolerance
                    >= candidate.meanBestNextDecisionEffectiveHp
                && challenger.meanBestNextDecisionPotionCount + tieTolerance
                    >= candidate.meanBestNextDecisionPotionCount
                && challenger.meanBestNextDecisionValue + tieTolerance
                    >= candidate.meanBestNextDecisionValue
                && challenger.meanValue + tieTolerance
                    >= candidate.meanValue;
            const bool strictlyBetter =
                challenger.nextDecisionReachRate
                    > candidate.nextDecisionReachRate + tieTolerance
                || challenger.meanReplanHorizonTurns
                    > candidate.meanReplanHorizonTurns + tieTolerance
                || challenger.meanBestNextDecisionEffectiveHp
                    > candidate.meanBestNextDecisionEffectiveHp + tieTolerance
                || challenger.meanBestNextDecisionPotionCount
                    > candidate.meanBestNextDecisionPotionCount + tieTolerance
                || challenger.meanBestNextDecisionValue
                    > candidate.meanBestNextDecisionValue + tieTolerance
                || challenger.meanValue
                    > candidate.meanValue + tieTolerance;
            if (noWorse && strictlyBetter) {
                return true;
            }
        }
        return false;
    };

    const auto isReliabilityDominated = [
        &candidates,
        hasCredibleWins,
        tieTolerance
    ] (int candidateIdx) {
        if (!hasCredibleWins) {
            return false;
        }
        const auto &candidate = candidates[candidateIdx];
        const auto candidateInterval = rootWinRateConfidenceInterval(
            candidate.winSampleRate,
            candidate.visits
        );
        for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
            if (i == candidateIdx || candidates[i].visits <= 0) {
                continue;
            }
            const auto &challenger = candidates[i];
            const auto challengerInterval = rootWinRateConfidenceInterval(
                challenger.winSampleRate,
                challenger.visits
            );
            const double candidateUpperUtility =
                candidate.meanBestWinUtility
                + std::max(
                    0.0,
                    candidate.bestWinUtilityStandardError
                );
            const double candidateLowerUtility =
                candidate.meanBestWinUtility
                - std::max(
                    0.0,
                    candidate.bestWinUtilityStandardError
                );
            const double challengerUpperUtility =
                challenger.meanBestWinUtility
                + std::max(
                    0.0,
                    challenger.bestWinUtilityStandardError
                );
            const double challengerLowerUtility =
                challenger.meanBestWinUtility
                - std::max(
                    0.0,
                    challenger.bestWinUtilityStandardError
                );
            const bool comparableTerminalUtility =
                candidateUpperUtility + tieTolerance
                    >= challengerLowerUtility
                && challengerUpperUtility + tieTolerance
                    >= candidateLowerUtility;
            if (
                challenger.rngWorlds == candidate.rngWorlds
                && challenger.rngWorlds > 0
                && challenger.winningRngWorlds
                    > candidate.winningRngWorlds
                && challenger.lowerQuartileWinSampleRate
                    > candidate.lowerQuartileWinSampleRate + tieTolerance
                && challengerInterval.lower
                    > candidateInterval.upper + tieTolerance
                && comparableTerminalUtility
            ) {
                return true;
            }
        }
        return false;
    };

    int bestIdx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const auto &candidate = candidates[i];
        if (
            candidate.visits <= 0
            || !hasCompetitiveCompletePlanEvidence(candidate)
            || isDominatedAtReplan(i)
            || isReliabilityDominated(i)
        ) {
            continue;
        }
        if (bestIdx < 0) {
            bestIdx = i;
            continue;
        }
        const auto &incumbent = candidates[bestIdx];

        if (hasWinningWorld) {
            int decision = 0;
            const std::vector<int> comparisons = hasCredibleWins
                ? std::vector<int>{
                    compareDouble(
                        candidate.successUtility,
                        incumbent.successUtility
                    ),
                    compareDouble(
                        candidate.meanBestWinUtility,
                        incumbent.meanBestWinUtility
                    ),
                    compareDouble(
                        candidate.winWorldRate,
                        incumbent.winWorldRate
                    ),
                    compareDouble(
                        candidate.meanBestWinPotionCount,
                        incumbent.meanBestWinPotionCount
                    ),
                    compareDouble(
                        candidate.meanBestWinEndHp,
                        incumbent.meanBestWinEndHp
                    ),
                }
                : std::vector<int>{
                    compareDouble(
                        candidate.successUtility,
                        incumbent.successUtility
                    ),
                    compareDouble(
                        candidate.meanBestWinUtility,
                        incumbent.meanBestWinUtility
                    ),
                    compareDouble(
                        candidate.lowerQuartileBestWinUtility,
                        incumbent.lowerQuartileBestWinUtility
                    ),
                    compareDouble(
                        candidate.meanBestWinPotionCount,
                        incumbent.meanBestWinPotionCount
                    ),
                    compareDouble(
                        candidate.meanBestWinEndHp,
                        incumbent.meanBestWinEndHp
                    ),
                };
            for (const int comparison : comparisons) {
                if (comparison != 0) {
                    decision = comparison;
                    break;
                }
            }
            if (decision != 0) {
                if (decision > 0) {
                    bestIdx = i;
                }
                continue;
            }
        } else if (hasSafeReplanValues) {
            int decision = compareDouble(
                candidate.nextDecisionReachRate,
                incumbent.nextDecisionReachRate
            );
            if (decision == 0) {
                decision = compareDouble(
                    candidate.meanReplanHorizonTurns,
                    incumbent.meanReplanHorizonTurns
                );
            }
            if (decision == 0) {
                // The root alternatives can require different numbers of
                // atomic choices before the next turn (for example a card
                // followed by a mandatory hand selection). Compare every
                // all-loss action with the same survival-plus-progress value;
                // switching to a max-only objective when HP happens to tie
                // reintroduces unequal-depth bias in favor of END_TURN.
                decision = compareDouble(
                    candidate.meanValue
                        + candidate.meanBestNextDecisionEffectiveHp
                          / candidate.rootMaxHp,
                    incumbent.meanValue
                        + incumbent.meanBestNextDecisionEffectiveHp
                          / incumbent.rootMaxHp
                );
            }
            if (decision == 0) {
                decision = compareDouble(
                    candidate.meanBestNextDecisionPotionCount,
                    incumbent.meanBestNextDecisionPotionCount
                );
            }
            if (decision == 0) {
                decision = compareDouble(
                    candidate.meanBestNextDecisionValue,
                    incumbent.meanBestNextDecisionValue
                );
            }
            if (decision == 0) {
                decision = compareDouble(
                    candidate.meanBestValue,
                    incumbent.meanBestValue
                );
            }
            if (decision != 0) {
                if (decision > 0) {
                    bestIdx = i;
                }
                continue;
            }
        }

        {
            const double candidatePlanningValue = hasComparableBestValues
                ? candidate.meanBestValue
                : candidate.meanValue;
            const double incumbentPlanningValue = hasComparableBestValues
                ? incumbent.meanBestValue
                : incumbent.meanValue;
            const int planningComparison = compareDouble(
                candidatePlanningValue,
                incumbentPlanningValue
            );
            if (planningComparison > 0) {
                bestIdx = i;
                continue;
            }
            if (planningComparison < 0) {
                continue;
            }
        }
        if (candidate.meanValue > incumbent.meanValue + tieTolerance) {
            bestIdx = i;
            continue;
        }
        if (candidate.meanValue + tieTolerance < incumbent.meanValue) {
            continue;
        }
        if (candidate.visits > incumbent.visits) {
            bestIdx = i;
        }
    }
    return bestIdx;
}

inline bool completeCombatParetoDominates(
    const RootActionCandidate &left,
    const RootActionCandidate &right
) {
    constexpr double tolerance = 1e-12;
    const bool noWorse =
        left.winningRngWorlds >= right.winningRngWorlds
        && left.successUtility + tolerance >= right.successUtility
        && left.winSampleRate + tolerance >= right.winSampleRate
        && left.meanBestWinUtility + tolerance >= right.meanBestWinUtility
        && left.expectedEndHpOnWin + tolerance >= right.expectedEndHpOnWin
        && left.meanBestWinEndHp + tolerance >= right.meanBestWinEndHp
        && left.meanBestWinPotionCount + tolerance
            >= right.meanBestWinPotionCount;
    const bool strictlyBetter =
        left.winningRngWorlds > right.winningRngWorlds
        || left.successUtility > right.successUtility + tolerance
        || left.winSampleRate > right.winSampleRate + tolerance
        || left.meanBestWinUtility > right.meanBestWinUtility + tolerance
        || left.expectedEndHpOnWin > right.expectedEndHpOnWin + tolerance
        || left.meanBestWinEndHp > right.meanBestWinEndHp + tolerance
        || left.meanBestWinPotionCount
            > right.meanBestWinPotionCount + tolerance;
    return noWorse && strictlyBetter;
}

inline int applyCompleteCombatParetoVeto(
    const std::vector<RootActionCandidate> &candidates,
    int baselineIdx
) {
    if (baselineIdx < 0 || baselineIdx >= static_cast<int>(candidates.size())) {
        return baselineIdx;
    }
    // Zero wins mean missing complete-combat evidence, not a proven loss.
    if (candidates[baselineIdx].winningRngWorlds <= 0) {
        return baselineIdx;
    }
    std::vector<RootActionCandidate> dominant;
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const std::vector<RootActionCandidate> challengerEvidence{
            candidates[i]
        };
        if (
            i != baselineIdx
            && (
                hasCredibleWinEvidence(challengerEvidence)
                || hasBroadSparseWinEvidence(challengerEvidence)
            )
            && (
                hasStableWinBasin(candidates[i])
                || candidates[i].winningRngWorlds
                    > candidates[baselineIdx].winningRngWorlds
            )
            && completeCombatParetoDominates(
                candidates[i], candidates[baselineIdx]
            )
        ) {
            dominant.push_back(candidates[i]);
            indices.push_back(i);
        }
    }
    if (dominant.empty()) {
        return baselineIdx;
    }
    const int selected = selectRootActionByWorldBest(dominant, false);
    return selected < 0 ? baselineIdx : indices[selected];
}

// `winSampleRate` and `successUtility` count UCT playouts.  A root action that
// collapses the tree can therefore accumulate more winning hits than an
// action that opens several useful continuations.  Preserve the established
// selector as the baseline, then override it only when one challenger is
// unambiguously better across the same RNG worlds: coverage is no worse, the
// lower quartile of per-world best complete plans is higher, and the mean
// utility intervals do not overlap.  Callers apply exact one-turn dominance
// rules first; those proofs must not be displaced by this statistical check.
inline int applyRobustContinuationDominance(
    const std::vector<RootActionCandidate> &candidates,
    int baselineIdx
) {
    if (
        baselineIdx < 0
        || baselineIdx >= static_cast<int>(candidates.size())
        || !hasCredibleWinEvidence(candidates)
    ) {
        return baselineIdx;
    }

    constexpr double tieTolerance = 1e-12;
    constexpr double maxWinBasinRatio = 4.0;
    const auto &baseline = candidates[baselineIdx];
    const double baselineUpperUtility =
        baseline.meanBestWinUtility
        + std::max(0.0, baseline.bestWinUtilityStandardError);
    int bestIdx = baselineIdx;
    double bestLowerQuartile = baseline.lowerQuartileBestWinUtility;
    double bestLowerUtility =
        baseline.meanBestWinUtility
        - std::max(0.0, baseline.bestWinUtilityStandardError);

    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (i == baselineIdx || candidates[i].visits <= 0) {
            continue;
        }
        const auto &challenger = candidates[i];
        const double challengerLowerUtility =
            challenger.meanBestWinUtility
            - std::max(
                0.0,
                challenger.bestWinUtilityStandardError
            );
        const bool robustlyDominatesBaseline =
            challenger.rngWorlds == baseline.rngWorlds
            && challenger.rngWorlds > 0
            && challenger.winningRngWorlds >= baseline.winningRngWorlds
            // A best leaf found hundreds of times less often is still a
            // brittle search accident, even when every RNG worker eventually
            // sees one.  Allow a substantial density loss (UCT density is
            // biased), but not an effectively vanishing continuation basin.
            && challenger.winSampleRate * maxWinBasinRatio + tieTolerance
                >= baseline.winSampleRate
            && challenger.lowerQuartileBestWinUtility
                > baseline.lowerQuartileBestWinUtility + tieTolerance
            && challengerLowerUtility
                > baselineUpperUtility + tieTolerance;
        if (!robustlyDominatesBaseline) {
            continue;
        }

        if (
            bestIdx == baselineIdx
            || challenger.lowerQuartileBestWinUtility
                > bestLowerQuartile + tieTolerance
            || (
                std::abs(
                    challenger.lowerQuartileBestWinUtility
                    - bestLowerQuartile
                ) <= tieTolerance
                && challengerLowerUtility
                    > bestLowerUtility + tieTolerance
            )
        ) {
            bestIdx = i;
            bestLowerQuartile =
                challenger.lowerQuartileBestWinUtility;
            bestLowerUtility = challengerLowerUtility;
        }
    }
    return bestIdx;
}

inline int selectRootActionBySuccessUtility(
    const std::vector<RootActionCandidate> &candidates
) {
    return selectRootActionByWorldBest(candidates);
}

inline int selectRootActionByWinRateBandThenEndHp(
    const std::vector<RootActionCandidate> &candidates
) {
    if (candidates.empty() || !hasCredibleWinEvidence(candidates)) {
        return selectRootActionByWorldBest(candidates, false);
    }

    constexpr double tieTolerance = 1e-12;
    int highestWinRateIdx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (
            candidates[i].visits > 0
            && (
                highestWinRateIdx < 0
                || candidates[i].winSampleRate
                   > candidates[highestWinRateIdx].winSampleRate
            )
        ) {
            highestWinRateIdx = i;
        }
    }
    if (highestWinRateIdx < 0) {
        return -1;
    }

    const auto highestInterval = rootWinRateConfidenceInterval(
        candidates[highestWinRateIdx].winSampleRate,
        candidates[highestWinRateIdx].visits
    );
    int bestIdx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const auto &candidate = candidates[i];
        if (candidate.visits <= 0) {
            continue;
        }
        const auto interval = rootWinRateConfidenceInterval(
            candidate.winSampleRate,
            candidate.visits
        );
        // Only treat rates as tied when the conservative confidence intervals
        // overlap.  A materially less reliable line must not buy priority by
        // ending the rare wins with more HP.
        if (interval.upper + tieTolerance < highestInterval.lower) {
            continue;
        }
        if (bestIdx < 0) {
            bestIdx = i;
            continue;
        }
        const auto &incumbent = candidates[bestIdx];
        if (
            candidate.expectedEndHpOnWin
            > incumbent.expectedEndHpOnWin + tieTolerance
        ) {
            bestIdx = i;
            continue;
        }
        if (
            candidate.expectedEndHpOnWin + tieTolerance
            < incumbent.expectedEndHpOnWin
        ) {
            continue;
        }
        if (candidate.winSampleRate > incumbent.winSampleRate + tieTolerance) {
            bestIdx = i;
            continue;
        }
        if (candidate.winSampleRate + tieTolerance < incumbent.winSampleRate) {
            continue;
        }
        if (candidate.meanValue > incumbent.meanValue + tieTolerance) {
            bestIdx = i;
            continue;
        }
        if (
            std::abs(candidate.meanValue - incumbent.meanValue) <= tieTolerance
            && candidate.visits > incumbent.visits
        ) {
            bestIdx = i;
        }
    }
    return bestIdx;
}

inline int selectRootPotionAction(
    const std::vector<RootActionCandidate> &candidates,
    bool allowSparseWinningWorlds = true
) {
    if (
        hasWinningWorldEvidence(candidates)
        && (
            allowSparseWinningWorlds
            || hasCredibleWinEvidence(candidates)
            || hasBroadSparseWinEvidence(candidates)
        )
    ) {
        return selectRootActionByWorldBest(
            candidates,
            allowSparseWinningWorlds
        );
    }
    if (candidates.empty()) {
        return -1;
    }

    constexpr double tieTolerance = 1e-12;
    const auto compare = [tieTolerance] (double left, double right) {
        if (left > right + tieTolerance) {
            return 1;
        }
        if (left + tieTolerance < right) {
            return -1;
        }
        return 0;
    };
    int bestIdx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const auto &candidate = candidates[i];
        if (candidate.visits <= 0) {
            continue;
        }
        if (bestIdx < 0) {
            bestIdx = i;
            continue;
        }
        const auto &incumbent = candidates[bestIdx];
        int decision = 0;
        for (const int result : {
                 compare(
                     candidate.nextDecisionReachRate,
                     incumbent.nextDecisionReachRate
                 ),
                 compare(
                     candidate.meanBestNextDecisionEffectiveHp,
                     incumbent.meanBestNextDecisionEffectiveHp
                 ),
                 compare(
                     candidate.meanBestNextDecisionPotionCount,
                     incumbent.meanBestNextDecisionPotionCount
                 ),
                 compare(candidate.meanBestValue, incumbent.meanBestValue),
                 compare(candidate.meanValue, incumbent.meanValue),
             }) {
            if (result != 0) {
                decision = result;
                break;
            }
        }
        if (decision > 0) {
            bestIdx = i;
        }
    }
    return bestIdx;
}

inline bool isDangerousRootActionSet(
    const std::vector<RootActionCandidate> &candidates
) {
    if (!hasCredibleWinEvidence(candidates)) {
        return false;
    }
    constexpr double tieTolerance = 1e-12;
    return std::any_of(
        candidates.begin(), candidates.end(), [tieTolerance] (const auto &c) {
            return c.isEndTurn
                && c.visits > 0
                && std::isfinite(c.directEndBoundaryReachRate)
                && c.directEndBoundaryReachRate < 1.0 - tieTolerance;
        }
    );
}

// If ending now can kill the player, do not buy higher HP conditional on
// victory by accepting a materially less reliable combat line. Small sampling
// differences stay with the normal complete-combat policy.
inline bool shouldPreferDangerWinRatePolicy(
    const std::vector<RootActionCandidate> &candidates,
    int baselineIdx
) {
    if (
        !isDangerousRootActionSet(candidates)
        || baselineIdx < 0
        || baselineIdx >= static_cast<int>(candidates.size())
    ) {
        return false;
    }
    int highestWinRateIdx = baselineIdx;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (
            candidates[i].visits > 0
            && candidates[i].winSampleRate
               > candidates[highestWinRateIdx].winSampleRate
        ) {
            highestWinRateIdx = i;
        }
    }
    const auto &baseline = candidates[baselineIdx];
    const auto &mostReliable = candidates[highestWinRateIdx];
    constexpr double relativeWinRateBand = 0.05;
    return mostReliable.winSampleRate
               > baseline.winSampleRate
                   + mostReliable.winSampleRate * relativeWinRateBand
        && baseline.expectedEndHpOnWin > mostReliable.expectedEndHpOnWin;
}

// A conditional-on-win HP average is vulnerable to survivor bias: END_TURN
// can discard more difficult rollouts and therefore report healthier *wins*
// while winning less often. Atomic card roots also have no lookahead
// advantage over END_TURN, so only reverse an END_TURN baseline when the
// non-END action is more reliable both in aggregate and in the lower quartile
// of equally weighted RNG worlds. This is deliberately one-sided and never
// reorders two card plays. Sparse worlds are handled by paired boundary
// dominance below; their correlated rollout counts never enter this rule.
inline int applyReliableActionEndTurnVeto(
    const std::vector<RootActionCandidate> &candidates,
    int baselineIdx
) {
    if (
        baselineIdx < 0
        || baselineIdx >= static_cast<int>(candidates.size())
        || !candidates[baselineIdx].isEndTurn
        || !hasCredibleWinEvidence(candidates)
    ) {
        return baselineIdx;
    }

    const auto &baseline = candidates[baselineIdx];
    const auto baselineInterval = rootWinRateConfidenceInterval(
        baseline.winSampleRate,
        baseline.visits
    );
    constexpr double tieTolerance = 1e-12;
    int bestIdx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const auto &candidate = candidates[i];
        if (
            candidate.isEndTurn
            || candidate.visits <= 0
            || candidate.rngWorlds <= 0
            || candidate.rngWorlds != baseline.rngWorlds
            || candidate.winningRngWorlds < baseline.winningRngWorlds
            || candidate.lowerQuartileWinSampleRate
               <= baseline.lowerQuartileWinSampleRate + tieTolerance
            || candidate.lowerQuartileBestWinUtility + tieTolerance
               < baseline.lowerQuartileBestWinUtility
        ) {
            continue;
        }
        const auto candidateInterval = rootWinRateConfidenceInterval(
            candidate.winSampleRate,
            candidate.visits
        );
        if (
            candidateInterval.lower
            <= baselineInterval.upper + tieTolerance
        ) {
            continue;
        }
        if (
            bestIdx < 0
            || candidate.winSampleRate
               > candidates[bestIdx].winSampleRate + tieTolerance
            || (
                std::abs(
                    candidate.winSampleRate
                    - candidates[bestIdx].winSampleRate
                ) <= tieTolerance
                && candidate.expectedEndHpOnWin
                   > candidates[bestIdx].expectedEndHpOnWin + tieTolerance
            )
        ) {
            bestIdx = i;
        }
    }
    return bestIdx < 0 ? baselineIdx : bestIdx;
}

// Sparse complete-combat rollouts inside one determinized world are highly
// correlated. They cannot supply a Wilson sample size. A non-END action may
// still replace END when the deterministic next-turn boundary proves, in every
// independently seeded world, that reachability, HP, potions, and state value
// are all no worse. Require either a strict survival-resource improvement, or
// a zero-energy value improvement with no loss of independent winning-world
// coverage. Pooled rollout confidence never enters this rule.
inline int applyPairedBoundaryEndTurnDominanceVeto(
    const std::vector<RootActionCandidate> &candidates,
    int baselineIdx
) {
    if (
        baselineIdx < 0
        || baselineIdx >= static_cast<int>(candidates.size())
        || !candidates[baselineIdx].isEndTurn
    ) {
        return baselineIdx;
    }

    constexpr double tieTolerance = 1e-12;
    const auto &baseline = candidates[baselineIdx];
    const auto hasCompleteSamples = [] (const RootActionCandidate &candidate) {
        const auto size = candidate.directEndBoundaryReached.size();
        return size > 0
            && candidate.directEndBoundaryHps.size() == size
            && candidate.directEndBoundaryPotionCounts.size() == size
            && candidate.directEndBoundaryValues.size() == size;
    };
    if (!hasCompleteSamples(baseline)) {
        return baselineIdx;
    }

    const auto dominates = [
        &baseline,
        &hasCompleteSamples,
        tieTolerance
    ] (const RootActionCandidate &candidate) {
        if (
            !hasCompleteSamples(candidate)
            || candidate.directEndBoundaryReached.size()
               != baseline.directEndBoundaryReached.size()
        ) {
            return false;
        }
        bool strictlyBetterResource = false;
        bool strictlyBetterValue = false;
        for (std::size_t i = 0;
             i < candidate.directEndBoundaryReached.size(); ++i) {
            const bool candidateReached =
                candidate.directEndBoundaryReached[i] != 0;
            const bool baselineReached =
                baseline.directEndBoundaryReached[i] != 0;
            if (baselineReached && !candidateReached) {
                return false;
            }
            if (!baselineReached) {
                strictlyBetterResource =
                    strictlyBetterResource || candidateReached;
                continue;
            }
            if (
                candidate.directEndBoundaryHps[i] + tieTolerance
                    < baseline.directEndBoundaryHps[i]
                || candidate.directEndBoundaryPotionCounts[i] + tieTolerance
                    < baseline.directEndBoundaryPotionCounts[i]
                || candidate.directEndBoundaryValues[i] + tieTolerance
                    < baseline.directEndBoundaryValues[i]
            ) {
                return false;
            }
            strictlyBetterResource = strictlyBetterResource
                || candidate.directEndBoundaryHps[i]
                    > baseline.directEndBoundaryHps[i] + tieTolerance
                || candidate.directEndBoundaryPotionCounts[i]
                    > baseline.directEndBoundaryPotionCounts[i] + tieTolerance;
            strictlyBetterValue = strictlyBetterValue
                || candidate.directEndBoundaryValues[i]
                    > baseline.directEndBoundaryValues[i] + tieTolerance;
        }
        return strictlyBetterResource
            || (
                candidate.zeroEnergyRootAction
                && candidate.winningRngWorlds >= baseline.winningRngWorlds
                && strictlyBetterValue
            );
    };
    const auto prefer = [tieTolerance] (
        const RootActionCandidate &candidate,
        const RootActionCandidate &incumbent
    ) {
        for (const auto comparison : {
                 candidate.directEndBoundaryReachRate
                     - incumbent.directEndBoundaryReachRate,
                 candidate.meanDirectEndBoundaryHp
                     - incumbent.meanDirectEndBoundaryHp,
                 candidate.meanDirectEndBoundaryPotionCount
                     - incumbent.meanDirectEndBoundaryPotionCount,
                 candidate.meanDirectEndBoundaryValue
                     - incumbent.meanDirectEndBoundaryValue,
             }) {
            if (comparison > tieTolerance) {
                return true;
            }
            if (comparison < -tieTolerance) {
                return false;
            }
        }
        return false;
    };

    int bestIdx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (
            candidates[i].isEndTurn
            || !dominates(candidates[i])
        ) {
            continue;
        }
        if (bestIdx < 0 || prefer(candidates[i], candidates[bestIdx])) {
            bestIdx = i;
        }
    }
    return bestIdx < 0 ? baselineIdx : bestIdx;
}

// Tail estimates are too sparse to rank ordinary card plays: doing so made one
// bad RNG quartile repeatedly override stronger aggregate survival evidence.
// They are still useful for one narrow question: whether taking another action
// is a material downside gamble compared with ending the turn now.  END_TURN
// must improve both tail survival and tail success utility; nominal victories
// such as letting a thief escape must not override a healthier, resource-
// preserving line. This veto can select END_TURN, but can never reorder two
// non-END actions.
inline int applyEndTurnLowerTailSafetyVeto(
    const std::vector<RootActionCandidate> &candidates,
    int baselineIdx
) {
    if (
        baselineIdx < 0
        || baselineIdx >= static_cast<int>(candidates.size())
        || candidates[baselineIdx].isEndTurn
    ) {
        return baselineIdx;
    }

    int bestEndTurnIdx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const auto &candidate = candidates[i];
        if (!candidate.isEndTurn || candidate.visits <= 0) {
            continue;
        }
        if (
            bestEndTurnIdx == -1
            || candidate.lowerQuartileWinSampleRate
               > candidates[bestEndTurnIdx].lowerQuartileWinSampleRate
        ) {
            bestEndTurnIdx = i;
        }
    }
    if (bestEndTurnIdx == -1) {
        return baselineIdx;
    }

    constexpr double kMinimumTailWinRateMargin = 0.01;
    const auto &baseline = candidates[baselineIdx];
    const auto &endTurn = candidates[bestEndTurnIdx];
    if (
        endTurn.lowerQuartileWinSampleRate
        <= baseline.lowerQuartileWinSampleRate + kMinimumTailWinRateMargin
    ) {
        return baselineIdx;
    }
    constexpr double kTailUtilityTieTolerance = 1e-12;
    if (
        endTurn.lowerQuartileSuccessUtility
        <= baseline.lowerQuartileSuccessUtility
           + kTailUtilityTieTolerance
    ) {
        return baselineIdx;
    }
    return bestEndTurnIdx;
}

// Atomic roots do not have equal effective depth: END_TURN reaches the next
// player decision immediately, while playing a card may require more card or
// selection actions first.  Terminal rollout rates therefore give END_TURN an
// artificial extra layer of lookahead.  Correct only the unambiguous case: if
// a non-END action reaches a structurally identical replanning boundary that
// is no worse in reachability, real/effective HP, potion inventory, and
// monotonic combat progress (damage, Weak, or Vulnerable), and is strictly
// better in at least one of them, END_TURN is dominated.
inline int applyCommonBoundaryEndTurnDominanceVeto(
    const std::vector<RootActionCandidate> &candidates,
    int baselineIdx
) {
    if (
        baselineIdx < 0
        || baselineIdx >= static_cast<int>(candidates.size())
        || !candidates[baselineIdx].isEndTurn
    ) {
        return baselineIdx;
    }

    constexpr double tieTolerance = 1e-12;
    const auto hasBoundary = [] (const RootActionCandidate &candidate) {
        return candidate.visits > 0
            && std::isfinite(candidate.directEndBoundaryReachRate)
            && std::isfinite(candidate.meanDirectEndBoundaryHp)
            && std::isfinite(candidate.meanDirectEndBoundaryPotionCount)
            && std::isfinite(candidate.meanDirectEndBoundaryValue)
            && !candidate.directEndBoundaryStateKeys.empty();
    };
    const auto &baseline = candidates[baselineIdx];
    if (!hasBoundary(baseline)) {
        return baselineIdx;
    }

    const auto compareComparableState = [&baseline] (
        const RootActionCandidate &candidate
    ) {
        if (
            candidate.directEndBoundaryStateKeys.size()
            != baseline.directEndBoundaryStateKeys.size()
        ) {
            return std::pair{false, false};
        }
        const bool hasMonotonicEvidence =
            candidate.monotonicBoundaryStateKeys.size()
                == candidate.directEndBoundaryStateKeys.size()
            && baseline.monotonicBoundaryStateKeys.size()
                == baseline.directEndBoundaryStateKeys.size()
            && candidate.directEndBoundaryMonotonicCardProgress.size()
                == candidate.directEndBoundaryStateKeys.size()
            && baseline.directEndBoundaryMonotonicCardProgress.size()
                == baseline.directEndBoundaryStateKeys.size()
            && candidate.directEndBoundaryStrength.size()
                == candidate.directEndBoundaryStateKeys.size()
            && baseline.directEndBoundaryStrength.size()
                == baseline.directEndBoundaryStateKeys.size();
        bool strictlyBetterProgress = false;
        for (std::size_t i = 0;
             i < candidate.directEndBoundaryStateKeys.size(); ++i) {
            const bool exactlyEqual =
                candidate.directEndBoundaryStateKeys[i] == 0
                    ? false
                    : candidate.directEndBoundaryStateKeys[i]
                          == baseline.directEndBoundaryStateKeys[i];
            if (exactlyEqual) {
                continue;
            }
            const bool equalIgnoringMonotonicProgress =
                hasMonotonicEvidence
                && candidate.monotonicBoundaryStateKeys[i] != 0
                && candidate.monotonicBoundaryStateKeys[i]
                    == baseline.monotonicBoundaryStateKeys[i]
                && candidate.directEndBoundaryMonotonicCardProgress[i]
                    >= baseline.directEndBoundaryMonotonicCardProgress[i]
                && candidate.directEndBoundaryStrength[i]
                    >= baseline.directEndBoundaryStrength[i];
            if (!equalIgnoringMonotonicProgress) {
                return std::pair{false, false};
            }
            strictlyBetterProgress = strictlyBetterProgress
                || candidate.directEndBoundaryMonotonicCardProgress[i]
                    > baseline.directEndBoundaryMonotonicCardProgress[i]
                || candidate.directEndBoundaryStrength[i]
                    > baseline.directEndBoundaryStrength[i];
        }
        return std::pair{true, strictlyBetterProgress};
    };

    const auto dominates = [&baseline, tieTolerance] (
        const RootActionCandidate &candidate,
        bool strictlyBetterProgress
    ) {
        if (
            candidate.directEndBoundaryReachRate
            > baseline.directEndBoundaryReachRate + tieTolerance
        ) {
            return true;
        }
        if (
            candidate.directEndBoundaryReachRate + tieTolerance
            < baseline.directEndBoundaryReachRate
        ) {
            return false;
        }
        const bool noWorse =
            candidate.meanDirectEndBoundaryHp + tieTolerance
                >= baseline.meanDirectEndBoundaryHp
            && candidate.meanDirectEndBoundaryPotionCount + tieTolerance
                >= baseline.meanDirectEndBoundaryPotionCount
            && candidate.meanDirectEndBoundaryValue + tieTolerance
                >= baseline.meanDirectEndBoundaryValue;
        const bool strictlyBetter =
            candidate.meanDirectEndBoundaryHp
                > baseline.meanDirectEndBoundaryHp + tieTolerance
            || candidate.meanDirectEndBoundaryPotionCount
                > baseline.meanDirectEndBoundaryPotionCount + tieTolerance
            || candidate.meanDirectEndBoundaryValue
                > baseline.meanDirectEndBoundaryValue + tieTolerance
            || strictlyBetterProgress;
        return noWorse && strictlyBetter;
    };
    const auto preferBoundary = [tieTolerance] (
        const RootActionCandidate &candidate,
        const RootActionCandidate &incumbent
    ) {
        const auto compare = [tieTolerance] (double left, double right) {
            if (left > right + tieTolerance) {
                return 1;
            }
            if (left + tieTolerance < right) {
                return -1;
            }
            return 0;
        };
        for (const auto result : {
                 compare(
                     candidate.directEndBoundaryReachRate,
                     incumbent.directEndBoundaryReachRate
                 ),
                 compare(
                     candidate.meanDirectEndBoundaryHp,
                     incumbent.meanDirectEndBoundaryHp
                 ),
                 compare(
                     candidate.meanDirectEndBoundaryPotionCount,
                     incumbent.meanDirectEndBoundaryPotionCount
                 ),
                 compare(
                     candidate.meanDirectEndBoundaryValue,
                     incumbent.meanDirectEndBoundaryValue
                 ),
             }) {
            if (result != 0) {
                return result > 0;
            }
        }
        return candidate.successUtility > incumbent.successUtility
               + tieTolerance;
    };

    int bestIdx = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const auto &candidate = candidates[i];
        const auto [sameComparableState, strictlyBetterProgress] =
            compareComparableState(candidate);
        if (candidate.isEndTurn || !hasBoundary(candidate)
            || !sameComparableState
            || !dominates(candidate, strictlyBetterProgress)) {
            continue;
        }
        if (
            bestIdx < 0
            || preferBoundary(candidate, candidates[bestIdx])
        ) {
            bestIdx = i;
        }
    }
    return bestIdx < 0 ? baselineIdx : bestIdx;
}

// END_TURN has one more layer of effective search depth than an atomic card
// play.  Override it only when every determinized RNG world proves that a
// candidate reaches the identical next-turn state with no difference except
// at least as much retained Block, and at least one world has strictly more.
// This deliberately does not reason from card names or encounter rules.
inline int applyStrictBlockEndTurnDominanceVeto(
    const std::vector<RootActionCandidate> &candidates,
    int baselineIdx
) {
    if (
        baselineIdx < 0
        || baselineIdx >= static_cast<int>(candidates.size())
        || !candidates[baselineIdx].isEndTurn
    ) {
        return baselineIdx;
    }

    const auto hasEvidence = [] (const RootActionCandidate &candidate) {
        return candidate.visits > 0
            && !candidate.strictBlockBoundaryStateKeys.empty()
            && candidate.strictBlockBoundaryStateKeys.size()
               == candidate.shuffledBlockBoundaryStateKeys.size()
            && candidate.strictBlockBoundaryStateKeys.size()
               == candidate.directEndBoundaryShuffleAdvances.size()
            && candidate.strictBlockBoundaryStateKeys.size()
               == candidate.directEndBoundaryBlocks.size();
    };
    const auto &baseline = candidates[baselineIdx];
    if (!hasEvidence(baseline)) {
        return baselineIdx;
    }

    const auto strictlyDominates = [&baseline, &hasEvidence] (
        const RootActionCandidate &candidate
    ) {
        if (
            !hasEvidence(candidate)
            || candidate.strictBlockBoundaryStateKeys.size()
               != baseline.strictBlockBoundaryStateKeys.size()
        ) {
            return false;
        }
        bool strictlyBetter = false;
        for (std::size_t i = 0;
             i < candidate.strictBlockBoundaryStateKeys.size(); ++i) {
            const bool exactFutureMatches =
                candidate.strictBlockBoundaryStateKeys[i] != 0
                && candidate.strictBlockBoundaryStateKeys[i]
                   == baseline.strictBlockBoundaryStateKeys[i];
            // A uniform shuffle has the same distribution for every ordering
            // of the same multiset. Pairing identical RNG seeds can nevertheless
            // produce different realized orders when the root card entered the
            // discard pile first. Accept that coupling difference only when
            // both paths consumed the same positive number of shuffle draws and
            // every other canonicalized future-state field still matches.
            const bool shuffledDistributionMatches =
                candidate.directEndBoundaryShuffleAdvances[i] > 0
                && candidate.directEndBoundaryShuffleAdvances[i]
                   == baseline.directEndBoundaryShuffleAdvances[i]
                && candidate.shuffledBlockBoundaryStateKeys[i] != 0
                && candidate.shuffledBlockBoundaryStateKeys[i]
                   == baseline.shuffledBlockBoundaryStateKeys[i];
            if (
                (!exactFutureMatches && !shuffledDistributionMatches)
                || candidate.directEndBoundaryBlocks[i]
                   < baseline.directEndBoundaryBlocks[i]
            ) {
                return false;
            }
            strictlyBetter = strictlyBetter
                || candidate.directEndBoundaryBlocks[i]
                   > baseline.directEndBoundaryBlocks[i];
        }
        return strictlyBetter;
    };

    int bestIdx = -1;
    std::int64_t bestTotalBlock = std::numeric_limits<std::int64_t>::min();
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const auto &candidate = candidates[i];
        if (candidate.isEndTurn || !strictlyDominates(candidate)) {
            continue;
        }
        const auto totalBlock = std::accumulate(
            candidate.directEndBoundaryBlocks.begin(),
            candidate.directEndBoundaryBlocks.end(),
            std::int64_t{0}
        );
        if (
            bestIdx < 0
            || totalBlock > bestTotalBlock
            || (
                totalBlock == bestTotalBlock
                && candidate.successUtility
                   > candidates[bestIdx].successUtility
            )
        ) {
            bestIdx = i;
            bestTotalBlock = totalBlock;
        }
    }
    return bestIdx < 0 ? baselineIdx : bestIdx;
}

inline int selectRootActionWithEndTurnSafety(
    const std::vector<RootActionCandidate> &candidates
) {
    const int selected = selectRootActionByWorldBest(candidates);
    const int commonBoundarySelected =
        applyCommonBoundaryEndTurnDominanceVeto(candidates, selected);
    return applyStrictBlockEndTurnDominanceVeto(
        candidates,
        commonBoundarySelected
    );
}

} // namespace sts::search
