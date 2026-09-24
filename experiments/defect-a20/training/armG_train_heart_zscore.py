#!/usr/bin/env python3
"""Arm G Heart (Transformer-style Z-score Normalized Logits):
使用标准正态分布对每一轮候选动作的得分进行 Z-score 归一化 (scores - mean) / (std + eps),
彻底消除尺度漂移与 Softmax 塌缩，保证梯度在全域平稳流动。
"""
import os, sys, json, re
from pathlib import Path
SB = os.environ.get("STS_BOT_DIR", str(Path(__file__).resolve().parents[1] / "data"))
sys.path.insert(0, os.environ.get("STS_SIM_BUILD", str(Path(__file__).resolve().parents[1] / "simulator" / "build")))
import slaythespire as sts
import torch, torch.nn as nn

CLS = getattr(sts.CharacterClass, os.environ.get("STS_CLASS", "DEFECT"))
_nn = sts.getNNInterface()
VOCAB_CAP = int(_nn.card_slot_count)

def card_name(c):
    m = re.search(r"Card (.+?)>", str(c))
    return m.group(1) if m else str(c)

def card_idx(c):
    i = _nn.getCardIdx(c)
    return i if 0 <= i < VOCAB_CAP else VOCAB_CAP - 1

try:
    _maxes = [max(1.0, float(x)) for x in _nn.getObservationMaximums()]
except Exception:
    _maxes = None

def obs_vec(gc):
    o = list(_nn.getObservation(gc))
    if _maxes and len(_maxes) == len(o):
        o = [a / b for a, b in zip(o, _maxes)]
    o.extend([float(gc.red_key), float(gc.green_key), float(gc.blue_key)])
    return o

BASE_OBS_DIM = len(_nn.getObservation(sts.GameContext(CLS, 1, 0)))
OBS_DIM = BASE_OBS_DIM + 3

DT_MAP, DT_REST, DT_SHOP, DT_EVENT, DT_CARD = 0, 1, 2, 3, 4
ROOM_IDX = {sts.Room.MONSTER: 0, sts.Room.ELITE: 1, sts.Room.REST: 2,
            sts.Room.SHOP: 3, sts.Room.EVENT: 4, sts.Room.TREASURE: 5, sts.Room.BOSS: 6}
SITEM_IDX = {sts.RewardsActionType.CARD: 0, sts.RewardsActionType.RELIC: 1,
             sts.RewardsActionType.POTION: 2, sts.RewardsActionType.CARD_REMOVE: 3,
             sts.RewardsActionType.SKIP: 4}
EVENT_CAP = 64
EOPT_CAP = 8

OFF_DTYPE = 0;                 W_DTYPE = 5
OFF_CARD  = OFF_DTYPE + W_DTYPE;  W_CARD = VOCAB_CAP
OFF_MROOM = OFF_CARD + W_CARD;    W_MROOM = 7
OFF_MLA1  = OFF_MROOM + W_MROOM;  W_MLA1 = 7
OFF_MLA2  = OFF_MLA1 + W_MLA1;    W_MLA2 = 7
OFF_REST  = OFF_MLA2 + W_MLA2;    W_REST = 7
OFF_SITEM = OFF_REST + W_REST;    W_SITEM = 5
OFF_SPRICE= OFF_SITEM + W_SITEM;  W_SPRICE = 1
OFF_EVID  = OFF_SPRICE + W_SPRICE; W_EVID = EVENT_CAP
OFF_EOPT  = OFF_EVID + W_EVID;    W_EOPT = EOPT_CAP
OFF_PASS  = OFF_EOPT + W_EOPT;    W_PASS = 1
DESC_DIM  = OFF_PASS + W_PASS
INPUT_DIM = OBS_DIM + DESC_DIM
GOLD_MAX  = 999.0

def _blank():
    return [0.0] * DESC_DIM

