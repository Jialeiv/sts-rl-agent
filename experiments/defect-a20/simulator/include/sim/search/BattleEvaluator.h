#pragma once

#include "sim/search/Action.h"
#include "sim/search/SearchTypes.h"

#include "combat/BattleContext.h"

namespace sts::search {

    // Pure state evaluation for the search: the terminal reward, the
    // immediate-delta action heuristics and the recovery-horizon snapshot.
    // None of these need search state; they only read battle states.
    //
    // The searcher keeps thin forwarding members (BattleScumSearcher2::
    // evaluateEndState / recoverySnapshot) for its own and callers' use.

    // Terminal reward in [-1, 1]: positive for a victory (bounded by the root
    // max HP, charged for consumed Lizard Tail reserve and escaped thief
    // gold), negative by how much enemy HP remains for losses/cutoffs.
    double evaluateEndState(
        const BattleContext &rootBc,
        const BattleContext &bc
    );

    // Immediate-delta heuristic used to bias expansion and rollouts.
    double evaluateActionHeuristic(
        const BattleContext &before,
        const Action &action,
        SearchIntent intent
    );

    // Pure before/after delta; callers that already executed the action on a
    // scratch state use this to avoid copying and executing it a second time.
    double evaluateActionDelta(
        const BattleContext &before,
        const BattleContext &after,
        SearchIntent intent
    );

    // Same idea for the fixed-horizon recovery objective.
    double evaluateRecoveryActionHeuristic(
        const BattleContext &before,
        const Action &action
    );

    // Pure before/after delta for the recovery objective.
    double evaluateRecoveryActionDelta(
        const BattleContext &before,
        const BattleContext &after
    );

    // Quality of `bc` at (or past) the fixed recovery horizon, relative to
    // the root state the search started from.
    RecoverySnapshot recoverySnapshot(
        const BattleContext &rootBc,
        const BattleContext &bc,
        int recoveryHorizonTurns
    );

    // Player HP including reachable one-shot healing reserves (Reaper).
    int getEffectiveReplanHp(const BattleContext &bc);

    // HP carried out of a won combat after deterministic end-of-combat healing.
    int getPostCombatPlayerHp(const BattleContext &bc);

    // The HP Lizard Tail would restore in this state (0 without the relic).
    double getLizardTailReserveHp(const BattleContext &bc);

    // Gold a thief (Looter/Mugger) has already escaped with.
    int getEscapedStolenGold(const BattleContext &bc);

    // Sum of non-minion enemy HP, scaled so splitting enemies count their
    // full split-off HP.  Also used by callers outside the search.
    double getNonMinionMonsterCurHpTotal(const BattleContext &bc);

}
