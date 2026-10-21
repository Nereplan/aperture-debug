import argparse,collections,csv,datetime,hashlib,io,json,math,statistics
from pathlib import Path
ROOT=Path(r'C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019')
EXPECTED_SOURCE='23fd026c2b114f8391465bfb5f7d28a79da02025'
EXPECTED_RUNTIME={
    'Aperture':('8a333e146c3153f0e00a350323a0402c265be4320fbc8bf08f70ac9530005ac8','78b59f4e3e98e3f5f7a3e3a309c522f3ce1828e3f58e22c1a85a12c008fa9a7f'),
    'VLC':('a48412d2ee742a32192cf0c8389d2251c180160c2bc615492044d1a73aa5b2ff','63d0d02df6e466d642b824dc66be6b5a134bf98b26747ffac8ec0d879dc180d7')}
EXPECTED_MEDIA={'h264':'86c249a196158ca2479bb9dd6e0045f24cbcbd2e04a80a41e37c2f6b78f70e68','hevc':'05c53f25a63790bb4f6b5d9b83b8818c405e35c6dbf44fa4a65d62b29d947161'}
def load_json(path): return json.loads(path.read_text(encoding='utf-8-sig'))
def utc(value): return datetime.datetime.fromisoformat(value.replace('Z','+00:00'))
p=argparse.ArgumentParser();p.add_argument('--label',default='v2-clear');p.add_argument('--final',action='store_true');args=p.parse_args()
assert args.label and all(c in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in args.label),'Invalid capture label'
collector=ROOT/f'cadence-collector-{args.label}-20261021';runsdir=ROOT/f'cadence-runs-{args.label}-20261021'
identity=load_json(runsdir/'identity.json');started=load_json(collector/'started.json')
finished=load_json(collector/'finished.json') if (collector/'finished.json').exists() else None
plan_path=ROOT/f'cadence-analysis-plan-{args.label}-20261021.json';plan_bytes=plan_path.read_bytes();plan=load_json(plan_path)
batch_reasons=[]
if (Path(identity['analysisPlanPath'])!=plan_path or identity['analysisPlanSha256'].lower()!=hashlib.sha256(plan_bytes).hexdigest()
        or {k:v for k,v in identity['analysisPlan'].items() if k!='recordedAtUtc'}!={k:v for k,v in plan.items() if k!='recordedAtUtc'}
        or utc(identity['analysisPlan']['recordedAtUtc'])!=utc(plan['recordedAtUtc']) or utc(plan['recordedAtUtc'])>utc(started['startedUtc'])):
    batch_reasons.append('Predeclared plan identity/content/time does not match this batch')
if plan['collector']!=collector.name or plan['runs']!=runsdir.name or plan['nominalFps']!={'h264':30,'hevc':60}:
    batch_reasons.append('Plan targets or nominal frame rates do not match this capture')
if identity.get('sourceCommit')!=EXPECTED_SOURCE or identity.get('sourceProductionDirty') is not False:
    batch_reasons.append('Frozen source identity changed')
for player,(runtime_hash,exe_hash) in EXPECTED_RUNTIME.items():
    manifest=identity.get('runtimeManifests',{}).get(player,{})
    executable=[f for f in manifest.get('files',[]) if f['path'].lower()==('aperture.exe' if player=='Aperture' else 'vlc.exe')]
    if manifest.get('sha256','').lower()!=runtime_hash or len(executable)!=1 or executable[0]['sha256'].lower()!=exe_hash:
        batch_reasons.append(f'{player} frozen batch runtime/executable identity changed')
for key in ['controlNonce','sessionName','presentMonPid','collectorPid']:
    if identity.get('collector',{}).get(key)!=started.get(key) or (finished and finished.get(key)!=started.get(key)):
        batch_reasons.append(f'Collector/coordinator binding changed: {key}')
if args.final:
    assert finished and finished.get('processExited') and not finished.get('streamError'),'Final analysis requires a closed, drained collector'
    if identity.get('coordinatorStatus')!='completed': batch_reasons.append('Coordinator did not complete the full planned batch')
    if not finished.get('captureQualified'): batch_reasons.append('Collector did not qualify this complete capture')
raw=(collector/'presents.csv').read_bytes()
snapshot_bytes=len(raw)
if args.final: assert raw.endswith(b'\n'),'Final CSV lacks complete last line'
else: raw=raw[:raw.rfind(b'\n')+1]
reader=csv.DictReader(io.StringIO(raw.decode('utf-8-sig')),strict=True)
fields=reader.fieldnames
required=['Application','ProcessID','SwapChainAddress','CPUStartQPC','DisplayedTime','VidPnSourceId','LayerIndex','PresentId']
assert fields and len(fields)==len(set(fields)) and all(f in fields for f in required),'Missing or duplicate CSV columns'
by=collections.defaultdict(list);rowcount=0
for row in reader:
    assert None not in row and all(v is not None for v in row.values()),'CSV contamination or incomplete row'
    pid=int(row['ProcessID']);assert pid>0 and int(row['CPUStartQPC'])>0,'Invalid event identity/time'
    assert row['Application'] in ['Aperture.exe','vlc.exe'],'Unexpected application in collected CSV'
    by[pid].append(row);rowcount+=1
