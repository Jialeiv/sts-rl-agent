#include "sim/search/ActionEnumerator.h"
#include "sim/search/CardKey.h"
#include "sim/search/ExpertKnowledge.h"

#include "data_structure/fixed_list.h"

#include <algorithm>
#include <cassert>
#include <iostream>

using namespace sts;

namespace {

    bool hasCurrentlyPlayableAttack(const BattleContext &bc) {
        for (int i = 0; i < bc.cards.cardsInHand; ++i) {
            const auto &card = bc.cards.hand[i];
            if (
                card.getType() == CardType::ATTACK
                && card.canUseOnAnyTarget(bc)
            ) {
                return true;
            }
        }
        return false;
    }

    bool cardUsesDexterityForBlock(CardId id) {
        switch (id) {
            case CardId::DEFEND_RED:
            case CardId::DEFEND_BLUE:
            case CardId::DEFEND_GREEN:
            case CardId::DEFEND_PURPLE:
            case CardId::ARMAMENTS:
            case CardId::FINESSE:
            case CardId::FLAME_BARRIER:
            case CardId::GHOSTLY_ARMOR:
            case CardId::GOOD_INSTINCTS:
            case CardId::IMPERVIOUS:
            case CardId::IRON_WAVE:
            case CardId::PANIC_BUTTON:
            case CardId::POWER_THROUGH:
            case CardId::SECOND_WIND:
            case CardId::SENTINEL:
            case CardId::SHRUG_IT_OFF:
            case CardId::TRUE_GRIT:
                return true;
            default:
                return false;
        }
    }

    bool hasCurrentlyPlayableDexterityCard(const BattleContext &bc) {
        for (int i = 0; i < bc.cards.cardsInHand; ++i) {
            const auto &card = bc.cards.hand[i];
            if (
                cardUsesDexterityForBlock(card.getId())
                && card.canUseOnAnyTarget(bc)
            ) {
                return true;
            }
        }
        return false;
    }

    void enumerateCardActions(const BattleContext &bc, std::vector<search::Action> &out) {
        if (!bc.isCardPlayAllowed()) {
            return;
        }

        struct CardOption {
            int handIdx;
            int playOrdering;
        };

        fixed_list<CardOption, 10> playableHandIdxs;
        for (int handIdx = 0; handIdx < bc.cards.cardsInHand; ++handIdx) {
            const auto &c = bc.cards.hand[handIdx];
            if (!c.canUseOnAnyTarget(bc)) {
                continue;
            }

            bool isUniqueAction = true;
            if (handIdx > 0) {
                const auto &lastCard = bc.cards.hand[handIdx-1];
                const auto cardKey = search::toCardKey(c);
                const auto lastCardKey = search::toCardKey(lastCard);
                isUniqueAction = cardKey.id != lastCardKey.id
                                 || cardKey.upgradeCount != lastCardKey.upgradeCount
                                 || cardKey.specialData != lastCardKey.specialData
                                 || cardKey.cost != lastCardKey.cost
                                 || cardKey.costForTurn != lastCardKey.costForTurn
                                 || cardKey.freeToPlayOnce != lastCardKey.freeToPlayOnce
                                 || cardKey.retain != lastCardKey.retain;
            }

            if (isUniqueAction) {
                // this is being called a *lot* maybe swap it for a lookup table instead of a switch statement
                playableHandIdxs.push_back({
                    handIdx,
                    search::Expert::getPlayOrdering(c.getId())
                });
            }
        }

        std::sort(playableHandIdxs.begin(), playableHandIdxs.end(), [](const auto &a, const auto &b) {
            return a.playOrdering < b.playOrdering;
        });

        for (const auto &option : playableHandIdxs) {
            const auto handIdx = option.handIdx;
            const auto &c = bc.cards.hand[handIdx];

            if (c.requiresTarget()) {
                for (int tIdx = bc.monsters.monsterCount-1; tIdx >= 0; --tIdx) {
                    if (!bc.monsters.arr[tIdx].isTargetable()) {
                        continue;
                    }
                    out.push_back({search::Action(search::ActionType::CARD, handIdx, tIdx)});
                }
            } else {
                out.push_back({search::Action(search::ActionType::CARD, handIdx)});
            }
        }
    }

