#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "game/Map.h"

#include <nlohmann/json.hpp>

using nlohmann::json;
using sts::Map;
using sts::Room;

namespace {

struct Coordinate {
    int x = -1;
    int y = -1;

    bool operator<(const Coordinate &other) const {
        return y < other.y || (y == other.y && x < other.x);
    }

    bool operator==(const Coordinate &other) const {
        return x == other.x && y == other.y;
    }
};

struct GraphNode {
    Coordinate coordinate;
    std::string symbol;
    std::vector<Coordinate> children;
};

std::string displaySymbol(const json &rawNode) {
    const auto symbol = rawNode.value("symbol", "UNKNOWN");
    if (symbol == "E" && rawNode.value("is_burning", false)) {
        return "E*";
    }
    return symbol;
}

Room roomFromSymbol(const std::string &symbol) {
    if (symbol == "$") return Room::SHOP;
    if (symbol == "R") return Room::REST;
    if (symbol == "?") return Room::EVENT;
    if (symbol == "E") return Room::ELITE;
    if (symbol == "M") return Room::MONSTER;
    if (symbol == "T") return Room::TREASURE;
    if (symbol == "B") return Room::BOSS;
    return Room::NONE;
}

const json &gameStateFrom(const json &input) {
    if (input.contains("game_state") && input.at("game_state").is_object()) {
        return input.at("game_state");
    }
    return input;
}

std::string nodeId(const Coordinate &coordinate) {
    std::ostringstream result;
    result << 'L' << std::setfill('0') << std::setw(2) << coordinate.y
           << 'C' << coordinate.x;
    return result.str();
}

std::map<Coordinate, GraphNode> graphFromCommunicationMod(const json &input) {
    const auto &gameState = gameStateFrom(input);
    if (!gameState.contains("map") || !gameState.at("map").is_array()) {
        throw std::runtime_error("input has no game_state.map array");
    }

    std::map<Coordinate, GraphNode> graph;
    for (const auto &rawNode : gameState.at("map")) {
        if (!rawNode.is_object()) continue;
        const Coordinate coordinate{
            rawNode.value("x", -1),
            rawNode.value("y", -1),
        };
        if (coordinate.x < 0 || coordinate.y < 0) continue;

        GraphNode node{
            coordinate,
            displaySymbol(rawNode),
            {},
        };
        if (rawNode.contains("children") &&
            rawNode.at("children").is_array()) {
            for (const auto &child : rawNode.at("children")) {
                if (!child.is_object()) continue;
                const Coordinate childCoordinate{
                    child.value("x", -1),
                    child.value("y", -1),
                };
                if (childCoordinate.x < 0 || childCoordinate.y < 0) continue;
                if (std::find(
                        node.children.begin(),
                        node.children.end(),
                        childCoordinate
                    ) == node.children.end()) {
                    node.children.push_back(childCoordinate);
                }
            }
        }
        std::sort(node.children.begin(), node.children.end());
        graph[coordinate] = std::move(node);
    }
    if (graph.empty()) {
        throw std::runtime_error("game_state.map has no valid nodes");
    }
    return graph;
}

std::vector<int> choiceColumns(const json &gameState) {
    if (!gameState.contains("choice_list") ||
        !gameState.at("choice_list").is_array()) {
        throw std::runtime_error("input has no game_state.choice_list array");
    }

    static const std::regex coordinatePattern(R"(x\s*=\s*(-?\d+))");
    std::vector<int> result;
    for (const auto &choice : gameState.at("choice_list")) {
        int x = -1;
        if (choice.is_object()) {
            x = choice.value("x", -1);
        } else if (choice.is_string()) {
            std::smatch match;
            const auto text = choice.get<std::string>();
            if (std::regex_search(text, match, coordinatePattern)) {
                x = std::stoi(match[1].str());
            }
        }
        if (x < 0) {
            throw std::runtime_error(
                "could not parse map choice " + choice.dump()
            );
        }
        result.push_back(x);
    }
    if (result.empty()) {
        throw std::runtime_error("game_state.choice_list is empty");
    }
    return result;
}

Coordinate currentCoordinate(const json &gameState) {
    if (!gameState.contains("screen_state") ||
        !gameState.at("screen_state").is_object()) {
        return {0, -1};
    }
    const auto &screenState = gameState.at("screen_state");
    if (!screenState.contains("current_node") ||
        !screenState.at("current_node").is_object()) {
        return {0, -1};
    }
    const auto &current = screenState.at("current_node");
    return {
        current.value("x", 0),
        current.value("y", -1),
    };
}

bool isBossCoordinate(
    const Coordinate &coordinate,
    const std::map<Coordinate, GraphNode> &graph
) {
    if (coordinate.y >= 15) return true;
    const auto found = graph.find(coordinate);
    return found != graph.end() && found->second.symbol == "B";
}

std::string renderReachableGraph(const json &input) {
    const auto &gameState = gameStateFrom(input);
    const auto graph = graphFromCommunicationMod(input);
    auto current = currentCoordinate(gameState);
    const auto columns = choiceColumns(gameState);

    auto rootsExistAt = [&](const int y) {
        return std::all_of(
            columns.begin(),
            columns.end(),
            [&](const int x) {
                return graph.find(Coordinate{x, y}) != graph.end();
            }
        );
    };
    int rootY = current.y + 1;
    if (!rootsExistAt(rootY)) {
        const int firstY = graph.begin()->first.y;
        const int lastY = graph.rbegin()->first.y;
        if (current.y > lastY && rootsExistAt(firstY)) {
            // During an Act transition CommunicationMod may publish the new
            // map and its entrance choices while current_node still refers to
            // the previous Act's Boss (normally y=15). Treat this one
            // unambiguous stale-coordinate state as START.
            current = {0, firstY - 1};
            rootY = firstY;
        }
    }

    std::vector<Coordinate> roots;
    roots.reserve(columns.size());
    for (const int x : columns) {
        const Coordinate root{x, rootY};
        if (graph.find(root) == graph.end()) {
            throw std::runtime_error(
                "map choice points to missing node " + nodeId(root)
            );
        }
        roots.push_back(root);
    }

    std::set<Coordinate> reachable;
    std::vector<Coordinate> pending(roots.begin(), roots.end());
    bool reachesBoss = false;
    while (!pending.empty()) {
        const auto coordinate = pending.back();
        pending.pop_back();
        if (!reachable.insert(coordinate).second) continue;

        const auto found = graph.find(coordinate);
        if (found == graph.end()) {
            throw std::runtime_error(
                "reachable edge points to missing node " + nodeId(coordinate)
            );
        }
        for (const auto &child : found->second.children) {
            if (isBossCoordinate(child, graph)) {
                reachesBoss = true;
                continue;
            }
            if (graph.find(child) == graph.end()) {
                throw std::runtime_error(
                    "reachable edge points to missing node " + nodeId(child)
                );
            }
            pending.push_back(child);
        }
    }

    std::map<int, std::vector<Coordinate>> layers;
    for (const auto &coordinate : reachable) {
        layers[coordinate.y].push_back(coordinate);
    }

    std::ostringstream output;
    output << "MAP_GRAPH v1\n"
           << "direction: layer increases toward BOSS\n"
           << "node_id: L<layer>C<column>\n"
           << "legend: M=Combat ?=Unknown(Event/Combat/Shop) "
              "E=Elite E*=BurningElite "
              "R=Rest T=Chest $=Shop\n"
           << "current: ";
    if (current.y < 0) {
        output << "START\n";
    } else {
        output << nodeId(current);
        const auto found = graph.find(current);
        if (found != graph.end()) output << ' ' << found->second.symbol;
        output << '\n';
    }

    output << "choices: ";
    for (std::size_t index = 0; index < roots.size(); ++index) {
        if (index != 0) output << " | ";
        output << index << "->" << nodeId(roots[index]);
    }
    output << '\n';

    for (const auto &[layer, coordinates] : layers) {
        output << 'L' << std::setfill('0') << std::setw(2) << layer << ": ";
        for (std::size_t index = 0; index < coordinates.size(); ++index) {
            if (index != 0) output << " | ";
            const auto &coordinate = coordinates[index];
            const auto &node = graph.at(coordinate);
            output << 'C' << coordinate.x << ' ' << node.symbol << "->[";
            for (std::size_t childIndex = 0;
                 childIndex < node.children.size();
                 ++childIndex) {
                if (childIndex != 0) output << ',';
                const auto &child = node.children[childIndex];
                if (isBossCoordinate(child, graph)) {
                    output << "BOSS";
                } else {
                    output << nodeId(child);
                }
            }
            output << ']';
        }
        output << '\n';
    }

    std::string boss = "UNKNOWN";
    if (gameState.contains("act_boss") &&
        gameState.at("act_boss").is_string()) {
        boss = gameState.at("act_boss").get<std::string>();
    } else if (gameState.contains("boss") &&
               gameState.at("boss").is_string()) {
        boss = gameState.at("boss").get<std::string>();
    }
    output << "BOSS: " << boss;
    if (!reachesBoss) output << " (no explicit reachable edge in input)";
    output << '\n';
    return output.str();
}

Map mapFromCommunicationMod(const json &input) {
    const auto &gameState = gameStateFrom(input);
    if (!gameState.contains("map") || !gameState.at("map").is_array()) {
        throw std::runtime_error("input has no game_state.map array");
    }

    Map map;
    for (int y = 0; y < 15; ++y) {
        for (int x = 0; x < 7; ++x) {
            auto &node = map.getNode(x, y);
            node.x = x;
            node.y = y;
        }
    }

    for (const auto &rawNode : gameState.at("map")) {
        if (!rawNode.is_object()) continue;
        const int x = rawNode.value("x", -1);
        const int y = rawNode.value("y", -1);
        if (x < 0 || x >= 7 || y < 0 || y >= 15) continue;

        auto &node = map.getNode(x, y);
        node.room = roomFromSymbol(rawNode.value("symbol", ""));
        if (node.room == Room::ELITE &&
            rawNode.value("is_burning", false)) {
            map.burningEliteX = x;
            map.burningEliteY = y;
        }
        if (!rawNode.contains("children") || !rawNode.at("children").is_array()) {
            continue;
        }
        for (const auto &child : rawNode.at("children")) {
            if (!child.is_object()) continue;
            const int childX = child.value("x", -1);
            const int childY = child.value("y", -1);
            if (childX < 0 || childX >= 7 || node.edgeCount >= 3) continue;

            bool duplicate = false;
            for (int index = 0; index < node.edgeCount; ++index) {
                duplicate = duplicate || node.edges[index] == childX;
            }
            if (duplicate) continue;
            node.edges[node.edgeCount++] = childX;

            if (childY >= 0 && childY < 15) {
                auto &childNode = map.getNode(childX, childY);
                if (childNode.parentCount < 6) {
                    childNode.parents[childNode.parentCount++] = x;
                }
            }
        }
    }
    return map;
}

} // namespace

int main(int argc, char **argv) {
    try {
        json input;
        std::cin >> input;
        if (argc == 2 && std::string(argv[1]) == "--ascii") {
            const auto map = mapFromCommunicationMod(input);
            std::cout << map.toString(true);
        } else if (argc == 1) {
            std::cout << renderReachableGraph(input);
        } else {
            throw std::runtime_error("usage: map-render [--ascii]");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "map-render: " << error.what() << '\n';
        return 1;
    }
}
