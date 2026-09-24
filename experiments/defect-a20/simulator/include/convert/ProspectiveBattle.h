#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "combat/BattleContext.h"
#include "constants/MonsterEncounters.h"
#include "game/Card.h"
#include "game/GameContext.h"

namespace sts::evaluation {

struct CardSpec {
    std::string id;
    int upgrades = 0;
    int misc = 0;
    bool bottled = false;
    Card card;
};

struct CandidateSpec {
    int choiceId = -1;
    CardSpec card;
};

struct ProspectiveBattleSpec {
    nlohmann::json source;
    std::vector<CardSpec> deck;
    std::vector<CandidateSpec> candidates;
    std::vector<MonsterEncounter> targets;
};

CardSpec parseCardSpec(const nlohmann::json &json, const std::string &location);
MonsterEncounter parseEncounter(const std::string &name);
ProspectiveBattleSpec parseProspectiveBattleSpec(const nlohmann::json &json);

GameContext buildProspectiveGame(
    const ProspectiveBattleSpec &spec,
    const std::optional<CandidateSpec> &candidate
);

BattleContext buildProspectiveBattle(
    const GameContext &gc,
    MonsterEncounter encounter,
    int rngWorld
);

}  // namespace sts::evaluation
