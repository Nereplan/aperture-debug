import datetime, hashlib, json, statistics
from pathlib import Path

root = Path(__file__).parent
def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
plan_path = root/'installed-retirement-comparison-plan-20261021.json'
report_path = root/'installed-retirement-comparison-runs-20261021/comparison.json'
output = root/'installed-retirement-comparison-analysis-20261021.json'
assert not output.exists(), 'Retained analysis must not be overwritten'
plan, report = read(plan_path), read(report_path)
for item in plan['inputs']:
    assert sha(Path(item['path'])) == item['sha256'], item['path']
assert plan['candidateSourceDirty'] is False
assert report['mediaSha256'].lower() == '84630e2bec31bbda663d634e2a7fc13cf1c81f983565d3942094e8dc9808fdd8'
assert report['launchMode'] == 'HubThenOpen' and report['hubSettleSeconds'] == 3
assert report['iterations'] == 3 and report['warmupIterations'] == 1
assert report['videoCanvasTarget'] == [1280,720]
assert report['apertureStatusProbeSha256'].lower() == 'ffffa47050c6a3b04f34c7f26228fa01981345a3321725ddcd2d6676c09a0ec3'
prepared = datetime.datetime.fromisoformat(plan['preparedAtUtc'].replace('Z','+00:00'))
assert datetime.datetime.fromisoformat(report['createdUtc'].replace('Z','+00:00')) >= prepared
inputs = {Path(item['path']).name:item['sha256'] for item in plan['inputs']}
for name, digest in report['harnessSourceSha256'].items():
    assert inputs[name].lower() == digest.lower(), name
previous = read(root/'hidden-device-comparison-clean-runs-20261021/comparison.json')['environment']
for key in ['osVersion','cpuName','logicalProcessors','gpuName','gpuDriverVersion']:
    assert report['environment'][key] == previous[key], key
def timestamp(value): return datetime.datetime.fromisoformat(value.replace('Z','+00:00'))
assert abs((timestamp(report['environment']['lastBootUpTimeUtc'])-timestamp(previous['lastBootUpTimeUtc'])).total_seconds()) <= 5
players = {p['variant']:p for p in report['players']}
expected = {'stock601':plan['baselineExecutableSha256'],'installed-retirement':plan['candidateExecutableSha256']}
assert set(players) == set(expected)
for variant, player in players.items():
    assert player['exeHash'].lower() == expected[variant].lower()
    entries = player['runtimeFiles']
    assert len(entries) == player['runtimeFileCount'] and sum(e['bytes'] for e in entries) == player['runtimeBytes']
    assert len({e['path'] for e in entries}) == len(entries)
    lines = []
    for entry in sorted(entries,key=lambda e:e['path']):
        path = Path(player['source'])/entry['path']
        assert path.stat().st_size == entry['bytes'] and sha(path) == entry['sha256'].lower(), str(path)
        lines.append(f"{entry['path']}\t{entry['bytes']}\t{entry['sha256'].lower()}\n")
    assert hashlib.sha256(''.join(lines).encode('utf-8')).hexdigest() == player['runtimeManifestSha256'].lower()
