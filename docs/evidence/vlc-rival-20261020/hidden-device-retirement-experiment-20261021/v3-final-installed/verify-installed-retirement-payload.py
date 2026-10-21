import datetime, hashlib, json, subprocess, sys, zipfile
from pathlib import Path

repo, expected_commit, out, runtime = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
def sha(data): return hashlib.sha256(data).hexdigest()
def read(path): return json.loads(path.read_text(encoding='utf-8-sig'))
assert not out.exists() and not runtime.exists()
assert subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip() == expected_commit
assert subprocess.run(['git','-C',str(repo),'diff','--quiet','HEAD','--','src','tests','CMakeLists.txt']).returncode == 0
manifest = read(repo/'release/manifest.json')
entries = manifest['files']
assert len(entries) == 10 and len({e['path'] for e in entries}) == 10
zip_path = repo/'dist'/('Aperture-'+manifest['version']+'-windows-x64.zip')
counts = {}
for label in ['release','debug']:
    own_manifest = read(repo/label/'manifest.json')
    assert own_manifest['files'] == entries
    for entry in entries:
        data = (repo/label/entry['path']).read_bytes()
        assert len(data) == entry['size'] and sha(data) == entry['sha256'].lower(), (label,entry['path'])
    counts[label] = len(entries)
with zipfile.ZipFile(zip_path) as archive:
    for entry in entries:
        data = archive.read(entry['path'])
        assert len(data) == entry['size'] and sha(data) == entry['sha256'].lower(), entry['path']
    assert json.loads(archive.read('manifest.json').decode('utf-8-sig')) == manifest
    counts['zip'] = len(entries)
runtime.mkdir()
for entry in entries:
    target = runtime/entry['path']
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes((repo/'release'/entry['path']).read_bytes())
(runtime/'manifest.json').write_bytes((repo/'release/manifest.json').read_bytes())
assert not (runtime/'data').exists()
result = dict(verifiedAtUtc=datetime.datetime.now(datetime.timezone.utc).isoformat(),buildSourceCommit=expected_commit,
    productionSourceDirty=False,managedPayloadsVerified=counts,executable=next(e for e in entries if e['path']=='Aperture.exe'),
    distribution=dict(path=str(zip_path),bytes=zip_path.stat().st_size,sha256=sha(zip_path.read_bytes())),
    cleanBenchmarkRuntime=str(runtime),scope='Exact locally built release/debug/ZIP managed payload verification; fresh benchmark runtime copies only verified managed files and manifest, without user data.')
out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
