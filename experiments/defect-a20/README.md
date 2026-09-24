# Slay the Spire Defect A20 policy experiment

This is a separate experimental track from the repository's original A0 Ironclad release. Do not compare or combine their reported averages as if they came from the same character, difficulty, simulator, feature schema, or model. The corresponding weights are published under [`defect-a20/` on Hugging Face](https://huggingface.co/Jialeiv/sts-rl-agent/tree/main/defect-a20).

Research code and frozen artifacts for a small learned non-combat policy paired with MCTS combat in a customized `sts_lightspeed` simulator.

## Current status

Training is paused and incomplete. Both active branches have a last confirmed formal checkpoint at 22,008 games against an original target of 30,000 games.

| Branch | Last formal evaluation | Best observed mean floor | Heart wins in the retained 50-seed evaluations |
| --- | ---: | ---: | ---: |
| V28 SAFE | 37.10 mean floor | 38.28 | 0 |
| V3 CURRICULUM | 34.34 mean floor | 37.80 | 0 |

These are repeated evaluations on one fixed set of 50 seeds, without confidence intervals. They are not a claim of state of the art performance. A zero Heart count in the retained evaluations does not prove that the policy can never win Act 4.

The separately released gameplay video used an older `demo-step15000.pt` model. It shows a standard Act 3 floor-52 victory and does not evaluate either active branch. The archive label records 15,000 training games, but no retained result binds this exact demo weight to a verified MCTS budget and 52-floor pass rate, so no batch performance claim is made for it.

## What the model learns

The two-layer MLP scores legal non-combat choices (map, rewards, shops, rest sites, and events). Combat remains MCTS in the simulator. The active checkpoints use an input width of 1,103 and hidden widths `128,128` (157,953 parameters).

- `training/v28.py`: floor-depth reward with entropy regularization.
- `training/curriculum.py`: key/Act-4-oriented curriculum reward.
- `training/armG_train_heart_zscore.py`: observation and action encoding, policy network, and simulated rollout.
- `training/resume.py`: safe warm-start/full-state runner. It prints a read-only plan unless `--run` is explicitly supplied.
- `training/evaluate.py`: fixed-seed evaluation or model shape check; it never trains.

## Requirements

- Python 3.12
- PyTorch and NumPy suitable for the host OS
- CMake, Git, and a C++17 compiler
- The public dependencies at the commits recorded in `ENVIRONMENT.json`

The archived training environment recorded Python 3.12.14, Torch 2.14.0+cpu, and NumPy 2.5.3. Those Linux package versions are provenance, not a promise that the same wheels exist for every platform.

## Build the simulator

```bash
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install torch numpy
git clone https://github.com/nlohmann/json.git simulator/json
git -C simulator/json checkout d8ebaf61d79512e3feea44c69128dea82eff59d9
git clone https://github.com/pybind/pybind11.git simulator/pybind11
git -C simulator/pybind11 checkout b0fa039cfdf22618599127fe52e70ad291d691e3
cmake -S simulator -B simulator/build -DCMAKE_BUILD_TYPE=Release -DPYBIND11_FINDPYTHON=ON -DPython_EXECUTABLE="$VIRTUAL_ENV/bin/python"
cmake --build simulator/build --target slaythespire -j 4
```

On an Intel Mac whose Command Line Tools do not expose the default libc++ header path:

```bash
SDK="$(xcrun --show-sdk-path)"
cmake -S simulator -B simulator/build -DCMAKE_BUILD_TYPE=Release \
  -DPYBIND11_FINDPYTHON=ON -DPython_EXECUTABLE="$VIRTUAL_ENV/bin/python" \
  -DCMAKE_OSX_SYSROOT="$SDK" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -isystem $SDK/usr/include/c++/v1"
cmake --build simulator/build --target slaythespire -j 4
```

## Check and evaluate a model

Clone or download the Hugging Face model repository, then point the scripts to its `defect-a20/models` directory:

```bash
git clone https://huggingface.co/Jialeiv/sts-rl-agent ../sts-rl-agent-models
MODEL_ROOT=../sts-rl-agent-models/defect-a20/models

python training/evaluate.py \
  --variant v28 \
  --checkpoint "$MODEL_ROOT/ckpts_CN1_V28_SAFE/step22008_r1.16_fl37.1_a4_1_h0.pt" \
  --check-model

python training/evaluate.py \
  --variant curriculum \
  --checkpoint "$MODEL_ROOT/ckpts_CN2_V3_CURRICULUM/step22008_r1.93_fl34.3_k3_14_a4_0_h0.pt" \
  --workers 4 --output evaluation.json
```

Evaluation uses the 50 seeds in `data/eval_seeds_50.txt` unless another seed file is supplied. Different hardware, compiler versions, PyTorch versions, and simulator changes can change results.

## Inspect a continuation plan

Point the runner to the downloaded model directory, then run without `--run`:

```bash
MODEL_ROOT=../sts-rl-agent-models/defect-a20/models
python training/resume.py --model-root "$MODEL_ROOT" --variant v28
python training/resume.py --model-root "$MODEL_ROOT" --variant curriculum --tail
```

Formal legacy checkpoints start a new optimizer from the known 22,008-game boundary. The two `paused-tail` weights are newer but have no trustworthy exact step; `--tail` starts a newly numbered phase at zero. Only a future `latest.state.pt` written by this runner can restore the saved optimizer and main-process RNG state. Even then, cross-machine bitwise identity is not guaranteed.

Starting a run requires an explicit new output directory and `--run`. Do not treat the example as a recommendation to train on a laptop:

```bash
python training/resume.py --model-root "$MODEL_ROOT" \
  --variant v28 --tail --workers 4 \
  --games 7992 --output runs/v28-tail-01 --run
```

## Provenance and license

The simulator is derived from `gamerpuppy/sts_lightspeed` and a Defect-capable fork described in `simulator/BUILD_HISTORY.md`. See `THIRD_PARTY_NOTICES.md` and `simulator/LICENSE.md`.

This is an unofficial research project. Slay the Spire and its marks belong to Mega Crit Games. No game binaries, JAR files, or extracted art assets are included.