reasons, rows = [], []
if len(report['results']) != 8: reasons.append('Incomplete eight-process batch')
metrics = ['privateBytes','workingSetBytes','dedicatedGpuBytes','sharedGpuBytes']
for run in report['results']:
    variant = run['variant']
    assert variant in players
    assert run['executableHash'].lower() == expected[variant].lower()
    assert run['runtimeManifestSha256'].lower() == players[variant]['runtimeManifestSha256'].lower()
    assert run['runtimeFileCount'] == players[variant]['runtimeFileCount'] and run['runtimeBytes'] == players[variant]['runtimeBytes']
    assert timestamp(run['startedUtc']) >= prepared
    failures = []
    if run['status'] != 'sampled' or run['playbackVerification'] != 'advancing-playback-verified': failures.append('Playback sampling failed')
    if run['cleanup'] != 'owned-window-closed': failures.append('Owned window did not close normally')
    samples = run.get('samples',[])
    if len(samples) != 7: failures.append('Not seven snapshots')
    if run.get('alignedPlaybackPosition',0) < 20.5: failures.append('Media alignment failed')
    canvas = run.get('videoCanvas',{})
    if canvas.get('clientWidth') != 1280 or canvas.get('clientHeight') != 720: failures.append('Actual canvas differs')
    for sample in samples:
        for key in ['visibleCanvasBefore','visibleCanvasAfter']:
            if sample.get(key,{}).get('status') != 'validated' or not sample[key].get('safe'): failures.append('Visibility rejected')
    positions = [sample['playback']['positionSec'] for sample in samples]
    if len(positions) < 2 or not all(b>a for a,b in zip(positions,positions[1:])) or positions[-1]-positions[0] < 28: failures.append('Playback progression failed')
    values_by_metric = {}
    for metric in metrics:
        values = [sample.get(metric) for sample in samples]
        if len(values) == 7 and all(isinstance(value,(int,float)) and value >= 0 for value in values):
            values_by_metric[metric] = dict(mean=statistics.mean(values),minimum=min(values),maximum=max(values))
        else: failures.append('Missing/invalid '+metric)
    reasons.extend(f'{variant} round{run["round"]}: {reason}' for reason in sorted(set(failures)))
    rows.append(dict(variant=variant,round=run['round'],warmup=run['warmup'],pid=run['pid'],canvas=canvas,startedUtc=run['startedUtc'],samplingStartedUtc=run.get('samplingStartedUtc'),samplingEndedUtc=run.get('samplingEndedUtc'),metrics=values_by_metric))
for variant in players:
    selected = [row for row in rows if row['variant']==variant]
    if len(selected)!=4 or {row['round'] for row in selected}!={0,1,2,3} or sum(row['warmup'] for row in selected)!=1 or any(row['warmup'] != (row['round']==0) for row in selected): reasons.append('Incomplete rounds for '+variant)
if len({run['runDirectory'] for run in report['results']}) != len(rows): reasons.append('Run directory reused within batch')
if any(row['canvas'].get('dpi') != 96 for row in rows): reasons.append('DPI differs from96')
summaries, pairs = {}, []
if not reasons:
    for variant in players:
        selected = [row for row in rows if row['variant']==variant and not row['warmup']]
        summaries[variant] = {}
        for metric in metrics:
            values = [row['metrics'][metric]['mean'] for row in selected]
            summaries[variant][metric] = dict(meanOfRunMeans=statistics.mean(values),minRunMean=min(values),maxRunMean=max(values),runs=3)
    for number in [1,2,3]:
        selected = {row['variant']:row for row in rows if row['round']==number}
        pairs.append(dict(round=number,candidateMinusStockBytes={metric:selected['installed-retirement']['metrics'][metric]['mean']-selected['stock601']['metrics'][metric]['mean'] for metric in metrics}))
result = dict(recordedAtUtc=datetime.datetime.now(datetime.timezone.utc).isoformat(),qualified=not reasons,reasons=reasons,
    baselineSourceCommit=plan['baselineSourceCommit'],candidateSourceCommit=plan['candidateSourceCommit'],candidateSourceDirty=False,
    planSha256=sha(plan_path),comparisonSha256=sha(report_path),analyzerSha256=sha(Path(__file__)),environment=report['environment'],
    bootTimestampToleranceSeconds=5,aggregation=plan['aggregation'],rows=rows,summaries=summaries,pairs=pairs,
    limits=['Only this installed-source versus stock601 AV1 resource batch; no CPU or VLC ranking, cadence/audio/drop, picture-quality equivalence or statistical significance claim.',
            'Memory accounts overlap and cannot be added. Three run means and their ranges are observations, not confidence intervals.',
            'Sampling after20.5s excludes the15s retirement transition; separate return correctness checks do not measure panel latency.',
            'Source/staged-runtime identity relies on the presealed harness enforcing its recorded inventory; staged directories are cleaned after runs.'])
output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps({key:result[key] for key in ['qualified','reasons','summaries','pairs']},indent=2))