records=[];read_errors=[]
for path in sorted(runsdir.glob('*/run.json')):
    try: records.append(json.loads(path.read_text(encoding='utf-8-sig')))
    except Exception as e:
        if args.final: raise
        read_errors.append(str(e))
records.sort(key=lambda r:r['startedUtc'])
bound_records=sorted(identity.get('runs',[]),key=lambda r:r['startedUtc'])
if records!=bound_records: batch_reasons.append('Saved run records do not match the coordinator batch inventory')
if len({r['pid'] for r in records})!=len(records): batch_reasons.append('Owned PID reused or duplicated between runs')
def percentile(values,fraction):
    values=sorted(values);index=(len(values)-1)*fraction;lower=math.floor(index);upper=math.ceil(index)
    return values[lower]+(values[upper]-values[lower])*(index-lower)
def run_contract(run):
    reasons=[];player=run.get('player');workload=run.get('workload');round_=run.get('round')
    if player not in EXPECTED_RUNTIME or workload not in EXPECTED_MEDIA or type(round_) is not int or round_ not in range(4):
        return ['Unexpected player/workload/round']
    if run.get('warmup') is not (round_==0): reasons.append('Warmup flag does not match round zero')
    expected_runtime,expected_exe=EXPECTED_RUNTIME[player]
    for key,expected in [('runtimeManifestSha256',expected_runtime),('executableSha256',expected_exe),('mediaSha256',EXPECTED_MEDIA[workload])]:
        if run.get(key,'').lower()!=expected: reasons.append(f'Frozen run identity changed: {key}')
    if any(row['Application']!=('Aperture.exe' if player=='Aperture' else 'vlc.exe') for row in by[run['pid']]):
        reasons.append('Collected application does not match owned PID/player')
    if run.get('status')!='sampled': return reasons+[run.get('failure','Run not sampled')]
    try:
        q=run['qpcFrequency'];start=run['windowStartQpc'];end=run['windowEndQpc']
        ps,pe,rs,re=(run[k] for k in ['pauseStartQpc','pauseEndQpc','resumeStartQpc','resumeEndQpc'])
        if not all(type(v) is int and v>0 for v in [q,start,end,ps,pe,rs,re]) or not start<end<ps<pe<=rs<re:
            return reasons+['Missing or non-monotonic phase QPC']
        if end-start<40*q or pe-ps<3*q or re-rs<5*q: reasons.append('Planned 40s/3s/5s phase duration incomplete')
        guards=run['guards']
        if len(guards)!=11 or not all(g.get('safe') is True and g.get('status')=='validated' for g in guards):
            reasons.append('Expected eleven validated initial/timed/pause/resume guards')
        else:
            spans=[(g['qpcStart'],g['qpcEnd']) for g in guards]
            if (not all(a<=b for a,b in spans) or not all(spans[i][1]<=spans[i+1][0] for i in range(10))
                    or spans[0][1]>start or not all(start<=a<=b<=end for a,b in spans[1:9])
                    or not pe<=spans[9][0]<=spans[9][1]<=rs or spans[10][0]<re):
                reasons.append('Visibility guard timestamps do not bracket their recorded phases')
        canvas=run['canvas']
        if canvas['clientWidth']!=1280 or canvas['clientHeight']!=720 or canvas['dpi']<=0: reasons.append('Invalid matched canvas/DPI')
        if any(g.get('dpi')!=canvas['dpi'] for g in guards): reasons.append('Guard DPI changed within owned run')
        settlement=run['pauseSettlement'];observations=settlement.get('observations',[])
        if settlement.get('status')!='settled' or not observations or settlement['qpcEnd']>ps:
            reasons.append('Pause settlement evidence is incomplete')
        else:
            settled=observations[-1];confirmation=run['pauseConfirmation']
            for status in [settled,confirmation]:
                if status.get('reason') or status.get('paused') is not True or not math.isfinite(status['positionSec']):
                    reasons.append('Pause was not explicitly confirmed');break
                if player=='Aperture' and status.get('state')!=3: reasons.append('Aperture pause state not confirmed')
                if player=='VLC' and (status.get('hasInput') is not True or not (status.get('state')==4 or status.get('pauseEvidence')=='input-pause-guard')):
                    reasons.append('VLC input pause state not confirmed')
            if (abs(confirmation['positionSec']-settled['positionSec'])>.05
                    or not ps<=confirmation['qpcStart']<=confirmation['qpcEnd']<=pe):
                reasons.append('Paused playback moved or confirmation lies outside the pause')
        for key in ['before','after','resumedStatus']:
            if run[key].get('reason') or not math.isfinite(run[key]['positionSec']): reasons.append(f'Playback status unavailable: {key}')
        if run['after']['positionSec']-run['before']['positionSec']<38: reasons.append('Timed playback did not advance')
        if run['resumedStatus']['positionSec']-run['pauseConfirmation']['positionSec']<3: reasons.append('Resumed playback did not advance')
        resume=run['resume']
        if player=='Aperture':
            if resume.get('reason') or resume.get('state')!=2 or run['resumedStatus'].get('state')!=2: reasons.append('Aperture resume state not confirmed')
        else:
            observations=resume.get('observations',[]);last=observations[-1] if observations else {}
            if (resume.get('status')!='settled' or resume['qpcEnd']>rs or last.get('reason') or last.get('paused') is not False
                    or last.get('state')!=3 or last.get('hasInput') is not True): reasons.append('VLC resume settlement not confirmed')
    except (KeyError,TypeError,ValueError,IndexError) as error:
        reasons.append(f'Incomplete or malformed phase evidence: {error}')
    return reasons
