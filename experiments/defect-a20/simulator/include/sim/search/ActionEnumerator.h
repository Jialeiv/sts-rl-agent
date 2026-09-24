#pragma once

#include "sim/search/Action.h"

#include <cstdint>
#include <vector>

namespace sts::search {

    // Which potion actions the caller wants enumerated.  Root actions are
    // controlled separately from the rest of the tree so callers can compare
    // every root potion while still forbidding them deeper in the search.
    struct ActionEnumerationOptions {
        bool allowPotions = false;
        bool allowRootPotions = false;
        std::uint32_t allowedPotionSlotMask = 0;
    };

    // Appends every legal action in `bc` to `out` (out is cleared first).
    // `isRoot` enables the allowRootPotions option for the tree root only.
    void enumerateActions(
        const BattleContext &bc,
        const ActionEnumerationOptions &opts,
        bool isRoot,
        std::vector<Action> &out
    );

}
