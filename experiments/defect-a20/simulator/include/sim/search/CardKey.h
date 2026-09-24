#pragma once

#include "combat/CardInstance.h"

#include <tuple>

namespace sts::search {

    // Stable identity of a card for hashing and ordering: card id plus every
    // modifier that changes how the card behaves in combat.
    struct CardKey {
        int id;
        int upgradeCount;
        int specialData;
        int cost;
        int costForTurn;
        bool freeToPlayOnce;
        bool retain;
    };

    inline CardKey toCardKey(const CardInstance &card) {
        return {
            static_cast<int>(card.getId()),
            card.getUpgradeCount(),
            card.specialData,
            card.cost,
            card.costForTurn,
            card.freeToPlayOnce,
            card.retain
        };
    }

    // Stable ordering of two cards by identity and modifiers; used to make
    // spent-pile order irrelevant in boundary-state keys.
    inline bool cardLess(const CardInstance &left, const CardInstance &right) {
        const auto l = toCardKey(left);
        const auto r = toCardKey(right);
        return std::tie(
            l.id, l.upgradeCount, l.specialData, l.cost, l.costForTurn,
            l.freeToPlayOnce, l.retain
        ) < std::tie(
            r.id, r.upgradeCount, r.specialData, r.cost, r.costForTurn,
            r.freeToPlayOnce, r.retain
        );
    }

}