    void enumeratePotionActions(
        const BattleContext &bc,
        const search::ActionEnumerationOptions &opts,
        std::vector<search::Action> &out
    ) {
        const auto hasValidTarget = bc.monsters.getTargetableCount() > 0;

        for (int pIdx = 0; pIdx < bc.potionCapacity; ++pIdx) {
            if (
                opts.allowedPotionSlotMask != 0
                && (
                    pIdx >= 32
                    || (opts.allowedPotionSlotMask & (std::uint32_t{1} << pIdx)) == 0
                )
            ) {
                continue;
            }

            const auto p = bc.potions[pIdx];
            if (p == Potion::EMPTY_POTION_SLOT) {
                continue;
            }

            // Flex Potion's Strength is lost at end of turn.  If no attack can be
            // played in the current state, drinking it before a draw/setup action
            // has no advantage over waiting for the next decision, while an
            // independently replanned continuation may end the turn and waste the
            // potion.  Artifact is the exception because it can prevent Strength
            // Down and make the gain persistent.
            if (
                p == Potion::FLEX_POTION
                && !bc.player.hasStatus<PS::ARTIFACT>()
                && !hasCurrentlyPlayableAttack(bc)
            ) {
                continue;
            }
            if (
                p == Potion::SPEED_POTION
                && !bc.player.hasStatus<PS::ARTIFACT>()
                && !hasCurrentlyPlayableDexterityCard(bc)
            ) {
                continue;
            }

            // fairy potions cannot be used directly
            // TODO: smoke bombs are also not implemented lol
            if (p == Potion::FAIRY_POTION || p == Potion::SMOKE_BOMB) {
                continue;
            }

            if (p == Potion::LIQUID_MEMORIES && bc.cards.discardPile.empty()) {
                continue;
            }

            // if the potion requires a valid target and there are none, it cannot be used
            if (potionRequiresTarget(p) && !hasValidTarget) {
                continue;
            }

            // otherwise enumerate all valid ways to use the potion
            if (!potionRequiresTarget(p)) {
                // non-targeted potions have one use action
                out.push_back({search::Action(search::ActionType::POTION, pIdx)});
            } else {
                // targeted potions have one use action per valid monster target
                for (int tIdx = 0; tIdx < bc.monsters.monsterCount; ++tIdx) {
                    if (bc.monsters.arr[tIdx].isTargetable()) {
                        out.push_back({search::Action(search::ActionType::POTION, pIdx, tIdx)});
                    }
                }
            }
        }
    }

    void enumerateCardSelectActions(const BattleContext &bc, std::vector<search::Action> &out) {
        for (const auto &action : search::Action::enumerateCardSelectActions(bc)) {
            out.push_back(action);
        }
    }

}

namespace sts::search {

void enumerateActions(
    const BattleContext &bc,
    const ActionEnumerationOptions &opts,
    bool isRoot,
    std::vector<Action> &out
) {
    out.clear();
    switch (bc.inputState) {
        case InputState::PLAYER_NORMAL:
            enumerateCardActions(bc, out);
            if (
                opts.allowPotions
                || opts.allowedPotionSlotMask != 0
                || (opts.allowRootPotions && isRoot)
            ) {
                enumeratePotionActions(bc, opts, out);
            }
            out.push_back(Action(ActionType::END_TURN));
            break;

        case InputState::CARD_SELECT:
            enumerateCardSelectActions(bc, out);
            break;

        default:
#ifdef sts_asserts
            std::cerr << "enumerateActions: invalid input state: " << static_cast<int>(bc.inputState) << std::endl;
            assert(false);
#endif
            break;
    }
}

}