results=[];owned=set()
for run in records:
    pid=run['pid'];owned.add(pid);result={k:run[k] for k in ['player','workload','round','warmup','pid','status','startedUtc','endedUtc']}
    result['reasons']=run_contract(run);result['identified']=False;result['evidenceValid']=False;result['qualified']=False
    result['guardCount']=len(run.get('guards',[]))
    if run['status']!='sampled': result['failureStage']='timed-window' if run.get('windowStartQpc') and not run.get('windowEndQpc') else 'setup-or-later-phase'
    if result['reasons']: results.append(result);continue
    q=run['qpcFrequency'];fps=30 if run['workload']=='h264' else 60;end=run['windowEndQpc'];start=run['windowStartQpc']
    assert start<end<run['pauseStartQpc']<run['pauseEndQpc']<=run['resumeStartQpc']<run['resumeEndQpc'],'Non-monotonic phase QPC'
    ps=run['pauseStartQpc']+int(.5*q);pe=run['pauseEndQpc']-int(.5*q);rs=run['resumeEndQpc']-int(1.5*q);re=run['resumeEndQpc']
    chains=collections.defaultdict(list)
    for row in by[pid]: chains[row['SwapChainAddress']].append(row)
    summaries=[]
    for chain,rows in chains.items():
        times=[int(row['CPUStartQPC']) for row in rows]
        playing=sum(end-5*q<=t<end for t in times)/5
        paused=sum(ps<=t<pe for t in times)/((pe-ps)/q)
        resumed=sum(rs<=t<re for t in times)/1.5
        candidate=.8*fps<=playing<=1.2*fps and paused<=.5*fps and .8*fps<=resumed<=1.2*fps
        summaries.append(dict(chain=chain,rows=len(rows),playingRate=playing,pausedRate=paused,resumedRate=resumed,candidate=candidate))
    result['chains']=summaries;candidates=[s['chain'] for s in summaries if s['candidate']]
    if len(candidates)!=1: result['reasons'].append(f'Expected one identified chain, found{len(candidates)}');results.append(result);continue
    chain=candidates[0];result['identified']=True;result['videoChain']=chain
    selected=[row for row in chains[chain] if start+5*q<=int(row['CPUStartQPC'])<start+35*q]
    durations=[];undisplayed=0;invalid=0;shown=[]
    for row in selected:
        value=row['DisplayedTime']
        if value=='NA': undisplayed+=1;continue
        value=float(value)
        if not math.isfinite(value) or value<=0: invalid+=1;continue
        durations.append(value);shown.append(row)
    if not durations: result['reasons'].append('No finite positive DisplayedTime values')
    if invalid: result['reasons'].append(f'{invalid} nonpositive/nonfinite duration values')
    if not all(g['safe'] and g['status']=='validated' for g in run['guards']): result['reasons'].append('Visibility/cursor guard failed')
    if run['canvas']['clientWidth']!=1280 or run['canvas']['clientHeight']!=720: result['reasons'].append('Wrong actual canvas')
    if run['after']['positionSec']-run['before']['positionSec']<38: result['reasons'].append('Playback did not advance')
    sources=sorted(set(row['VidPnSourceId'] for row in shown));layers=sorted(set(row['LayerIndex'] for row in shown))
    if len(sources)!=1 or sources[0] in ['', 'NA']: result['reasons'].append('Missing or ambiguous display source metadata')
    if any(row['LayerIndex'] in ['', 'NA'] or row['PresentId'] in ['', 'NA'] for row in shown): result['reasons'].append('Missing shown-frame display metadata')
    result['interior']=dict(startQpc=start+5*q,endQpc=start+35*q,seconds=30,submittedRows=len(selected),submissionRate=len(selected)/30,displayedRows=len(durations),undisplayedRows=undisplayed,undisplayedFraction=undisplayed/len(selected) if selected else None,invalidDurationRows=invalid,displaySources=sources,layers=layers,presentModes=dict(collections.Counter(row['PresentMode'] for row in selected)),syncIntervals=dict(collections.Counter(row['SyncInterval'] for row in selected)),dpi=run['canvas']['dpi'])
    if durations:
        threshold=1.5*1000/fps
        result['durationsMs']=dict(median=statistics.median(durations),p95=percentile(durations,.95),p99=percentile(durations,.99),maximum=max(durations),aboveOneAndHalfNominal=sum(v>threshold for v in durations),longThresholdMs=threshold)
    result['evidenceValid']=not result['reasons'];results.append(result)
