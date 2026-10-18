from pathlib import Path
import argparse,subprocess,json,hashlib,datetime,re,math
from PIL import Image,ImageChops
parser=argparse.ArgumentParser();parser.add_argument('--probe',required=True);parser.add_argument('--baseline',required=True);parser.add_argument('--candidate',required=True);parser.add_argument('--out',required=True);parser.add_argument('--full',action='store_true');args=parser.parse_args()
repo=Path(r'C:\Users\Alperen\Desktop\Antigravity\Aperture Final');perf=Path(r'C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-vlc-performance-20261017');media=repo/'scratch/libmpv-tests/media';out=Path(args.out);out.mkdir()
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
cases=[
 {'id':'h264-down','dir':perf,'file':'h264-1080p30-audio.mp4','direct':'yes','limit':1},
 {'id':'av1-down','dir':perf,'file':'av1-1080p30-audio.mp4','direct':'yes','limit':1},
 {'id':'vp9-up','dir':media,'file':'vp9_opus.webm','direct':'yes','limit':1},
 {'id':'hevc-8bit','dir':media,'file':'hevc_aac_720p.mp4','direct':'yes','limit':1},
 {'id':'hevc-10bit','dir':perf,'file':'hevc-2160p60-audio.mp4','direct':'no','limit':0},
 {'id':'hdr-pq','dir':media,'file':'hevc_hdr10.mp4','direct':'no','limit':None},
 {'id':'alpha','dir':perf/'sdr-fbo-experiment','file':'alpha-bt709-rgb-ffv1.mkv','direct':'no','limit':0,'hwdec':'no'},
 {'id':'h264-software','dir':perf,'file':'h264-1080p30-audio.mp4','direct':'no','limit':0,'hwdec':'no'},
 {'id':'native-quality','dir':perf,'file':'h264-1080p30-audio.mp4','direct':'no','limit':0,'scaling':'default'},
]
if args.full:
 cases += [
  {'id':'explicit-float','dir':perf,'file':'h264-1080p30-audio.mp4','direct':'no','limit':0,'extra':['--fbo-format','rgba16hf']},
  {'id':'explicit-rgba8','dir':perf,'file':'h264-1080p30-audio.mp4','direct':'no','limit':0,'extra':['--fbo-format','rgba8']},
  {'id':'compute-dither','dir':perf,'file':'h264-1080p30-audio.mp4','direct':'no','limit':0,'extra':['--frame-dither','error-diffusion']},
  {'id':'zoom-crop','dir':perf,'file':'h264-1080p30-audio.mp4','direct':'no','limit':0,'extra':['--frame-zoom','0.5']},
  {'id':'rotated','dir':perf,'file':'h264-1080p30-audio.mp4','direct':'no','limit':0,'extra':['--frame-rotate','90']},
 ]
report={'createdUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'visibility':'hidden-correctness-only','scope':'Exact paused time 2s, rendered GPU screenshot. No playback CPU/RAM/throughput ranking. No VLC process. HDR differences are reported without a pixel bound because adaptive peak history can vary.','probe':{'path':args.probe,'sha256':sha(args.probe)},'libraries':{k:{'path':v,'sha256':sha(v)} for k,v in [('baseline',args.baseline),('candidate',args.candidate)]},'cases':[]}
def save(): (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
def record_json(log,name):
 match=re.search(r'^'+name+r' (.+)$',log,re.M);assert match,name
 return json.loads(match[1])
for case in cases:
 row={**{k:str(v) if isinstance(v,Path) else v for k,v in case.items()},'fixtureSha256':sha(case['dir']/case['file']),'variants':[],'status':'running'};report['cases'].append(row);save()
 try:
  for variant,dll in [('baseline',args.baseline),('candidate',args.candidate)]:
   run=out/case['id']/variant;run.mkdir(parents=True);capture=run/'frame.png'
   command=[args.probe,'--dll',str(Path(dll).resolve()),'--media',str(case['dir']),'--scenario','framecheck','--headless','--resource-file',case['file'],'--capture-window',str(capture),'--scaling',case.get('scaling','balanced'),'--gpu-timing','no','--vertex-cache-entries','4','--resource-hwdec',case.get('hwdec','d3d11va'),'--fbo-diagnostics',*case.get('extra',[])]
   if variant=='candidate': command+=['--scale-direct-nv12','yes']
   started=datetime.datetime.now(datetime.timezone.utc).isoformat()
   result=subprocess.run(command,cwd=run,capture_output=True,timeout=60,creationflags=subprocess.CREATE_NO_WINDOW)
   (run/'stdout.txt').write_bytes(result.stdout);(run/'stderr.txt').write_bytes(result.stderr)
   log=result.stdout.decode('utf-8',errors='replace');assert result.returncode==0,(variant,result.returncode,log[-1200:])
   assert 'Loaded module path matches requested: YES' in log
   assert '[PASS] headless test owner windows remained hidden' in log
   check=record_json(log,'FRAME_CHECK');options=record_json(log,'RENDER_OPTIONS');passes=record_json(log,'RENDER_PASSES')
   assert check['headless'] is True and abs(check['position']-2)<.02 and check['hwdec']==case.get('hwdec','d3d11va')
   expected=case['direct'] if variant=='candidate' else 'no';assert check['direct_filter']==expected,(case['id'],expected,check)
   if variant=='candidate':assert options['scale-direct-nv12']=='yes'
   assert capture.is_file();matches=re.search(r'SUMMARY: (\d+) passed, 0 failed',log);assert matches
   row['variants'].append({'variant':variant,'startedUtc':started,'arguments':command,'checksPassed':int(matches[1]),'frame':check,'renderOptions':options,'renderPasses':passes,'captureSha256':sha(capture)})
   save()
  a=Image.open(out/case['id']/'baseline/frame.png').convert('RGB');b=Image.open(out/case['id']/'candidate/frame.png').convert('RGB');assert a.size==b.size
  assert a.getcolors(32) is None and b.getcolors(32) is None, 'Captured image lacks the expected picture detail'
  diff=ImageChops.difference(a,b);hist=diff.histogram();n=a.width*a.height*3;mae=sum((i%256)*v for i,v in enumerate(hist))/n;mse=sum((i%256)**2*v for i,v in enumerate(hist))/n
  maximum=ImageChops.lighter(ImageChops.lighter(diff.getchannel('R'),diff.getchannel('G')),diff.getchannel('B'));ph=maximum.histogram();max_error=maximum.getextrema()[1]
  row['pixels']={'size':a.size,'meanAbsoluteChannelError':mae,'maximumChannelError':max_error,'psnrDb':10*math.log10(255**2/mse) if mse else None,'fractionPixelsDiffer':1-ph[0]/(a.width*a.height),'limit':case['limit']}
  if case['limit'] is not None:assert max_error<=case['limit'],(case['id'],row['pixels'])
  row['status']='pass' if case['limit'] is not None else 'fallback-verified-pixels-reported'
  print(case['id'],row['status'],'direct='+case['direct'],'max='+str(max_error),flush=True)
 except Exception as error:
  row['status']='fail';row['error']=repr(error);save();raise
 save()
report['completedUtc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save()
