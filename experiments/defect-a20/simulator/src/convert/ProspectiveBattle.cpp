#include "convert/ProspectiveBattle.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

#include "constants/Cards.h"
#include "constants/CharacterClasses.h"
#include "constants/Potions.h"
#include "constants/Relics.h"
#include "game/Deck.h"
#include "game/Random.h"

using namespace sts;

namespace {

std::string requireString(
    const nlohmann::json &value,
    const char *field,
    const std::string &location
) {
    const auto it = value.find(field);
    if (it == value.end() || !it->is_string() || it->get<std::string>().empty()) {
        throw std::runtime_error(location + "." + field + " must be a non-empty string");
    }
    return it->get<std::string>();
}

int requireInt(
    const nlohmann::json &value,
    const char *field,
    const std::string &location
) {
    const auto it = value.find(field);
    if (it == value.end() || !it->is_number_integer()) {
        throw std::runtime_error(location + "." + field + " must be an integer");
    }
    return it->get<int>();
}

std::string normalizedName(const std::string &value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char c : value) {
        if (std::isalnum(c)) {
            result.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return result;
}

RelicId parseRelicId(const std::string &id) {
    const auto normalized = normalizedName(id);
    for (int index = 0; index < static_cast<int>(std::size(relicIds)); ++index) {
        if (
            normalizedName(relicIds[index]) == normalized
            || normalizedName(relicNames[index]) == normalized
            || normalizedName(relicEnumNames[index]) == normalized
        ) {
            return static_cast<RelicId>(index);
        }
    }
    throw std::runtime_error("unknown relic id or name: " + id);
}

CardId parseCardId(const std::string &id) {
    const auto exact = getCardIdFromId(id);
    if (exact != CardId::INVALID) {
        return exact;
    }
    const auto normalized = normalizedName(id);
    // Display names are intentionally colorless.  This evaluator only accepts
    // Ironclad decks, so the two duplicated starter names resolve to red.
    if (normalized == "strike") {
        return CardId::STRIKE_RED;
    }
    if (normalized == "defend") {
        return CardId::DEFEND_RED;
    }
    for (int index = 1; index < static_cast<int>(std::size(cardNames)); ++index) {
        if (
            normalizedName(cardNames[index]) == normalized
            || normalizedName(cardEnumStrings[index]) == normalized
        ) {
            return static_cast<CardId>(index);
        }
    }
    return CardId::INVALID;
}

CharacterClass parseCharacter(const std::string &name) {
    if (name == "IRONCLAD") {
        return CharacterClass::IRONCLAD;
    }
    if (name == "DEFECT") {
        return CharacterClass::DEFECT;
    }
    throw std::runtime_error(
        "prospective battle evaluator supports IRONCLAD and DEFECT only"
    );
}

Room roomForEncounter(MonsterEncounter encounter) {
    if (isBossEncounter(encounter)) {
        return Room::BOSS;
    }
    switch (encounter) {
        case MonsterEncounter::GREMLIN_NOB:
        case MonsterEncounter::LAGAVULIN:
        case MonsterEncounter::THREE_SENTRIES:
        case MonsterEncounter::GREMLIN_LEADER:
        case MonsterEncounter::SLAVERS:
        case MonsterEncounter::BOOK_OF_STABBING:
        case MonsterEncounter::GIANT_HEAD:
        case MonsterEncounter::NEMESIS:
        case MonsterEncounter::REPTOMANCER:
            return Room::ELITE;
        case MonsterEncounter::LAGAVULIN_EVENT:
        case MonsterEncounter::COLOSSEUM_EVENT_SLAVERS:
        case MonsterEncounter::COLOSSEUM_EVENT_NOBS:
        case MonsterEncounter::MASKED_BANDITS_EVENT:
        case MonsterEncounter::MUSHROOMS_EVENT:
        case MonsterEncounter::MYSTERIOUS_SPHERE_EVENT:
            return Room::EVENT;
        default:
            return Room::MONSTER;
    }
}

void importRelics(const nlohmann::json &gameState, GameContext &gc) {
    const auto it = gameState.find("relics");
    if (it == gameState.end() || !it->is_array()) {
        throw std::runtime_error("game_state.relics must be an array");
    }
    for (int index = 0; index < static_cast<int>(it->size()); ++index) {
        const auto &raw = (*it)[index];
        const auto location = "game_state.relics[" + std::to_string(index) + "]";
        if (!raw.is_object()) {
            throw std::runtime_error(location + " must be an object");
        }
        const auto relicId = parseRelicId(requireString(raw, "id", location));
        int counter = requireInt(raw, "counter", location);
        if (relicId == RelicId::LIZARD_TAIL) {
            counter = counter == -2 ? 0 : 1;
        }
        gc.relics.add({relicId, counter});
    }
}

void importPotions(const nlohmann::json &gameState, GameContext &gc) {
    std::fill(
        gc.potions.begin(),
        gc.potions.end(),
        Potion::EMPTY_POTION_SLOT
    );
    const auto it = gameState.find("potions");
    if (it == gameState.end() || !it->is_array()) {
        throw std::runtime_error("game_state.potions must be an array");
    }
    if (it->size() > gc.potions.size()) {
        throw std::runtime_error("game_state.potions exceeds simulator capacity");
    }
    gc.potionCapacity = static_cast<int>(it->size());
    gc.potionCount = 0;
    for (int index = 0; index < static_cast<int>(it->size()); ++index) {
        const auto &raw = (*it)[index];
        const auto location = "game_state.potions[" + std::to_string(index) + "]";
        if (!raw.is_object()) {
            throw std::runtime_error(location + " must be an object");
        }
        const auto id = requireString(raw, "id", location);
        const auto potion = getPotionFromId(id);
        if (potion == Potion::INVALID) {
            throw std::runtime_error("unknown potion id: " + id);
        }
        gc.potions[index] = potion;
        if (potion != Potion::EMPTY_POTION_SLOT) {
            ++gc.potionCount;
        }
    }
}

void importDeck(
    const std::vector<evaluation::CardSpec> &cards,
    GameContext &gc
) {
    if (cards.empty()) {
        throw std::runtime_error("game_state.deck must not be empty");
    }
    if (cards.size() > Deck::MAX_SIZE) {
        throw std::runtime_error("prospective deck exceeds simulator capacity");
    }

    std::array<bool, 3> bottledTypes {false, false, false};
    for (const auto &spec : cards) {
        gc.deck.obtainRaw(spec.card);
        if (!spec.bottled) {
            continue;
        }
        const auto type = spec.card.getType();
        if (
            type != CardType::ATTACK
            && type != CardType::SKILL
            && type != CardType::POWER
        ) {
            throw std::runtime_error(
                "only Attack, Skill, or Power cards can be bottled: " + spec.id
            );
        }
        const auto typeIdx = static_cast<int>(type);
        if (bottledTypes[typeIdx]) {
            throw std::runtime_error(
                "more than one bottled card was supplied for the same card type"
            );
        }
        bottledTypes[typeIdx] = true;
        gc.deck.bottleCard(gc.deck.size() - 1, type);
    }

    const std::array<std::pair<RelicId, CardType>, 3> bottleRelics {{
        {RelicId::BOTTLED_FLAME, CardType::ATTACK},
        {RelicId::BOTTLED_LIGHTNING, CardType::SKILL},
        {RelicId::BOTTLED_TORNADO, CardType::POWER},
    }};
    for (const auto &[relic, type] : bottleRelics) {
        const auto hasRelic = gc.relics.has(relic);
        const auto hasCard = bottledTypes[static_cast<int>(type)];
        if (hasRelic != hasCard) {
            throw std::runtime_error(
                std::string(getRelicName(relic))
                + " requires exactly one deck card with bottled=true"
            );
        }
    }
}

void setWorldCounter(Random &rng, int rngWorld) {
    constexpr int kCounterStride = 1000;
    rng.setCounter(rngWorld * kCounterStride);
}

}  // namespace

evaluation::CardSpec evaluation::parseCardSpec(
    const nlohmann::json &json,
    const std::string &location
) {
    if (!json.is_object()) {
        throw std::runtime_error(location + " must be an object");
    }
    CardSpec result;
    result.id = requireString(json, "id", location);
    result.upgrades = json.value("upgrades", 0);
    if (result.upgrades < 0 || result.upgrades > 1) {
        throw std::runtime_error(
            location
            + ".upgrades must be 0 or 1; multi-upgrade cards are not yet exact"
        );
    }
    const auto cardId = parseCardId(result.id);
    if (cardId == CardId::INVALID) {
        throw std::runtime_error("unknown card id: " + result.id);
    }
    if (
        cardId == CardId::RITUAL_DAGGER
        || cardId == CardId::GENETIC_ALGORITHM
    ) {
        if (!json.contains("misc") || !json["misc"].is_number_integer()) {
            throw std::runtime_error(
                location + ".misc is required for permanently scaling card "
                + result.id
            );
        }
    }
    result.misc = json.value("misc", 0);
    if (
        result.misc < std::numeric_limits<std::int16_t>::min()
        || result.misc > std::numeric_limits<std::int16_t>::max()
    ) {
        throw std::runtime_error(location + ".misc exceeds simulator range");
    }
    result.bottled = json.value("bottled", false);
    result.card = Card(cardId, result.upgrades);
    result.card.misc = static_cast<std::int16_t>(result.misc);
    if (result.card.getType() == CardType::STATUS) {
        throw std::runtime_error(
            "status cards are not supported in the permanent deck: " + result.id
        );
    }
    return result;
}

MonsterEncounter evaluation::parseEncounter(const std::string &name) {
    const auto normalized = normalizedName(name);
    for (
        int index = 1;
        index < static_cast<int>(std::size(monsterEncouterNames));
        ++index
    ) {
        if (
            normalizedName(monsterEncouterNames[index]) == normalized
            || normalizedName(monsterEncounterEnumNames[index]) == normalized
        ) {
            return static_cast<MonsterEncounter>(index);
        }
    }
    throw std::runtime_error("unknown encounter: " + name);
}

evaluation::ProspectiveBattleSpec evaluation::parseProspectiveBattleSpec(
    const nlohmann::json &json
) {
    if (!json.is_object()) {
        throw std::runtime_error("input must be a JSON object");
    }
    const auto gameStateIt = json.find("game_state");
    if (gameStateIt == json.end() || !gameStateIt->is_object()) {
        throw std::runtime_error("game_state must be an object");
    }
    const auto &gameState = *gameStateIt;

    ProspectiveBattleSpec result;
    result.source = json;

    const auto deckIt = gameState.find("deck");
    if (deckIt == gameState.end() || !deckIt->is_array()) {
        throw std::runtime_error("game_state.deck must be an array");
    }
    for (int index = 0; index < static_cast<int>(deckIt->size()); ++index) {
        result.deck.push_back(parseCardSpec(
            (*deckIt)[index],
            "game_state.deck[" + std::to_string(index) + "]"
        ));
    }

    const auto candidatesIt = json.find("candidates");
    if (candidatesIt == json.end() || !candidatesIt->is_array()) {
        throw std::runtime_error("candidates must be an array");
    }
    std::set<int> choiceIds;
    for (int index = 0; index < static_cast<int>(candidatesIt->size()); ++index) {
        const auto &raw = (*candidatesIt)[index];
        CandidateSpec candidate;
        candidate.choiceId = raw.value("choice_id", index);
        if (candidate.choiceId < 0) {
            throw std::runtime_error("candidate choice_id must be non-negative");
        }
        if (!choiceIds.insert(candidate.choiceId).second) {
            throw std::runtime_error("candidate choice_id values must be unique");
        }
        candidate.card = parseCardSpec(
            raw,
            "candidates[" + std::to_string(index) + "]"
        );
        if (candidate.card.bottled) {
            throw std::runtime_error("a newly selected reward card cannot be bottled");
        }
        result.candidates.push_back(candidate);
    }

    const auto targetsIt = json.find("targets");
    if (targetsIt == json.end() || !targetsIt->is_array() || targetsIt->empty()) {
        throw std::runtime_error("targets must be a non-empty array");
    }
    for (const auto &target : *targetsIt) {
        if (!target.is_string()) {
            throw std::runtime_error("each target must be an encounter name string");
        }
        result.targets.push_back(parseEncounter(target.get<std::string>()));
    }
    return result;
}

GameContext evaluation::buildProspectiveGame(
    const ProspectiveBattleSpec &spec,
    const std::optional<CandidateSpec> &candidate
) {
    const auto &gameState = spec.source.at("game_state");
    GameContext gc;
    const auto rawSeed = gameState.at("seed").get<std::int64_t>();
    gc.seed = static_cast<std::uint64_t>(rawSeed);
    gc.outcome = GameOutcome::UNDECIDED;
    gc.ascension = requireInt(gameState, "ascension_level", "game_state");
    if (gc.ascension < 0 || gc.ascension > 20) {
        throw std::runtime_error("game_state.ascension_level must be between 0 and 20");
    }
    gc.act = requireInt(gameState, "act", "game_state");
    if (gc.act < 1 || gc.act > 4) {
        throw std::runtime_error("game_state.act must be between 1 and 4");
    }
    gc.floorNum = requireInt(gameState, "floor", "game_state");
    gc.curMapNodeX = 0;
    gc.curMapNodeY = 0;
    gc.cc = parseCharacter(requireString(gameState, "class", "game_state"));
    gc.curHp = requireInt(gameState, "current_hp", "game_state");
    gc.maxHp = requireInt(gameState, "max_hp", "game_state");
    if (gc.maxHp <= 0 || gc.curHp <= 0 || gc.curHp > gc.maxHp) {
        throw std::runtime_error("game_state HP values are invalid");
    }
    gc.gold = requireInt(gameState, "gold", "game_state");
    gc.speedrunPace = false;
    gc.curRoom = Room::BOSS;
    gc.lastRoom = Room::INVALID;

    gc.treasureRng = Random(gc.seed);
    gc.eventRng = Random(gc.seed);
    gc.relicRng = Random(gc.seed);
    gc.potionRng = Random(gc.seed);
    gc.cardRng = Random(gc.seed);
    gc.cardRandomRng = Random(gc.seed);
    gc.merchantRng = Random(gc.seed);
    gc.mathUtilRng = Random(gc.seed);
    gc.miscRng = Random(gc.seed + gc.floorNum);
    gc.monsterRng = Random(gc.seed);

    importRelics(gameState, gc);
    importPotions(gameState, gc);

    auto deck = spec.deck;
    if (candidate.has_value()) {
        if (deck.size() >= Deck::MAX_SIZE) {
            throw std::runtime_error("candidate would exceed simulator deck capacity");
        }
        deck.push_back(candidate->card);
    }
    importDeck(deck, gc);
    return gc;
}

BattleContext evaluation::buildProspectiveBattle(
    const GameContext &gc,
    MonsterEncounter encounter,
    int rngWorld
) {
    if (rngWorld < 0) {
        throw std::runtime_error("rngWorld must be non-negative");
    }
    auto encounterGame = gc;
    encounterGame.curRoom = roomForEncounter(encounter);
    BattleContext bc;
    bc.partialInitOne(encounterGame, encounter);
    setWorldCounter(bc.aiRng, rngWorld);
    setWorldCounter(bc.monsterHpRng, rngWorld);
    setWorldCounter(bc.shuffleRng, rngWorld);
    setWorldCounter(bc.cardRandomRng, rngWorld);
    setWorldCounter(bc.miscRng, rngWorld);
    setWorldCounter(bc.potionRng, rngWorld);

    bc.monsters.init(bc, encounter, false);
    bc.partialInitTwo(encounterGame);
    bc.initRelics(encounterGame);
    bc.player.energy += bc.player.energyPerTurn;
    bc.executeActions();
    return bc;
}
