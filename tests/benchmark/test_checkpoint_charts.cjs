// Regression checks for historical ordering, a single current VLC reference,
// and broken axes that preserve the recorded numeric values.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '../..');
const html = fs.readFileSync(path.join(root, 'docs/checkpoints.html'), 'utf8');
const DATA = JSON.parse(html.match(/<script id="checkpoint-data" type="application\/json">([\s\S]*?)<\/script>/)[1]);
const script = html.match(/<script id="dashboard-code">([\s\S]*?)<\/script>/)[1];
const logic = script.slice(script.indexOf('function machineCpu('), script.indexOf('function renderLedger('));
const ordered = [...DATA.checkpoints].sort((a, b) => Date.parse(a.measuredAt) - Date.parse(b.measuredAt));
const context = {
  DATA, ordered, visible: new Set(ordered.map(c => c.id)),
  key: r => JSON.stringify([r.metric, r.protocol, r.workload, r.environment]),
};
vm.createContext(context);
vm.runInContext(logic, context);
let checks = 0;
function test(name, fn) {
  fn(); checks++; process.stdout.write(`PASS ${name}\n`);
}
function withVlcVersion(version, fn) {
  const previous = DATA.references;
  DATA.references = Object.fromEntries(Object.entries(previous).filter(([, r]) => r.version === version));
  try { fn(); } finally { DATA.references = previous; }
}

test('Validation records leave the latest actual measurement selected', () => {
  const measured = {id: 'measured', results: [{value: 12}]};
  const empty = {id: 'validation', results: []};
  const missing = {id: 'unmeasured', results: [{value: null}]};
  assert.equal(context.latestMeasured([measured, empty, missing]).id, 'measured');
  assert.equal(context.latestMeasured([empty, missing]), undefined);
  assert.equal(context.latestMeasured([empty, {id: 'zero', results: [{value: 0}]}]).id, 'zero');
  const previous = context.ordered;
  const r = DATA.checkpoints.find(c => c.id === 'players-20261014').results.find(r => r.metric === 'visible-ms');
  const before = context.historyPoints(context.seriesKey(r)).map(p => p.id).join(',');
  context.ordered = [...previous, empty]; context.visible.add(empty.id);
  assert.equal(context.historyPoints(context.seriesKey(r)).map(p => p.id).join(','), before);
  context.ordered = previous; context.visible.delete(empty.id);
});

test('Aperture playback is a dated history, not just the latest pair', () => {
  const cp = DATA.checkpoints.find(c => c.id === 'players-20261014');
  const r = cp.results.find(r => r.metric === 'visible-ms');
  const points = context.historyPoints(context.seriesKey(r));
  assert.deepEqual(points.map(p => p.id).join(','), 'players-20261014,backend-balance-20261017-before,backend-balance-20261017-after');
  assert.equal(points[0].y, 111.7763);
  assert.equal(points.at(-1).y, 141.5932);
});

test('empty-profile startup spans seven actual recordings', () => {
  const r = DATA.checkpoints.find(c => c.id === 'backend-balance-20261017-after').results.find(r => r.metric === 'shell-ms');
  const points = context.historyPoints(context.seriesKey(r));
  assert.equal(points.length, 7);
  assert.equal(points[0].date, '2026-10-03');
  assert.equal(points.at(-1).id, 'backend-balance-20261017-after');
  for (let i = 1; i < points.length; i++) assert(Date.parse(points[i].date) >= Date.parse(points[i - 1].date));
});

test('startup and playback endpoints are not merged', () => {
  const r = DATA.checkpoints.find(c => c.id === 'backend-balance-20261017-after').results;
  assert.notEqual(context.seriesKey(r.find(r => r.metric === 'visible-ms')), context.seriesKey(r.find(r => r.metric === 'shell-ms')));
});

test('compatible historical checkpoints use the latest available same-version reference', () => withVlcVersion('3.0.23', () => {
  const r = DATA.checkpoints.find(c => c.id === 'players-20261014').results.find(r => r.metric === 'visible-ms');
  const b = context.baseline(r);
  assert.equal(b.value, 1952.0802);
  assert.equal(b.measuredAt, '2026-10-17');
  assert.equal(b.label, 'VLC 3.0.23');
}));

