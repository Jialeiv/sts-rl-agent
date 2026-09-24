#!/usr/bin/env python3
"""Arm G V3 · 课程学习奖励 (Curriculum: 碎心导向) — B 方案
【目标切换】从"平均楼层最大化" -> "集齐3钥匙 + 打进第四幕 + 碎心"。
【B 方案奖励】(用户确认)
  - 第3把钥匙 = 跃迁奖励 (0.5 -> 2.0): "集齐"才是目标, 不是"拿几把"
  - FLOOR_W = 0.03: 让生存有意义, 消除"抢一把钥匙就自杀"的激励
  - 进第四幕/碎心: 最高奖励
【防死锁】复用 V2.8 的 batch_timeout + worker try/except。
"""
import os, sys, json, time, random, statistics, tempfile, shutil
os.environ["OMP_NUM_THREADS"] = "1"; os.environ["MKL_NUM_THREADS"] = "1"
os.environ["OPENBLAS_NUM_THREADS"] = "1"; os.environ["VECLIB_MAXIMUM_THREADS"] = "1"
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
BATCH_TIMEOUT = float(os.environ.get("BATCH_TIMEOUT", "900"))

# ---- B 方案: 课程学习奖励 (集齐跃迁) ----
KEY_BONUS = [0.0, 0.2, 0.5, 2.0]   # 第3把钥匙跃迁
FLOOR_W   = 0.03

def compute_tiered_reward(r):
    floor = r["floor"]; act = r.get("act", 1); win = r.get("win", False)
    k = sum(1 for x in r.get("keys", (False, False, False)) if x)
    if win and act == 4: return 10.0                        # 碎心
    if act >= 4: return 6.0 + max(0, floor - 53) * 0.3      # 进第四幕, 按深入程度
    return KEY_BONUS[k] + floor * FLOOR_W                    # 未进门: 钥匙(跃迁) + 生存

def worker_play(args):
    import torch; torch.set_num_threads(1)
    seed, wpath, *rest = args
    greedy = rest[0] if rest else False
    try:
        net = A.Scorer(ARCH)
        net.load_state_dict(torch.load(wpath, weights_only=True)); net.eval()
        r = A.play_game(seed, net, train=not greedy)
        if greedy: r["traj"] = []
        return r
    except Exception as e:
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
