#!/usr/bin/env python3
"""Evaluate a weight-only checkpoint on fixed seeds. This script never trains."""
import argparse
import hashlib
import importlib
import json
import multiprocessing as mp
import os
from pathlib import Path
import statistics

BASE = Path(__file__).resolve().parents[1]
_algorithm = None
_network = None


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def configure(variant, simulations, simulator_build):
    os.environ["STS_BOT_DIR"] = str(BASE / "data")
    os.environ["STS_SIM_BUILD"] = str(simulator_build)
    os.environ["STS_SIM_COUNT"] = str(simulations)
    os.environ["STS_CLASS"] = "DEFECT"
    os.environ["ASC"] = "20"
    os.environ["ARM_G_ARCH"] = "128,128"
    os.environ["OMP_NUM_THREADS"] = "1"
    os.environ["MKL_NUM_THREADS"] = "1"
    os.environ["OPENBLAS_NUM_THREADS"] = "1"
    os.environ["VECLIB_MAXIMUM_THREADS"] = "1"
    os.environ["NUMEXPR_NUM_THREADS"] = "1"
    os.environ["STS_EVAL_VARIANT"] = variant


def initialize_worker(variant, checkpoint, simulations, simulator_build):
    global _algorithm, _network
    configure(variant, simulations, simulator_build)
    import torch

    torch.set_num_threads(1)
    _algorithm = importlib.import_module(variant)
    _network = _algorithm.A.Scorer((128, 128))
    weights = torch.load(checkpoint, map_location="cpu", weights_only=True)
    _network.load_state_dict(weights)
    _network.eval()


def evaluate_seed(seed):
    result = _algorithm.A.play_game(seed, _network, train=False)
    result["traj"] = []
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=["v28", "curriculum"], required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--seeds", type=Path, default=BASE / "data/eval_seeds_50.txt")
    parser.add_argument("--simulator-build", type=Path, default=BASE / "simulator/build")
    parser.add_argument("--simulations", type=int, default=8000)
    parser.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--max-seeds", type=int)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check-model", action="store_true")
    args = parser.parse_args()

    checkpoint = args.checkpoint.expanduser().resolve()
    seeds_path = args.seeds.expanduser().resolve()
    simulator_build = args.simulator_build.expanduser().resolve()
    if not checkpoint.is_file():
        raise FileNotFoundError(checkpoint)
    if args.simulations < 1 or args.workers < 1:
        raise ValueError("simulations and workers must be positive")

    initialize_worker(args.variant, str(checkpoint), args.simulations, str(simulator_build))
    parameters = sum(parameter.numel() for parameter in _network.parameters())
    identity = {
        "variant": args.variant,
        "checkpoint": str(checkpoint),
        "checkpoint_sha256": file_sha256(checkpoint),
        "parameters": parameters,
        "model_load": "PASS",
        "training_started": False,
    }
    if args.check_model:
        print(json.dumps(identity, indent=2))
        return

    seeds = [
        int(line)
        for line in seeds_path.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if args.max_seeds is not None:
        if args.max_seeds < 1:
            raise ValueError("max-seeds must be positive")
        seeds = seeds[: args.max_seeds]

    context = mp.get_context("spawn")
    with context.Pool(
        args.workers,
        initializer=initialize_worker,
        initargs=(args.variant, str(checkpoint), args.simulations, str(simulator_build)),
    ) as pool:
        results = pool.map(evaluate_seed, seeds)

    summary = {
        **identity,
        "character": "DEFECT",
        "ascension": 20,
        "simulations": args.simulations,
        "seeds_file": str(seeds_path),
        "seeds_sha256": file_sha256(seeds_path),
        "evaluated_seeds": len(results),
        "mean_floor": statistics.mean(row["floor"] for row in results),
        "act4": sum(row.get("act", 1) == 4 or row["floor"] >= 53 for row in results),
        "victories": sum(bool(row.get("win")) for row in results),
        "heart_victories": sum(row.get("act", 1) == 4 and bool(row.get("win")) for row in results),
        "results": [{key: value for key, value in row.items() if key != "traj"} for row in results],
        "limitations": "Fixed-seed point estimate; no confidence interval; hardware and build changes may affect results.",
    }
    rendered = json.dumps(summary, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="")


if __name__ == "__main__":
    main()