def _room_counts(gc, x, y, vec, off):
    if y < 0 or y > 13: return []
    kids = gc.map_node_children(x, y)
    for cx in kids:
        ri = ROOM_IDX.get(gc.map_node_room(cx, y + 1))
        if ri is not None: vec[off + ri] += 1.0
    return kids

def build_choices(gc):
    ss = gc.screen_state
    descs, execs = [], []

    if ss == sts.ScreenState.REWARDS:
        acts = sts.get_legal_game_actions(gc)
        if not acts: return ("reward_empty", [], [])
        has_cards = any(a.rewards_action_type == sts.RewardsActionType.CARD for a in acts)
        cards = gc.get_card_reward() if has_cards else []
        for a in acts:
            d = _blank(); d[OFF_DTYPE + DT_CARD] = 1.0
            t = a.rewards_action_type
            if t == sts.RewardsActionType.CARD:
                if cards and 0 <= a.idx2 < len(cards):
                    d[OFF_CARD + card_idx(cards[a.idx2])] = 1.0
                d[OFF_SITEM + 0] = 1.0
            elif t == sts.RewardsActionType.GOLD:
                d[OFF_SITEM + 0] = 1.0; d[OFF_SPRICE] = 1.0
            elif t == sts.RewardsActionType.RELIC:
                d[OFF_SITEM + 1] = 1.0
            elif t == sts.RewardsActionType.POTION:
                d[OFF_SITEM + 2] = 1.0
            elif t == sts.RewardsActionType.KEY:
                d[OFF_SITEM + 3] = 1.0
            elif t == sts.RewardsActionType.SKIP:
                d[OFF_PASS] = 1.0
            descs.append(d)
            execs.append((lambda act: lambda g: act.execute(g))(a))
        return ("reward", descs, execs)

    acts = sts.get_legal_game_actions(gc)
    if not acts: return (str(ss), [], [])

    if ss == sts.ScreenState.MAP_SCREEN:
        cy = gc.cur_map_node_y
        for a in acts:
            dx, dy = a.idx1, cy + 1
            d = _blank(); d[OFF_DTYPE + DT_MAP] = 1.0
            ri = ROOM_IDX.get(gc.map_node_room(dx, dy))
            if ri is not None: d[OFF_MROOM + ri] = 1.0
            elif dy >= 15: d[OFF_MROOM + ROOM_IDX[sts.Room.BOSS]] = 1.0
            if dx == gc.burning_elite_x and dy == gc.burning_elite_y:
                d[OFF_PASS] = 1.0
            kids = _room_counts(gc, dx, dy, d, OFF_MLA1)
            for cx in kids: _room_counts(gc, cx, dy + 1, d, OFF_MLA2)
            descs.append(d)
        execs = [(lambda aa: (lambda g: aa.execute(g)))(a) for a in acts]
        return ("map", descs, execs)

    if ss == sts.ScreenState.REST_ROOM:
        for a in acts:
            d = _blank(); d[OFF_DTYPE + DT_REST] = 1.0
            if 0 <= a.idx1 < W_REST: d[OFF_REST + a.idx1] = 1.0
            descs.append(d)
        execs = [(lambda aa: (lambda g: aa.execute(g)))(a) for a in acts]
        return ("rest", descs, execs)

    if ss == sts.ScreenState.SHOP_ROOM:
        shop_cards = gc.get_shop_cards()
        for a in acts:
            d = _blank(); d[OFF_DTYPE + DT_SHOP] = 1.0
            t = a.rewards_action_type
            if t in SITEM_IDX: d[OFF_SITEM + SITEM_IDX[t]] = 1.0
            if t == sts.RewardsActionType.SKIP: d[OFF_PASS] = 1.0
            if t == sts.RewardsActionType.CARD and 0 <= a.idx1 < len(shop_cards):
                card, price = shop_cards[a.idx1]
                d[OFF_CARD + card_idx(card)] = 1.0
                d[OFF_SPRICE] = min(price, GOLD_MAX) / GOLD_MAX if price and price > 0 else 0.0
            descs.append(d)
        execs = [(lambda aa: (lambda g: aa.execute(g)))(a) for a in acts]
        return ("shop", descs, execs)

    if ss == sts.ScreenState.EVENT_SCREEN:
        ev_idx = max(0, min(EVENT_CAP - 1, int(gc.cur_event)))
        for i, a in enumerate(acts):
            d = _blank(); d[OFF_DTYPE + DT_EVENT] = 1.0
            d[OFF_EVID + ev_idx] = 1.0
            d[OFF_EOPT + min(i, EOPT_CAP - 1)] = 1.0
            descs.append(d)
        execs = [(lambda aa: (lambda g: aa.execute(g)))(a) for a in acts]
        return ("event", descs, execs)

    return (str(ss), [], [])

