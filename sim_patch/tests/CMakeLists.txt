cmake_minimum_required(VERSION 3.19)
project(combat_rules_regression LANGUAGES CXX)

set(STS_SIM_ROOT "" CACHE PATH "sts_lightspeed checkout with the documented patches")
if(NOT EXISTS "${STS_SIM_ROOT}/src/combat/BattleContext.cpp")
    message(FATAL_ERROR "Set STS_SIM_ROOT to a sts_lightspeed checkout")
endif()
set(STS_JSON_INCLUDE "${STS_SIM_ROOT}/json/single_include" CACHE PATH "nlohmann/json include directory")
if(NOT EXISTS "${STS_JSON_INCLUDE}/nlohmann/json.hpp")
    message(FATAL_ERROR "Initialize the simulator's json submodule or set STS_JSON_INCLUDE")
endif()

file(GLOB_RECURSE SIM_SOURCES CONFIGURE_DEPENDS "${STS_SIM_ROOT}/src/*.cpp")
add_executable(combat_rules combat_rules.cpp ${SIM_SOURCES})
target_compile_features(combat_rules PRIVATE cxx_std_17)
target_include_directories(combat_rules PRIVATE "${STS_SIM_ROOT}/include" "${STS_JSON_INCLUDE}")

enable_testing()
foreach(case_name IN ITEMS blood_normal blood_bark blood_rounding blood_cap
        iron_wave_base iron_wave_dex iron_wave_frail iron_wave_negative_dex
        cardless_poison cardless_fire cardless_death poison_with_cards
        poison_block poison_intangible poison_expiration poison_artifact poison_targets)
    add_test(NAME ${case_name} COMMAND combat_rules ${case_name})
    set_tests_properties(${case_name} PROPERTIES TIMEOUT 10)
endforeach()
