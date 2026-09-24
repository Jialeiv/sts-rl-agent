#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "combat/BattleContext.h"
#include "convert/BattleConverter.h"

using namespace sts;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

BattleContext convert(const nlohmann::json &json) {
    int monsterIdxMap[5] {-1, -1, -1, -1, -1};
    BattleConverter converter;
    return converter.convertFromJson(json, monsterIdxMap);
}

bool conversionThrows(const nlohmann::json &json) {
    try {
        (void) convert(json);
        return false;
    } catch (const std::runtime_error &) {
        return true;
    }
}

} // namespace

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        std::cerr << "usage: defect-converter-test <communication-state.json>\n";
        return 2;
    }

    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "cannot open fixture: " << argv[1] << '\n';
        return 2;
    }
    nlohmann::json snapshot;
    input >> snapshot;

    const BattleContext imported = convert(snapshot);
    check(imported.player.cc == CharacterClass::DEFECT,
          "fixture must import as Defect");
    check(imported.player.orbSlots == 4,
          "orb_slots must preserve maxOrbs");
    check(imported.player.getOrbCount() == 3,
          "three non-empty CommunicationMod orb entries must be restored");
    check(imported.player.orbTypes[0] == Orb::LIGHTNING
              && imported.player.orbTypes[1] == Orb::DARK
              && imported.player.orbTypes[2] == Orb::PLASMA
              && imported.player.orbTypes[3] == Orb::EMPTY,
          "orb order and empty slot must match the real player array");
    check(imported.player.orbData[1] == 31,
          "Dark charge must use evoke_amount rather than passive_amount");
    check(imported.player.lightningChanneled == 7
              && imported.player.frostChanneled == 5,
          "Thunder Strike and Blizzard combat history must be restored");
    check(imported.player.lastDamageTaken > 0,
          "Emotion Chip pending state must survive conversion");

    auto drawPowerState = snapshot;
    drawPowerState["game_state"]["combat_state"]["player"]["powers"].push_back({
        {"id", "Draw"}, {"name", "Machine Learning"}, {"amount", 1}
    });
    const BattleContext drawPowerImported = convert(drawPowerState);
    check(drawPowerImported.player.getStatus<PS::MACHINE_LEARNING>() == 1,
          "CommunicationMod Draw power must import as Machine Learning");
    check(drawPowerImported.player.getRecurringDrawPerTurn()
              == drawPowerImported.player.cardDrawPerTurn + 1,
          "imported draw powers must contribute to recurring turn draw");

    auto combatCostState = snapshot;
    auto &forceField = combatCostState["game_state"]["combat_state"]["hand"][2];
    forceField["id"] = "Force Field";
    forceField["cost"] = 0;
    forceField["combat_cost"] = 2;
    forceField["misc"] = 0;
    combatCostState["game_state"]["combat_state"]
                   ["powers_played_this_combat"] = 2;
    const BattleContext combatCostImported = convert(combatCostState);
    check(combatCostImported.cards.hand[2].cost == 2
              && combatCostImported.cards.hand[2].costForTurn == 0,
          "combat_cost must remain distinct from a temporary turn cost");
    check(combatCostImported.cards.hand[2].specialData == 2,
          "Force Field must remember already-applied power discounts");

    check(imported.cards.cardsInHand == 3,
          "fixture hand must be fully imported");
    check(imported.cards.hand[0].id == CardId::CLAW
              && imported.cards.hand[0].specialData == 8,
          "Claw dynamic base damage must become its combat damage increment");
    check(imported.cards.hand[1].id == CardId::STEAM_BARRIER
              && imported.cards.hand[1].specialData == 4,
          "Steam Barrier dynamic base block must become its use count");
    check(imported.cards.hand[2].id == CardId::GENETIC_ALGORITHM
              && imported.cards.hand[2].specialData == 17,
          "misc-based dynamic card state must remain supported");

    auto nativeCommunicationModShape = snapshot;
    nativeCommunicationModShape["game_state"]["combat_state"]["player"]
                               .erase("orb_slots");
    const BattleContext fallbackImported = convert(nativeCommunicationModShape);
    check(fallbackImported.player.orbSlots == 4,
          "unpatched CommunicationMod shape must derive slots from orbs array length");

    auto missingHistory = snapshot;
    missingHistory["game_state"]["combat_state"]
                  .erase("lightning_channeled_this_combat");
    check(conversionThrows(missingHistory),
          "Defect conversion must fail instead of silently resetting missing orb history");

    auto missingClawState = snapshot;
    missingClawState["game_state"]["combat_state"]["hand"][0]
                    .erase("base_damage");
    check(conversionThrows(missingClawState),
          "Defect conversion must reject a Claw whose dynamic base damage is absent");

    if (failures != 0) {
        return 1;
    }
    std::cout << "Defect converter tests passed\n";
    return 0;
}
