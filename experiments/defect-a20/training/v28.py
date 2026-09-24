#!/usr/bin/env python3
"""Arm G V2.8 · 深度势能 + 熵正则 + **防死锁 multiprocessing** (CN1)
【A 方案修复】诊断确认: 卡死根因是 multiprocessing 队列死锁 (主进程与全部 worker 同时 sleeping,
CPU 零增长)。不是 MCTS 慢, 是 pool.map 在 worker 异常时永久阻塞。
【修复】
  1. pool.map -> map_async + get(timeout): 超时则销毁并重建 pool, 跳过该批。
  2. worker 内部包 try/except: 任何异常都返回一个"失败但可序列化"的结果, 绝不污染队列。
  3. 主循环加心跳日志, 便于事后判断卡死点。
"""
import os, sys, json, time, random, statistics, tempfile, shutil, traceback
os.environ["OMP_NUM_THREADS"] = "1"
os.environ["MKL_NUM_THREADS"] = "1"
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["VECLIB_MAXIMUM_THREADS"] = "1"
os.environ["NUMEXPR_NUM_THREADS"] = "1"
import multiprocessing as mp
from pathlib import Path
SB = os.environ.get("STS_BOT_DIR", str(Path(__file__).resolve().parents[1] / "data"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import torch
torch.set_num_threads(1)
import armG_train_heart_zscore as A

ARCH = tuple(int(x) for x in os.environ.get("ARM_G_ARCH", "128,128").split(","))
BASE_CKPT = os.environ.get("RESUME_CKPT", os.path.join(SB, "ckpts_warmstart/step2016_shaped.pt"))
ENT_COEF = float(os.environ.get("ENT_COEF", "0.02"))
BATCH_TIMEOUT = float(os.environ.get("BATCH_TIMEOUT", "900"))   # 单批硬超时(秒)
KEY_BONUS = [0.0, 0.10, 0.30, 1.00]

def compute_tiered_reward(r):
    floor = r["floor"]; act = r.get("act", 1); win = r.get("win", False)
    k = sum(1 for x in r.get("keys", (False, False, False)) if x)
    if win and act == 4: return 6.0
    if act == 4 or floor >= 53: return 2.0 + (floor - 52) * 0.15
    if floor >= 51: return 1.0
    return (floor / 50.0) + KEY_BONUS[k]

def worker_play(args):
    """带内部异常捕获: 任何异常都返回一个可序列化的失败结果, 不让队列挂掉"""
    import torch
    torch.set_num_threads(1)
    seed, wpath, *rest = args
    greedy = rest[0] if rest else False
    try:
        net = A.Scorer(ARCH)
        net.load_state_dict(torch.load(wpath, weights_only=True))
        net.eval()
        r = A.play_game(seed, net, train=not greedy)
        if greedy:
            r["traj"] = []
        return r
    except Exception as e:
        # 失败结果: floor=0, 空轨迹 (不会污染训练, 也不会卡死队列)
        return {"seed": seed, "floor": 0, "act": 1, "win": False, "hp": 0, "deck": 0,
                "traj": [], "keys": (False, False, False), "worker_err": f"{type(e).__name__}:{str(e)[:60]}"}

def game_loss(net, traj, adv):
    lps, ents = [], []
    temp = A.TEMPERATURE
    for o, descs, a in traj:
        raw = net.score(torch.tensor(o, dtype=torch.float32), descs)
        logp = torch.log_softmax(A.normalize_scores(raw) / temp, dim=0)
        lps.append(logp[a]); p = logp.exp(); ents.append(-(p * logp).sum())
    if not lps: return None
    return -(torch.stack(lps).sum()) * adv - ENT_COEF * torch.stack(ents).sum()

def load_warm_start(net, base_path):
    if os.path.exists(base_path):
        net.load_state_dict(torch.load(base_path, weights_only=True))
        print(f"Loaded warm-start from {os.path.basename(base_path)}!", flush=True)
    else:
        print(f"Warning: base ckpt {base_path} not found", flush=True)
