import datetime, hashlib, json, statistics
from pathlib import Path

ROOT = Path(__file__).parent
RUNS = ROOT / 'av1-launch-footprint-away-runs-20261021'
def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
plan = load(RUNS / 'plan.json')
index = load(RUNS / 'index.json')
reasons, rows = [], []
if index['status'] != 'complete' or len(index['calls']) != len(plan['calls']):
    reasons.append('Full planned eight-call batch is incomplete')
for expected, actual in zip(plan['calls'], index['calls']):
    if any(actual[k] != expected[k] for k in ['sequence', 'pair', 'phase', 'mode', 'excludeFromMeasuredSummary']):
        reasons.append('Call identity/order mismatch')
    if actual['status'] != 'validated':
        reasons.append('Failed call ' + str(actual['sequence']))
        continue
    path = Path(actual['output']) / 'comparison.json'
    assert path.resolve().is_relative_to(RUNS.resolve())
    assert sha(path).lower() == actual['comparisonSha256'].lower()
    report = load(path)
    assert report['launchMode'] == actual['mode']
    assert report['mediaSha256'].lower() == plan['mediaSha256'].lower()
    assert report['apertureStatusProbeSha256'].lower() == plan['statusProbeSha256'].lower()
    assert len(report['players']) == len(report['results']) == 1
    assert report['players'][0]['runtimeManifestSha256'].lower() == plan['runtimeManifestSha256'].lower()
    run = report['results'][0]
    assert run['executableHash'].lower() == plan['executableSha256'].lower()
    assert run['status'] == 'sampled' and run['playbackVerification'] == 'advancing-playback-verified'
    assert run['cleanup'] == 'owned-window-closed'
    assert run['videoCanvas']['clientWidth'] == 1280 and run['videoCanvas']['clientHeight'] == 720
    assert run['alignedPlaybackPosition'] >= 20.5 and len(run['samples']) == 7
    for sample in run['samples']:
        for key in ['visibleCanvasBefore', 'visibleCanvasAfter']:
            assert sample[key]['status'] == 'validated' and sample[key]['safe']
        assert sample['privateBytes'] > 0 and sample['workingSetBytes'] > 0
    positions = [s['playback']['positionSec'] for s in run['samples']]
    assert all(b > a for a, b in zip(positions, positions[1:]))
    assert positions[-1] - positions[0] >= 28
    metrics = {}
    for key in ['privateBytes', 'workingSetBytes', 'dedicatedGpuBytes', 'sharedGpuBytes']:
        values = [s.get(key) for s in run['samples']]
        metrics[key] = dict(mean=statistics.mean(values), minimum=min(values), maximum=max(values)) if all(isinstance(v, (int, float)) for v in values) else None
    rows.append(dict(sequence=actual['sequence'], pair=actual['pair'], mode=actual['mode'], warmup=actual['excludeFromMeasuredSummary'], pid=run['pid'], startedUtc=run['startedUtc'], samplingStartedUtc=run['samplingStartedUtc'], samplingEndedUtc=run['samplingEndedUtc'], canvas=run['videoCanvas'], comparisonSha256=sha(path), metrics=metrics))
measured = [r for r in rows if not r['warmup']]
if len(measured) != 6 or any(sum(r['mode'] == mode for r in measured) != 3 for mode in ['Direct', 'HubThenOpen']):
    reasons.append('Not three valid measured calls per mode')
if len({r['canvas']['dpi'] for r in rows}) != 1:
    reasons.append('DPI differs')
summary, pairs = {}, []
if not reasons:
    for mode in ['Direct', 'HubThenOpen']:
        selected = [r for r in measured if r['mode'] == mode]
        summary[mode] = {}
        for key in selected[0]['metrics']:
            values = [r['metrics'][key]['mean'] for r in selected if r['metrics'][key] is not None]
            summary[mode][key] = dict(meanOfRunMeans=statistics.mean(values), minRunMean=min(values), maxRunMean=max(values), runs=len(values)) if len(values) == 3 else None
    for pair in [1, 2, 3]:
        selected = {r['mode']:r for r in measured if r['pair'] == pair}
        delta = {}
        for key in selected['Direct']['metrics']:
            direct = selected['Direct']['metrics'][key]
            hub = selected['HubThenOpen']['metrics'][key]
            delta[key] = hub['mean'] - direct['mean'] if direct is not None and hub is not None else None
        pairs.append(dict(pair=pair, hubMinusDirectBytes=delta))
result = dict(recordedAtUtc=datetime.datetime.now(datetime.timezone.utc).isoformat(), qualified=not reasons, reasons=reasons, sourceContentCommit=plan['sourceContentCommit'], runtimeManifestSha256=plan['runtimeManifestSha256'], executableSha256=plan['executableSha256'], planSha256=sha(RUNS/'plan.json'), indexSha256=sha(RUNS/'index.json'), rows=rows, summaries=summary, pairs=pairs, limits=plan['limits'] + ['Measured memory deltas summarize seven guarded snapshots per run and three fresh measured processes per mode. GPU metrics overlap with process figures and are not additive. No CPU, VLC, unique-content, audio or causal D3D-allocation claim.'])
out = ROOT / 'av1-launch-footprint-away-analysis-20261021.json'
assert not out.exists()
out.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
print(json.dumps({k:result[k] for k in ['qualified','reasons','summaries','pairs']}, indent=2))