test('a newer incompatible VLC reference leaves older comparisons unavailable', () => {
  const legacy = DATA.checkpoints.find(c => c.id === 'players-20261014').results.find(r => r.metric === 'visible-ms');
  const current = DATA.checkpoints.find(c => c.id === 'rival-h264-20261020-current').results.find(r => r.metric === 'cpu-core');
  const previous = DATA.references;
  const measured = previous[current.reference];
  DATA.references = {...previous, 'test-incompatible-newest': {...measured, version: '99.0.0'}};
  try {
    assert.equal(context.baseline(legacy), null);
    const b = context.baseline(current);
    assert.equal(b.label, 'VLC 99.0.0');
    assert.equal(b.measuredAt, measured.measuredAt);
    assert.equal(b.value, measured.results.find(r => r.metric === current.metric && r.protocol === current.protocol).value);
  } finally { DATA.references = previous; }
});

test('recorded one-processor CPU agrees with roughly one percent total capacity', () => withVlcVersion('3.0.23', () => {
  const r = DATA.checkpoints.find(c => c.id === 'backend-balance-20261017-after').results.find(r => r.metric === 'cpu-core');
  const b = context.baseline(r);
  assert(Math.abs(context.machineCpu(r) - 0.9224665486604366) < 1e-9);
  assert(Math.abs(context.machineCpu(b) - 0.1534810471996232) < 1e-9);
  assert.equal(context.machineCpu({...r, environment: 'unknown-machine'}), null);
  assert.equal(context.machineCpu({...r, metric: 'private-mib'}), null);
}));

test('new playback protocols get their own map history and CPU summary source', () => {
  const legacy = DATA.checkpoints.find(c => c.id === 'backend-balance-20261017-after');
  const cp = {id: 'test-audio-checkpoint', results: legacy.results
    .filter(r => r.protocol === 'playback-1080p30')
    .map(r => ({...r, protocol: 'test-audio-protocol', workload: 'audio fixture'}))};
  DATA.protocols['test-audio-protocol'] = {category: 'Playback'};
  ordered.push(cp);
  try {
    const origin = context.playerOrigin(cp);
    assert.equal(origin.protocol, 'test-audio-protocol');
    assert.equal(context.resourceHistory(origin).map(c => c.id).join(','), cp.id);
    assert(!context.resourceHistory(context.playerOrigin(legacy)).some(c => c.id === cp.id));
    const cpu = cp.results.find(r => r.metric === 'cpu-core' && context.sameWorkload(r, origin));
    assert.equal(context.machineCpu(cpu), cpu.value / 24);
  } finally {
    ordered.pop(); delete DATA.protocols['test-audio-protocol'];
  }
});

test('an older VLC version cannot displace the newest version', () => {
  const refs = DATA.references;
  refs['test-new-version'] = {...refs['vlc-20261017'], version: '4.0.0', measuredAt: '2026-10-18'};
  refs['test-old-version-new-date'] = {...refs['vlc-20261017'], version: '3.0.23', measuredAt: '2026-10-19'};
  assert(context.latestVlcReferences().every(r => r.version === '4.0.0'));
  delete refs['test-new-version']; delete refs['test-old-version-new-date'];
});

