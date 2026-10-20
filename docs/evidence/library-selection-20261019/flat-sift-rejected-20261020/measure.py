import csv,hashlib,json,os,statistics,subprocess,sys
from datetime import datetime,timezone
from pathlib import Path
root=Path(__file__).resolve().parent
manifestPath=root/'pre-timing-manifest.json'
manifest=json.loads(manifestPath.read_text())
manifestHash=hashlib.sha256(manifestPath.read_bytes()).hexdigest()
for item in manifest['sourceAndBuildFiles']:
    path=root/item['path'];assert path.stat().st_size==item['bytes'] and hashlib.sha256(path.read_bytes()).hexdigest()==item['sha256'],path
runs=[];grouped={}
for pair,order in enumerate(manifest['protocol']['processOrder'],1):
    for variant in order:
        executable=root/(variant+'-build')/'flat-sift-probe.exe'
        command=[str(executable),str(pair-1)]
        started=datetime.now(timezone.utc).isoformat()
        result=subprocess.run(command,capture_output=True,check=False)
        ended=datetime.now(timezone.utc).isoformat()
        stem=f'{variant}-pair{pair}'
        (root/(stem+'.csv')).write_bytes(result.stdout)
        (root/(stem+'.stderr.txt')).write_bytes(result.stderr)
        metadata={'variant':variant,'pair':pair,'startedAtUtc':started,'endedAtUtc':ended,'command':command,'returnCode':result.returncode,'exeSha256':hashlib.sha256(executable.read_bytes()).hexdigest(),'preTimingManifestSha256':manifestHash}
        (root/(stem+'.json')).write_text(json.dumps(metadata,indent=2)+'\n')
        if result.returncode:
            print(result.stderr.decode(errors='replace'),flush=True);raise SystemExit('Probe failed')
        rows=list(csv.DictReader(result.stdout.decode('utf-8-sig').splitlines()))
        assert len(rows)==35 and {int(r['mode']) for r in rows}==set(range(5))
        assert 'verified_affinity_mask=' in result.stderr.decode()
        for mode in range(5):
            selected=[r for r in rows if int(r['mode'])==mode]
            assert sorted(int(r['sample']) for r in selected)==list(range(1,8))
            for r in selected:
                assert int(r['queries'])==64
                r['_pair']=pair
            grouped.setdefault((variant,mode),[]).extend(selected)
        runs.append(metadata)
        print(f'Completed pair{pair} {variant}:35 measured rows, all query oracles passed',flush=True)
summary={'schemaVersion':1,'preTimingManifestSha256':manifestHash,'runs':runs,'workloads':[],'validation':{'measuredRows':210,'samplesPerModePerVariant':21,'processesPerVariant':3,'queriesPerSample':64,'measuredQueries':13440,'allSignaturesAndAllocationsEquivalent':True}}
for mode in range(5):
    workload={'mode':mode,'variants':{}}
    signatures=set()
    for variant in ('baseline','candidate'):
        rows=grouped[(variant,mode)];assert len(rows)==21
        signatures.update((r['items'],r['filtered_count'],r['order_hash'],r['allocation_calls'],r['allocated_bytes'],r['peak_live_cpp_bytes']) for r in rows)
        values=[float(r['mean_query_ms']) for r in rows]
        perProcess=[statistics.median(float(r['mean_query_ms']) for r in rows if r['_pair']==p) for p in range(1,4)]
        workload['variants'][variant]={'medianMeanQueryMs':statistics.median(values),'minMeanQueryMs':min(values),'maxMeanQueryMs':max(values),'processMedianMeanQueryMs':perProcess,'allocationCallsPerQuery':int(rows[0]['allocation_calls'])//64,'allocatedBytesPerQuery':int(rows[0]['allocated_bytes'])//64,'peakLiveCppBytes':int(rows[0]['peak_live_cpp_bytes'])}
    assert len(signatures)==1,(mode,signatures)
    workload['signature']=list(next(iter(signatures)))
    before=workload['variants']['baseline'];after=workload['variants']['candidate']
    workload['medianChangePercent']=100*(after['medianMeanQueryMs']/before['medianMeanQueryMs']-1)
    workload['pairedProcessMedianChangePercent']=[100*(a/b-1) for a,b in zip(after['processMedianMeanQueryMs'],before['processMedianMeanQueryMs'])]
    workload['rangesOverlap']=max(before['minMeanQueryMs'],after['minMeanQueryMs'])<=min(before['maxMeanQueryMs'],after['maxMeanQueryMs'])
    summary['workloads'].append(workload)
(root/'comparison.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({'summary':str(root/'comparison.json'),'workloads':[{'mode':w['mode'],'baselineMs':w['variants']['baseline']['medianMeanQueryMs'],'candidateMs':w['variants']['candidate']['medianMeanQueryMs'],'changePercent':w['medianChangePercent'],'pairedChanges':w['pairedProcessMedianChangePercent'],'rangesOverlap':w['rangesOverlap']} for w in summary['workloads']]},indent=2),flush=True)