def normalize_scores(scores):
    """Transformer 风格的 Z-score 局部正态分布归一化"""
    if len(scores) > 1:
        std = torch.std(scores, unbiased=False)
        mean = torch.mean(scores)
        if std > 1e-5:
            return (scores - mean) / (std + 1e-5)
    return scores

class Scorer(nn.Module):
    def __init__(self, arch=(128, 128)):
        super().__init__()
        layers, prev = [], INPUT_DIM
        for h in arch:
            layers += [nn.Linear(prev, h), nn.ReLU()]; prev = h
        layers += [nn.Linear(prev, 1)]
        self.net = nn.Sequential(*layers)
    def score(self, o, descs):
        rows = torch.stack([torch.cat([o, torch.tensor(d, dtype=torch.float32)]) for d in descs])
        return self.net(rows).squeeze(-1)

SIMCOUNT = int(os.environ.get("STS_SIM_COUNT", "2000"))
ASC = int(os.environ.get("ASC", "20"))
TEMPERATURE = float(os.environ.get("STS_TRAIN_TEMPERATURE", "1.0"))

def play_game(seed, net, train=True, max_steps=600):
    gc = sts.GameContext(CLS, seed, ASC)
    ag = sts.Agent(); ag.simulation_count_base = SIMCOUNT
    ag.pause_on_rewards = True
    ag.pause_on_card_reward = False
    ag.pause_on_map = ag.pause_on_rest = ag.pause_on_shop = ag.pause_on_event = True
    traj = []; steps = 0
    while gc.outcome == sts.GameOutcome.UNDECIDED and steps < max_steps:
        steps += 1
        ag.playout(gc)
        if gc.outcome != sts.GameOutcome.UNDECIDED: break
        kind, descs, execs = build_choices(gc)
        if not descs:
            if gc.screen_state == sts.ScreenState.REWARDS:
                acts = sts.get_legal_game_actions(gc)
                if acts: acts[-1].execute(gc); continue
            break
        if len(descs) == 1:
            execs[0](gc); continue
        o = obs_vec(gc)
        with torch.no_grad():
            raw_scores = net.score(torch.tensor(o, dtype=torch.float32), descs)
            norm_scores = normalize_scores(raw_scores)
            if train:
                probs = torch.softmax(norm_scores / TEMPERATURE, dim=0)
                a = torch.multinomial(probs, 1).item()
                traj.append((o, descs, a))
            else:
                probs = torch.softmax(norm_scores, dim=0)
                a = int(torch.argmax(probs).item())
        execs[a](gc)
    win = gc.outcome == sts.GameOutcome.PLAYER_VICTORY
    return {"seed": seed, "floor": gc.floor_num, "act": gc.act, "win": win,
            "hp": gc.cur_hp, "deck": len(gc.deck), "traj": traj,
            "keys": (gc.red_key, gc.green_key, gc.blue_key)}

def read_seeds(fn):
    return [int(x) for x in open(os.path.join(SB, fn)) if x.strip() and not x.startswith("#")]
