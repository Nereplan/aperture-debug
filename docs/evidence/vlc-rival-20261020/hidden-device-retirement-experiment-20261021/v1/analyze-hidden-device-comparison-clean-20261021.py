import datetime, hashlib, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).parent
def load(p): return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
plan = load(ROOT/'hidden-device-comparison-clean-plan-20261021.json')
path = ROOT/'hidden-device-comparison-clean-runs-20261021/comparison.json'
report = load(path)
for item in plan['inputs']:
    assert sha(Path(item['path'])) == item['sha256']
assert hashlib.sha256(subprocess.check_output(['git','-C',plan['candidateWorktree'],'diff','--binary'])).hexdigest() == plan['candidatePatchSha256']
assert report['mediaSha256'].lower() == '84630e2bec31bbda663d634e2a7fc13cf1c81f983565d3942094e8dc9808fdd8'
assert report['launchMode'] == 'HubThenOpen' and report['hubSettleSeconds'] == 3
assert report['iterations'] == 3 and report['warmupIterations'] == 1
players = {p['variant']:p for p in report['players']}
assert set(players) == {'stock601','retired-device'}
expected_hashes = {'stock601':plan['baselineExecutableSha256'], 'retired-device':plan['candidateExecutableSha256']}
reasons, rows = [], []
if len(report['results']) != 8: reasons.append('Incomplete eight-process batch')
for row in report['results']:
    variant = row['variant']
    assert variant in players
    assert row['executableHash'].lower() == expected_hashes[variant].lower()
    assert row['runtimeManifestSha256'].lower() == players[variant]['runtimeManifestSha256'].lower()
    failures = []
    if row['status'] != 'sampled' or row['playbackVerification'] != 'advancing-playback-verified': failures.append('Playback sampling failed')
    if row['cleanup'] != 'owned-window-closed': failures.append('Owned window did not close normally')
    if len(row.get('samples',[])) != 7: failures.append('Not seven samples')
    if row.get('alignedPlaybackPosition',0) < 20.5: failures.append('Media alignment failed')
    canvas = row.get('videoCanvas',{})
    if canvas.get('clientWidth') != 1280 or canvas.get('clientHeight') != 720: failures.append('Actual canvas differs')
    for sample in row.get('samples',[]):
        for key in ['visibleCanvasBefore','visibleCanvasAfter']:
            if sample.get(key,{}).get('status') != 'validated' or not sample[key].get('safe'): failures.append('Visibility rejected')
    positions = [s['playback']['positionSec'] for s in row.get('samples',[])]
    if len(positions) < 2 or not all(b>a for a,b in zip(positions,positions[1:])) or positions[-1]-positions[0] < 28: failures.append('Playback progression failed')
    metrics = {}
    for key in ['privateBytes','workingSetBytes','dedicatedGpuBytes','sharedGpuBytes']:
        values = [s.get(key) for s in row.get('samples',[])]
        metrics[key] = dict(mean=statistics.mean(values), minimum=min(values), maximum=max(values)) if len(values)==7 and all(isinstance(v,(int,float)) for v in values) else None
    reasons += [variant+' round'+str(row['round'])+': '+s for s in set(failures)]
    rows.append(dict(variant=variant,round=row['round'],warmup=row['warmup'],pid=row['pid'],canvas=canvas,startedUtc=row['startedUtc'],samplingStartedUtc=row.get('samplingStartedUtc'),samplingEndedUtc=row.get('samplingEndedUtc'),metrics=metrics))
for variant in players:
    selected=[r for r in rows if r['variant']==variant]
    if {r['round'] for r in selected} != {0,1,2,3} or len(selected)!=4 or sum(r['warmup'] for r in selected)!=1: reasons.append('Incomplete rounds for '+variant)
if len({r['canvas'].get('dpi') for r in rows}) != 1: reasons.append('DPI differs')
summary,pairs={},[]
if not reasons:
    for variant in players:
        selected=[r for r in rows if r['variant']==variant and not r['warmup']]
        summary[variant]={}
        for key in selected[0]['metrics']:
            values=[r['metrics'][key]['mean'] for r in selected if r['metrics'][key] is not None]
            summary[variant][key]=dict(meanOfRunMeans=statistics.mean(values),minRunMean=min(values),maxRunMean=max(values),runs=3) if len(values)==3 else None
    for round_number in [1,2,3]:
        selected={r['variant']:r for r in rows if r['round']==round_number}
        deltas={}
        for key in selected['stock601']['metrics']:
            old,new=selected['stock601']['metrics'][key],selected['retired-device']['metrics'][key]
            deltas[key]=new['mean']-old['mean'] if old is not None and new is not None else None
        pairs.append(dict(round=round_number,candidateMinusStockBytes=deltas))
result=dict(recordedAtUtc=datetime.datetime.now(datetime.timezone.utc).isoformat(),qualified=not reasons,reasons=reasons,baseSourceCommit=plan['baseSourceCommit'],candidateSourceDirty=True,candidatePatchSha256=plan['candidatePatchSha256'],planSha256=sha(ROOT/'hidden-device-comparison-clean-plan-20261021.json'),comparisonSha256=sha(path),aggregation='Mean of seven guarded snapshots per run, mean across three fresh measured runs; all run-mean ranges and paired differences retained. Chosen before first duration/memory analysis; no significance test or overall pass threshold.',rows=rows,summaries=summary,pairs=pairs,limits=['Isolated prototype not installed; source patch and nine byte-identical non-EXE payloads retained. No CPU or VLC ranking.','Resource account figures overlap and are not additive. Three repeats and ranges do not establish a confidence interval.','Sampling begins after20.5s and does not characterize the15s retirement transition, presentation cadence, audio continuity or return-to-library visual/latency behavior. Those require independent functional checks before adoption.'])
out=ROOT/'hidden-device-comparison-clean-analysis-20261021.json';assert not out.exists();out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps({k:result[k] for k in ['qualified','reasons','summaries','pairs']},indent=2))
