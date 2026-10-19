"""Audit the single approved visible run; never launches a player."""
import argparse, datetime as dt, hashlib, json, pathlib, statistics
ROOT=pathlib.Path.cwd()
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--input',type=pathlib.Path,default=pathlib.Path(__file__).resolve().parent)
parser.add_argument('--out',type=pathlib.Path,required=True)
args=parser.parse_args(); TASK=args.input.resolve(); OUTPUT=args.out.resolve()
assert not OUTPUT.exists(), 'Use a fresh output directory'
OUTPUT.mkdir(parents=True)

def load(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def metric(report,cal):
    runs=[r for r in report['results'] if not r['warmup']]
    assert len(runs)==1
    r=runs[0]; s=r['samples']; a,b=s[0],s[-1]
    assert r['status']=='sampled' and r['playbackVerification']=='advancing-playback-verified'
    assert len(s)==7 and r['videoCanvas']['clientWidth']==1280 and r['videoCanvas']['clientHeight']==720
    assert cal['supported'] and cal['invariant_tsc'] and cal['logical_processors']==24
    assert cal['relative_rate_spread']<.001 and .95<cal['process_busy_ratio']<1.05
    rate=cal['tsc_ticks_per_second']; seconds=b['elapsedSeconds']-a['elapsedSeconds']
    assert 29.5<seconds<31
    cpu=(b['cpuCycles']-a['cpuCycles'])/seconds/rate*100
    med=lambda key:statistics.median(x[key] for x in s)/1048576
    return {'values':{'cpu-core':cpu,'visible-ms':r['processToVisibleWindowMs'],'runtime-mib':r['runtimeBytes']/1048576,
        'private-mib':med('privateBytes'),'working-set-mib':med('workingSetBytes'),'gpu-mib':med('dedicatedGpuBytes')},
        'sampledCpuOneLogicalPercent':(b['cpuSeconds']-a['cpuSeconds'])/seconds*100,
        'cpuTotalCapacityPercent':cpu/24,'cycleDelta':b['cpuCycles']-a['cpuCycles'],
        'sampledCpuSecondsDelta':b['cpuSeconds']-a['cpuSeconds'],'actualElapsedSeconds':seconds,
        'privateMiBSnapshotRange':[min(x['privateBytes'] for x in s)/1048576,max(x['privateBytes'] for x in s)/1048576],
        'sharedGpuMiB':med('sharedGpuBytes'),'snapshotCount':len(s),
        'mediaIntervalSeconds':[a['playback']['positionSec'],b['playback']['positionSec']],
        'intervals':[{'fromMediaSeconds':x['playback']['positionSec'],'toMediaSeconds':y['playback']['positionSec'],
            'elapsedSeconds':y['elapsedSeconds']-x['elapsedSeconds'],
            'cpuOneLogicalPercent':(y['cpuCycles']-x['cpuCycles'])/(y['elapsedSeconds']-x['elapsedSeconds'])/rate*100}
            for x,y in zip(s,s[1:])]},r

def thread_pair(a,b,rate,ui):
    aa,bb=a['threadCounters'],b['threadCounters']; assert not aa['unavailableReason'] and not bb['unavailableReason']
    def identity(t):return (t['id'],t['createdFileTimeUtc'])
    def index(s):
        return {identity(t):t for t in s['threads'] if t['createdFileTimeUtc'] and t['cpuCycles'] is not None and not t['unavailableReason']}
    ai,bi=index(aa),index(bb); seconds=b['elapsedSeconds']-a['elapsedSeconds']; process_delta=b['cpuCycles']-a['cpuCycles']
    rows=[]
    for key in ai.keys()&bi.keys():
        x,y=ai[key],bi[key]; delta=y['cpuCycles']-x['cpuCycles']; qseconds=(y['qpc']-x['qpc'])/aa['qpcFrequency']
        assert delta>=0 and qseconds>0
        rows.append({'id':key[0],'createdFileTimeUtc':key[1],'description':y['description'] or x['description'],
            'isWindowOwnerThread':key[0]==ui,'cycleDelta':delta,'ownCounterElapsedSeconds':qseconds,
            'ownCounterCpuOneLogicalPercent':delta/qseconds/rate*100,
            'contributionOnProcessWindowOneLogicalPercent':delta/seconds/rate*100,
            'cpuTimeSecondsDelta':(y['kernelCpu100ns']+y['userCpu100ns']-x['kernelCpu100ns']-x['userCpu100ns'])/1e7})
    rows.sort(key=lambda t:t['cycleDelta'],reverse=True)
    total=sum(t['cycleDelta'] for t in rows)
    keylist=lambda keys:[{'id':k[0],'createdFileTimeUtc':k[1]} for k in sorted(keys)]
    return {'fromMediaSeconds':a['playback']['positionSec'],'toMediaSeconds':b['playback']['positionSec'],
        'processElapsedSeconds':seconds,'processCycleDelta':process_delta,
        'processCpuOneLogicalPercent':process_delta/seconds/rate*100,
        'matchedIdentityCount':len(rows),'firstSnapshotThreadCount':len(aa['threads']),'lastSnapshotThreadCount':len(bb['threads']),
        'matchedThreadCycleDeltaSum':total,'processMinusMatchedThreadCycleDelta':process_delta-total,
        'matchedThreadCycleSumToProcessRatio':total/process_delta,
        'newOrNewlyReadableIdentities':keylist(bi.keys()-ai.keys()),'vanishedOrUnreadableIdentities':keylist(ai.keys()-bi.keys()),
        'threads':rows}

if __name__=='__main__':
    report=load(TASK/'comparison.json');cal=load(TASK/'cpu-calibration.json')
    aperture,r=metric(report,cal)
    vlc_report=load(ROOT/'docs/evidence/balanced-memory-20261018/app/comparison.json')
    # Select ONLY original VLC run; the saved report also contains Aperture.
    vlc_report={**vlc_report,'results':[x for x in vlc_report['results'] if x['player']=='VLC']}
    vlc,_=metric(vlc_report,load(ROOT/'docs/evidence/balanced-memory-20261018/app/cpu-calibration.json'))
    old=load(ROOT/'docs/evidence/native-performance-20261019/visible-once/review.json')
    assert len(report['results'])==1 and report['results'][0]['player']=='Aperture'
    assert report['iterations']==1 and report['warmupIterations']==0 and report['threadCounterCapture']['sourceSha256'].lower()==sha(ROOT/'tests/benchmark/PlayerThreadCounters.cs')
    samples=r['samples'];ui=r['uiThreadId']
    threads={'method':'Match thread ID plus lossless creation FILETIME. Use each thread own QPC duration for its CPU rate; contribution uses the root-process wall interval. No suspension/stacks. Do not infer code cause from names.',
        'limits':'Snapshots are not atomic. Threads exiting between endpoints may be absent, and new/unreadable identities are explicit. Process minus matched-thread cycles is a signed accounting residual, not measured unattributed work. Root process CPU is primary; all intervals retained.',
        'uiThreadId':ui,'fullWindow':thread_pair(samples[0],samples[-1],cal['tsc_ticks_per_second'],ui),
        'intervals':[thread_pair(x,y,cal['tsc_ticks_per_second'],ui) for x,y in zip(samples,samples[1:])],
        'snapshotCaptureMilliseconds':[(s['threadCounters']['qpcEnd']-s['threadCounters']['qpcStart'])/s['threadCounters']['qpcFrequency']*1000 for s in samples],
        'unavailableCounters':[{'sample':i,'thread':t} for i,s in enumerate(samples) for t in s['threadCounters']['threads'] if t['unavailableReason']]}
    def diff(a,b):
        av,bv=a['values'],b['values']
        return {'privateMiBDifference':av['private-mib']-bv['private-mib'],
            'privateRelativePercent':(av['private-mib']/bv['private-mib']-1)*100,
            'calibratedCpuRelativePercent':(av['cpu-core']/bv['cpu-core']-1)*100,
            'calibratedCpuMachinePercentagePointDifference':a['cpuTotalCapacityPercent']-b['cpuTotalCapacityPercent'],
            'dedicatedGpuMiBDifference':av['gpu-mib']-bv['gpu-mib'],
            'suppliedRuntimeMiBDifference':av['runtime-mib']-bv['runtime-mib']}
    review={'recordedUtc':dt.datetime.now(dt.timezone.utc).isoformat(),'batchCompletedUtc':report['createdUtc'],
        'aperture':aperture,'savedVlc':{'rawEvidence':'docs/evidence/balanced-memory-20261018/app/comparison.json',
            'originalBatchDate':vlc_report['createdUtc'],**vlc},'versusSavedVlc':diff(aperture,vlc),
        'previousO3Aperture':{'rawEvidence':'docs/evidence/native-performance-20261019/visible-once/comparison.json',**old['aperture']},
        'versusPreviousO3Aperture':diff(aperture,old['aperture']),
        'threadDiagnosticsEvidence':'thread-review.json'}
    for name,value in [('audit-review.json',review),('audit-threads.json',threads)]:
        path=OUTPUT/name;assert not path.exists(),f'Not fresh: {path}'
        path.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'aperture':aperture,'versusSavedVlc':review['versusSavedVlc'],
        'threadTop5':threads['fullWindow']['threads'][:5],
        'captureMilliseconds':threads['snapshotCaptureMilliseconds']},indent=2))