unassigned=sorted(set(by)-owned)
workloads={}
planned_workloads=plan.get('workloads',['h264','hevc'])
assert planned_workloads and len(set(planned_workloads))==len(planned_workloads) and set(planned_workloads)<=set(EXPECTED_MEDIA),'Unexpected planned workloads'
for workload in planned_workloads:
    group=[r for r in results if r['workload']==workload]
    expected={(player,round_) for player in ['Aperture','VLC'] for round_ in range(4)}
    actual={(r['player'],r['round']) for r in group}
    valid=[r for r in group if r['evidenceValid']]
    sources={tuple(r['interior']['displaySources']) for r in valid}
    dpis={r['interior']['dpi'] for r in valid}
    reasons=list(batch_reasons)
    if actual!=expected or len(group)!=8: reasons.append('Incomplete planned eight fresh processes for this workload')
    if len(valid)!=8: reasons.append('One or more owned runs lack complete valid evidence')
    if len(sources)!=1: reasons.append('Display source differs or is unavailable across matched runs')
    if len(dpis)!=1: reasons.append('DPI differs or is unavailable across matched runs')
    if unassigned: reasons.append('CSV contains unassigned player PIDs')
    if read_errors: reasons.append('One or more raw run records could not be read')
    qualified=bool(args.final and not reasons and finished and finished.get('captureQualified'))
    for r in group: r['qualified']=qualified and r['evidenceValid']
    workloads[workload]=dict(completePlannedRuns=actual==expected,identifiedRuns=sum(r['identified'] for r in group),evidenceValidRuns=len(valid),qualifiedRuns=sum(r['qualified'] for r in group),runCount=len(group),captureQualified=bool(finished and finished.get('captureQualified')),qualified=qualified,reasons=reasons,displaySources=sorted(sources),dpis=sorted(dpis),players={})
    if not qualified: continue
    for player in ['Aperture','VLC']:
        measured=[r for r in group if r['player']==player and not r['warmup']]
        assert len(measured)==3
        summary={}
        for key in ['median','p95','p99','maximum','aboveOneAndHalfNominal']:
            values=[r['durationsMs'][key] for r in measured];summary[key]=dict(medianOfRuns=statistics.median(values),min=min(values),max=max(values))
        workloads[workload]['players'][player]=summary

report=dict(recordedAtUtc=datetime.datetime.now(datetime.timezone.utc).isoformat(),mode='final' if args.final else 'live diagnostic; collector qualification pending',sourceCsv=str(collector/'presents.csv'),sourceCsvSha256=hashlib.sha256(raw).hexdigest(),hashScope='entire closed final raw CSV' if args.final else 'complete-row prefix snapshot of still-streaming CSV',snapshotFileBytes=snapshot_bytes,hashedPrefixBytes=len(raw),batchReasons=batch_reasons,rows=rowcount,fields=fields,unassignedPids=unassigned,readErrors=read_errors,percentileMethod='Type7 linear interpolation of sorted finite positive durations, fixed in implementation before first duration inspection',runs=results,workloads=workloads,limits='Operational video-chain identification only; no unique decoded-content, physical-panel, audio continuity, equal decoder/pixel quality, resource CPU or overall superiority claim. Interior rows selected by CPUStartQPC; final displayed intervals may extend past the interior boundary. Nominal164Hz OS display mode imposes noninteger30/60FPS refresh cadence.')
out=ROOT/f'cadence-{args.label}-analysis-20261021{"-final" if args.final else "-live"}.json'
out.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(rows=rowcount,unassignedPids=unassigned,runs=[dict(player=r['player'],workload=r['workload'],round=r['round'],evidenceValid=r['evidenceValid'],qualified=r['qualified'],reasons=r['reasons']) for r in results],workloads=workloads),indent=2))