test('compatible replays connect saved history and retain the original VLC date', () => withVlcVersion('3.0.23', () => {
  const cp = DATA.checkpoints.find(c => c.id === 'direct-nv12-20261018-visible-once');
  const r = cp.results.find(r => r.metric === 'cpu-core');
  const points = context.historyPoints(context.seriesKey(r));
  const latestRecorded = DATA.checkpoints.flatMap(c => c.results
    .filter(v => v.metric === r.metric && v.historySeries === r.historySeries &&
      v.environment === r.environment && v.workload === r.workload && v.value !== null)
    .map(v => ({id: c.id, date: v.measuredAt || c.measuredAt})))
    .sort((a, b) => Date.parse(a.date) - Date.parse(b.date)).at(-1);
  assert(points.length >= 5);
  assert(points.some(p => p.id === 'balanced-overlay-20261018-released'));
  assert(points.some(p => p.id === cp.id));
  assert.equal(points.at(-1).id, latestRecorded.id);
  const b = context.baseline(r);
  assert.equal(b.measuredAt, '2026-10-18T18:57:51.4393218Z');
  assert.equal(b.value, 5.091447168676399);
  assert.notEqual(b.session, r.session);
  assert.equal(r.reference, undefined);
  assert.equal(context.baseline({...r, environment: 'different-machine'}), null);
  assert.equal(context.baseline({...r, workload: 'different-fixture'}), null);
  assert(context.resourceHistory(context.playerOrigin(cp)).some(c => c.id === 'balanced-overlay-20261018-released'));
  const mapPoints = context.resourcePoints(context.playerOrigin(cp));
  assert(mapPoints.length >= 5);
  assert(mapPoints.some(p => p.id === 'balanced-overlay-20261018-released'));
  assert(mapPoints.some(p => p.id === cp.id));
  assert.equal(mapPoints.at(-1).id, latestRecorded.id);
  assert.equal(mapPoints.find(p => p.id === 'balanced-overlay-20261018-released').y, 174.515625);
  for (let i = 1; i < mapPoints.length; i++) assert(Date.parse(mapPoints[i].date) >= Date.parse(mapPoints[i - 1].date));
}));

test('ambiguous inferred history groups leave observations separate', () => {
  const r = {metric: 'cpu-core', protocol: 'test-ambiguous', workload: 'fixture', environment: 'machine'};
  const group = {label: 'test', method: 'test', workload: r.workload, protocols: [r.protocol]};
  DATA.historySeries['test-group-a'] = group;
  DATA.historySeries['test-group-b'] = group;
  try {
    assert.equal(context.historyGroup(r), null);
    assert.equal(context.seriesKey(r), context.key(r));
    assert.equal(context.historyGroup({...r, historySeries: 'test-group-a'}), 'test-group-a');
  } finally {
    delete DATA.historySeries['test-group-a']; delete DATA.historySeries['test-group-b'];
  }
});

test('1, 5, 10 and 200 retain detail without reserving the empty gap', () => {
  const a = context.numericAxis([1, 5, 10, 200], 0, 700, 'cuts');
  assert.equal(a.breaks.length, 1);
  assert(a.breaks[0].from > 10 && a.breaks[0].to < 200);
  assert(Math.abs(a.breaks[0].end - a.breaks[0].start) <= 20.01);
  const firstRate = (a.scale(5) - a.scale(1)) / 4;
  const secondRate = (a.scale(10) - a.scale(5)) / 5;
  assert(Math.abs(firstRate - secondRate) < 1e-8);
});

test('only two widely separated readings still produce a cut', () => {
  assert.equal(context.numericAxis([10, 200], 0, 500, 'cuts').breaks.length, 1);
});

test('VLC cannot compress Aperture on either resource axis', () => {
  const x = context.numericAxis([111.7763, 123.8871, 141.5932, 1952.0802], 0, 650, 'cuts');
  const y = context.numericAxis([255.8125, 256.58, 251.31, 144.73], 400, 0, 'cuts');
  assert.equal(x.breaks.length, 1); assert.equal(y.breaks.length, 1);
  assert(x.scale(141.5932) - x.scale(111.7763) > 100);
  assert(Math.abs(y.scale(256.58) - y.scale(251.31)) > 100);
});

test('two Aperture readings and one distant VLC point keep local detail', () => {
  const values = [118.4283, 132.7196, 1854.4517];
  const axis = context.numericAxis(values, 0, 650, 'cuts');
  assert.equal(axis.breaks.length, 1);
  assert(axis.scale(values[1]) - axis.scale(values[0]) > 100);
  assert(axis.segments.at(-1).hi - axis.segments.at(-1).lo < 50);
  assert.equal(values.map(v => Number.isFinite(axis.scale(v))).every(Boolean), true);
});

