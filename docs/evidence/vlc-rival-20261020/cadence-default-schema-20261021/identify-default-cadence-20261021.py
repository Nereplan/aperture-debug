import csv,json,collections
from pathlib import Path
root=Path(r'C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019')
identity=json.loads((root/'cadence-runs-20261021/identity.json').read_text(encoding='utf-8-sig'))
with (root/'cadence-collector-20261021/presents.csv').open(encoding='utf-8-sig',newline='') as f:
    reader=csv.DictReader(f); fields=reader.fieldnames; rows=list(reader)
by=collections.defaultdict(list)
for row in rows: by[int(row['ProcessID'])].append(row)
results=[]; owned=set()
for run in identity['runs']:
    pid=run['pid']; owned.add(pid)
    frequency=run['qpcFrequency']; end=run['windowEndQpc']; fps=30 if run['workload']=='h264' else 60
    ps=run['pauseStartQpc']+int(.5*frequency); pe=run['pauseEndQpc']-int(.5*frequency)
    rs=run['resumeEndQpc']-int(1.5*frequency); re=run['resumeEndQpc']
    chains=collections.defaultdict(list)
    for row in by[pid]: chains[row['SwapChainAddress']].append(int(row['CPUStartQPC']))
    summary=[]
    for chain,times in chains.items():
        playing=sum(end-5*frequency<=t<end for t in times)/5
        paused=sum(ps<=t<pe for t in times)/((pe-ps)/frequency)
        resumed=sum(rs<=t<re for t in times)/1.5
        candidate=.8*fps<=playing<=1.2*fps and paused<=1 and .8*fps<=resumed<=1.2*fps
        summary.append(dict(chain=chain,rows=len(times),playingRate=playing,pausedRate=paused,resumedRate=resumed,candidate=candidate))
    results.append(dict(player=run['player'],workload=run['workload'],round=run['round'],pid=pid,chains=summary,candidates=sum(s['candidate'] for s in summary)))
report=dict(scope='Diagnostic chain-identification rates only. Default schema lacks predeclared DisplayedTime; no duration metric or quality ranking. Incomplete HEVC batch retained without splicing.',csvFields=fields,rowCount=len(rows),foreignPids=sorted(set(by)-owned),runs=results)
(root/'cadence-default-schema-identification-20261021.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'rowCount':len(rows),'runs':results,'foreignPids':report['foreignPids']},indent=2))
