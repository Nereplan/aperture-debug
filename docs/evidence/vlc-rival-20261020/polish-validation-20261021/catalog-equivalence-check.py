import hashlib,json,pathlib,re,subprocess
r=pathlib.Path(r'C:\Users\Alperen\Desktop\Antigravity\Aperture Final')
old_bytes=subprocess.check_output(['git','show','f9ad8e3:locales/en-US.json'],cwd=r)
new_bytes=(r/'locales/en-US.json').read_bytes()
source=(r/'src/Localization.cpp').read_text(encoding='utf-8')
old=json.loads(old_bytes);new=json.loads(new_bytes)
quoted=r'"(?:[^"\\]|\\.)*"'
def cpp_string(s):
 s=re.sub(r'(?:\\x[0-9A-Fa-f]{2})+',lambda m:bytes(int(x,16) for x in re.findall(r'\\x([0-9A-Fa-f]{2})',m[0])).decode('utf-8'),s)
 return json.loads(s)
strings=source.split('constexpr StringEntryView kCanonicalStringEntries[] = {',1)[1].split('\n};',1)[0]
entries=[(cpp_string(a),cpp_string(b)) for a,b in re.findall(r'\{\s*('+quoted+r')\s*,\s*('+quoted+r')\s*\}',strings)]
canonical=dict(entries);assert len(entries)==len(canonical)==379
before=canonical|old['strings'];after=canonical|new['strings'];assert before==after
assert {k:v for k,v in old.items() if k not in ('strings','plurals')}=={k:v for k,v in new.items() if k not in ('strings','plurals')}
forms=('zero','one','two','few','many','other')
plural_block=source.split('constexpr PluralEntryView kCanonicalPluralEntries[] = ',1)[1].split('\n};',1)[0]
tokens=[cpp_string(x) for x in re.findall(quoted,plural_block)];assert len(tokens)==77
plurals={tokens[i]:dict(zip(forms,tokens[i+1:i+7])) for i in range(0,len(tokens),7)}
for overlay in old,new:
 for key,value in overlay.get('plurals',{}).items():assert plurals[key]=={form:value.get(form,'') for form in forms}
assert len(plurals)==11 and len(new['strings'])==4
result=dict(scope='Independent source/previous-Git-catalog semantic comparison; native loader behavior is verified separately by the regression gate.',allEffectiveStringsEqual=len(after),allPluralRecordsEqual=len(plurals),metadataEqual=True,retainedOverrides=sorted(new['strings']),oldGitBlobBytes=len(old_bytes),newWorkingFileBytes=len(new_bytes),oldGitLines=len(old_bytes.splitlines()),newWorkingFileLines=len(new_bytes.splitlines()),oldGitSha256=hashlib.sha256(old_bytes).hexdigest(),newWorkingFileSha256=hashlib.sha256(new_bytes).hexdigest(),canonicalSourceSha256=hashlib.sha256((r/'src/Localization.cpp').read_bytes()).hexdigest(),lineEndingNote='Git stores LF; current Windows working file has CRLF. Compare byte counts only within the declared representation.')
p=r'C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019\sparse-english-catalog-validation-20261020.json'
with open(p,'x',encoding='utf-8') as f:json.dump(result,f,indent=2);f.write('\n')
print(json.dumps(result,indent=2))