test('linear axes do not imply breaks and log axes reject zero', () => {
  assert.equal(context.numericAxis([1, 5, 10, 200], 0, 500, 'linear').breaks.length, 0);
  assert.throws(() => context.numericAxis([0, 10], 0, 500, 'log'), /positive/);
  assert(Number.isFinite(context.numericAxis([0], 0, 500, 'cuts').scale(0)));
});

test('every recorded metric maps to a finite monotonic position', () => {
  for (const [k] of context.historyCatalog()) {
    const points = context.historyPoints(k);
    const values = [...new Set(points.map(p => p.y))].sort((a, b) => a - b);
    if (!values.length) continue;
    for (const mode of ['cuts', 'linear']) {
      const a = context.numericAxis(values, 0, 650, mode);
      let previous = -Infinity;
      for (const value of values) {
        const position = a.scale(value);
        assert(Number.isFinite(position) && position >= -1e-6 && position <= 650.000001);
        assert(position >= previous); previous = position;
      }
      for (const cut of a.breaks) assert(!values.some(v => v > cut.from && v < cut.to));
    }
  }
});

test('cross-PC diff matches Desktop (RTX 4070S) and This PC (AMD 860M) bidirectionally', () => {
  const desktopAudit = context.ordered.find(c => c.id === 'audit-optimizations-20261103-after');
  const laptop = context.ordered.find(c => c.id === 'laptop-amd860m-20261114');
  assert(desktopAudit && laptop);
  assert.strictEqual(context.checkpointPcTag(desktopAudit), 'Desktop · RTX 4070S');
  assert.strictEqual(context.checkpointPcTag(laptop), 'This PC · AMD 860M');

  const deskRss = desktopAudit.results.find(r => r.metric === 'elapsed-ms' && /ParseRssFeed/.test(r.workload));
  const peerLaptop = context.peerPcResult(deskRss);
  assert(peerLaptop && peerLaptop.exact === true && peerLaptop.r.environment === 'ryzen-ai7-350-radeon860m');

  const laptopRss = laptop.results.find(r => r.metric === 'elapsed-ms' && /ParseRssFeed/.test(r.workload));
  const peerDesktop = context.peerPcResult(laptopRss);
  assert(peerDesktop && peerDesktop.exact === true && context.pcGroup(peerDesktop.r.environment) === 'desktop');

  const laptopTsc = laptop.results.find(r => r.metric === 'cpu-core' && /tsc/i.test(r.protocol));
  const desktopTscPeer = context.peerPcResult(laptopTsc);
  assert(desktopTscPeer && /tsc/i.test(desktopTscPeer.r.protocol) && context.pcGroup(desktopTscPeer.r.environment) === 'desktop');

  const laptopRawCpu = laptop.results.find(r => r.metric === 'cpu-core' && !/tsc/i.test(r.protocol));
  const desktopRawPeer = context.peerPcResult(laptopRawCpu);
  assert(desktopRawPeer && !/tsc/i.test(desktopRawPeer.r.protocol) && context.pcGroup(desktopRawPeer.r.environment) === 'desktop');
});

test('This PC keeps its own VLC 3.0.24 baseline isolated from Desktop VLC 3.0.24', () => {
  const laptop = context.ordered.find(c => c.id === 'laptop-amd860m-20261114');
  const desktopH264 = context.ordered.find(c => c.id === 'rival-h264-20261020-pruned');
  const laptopGpu = laptop.results.find(r => r.metric === 'gpu-mib');
  const desktopGpu = desktopH264.results.find(r => r.metric === 'gpu-mib');
  const lb = context.baseline(laptopGpu);
  const db = context.baseline(desktopGpu);
  assert(lb && lb.environment === 'ryzen-ai7-350-radeon860m');
  assert(db && db.environment === 'ryzen3900x-4070super-20261020');
  assert(lb.value !== db.value);
});
process.stdout.write(`${checks} chart checks passed.\n`);
