#!/usr/bin/env python3
"""Safe warm-start/full-checkpoint continuation. Default: read-only plan; --run starts games."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib
import json
import multiprocessing as mp
import os
from pathlib import Path
import random
import statistics
import time

BASE = Path(__file__).resolve().parents[1]
FORMAL = {
    'v28': 'ckpts_CN1_V28_SAFE/step22008_r1.16_fl37.1_a4_1_h0.pt',
    'curriculum': 'ckpts_CN2_V3_CURRICULUM/step22008_r1.93_fl34.3_k3_14_a4_0_h0.pt',
}
TAIL = {'v28': 'paused-tail/CN1_V28_SAFE.pt', 'curriculum': 'paused-tail/CN2_V3_CURRICULUM.pt'}

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def source_fingerprint(base):
    paths = list((base/'training').glob('*.py'))
    for folder in ['src','include','bindings']:
        paths.extend(p for p in (base/'simulator'/folder).rglob('*') if p.is_file())
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update((str(path.relative_to(base)) + '\0' + sha(path) + '\n').encode())
    return digest.hexdigest()

def read_state(path):
    import torch
    state = torch.load(path, map_location='cpu', weights_only=True)
    if not isinstance(state, dict) or state.get('format') != 'sts-training-state-v1':
        raise ValueError('Expected a full checkpoint created by this resume tool, not a legacy weight file')
    return state

def make_plan(args, base=BASE):
    state = read_state(args.state) if args.state else None
    variant = args.variant or (state['config']['variant'] if state else 'v28')
    if state and variant != state['config']['variant']:
        raise ValueError('A full checkpoint must resume the same algorithm variant')
    model_root = (args.model_root or base/'models').expanduser().resolve()
    source = Path(args.state).expanduser().resolve() if state else (model_root / (TAIL if args.tail else FORMAL)[variant]).resolve()
    if not source.is_file(): raise FileNotFoundError(source)
    if not state:
        index = json.loads((model_root/'index.json').read_text())
        def indexed_path(entry):
            parts = Path(entry['path']).parts
            return (model_root.joinpath(*parts[1:]) if parts and parts[0] == 'models' else model_root/entry['path']).resolve()
        entry = next(e for e in index if indexed_path(e)==source)
        if sha(source) != entry['sha256']: raise ValueError('Model checksum mismatch')
    start = int(state['completed_games']) if state else (0 if args.tail else 22008)
    old = state['config'] if state else {}
    games = args.games if args.games is not None else (old['target_games']-start if state else 7992)
    if games <= 0: raise ValueError('Use --games with a positive number of additional games')
    output = (Path(args.output).expanduser() if args.output else base/'runs'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')).resolve()
    if output.exists(): raise FileExistsError('Output must be a NEW directory; existing runs are never overwritten')
    protected_roots = [model_root]
    protected_roots.extend((base/name).resolve() for name in ['models','metrics','training','simulator','video','data'])
    for protected in protected_roots:
        if output==protected or protected in output.parents: raise ValueError('Output cannot be inside archived inputs')
    config = {
        'variant':variant,'source_checkpoint':str(source),'source_sha256':sha(source),'output':str(output),
        'mode':'full_state' if state else ('warm_start_new_phase_unknown_parent_step' if args.tail else 'warm_start_formal_step'),
        'start_game':start,'target_games':start+games,'additional_games':games,
        'known_parent_last_evaluation':22008,'absolute_progress_exact':old.get('absolute_progress_exact',not args.tail),
        'optimizer_reinitialized':state is None,'seed':old.get('seed',43 if args.tail else 42),
        'workers':args.workers,'batch':old.get('batch',56),'eval_every':old.get('eval_every',500),
        'simulations':old.get('simulations',8000),'learning_rate':old.get('learning_rate',0.001),
        'batch_timeout':old.get('batch_timeout',900),'eval_seed_sha256':sha(base/'data/eval_seeds_50.txt'),
        'source_fingerprint':source_fingerprint(base),
        'per_game_worker_rng':'Python/Torch seeded by game seed; old worker RNG was not recorded',
    }
    if state and config['eval_seed_sha256'] != old['eval_seed_sha256']:
        raise ValueError('Evaluation seeds changed since checkpoint')
    if state and config['source_fingerprint'] != old['source_fingerprint']:
        raise ValueError('Training or simulator source changed since checkpoint')
    if config['workers'] < 1: raise ValueError('workers must be positive')
    return config,state

def seed_stream(count, excluded, seed):
    rng=random.Random(seed); values=[]
    while len(values)<count:
        value=rng.randint(1,10**9)
        if value not in excluded: values.append(value)
    return values

def load_algorithm(config, base=BASE):
    os.environ['STS_BOT_DIR']=str(base/'data')
    os.environ['STS_SIM_COUNT']=str(config['simulations'])
    os.environ['STS_CLASS']='DEFECT'; os.environ['ASC']='20'
    os.environ['ARM_G_ARCH']='128,128'; os.environ['ENT_COEF']='0.02'
    os.environ['STS_RESUME_VARIANT']=config['variant']
    os.environ.setdefault('STS_SIM_BUILD',str(base/'simulator/build'))
    os.environ['OMP_NUM_THREADS']='1'; os.environ['MKL_NUM_THREADS']='1'
    os.environ['OPENBLAS_NUM_THREADS']='1'
    module=importlib.import_module(config['variant'])
    import torch
    torch.set_num_threads(1)
    return module

def worker_job(args):
    import torch
    torch.set_num_threads(1)
    random.seed(args[0]); torch.manual_seed(args[0])
    return importlib.import_module(os.environ['STS_RESUME_VARIANT']).worker_play(args)

def save_state(path, net, opt, config, done, baseline, rewards, floors, next_eval):
    import torch
    state={'format':'sts-training-state-v1','model':net.state_dict(),'optimizer':opt.state_dict(),
           'config':config,'completed_games':done,'baseline':baseline,'rewards':rewards[-config['eval_every']:],
           'floors':floors[-config['eval_every']:],'next_eval':next_eval,
           'python_rng':random.getstate(),'torch_rng':torch.get_rng_state()}
    path=Path(path);tmp=path.with_name(path.name+'.tmp')
    try:
        torch.save(state,tmp)
        with tmp.open('rb') as f:os.fsync(f.fileno())
        os.replace(tmp,path)
    finally:tmp.unlink(missing_ok=True)

def run(config, state, algorithm):
    import torch
    output=Path(config['output']);net=algorithm.A.Scorer((128,128))
    weights=state['model'] if state else torch.load(config['source_checkpoint'],map_location='cpu',weights_only=True)
    net.load_state_dict(weights)
    opt=torch.optim.Adam(net.parameters(),lr=config['learning_rate'])
    if state:
        opt.load_state_dict(state['optimizer']);random.setstate(state['python_rng']);torch.set_rng_state(state['torch_rng'])
    else:random.seed(config['seed']);torch.manual_seed(config['seed'])
    eval_seeds=algorithm.A.read_seeds('eval_seeds_50.txt')
    games=seed_stream(config['target_games'],set(eval_seeds),config['seed'])
    done=config['start_game'];baseline=state['baseline'] if state else None
    rewards=list(state['rewards']) if state else [];floors=list(state['floors']) if state else []
    next_eval=state['next_eval'] if state else (done//config['eval_every']+1)*config['eval_every']
    output.mkdir(parents=True,exist_ok=False)
    (output/'run.json').write_text(json.dumps(config,ensure_ascii=False,indent=2)+'\n')
    latest=output/'latest.state.pt';wpath=output/'worker-weights.pt'
    save_state(latest,net,opt,config,done,baseline,rewards,floors,next_eval)
    pool=mp.get_context('spawn').Pool(config['workers']);started=time.time();last_evaluated=None
    def play(seeds,greedy=False):
        torch.save(net.state_dict(),wpath)
        result=pool.map_async(worker_job,[(s,str(wpath),greedy) for s in seeds]).get(timeout=config['batch_timeout'])
        failures=[r.get('worker_err') for r in result if r.get('worker_err')]
        if failures:raise RuntimeError('Worker failed; stopping at saved batch boundary: '+str(failures[0]))
        return result
    def evaluate(log):
        nonlocal next_eval,last_evaluated
        results=play(eval_seeds,True)
        row={'game':done,'phase_game':done-config['start_game'],'absolute_progress_exact':config['absolute_progress_exact'],
             'eval_r':statistics.mean(algorithm.compute_tiered_reward(r) for r in results),
             'eval_floor':statistics.mean(r['floor'] for r in results),
             'act4':sum(r.get('act',1)==4 or r['floor']>=53 for r in results),
             'win':sum(bool(r.get('win')) for r in results),
             'heart':sum(r.get('act',1)==4 and bool(r.get('win')) for r in results),
             'keys3':sum(sum(bool(k) for k in r.get('keys',()))==3 for r in results),'eval_n':len(results)}
        if rewards:row['train_r']=statistics.mean(rewards[-config['eval_every']:])
        log.write(json.dumps(row)+'\n');log.flush();print(json.dumps(row),flush=True)
        next_eval=(done//config['eval_every']+1)*config['eval_every']
        last_evaluated=done
        save_state(output/f'step-{done}.state.pt',net,opt,config,done,baseline,rewards,floors,next_eval)
        save_state(latest,net,opt,config,done,baseline,rewards,floors,next_eval)
    try:
        with (output/'metrics.jsonl').open('x') as log:
            if done>=next_eval:evaluate(log)
            for offset in range(done,len(games),config['batch']):
                results=play(games[offset:offset+config['batch']]);opt.zero_grad();losses=[]
                for r in results:
                    reward=algorithm.compute_tiered_reward(r)
                    baseline=reward if baseline is None else .99*baseline+.01*reward
                    loss=algorithm.game_loss(net,r['traj'],reward-baseline)
                    if loss is not None:losses.append(loss)
                    rewards.append(reward);floors.append(r['floor']);done+=1
                if losses:
                    torch.stack(losses).mean().backward();torch.nn.utils.clip_grad_norm_(net.parameters(),5.0);opt.step()
                save_state(latest,net,opt,config,done,baseline,rewards,floors,next_eval)
                if done>=next_eval:evaluate(log)
            if done!=last_evaluated:evaluate(log)
        print(json.dumps({'completed_games':done,'seconds':round(time.time()-started,1),'checkpoint':str(latest)}))
    finally:
        pool.terminate();pool.join();wpath.unlink(missing_ok=True)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--variant',choices=['v28','curriculum'])
    group=p.add_mutually_exclusive_group();group.add_argument('--tail',action='store_true');group.add_argument('--state',type=Path)
    p.add_argument('--model-root',type=Path,help='Directory containing index.json and checkpoint folders (default: repository models/)')
    p.add_argument('--games',type=int,help='Additional games; default continues the original target or adds 7992 for a warm start')
    p.add_argument('--workers',type=int,default=min(4,os.cpu_count() or 1))
    p.add_argument('--output',type=Path)
    p.add_argument('--check-model',action='store_true',help='Load model and validate tensor shapes, without creating a run or playing games')
    p.add_argument('--run',action='store_true',help='Explicitly start training; omission is read-only')
    args=p.parse_args();config,state=make_plan(args);print(json.dumps(config,ensure_ascii=False,indent=2))
    if not args.run and not args.check_model:return
    algorithm=load_algorithm(config)
    if args.check_model and not args.run:
        import torch
        net=algorithm.A.Scorer((128,128));weights=state['model'] if state else torch.load(config['source_checkpoint'],map_location='cpu',weights_only=True)
        net.load_state_dict(weights)
        print(json.dumps({'model_load':'PASS','parameters':sum(p.numel() for p in net.parameters()),'training_started':False}));return
    run(config,state,algorithm)

if __name__=='__main__':main()
