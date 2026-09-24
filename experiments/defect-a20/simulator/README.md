# sts_lightspeed

For tree search and simulation of the popular rogue-like deckbuilder game Slay The Spire

**Features**
* c++ 17 compiled with gcc
* Standalone
* Designed to be 100% RNG accurate*
* Playable in console
* Speed: 1M random playouts in 5s with 16 threads
* Loading from save files (loading into combat currently only supported)
* Tree Search (best result, knowing the state of the game's rng)

**Planned Features**
* Tree search of possible game outcomes (not given the state of rng)

**Implementation Progress**
* All enemies
* All relics
* All Ironclad cards
* All Defect cards
* All colorless cards
* Everything outside of combat / all acts

**Getting Started**
* The project was built with Clion2021 and the [mingw64 toolchain](https://www.msys2.org/) on Windows 10
* The main target creates a simulator of the game that can be played in console.
* The test target creates a program with various commands that can be run, including random simulation
* Click the star button at the top of the repo :)

**Build tips**
* If your build fails with an error about not-return-only `constexpr` methods, ensure your compiler supports c++17.
* If CLion shows an error about not finding python libs when loading the cmake project, try opening CLion from the msys2 shell.

## Card reward counterfactual evaluator

`card-reward-eval` is a standalone counterfactual executable for comparing
`skip` with one to three permanent card reward candidates. It creates a fresh
boss battle from the supplied permanent deck for every candidate and uses the
same RNG worlds and MCTS budget for each variant. The executable only reports
evidence; the Python agent applies its own quality gate and retains final
decision authority.

```text
card-reward-eval <input.json> [simulations_per_world] [rng_worlds] [max_time_ms_per_world]
card-reward-eval --self-test
```

The input contains:

- `game_state`: `seed`, `ascension_level`, `act`, `floor`, HP, gold,
  `class`, permanent `deck`, relics with counters, and potion slots;
- `candidates`: card `id`, `upgrades`, and optional `choice_id`;
- `targets`: boss encounter names such as `The Guardian` or `The Heart`.

The primary output is the MCTS rollout win rate for each candidate and its
delta from `skip`. Per-world opening hands, monster HP, selected root action,
visits, and terminal sample rates are retained for auditing.

This executable deliberately does not predict later cards, upgrades, relics,
route damage, or potion use. It rejects state that cannot yet be reconstructed
exactly, including unidentified bottled cards and cards upgraded more than
once. A zero-win early-game Heart result is saturation, not evidence that every
candidate has equal long-term value. Small differences can also change with
search budget, so callers must treat unstable or closely grouped results as
inconclusive.
