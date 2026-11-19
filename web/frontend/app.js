'use strict';
/* =====================================================================
   MedClip frontend
   ---------------------------------------------------------------------
   Talks to the hub server (server/server.py) under /api. If no hub
   answers (for example when index.html is opened straight from disk),
   it switches to demo data so the UI can still be explored.

   - The central hub reports every clip that is switched on; the server
     names them "Clip 1", "Clip 2", ... In Setup the user picks a free
     clip and types the medicine and its dose times.
   - The hub records an "intake" when a clip signals that its medicine
     was taken, or when the patient ticks the dose in the calendar.
   - Each scheduled dose then gets a status:
       taken within onTimeWindowMin of schedule  -> on time
       taken later                               -> late
       nothing recorded missedAfterMin after it  -> missed
   ===================================================================== */
const HUB_CONFIG = {
  apiBase: '/api',
  onTimeWindowMin: 30,
  missedAfterMin: 180,
  refreshSec: 60,        // normal refresh
  setupRefreshSec: 5     // faster on the Setup page, so newly switched-on clips appear quickly
};
let DEMO = false;

/* ------------------------- helpers ------------------------- */
const $ = s => document.querySelector(s);
const pad = n => String(n).padStart(2, '0');
const key = d => `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
const hm = d => `${pad(d.getHours())}:${pad(d.getMinutes())}`;
const addDays = (d, n) => { const x = new Date(d); x.setDate(x.getDate() + n); return x; };
const startOfDay = d => new Date(d.getFullYear(), d.getMonth(), d.getDate());
const parseKey = s => { const [y, m, d] = s.split('-').map(Number); return new Date(y, m - 1, d); };
const mondayOf = d => addDays(startOfDay(d), -((d.getDay() + 6) % 7));
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const DAYS_SHORT = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const DAYS_LONG = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
const fmtDay = d => `${DAYS_SHORT[d.getDay()]} ${d.getDate()} ${MONTHS[d.getMonth()].slice(0, 3)}`;
const fmtLong = d => `${DAYS_LONG[d.getDay()]} ${d.getDate()} ${MONTHS[d.getMonth()]}`;
const STATUS_LABEL = { on_time: 'On time', late: 'Late', missed: 'Missed', upcoming: 'Upcoming', due: 'Due now' };
const marker = (s, title) => `<span class="mk ${s}" ${title ? `title="${esc(title)}"` : ''} aria-hidden="true"></span>`;
const statusTag = s => `<span class="status ${s}">${marker(s)}${STATUS_LABEL[s]}</span>`;
const plural = (n, w) => `${n} ${w}${n === 1 ? '' : 's'}`;
const CLIP_PATH = '<path d="M20 11l-7.5 7.5a5 5 0 0 1-7-7L13 4a3.3 3.3 0 0 1 4.7 4.7l-7.5 7.5a1.7 1.7 0 0 1-2.4-2.4L14.5 7"/>';
const clipIcon = (size = 13) => `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">${CLIP_PATH}</svg>`;
const clipBadge = name => `<span class="clip-badge" title="Clip">${clipIcon()}${esc(name)}</span>`;
const TICK = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="3.4" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.5l4.5 4.5L19 7.5"/></svg>';
const CLOSE = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M6 6l12 12M18 6L6 18"/></svg>';
function nameFromEmail(e) { const p = (e.split('@')[0] || 'Patient').replace(/[._-]+/g, ' '); return p.replace(/\b\w/g, c => c.toUpperCase()); }
function daysText(days) {
  if (days.length === 7) return 'every day';
  return [1, 2, 3, 4, 5, 6, 0].filter(d => days.includes(d)).map(d => DAYS_SHORT[d]).join(', ');
}
function ago(d) {
  const mins = Math.round((Date.now() - d) / 60000);
  if (mins < 1) return 'just now';
  if (mins < 60) return `${mins} min ago`;
  if (mins < 1440) return `${Math.round(mins / 60)} h ago`;
  return fmtDay(d);
}

/* ========================= HUB API ========================= */
const Api = {
  token: null,
  async req(path, opts = {}) {
    let res;
    try {
      res = await fetch(HUB_CONFIG.apiBase + path, {
        ...opts,
        headers: { 'Content-Type': 'application/json', ...(Api.token ? { Authorization: 'Bearer ' + Api.token } : {}) }
      });
    } catch { throw new Error("can't reach the hub"); }
    let body = null;
    try { body = await res.json(); } catch { /* empty body */ }
    if (res.status === 401 && path !== '/login') { signOut(); throw new Error('session expired, sign in again'); }
    if (!res.ok) throw new Error((body && body.error) || `hub responded ${res.status}`);
    return body;
  },
  async detect() {
    try {
      const r = await fetch(HUB_CONFIG.apiBase + '/status', { cache: 'no-store' });
      const j = await r.json();
      DEMO = !(r.ok && j && j.ok);
    } catch { DEMO = true; }
  },
  login: (email, password) => DEMO ? Demo.login(email) : Api.req('/login', { method: 'POST', body: JSON.stringify({ email, password }) }),
  status: () => DEMO ? Demo.status() : Api.req('/status'),
  clips: () => DEMO ? Demo.clips() : Api.req('/clips'),
  forgetClip: clipId => DEMO ? Demo.forgetClip(clipId) : Api.req('/clips/' + encodeURIComponent(clipId), { method: 'DELETE' }),
  medicines: () => DEMO ? Demo.medicines() : Api.req('/medicines'),
  intakes: (from, to) => DEMO ? Demo.intakes(from, to) : Api.req(`/intakes?from=${key(from)}&to=${key(to)}`),
  saveMedicine: m => DEMO ? Demo.saveMedicine(m)
    : m.id ? Api.req('/medicines/' + m.id, { method: 'PUT', body: JSON.stringify(m) })
    : Api.req('/medicines', { method: 'POST', body: JSON.stringify(m) }),
  unpair: id => DEMO ? Demo.unpair(id) : Api.req('/medicines/' + id, { method: 'DELETE' }),
  markTaken: (medId, scheduled, takenAt) => DEMO ? Demo.markTaken(medId, scheduled, takenAt)
    : Api.req('/intakes', { method: 'POST', body: JSON.stringify({ medId, scheduled, takenAt: takenAt ? takenAt.toISOString() : null }) })
};

/* ========================= DEMO DATA ========================= */
const Demo = (() => {
  const today = startOfDay(new Date());
  const start = key(addDays(today, -120));
  const minsAgo = n => new Date(Date.now() - n * 60000).toISOString();
  let nextId = 5;
  const seeded = new Set(['1', '2', '3', '4']);
  const clips = [
    { clipId: 'A1F3B2', name: 'Clip 1', firstSeen: minsAgo(9000), lastSeen: minsAgo(3) },
    { clipId: 'B77C10', name: 'Clip 2', firstSeen: minsAgo(9000), lastSeen: minsAgo(8) },
    { clipId: 'D2905E', name: 'Clip 3', firstSeen: minsAgo(9000), lastSeen: minsAgo(15) },
    { clipId: 'E5A6C1', name: 'Clip 4', firstSeen: minsAgo(9000), lastSeen: minsAgo(40) },
    { clipId: 'C2E19A', name: 'Clip 5', firstSeen: minsAgo(22), lastSeen: minsAgo(1) }
  ];
  const meds = [
    { id: '1', clipId: 'A1F3B2', color: 1, name: 'Metformin', strength: '500 mg', form: 'Tablet', purpose: 'Blood sugar control (type 2 diabetes)', times: ['08:00', '20:00'], days: [0, 1, 2, 3, 4, 5, 6], startDate: start, stock: 34, stockMax: 60, refillAt: 10, instructions: 'Take with breakfast and with dinner. Swallow whole with a glass of water.', sideEffects: ['Nausea', 'Upset stomach', 'Diarrhoea'], prescriber: 'Dr. G. Bianchi', notes: '' },
    { id: '2', clipId: 'B77C10', color: 2, name: 'Lisinopril', strength: '10 mg', form: 'Tablet', purpose: 'Blood pressure', times: ['08:00'], days: [0, 1, 2, 3, 4, 5, 6], startDate: start, stock: 12, stockMax: 30, refillAt: 7, instructions: 'Take at the same time each morning. Stand up slowly if you feel dizzy.', sideEffects: ['Dry cough', 'Dizziness', 'Headache'], prescriber: 'Dr. G. Bianchi', notes: 'Check blood pressure weekly.' },
    { id: '3', clipId: 'D2905E', color: 3, name: 'Atorvastatin', strength: '20 mg', form: 'Tablet', purpose: 'Cholesterol', times: ['21:00'], days: [0, 1, 2, 3, 4, 5, 6], startDate: start, stock: 21, stockMax: 30, refillAt: 7, instructions: 'Take in the evening. Avoid large amounts of grapefruit juice.', sideEffects: ['Muscle aches', 'Headache'], prescriber: 'Dr. L. Conti', notes: '' },
    { id: '4', clipId: 'E5A6C1', color: 4, name: 'Vitamin D3', strength: '1000 IU', form: 'Capsule', purpose: 'Supplement', times: ['13:00'], days: [1, 3, 5], startDate: start, stock: 5, stockMax: 24, refillAt: 6, instructions: 'Take with lunch; works best with a meal that contains some fat.', sideEffects: [], prescriber: 'Dr. L. Conti', notes: '' }
  ];
  const overrides = new Map();
  // deterministic per-dose randomness so history doesn't change between syncs
  function rand(str) { let h = 2166136261; for (const c of str) { h ^= c.charCodeAt(0); h = Math.imul(h, 16777619); } h ^= h >>> 13; h = Math.imul(h, 1274126177); return ((h ^ (h >>> 16)) >>> 0) / 4294967296; }
  function clipTime(m, d, t) {
    if (!seeded.has(m.id)) return null;
    const [h, mi] = t.split(':').map(Number);
    const sched = new Date(d.getFullYear(), d.getMonth(), d.getDate(), h, mi);
    const r = rand(m.id + key(d) + t), r2 = rand(t + key(d) + m.id);
    let taken = null;
    if (r < 0.885) taken = new Date(sched.getTime() + (-10 + r2 * 34) * 60000);
    else if (r < 0.94) taken = new Date(sched.getTime() + (45 + r2 * 110) * 60000);
    return taken && taken <= new Date() ? taken : null;
  }
  function lastSignal(m) {
    for (let i = 0; i < 4; i++) {
      const d = addDays(today, -i);
      if (!m.days.includes(d.getDay())) continue;
      const ts = [...m.times].reverse().map(t => clipTime(m, d, t)).filter(Boolean);
      if (ts.length) return ts[0].toISOString();
    }
    return null;
  }
  const clipName = id => (clips.find(c => c.clipId === id) || {}).name || id;
  return {
    login: async email => ({ token: 'demo', user: { name: nameFromEmail(email), email } }),
    status: async () => {
      const last = meds.map(lastSignal).filter(Boolean).sort().pop() || null;
      return { ok: true, demo: true, lastEvent: last, clips: clips.length, freeClips: clips.filter(c => !meds.some(m => m.clipId === c.clipId)).length };
    },
    clips: async () => clips.map(c => {
      const m = meds.find(x => x.clipId === c.clipId);
      return { ...c, medicineId: m ? m.id : null, medicineName: m ? `${m.name} ${m.strength}`.trim() : null };
    }),
    forgetClip: async clipId => {
      if (meds.some(m => m.clipId === clipId)) throw new Error('unpair the medicine from this clip first');
      const i = clips.findIndex(c => c.clipId === clipId); if (i >= 0) clips.splice(i, 1); return { ok: true };
    },
    medicines: async () => meds.map(m => ({ ...m, clipName: clipName(m.clipId), lastSignal: lastSignal(m) })),
    intakes: async (from, to) => {
      const out = [];
      for (let d = startOfDay(from); d <= to; d = addDays(d, 1)) {
        for (const m of meds) {
          if (!m.days.includes(d.getDay()) || d < parseKey(m.startDate)) continue;
          for (const t of m.times) {
            const k = `${m.id}|${key(d)}T${t}`;
            if (overrides.has(k)) { out.push({ medId: m.id, scheduled: `${key(d)}T${t}`, ...overrides.get(k) }); continue; }
            const taken = clipTime(m, d, t);
            if (taken) out.push({ medId: m.id, scheduled: `${key(d)}T${t}`, takenAt: taken.toISOString(), source: 'clip' });
          }
        }
      }
      return out;
    },
    saveMedicine: async m => {
      if (!clips.some(c => c.clipId === m.clipId)) throw new Error("that clip isn't connected to the hub");
      const clash = meds.find(x => x.clipId === m.clipId && x.id !== m.id);
      if (clash) throw new Error(`${clipName(m.clipId)} is already paired with ${clash.name}`);
      if (m.id) { const j = meds.findIndex(x => x.id === m.id); meds[j] = { ...meds[j], ...m }; return meds[j]; }
      const saved = { ...m, id: String(nextId++), startDate: key(new Date()) };
      meds.push(saved); return saved;
    },
    unpair: async id => { const i = meds.findIndex(m => m.id === id); if (i >= 0) meds.splice(i, 1); return { ok: true }; },
    markTaken: async (medId, scheduled, takenAt) => { overrides.set(`${medId}|${scheduled}`, { takenAt: takenAt ? takenAt.toISOString() : null, source: 'manual' }); return { ok: true }; }
  };
})();

/* ========================= STATE ========================= */
const state = {
  user: null, meds: [], intakes: new Map(), clips: [], status: null,
  now: new Date(), lastSync: null,
  calMonth: null, selDay: null, calMode: 'week', weekStart: null,
  editingId: null, confirmRemove: null, confirmForget: null
};
const medColor = m => `var(--med-${((Number(m.color) || 1) - 1) % 6 + 1})`;
const medById = id => state.meds.find(m => m.id === String(id));

async function loadData() {
  const [meds, status, clips] = await Promise.all([Api.medicines(), Api.status(), Api.clips()]);
  state.now = new Date();
  state.meds = meds.map(m => ({ ...m, id: String(m.id) }));
  state.status = status; state.clips = clips;
  const earliest = state.meds.reduce((a, m) => Math.min(a, +parseKey(m.startDate)), +startOfDay(state.now));
  const list = await Api.intakes(new Date(earliest), state.now);
  state.intakes = new Map(list.map(i => [`${i.medId}|${i.scheduled}`, { takenAt: i.takenAt ? new Date(i.takenAt) : null, source: i.source }]));
  state.lastSync = new Date();
}

/* every scheduled dose on a day, with its status */
function dosesOn(day) {
  const d = startOfDay(day), out = [];
  for (const m of state.meds) {
    if (!m.days.includes(d.getDay()) || d < parseKey(m.startDate)) continue;
    for (const t of m.times) {
      const [h, mi] = t.split(':').map(Number);
      const sched = new Date(d.getFullYear(), d.getMonth(), d.getDate(), h, mi);
      if (m.createdAt && sched < new Date(m.createdAt)) continue;   // before the clip was paired
      const k = `${m.id}|${key(d)}T${t}`;
      const rec = state.intakes.get(k);
      const takenAt = rec ? rec.takenAt : null;
      const byYou = !!rec && rec.source === 'manual';
      const mins = (state.now - sched) / 60000;
      let status;
      if (takenAt) status = (takenAt - sched) / 60000 <= HUB_CONFIG.onTimeWindowMin ? 'on_time' : 'late';
      else if (mins < 0) status = 'upcoming';
      else if (mins < HUB_CONFIG.missedAfterMin) status = 'due';
      else status = 'missed';
      out.push({ med: m, time: t, sched, takenAt, status, k, byYou });
    }
  }
  return out.sort((a, b) => a.sched - b.sched);
}
function dosesBetween(from, to) { const out = []; for (let d = startOfDay(from); d <= to; d = addDays(d, 1)) out.push(...dosesOn(d)); return out; }
const isDone = s => s === 'on_time' || s === 'late' || s === 'missed';
const isTaken = s => s === 'on_time' || s === 'late';

/* ========================= AUTH & ROUTING ========================= */
const store = {
  get() { try { return JSON.parse(sessionStorage.getItem('medclip.session')); } catch { return null; } },
  set(v) { try { sessionStorage.setItem('medclip.session', JSON.stringify(v)); } catch { /* storage unavailable */ } },
  clear() { try { sessionStorage.removeItem('medclip.session'); } catch { /* storage unavailable */ } }
};
const VIEWS = {
  dashboard: { title: 'Dashboard', sub: () => `${fmtLong(state.now)}. Here is what the clips recorded.`, render: () => renderDashboard() },
  calendar: { title: 'Calendar', sub: () => 'Every medicine to take each day. Tick it off when it is taken.', render: () => renderCalendar() },
  medicines: { title: 'Medicines', sub: () => 'Each medicine and the clip on its box.', render: () => renderMedicines() },
  setup: { title: 'Setup', sub: () => 'Pick a clip the hub has found, then add the medicine it is on.', render: () => renderSetup() }
};
const currentView = () => (VIEWS[location.hash.slice(1)] ? location.hash.slice(1) : 'dashboard');

function route() {
  if (!state.user) { showLogin(); return; }
  const v = currentView();
  $('#login').hidden = true; $('#app').hidden = false;
  for (const name of Object.keys(VIEWS)) $('#view-' + name).hidden = name !== v;
  document.querySelectorAll('.nav a').forEach(a => { if (a.dataset.view === v) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current'); });
  $('#viewTitle').textContent = VIEWS[v].title;
  $('#viewSub').textContent = VIEWS[v].sub();
  VIEWS[v].render();
  document.title = `${VIEWS[v].title} | MedClip`;
}
function showLogin() { $('#app').hidden = true; $('#login').hidden = false; document.title = 'Sign in | MedClip'; }
window.addEventListener('hashchange', () => { hideTip(); route(); });

$('#loginForm').addEventListener('submit', async e => {
  e.preventDefault();
  const email = $('#email').value.trim(), pw = $('#password').value;
  const err = $('#loginError'); err.textContent = '';
  if (!/^\S+@\S+\.\S+$/.test(email)) { err.textContent = 'Enter a valid email address, like name@example.com.'; $('#email').focus(); return; }
  if (pw.length < 4) { err.textContent = 'Password must be at least 4 characters.'; $('#password').focus(); return; }
  const btn = $('#loginBtn'); btn.disabled = true; btn.textContent = 'Signing in…';
  try {
    const d = await Api.login(email, pw);
    Api.token = d.token; state.user = d.user;
    store.set({ user: d.user, token: d.token });
    await loadData(); setUser(); updateHub();
    if (VIEWS[location.hash.slice(1)]) route(); else location.hash = 'dashboard';
  } catch (ex) {
    err.textContent = `Sign-in failed: ${ex.message}.`;
  } finally { btn.disabled = false; btn.textContent = 'Sign in'; }
});
function signOut() { store.clear(); state.user = null; Api.token = null; history.replaceState(null, '', location.pathname); closeDrawer(); showLogin(); }
$('#logoutBtn').addEventListener('click', signOut);
function setUser() { $('#whoName').textContent = state.user.name; $('#whoMail').textContent = state.user.email; }

/* ========================= HUB STATUS & SYNC ========================= */
function updateHub() {
  if (!state.lastSync) return;
  const el = $('#hubStatus'); el.classList.toggle('demo', DEMO);
  const last = state.status && state.status.lastEvent ? new Date(state.status.lastEvent) : null;
  const label = DEMO ? 'Demo data' : 'Hub online';
  const detail = last ? `last clip signal ${ago(last)}` : `synced ${ago(state.lastSync)}`;
  $('#hubText').innerHTML = `${label} <span class="muted">${detail}</span>`;
}
async function refresh(quiet) {
  const focusedKey = document.activeElement && document.activeElement.dataset ? document.activeElement.dataset.k : null;
  const before = JSON.stringify([state.clips, state.meds.map(m => [m.id, m.stock])]);
  try {
    await loadData();
    const same = JSON.stringify([state.clips, state.meds.map(m => [m.id, m.stock])]) === before;
    if (!(quiet && same && currentView() === 'setup')) route();
    if (!quiet) toast('Synced with hub');
  }
  catch (ex) { if (!quiet) toast(`Sync failed: ${ex.message}`); }
  updateHub();
  if (focusedKey) { const el = document.querySelector(`input[data-k="${CSS.escape(focusedKey)}"]`); if (el) el.focus(); }
}
$('#syncBtn').addEventListener('click', async () => {
  const b = $('#syncBtn'); b.disabled = true; b.textContent = 'Syncing…';
  await refresh(false);
  b.disabled = false; b.textContent = 'Sync now';
});
let lastAuto = Date.now();
setInterval(() => {
  if (!state.user || $('#drawerRoot').firstChild) return;
  const every = (currentView() === 'setup' ? HUB_CONFIG.setupRefreshSec : HUB_CONFIG.refreshSec) * 1000;
  if (Date.now() - lastAuto >= every) { lastAuto = Date.now(); refresh(true); }
}, 1000);
function toast(msg) { const t = $('#toast'); t.textContent = msg; t.hidden = false; clearTimeout(toast.t); toast.t = setTimeout(() => { t.hidden = true; }, 2600); }

/* tooltip */
const tip = $('#tip');
function showTip(html, x, y) { tip.innerHTML = html; tip.hidden = false; tip.style.left = x + 'px'; tip.style.top = y + 'px'; }
function hideTip() { tip.hidden = true; }

/* ========================= DASHBOARD ========================= */
function renderDashboard() {
  const now = state.now, today = startOfDay(now);
  const from30 = addDays(today, -29);
  const last30 = dosesBetween(from30, now).filter(d => isDone(d.status));
  const onTime = last30.filter(d => d.status === 'on_time').length;
  const late = last30.filter(d => d.status === 'late').length;
  const missed = last30.filter(d => d.status === 'missed').length;
  const total = last30.length;
  const pct = total ? Math.round(onTime / total * 100) : 0;

  // days in a row without a missed dose, counting back from today
  let streak = 0;
  for (let i = 0; i < 400; i++) {
    const ds = dosesOn(addDays(today, -i));
    if (ds.some(x => x.status === 'missed')) break;
    if (ds.some(x => isDone(x.status))) streak++;
  }

  const R = 70, C = 2 * Math.PI * R;
  $('#heroCard').innerHTML = !state.meds.length ? `
    <div class="hero-text"><h2>No medicines yet</h2><p>Pair a clip with each medicine to start tracking doses.</p><a class="btn" href="#setup">Pair a clip</a></div>` : `
    <div class="ring" role="img" aria-label="${total ? pct + '% of doses taken on time' : 'No doses due yet'}">
      <svg viewBox="0 0 168 168"><circle cx="84" cy="84" r="${R}" fill="none" stroke="#e7ecea" stroke-width="16"/>
      ${pct > 0 ? `<circle cx="84" cy="84" r="${R}" fill="none" stroke="var(--accent)" stroke-width="16" stroke-linecap="round" stroke-dasharray="${C * pct / 100} ${C}"/>` : ''}</svg>
      <div class="val"><div>${total ? `<b class="num">${pct}%</b><span>on time</span>` : '<b class="dim">–</b><span>no doses yet</span>'}</div></div>
    </div>
    <div class="hero-text">
      <h2>${!total ? 'Waiting for the first dose' : pct >= 90 ? 'Right on schedule' : pct >= 75 ? 'Mostly on schedule' : 'Several doses slipped'}</h2>
      <p>${total ? `${onTime} of ${total} doses in the last 30 days were taken within ${HUB_CONFIG.onTimeWindowMin} minutes of their scheduled time.`
        : 'No dose has been due since the clip was paired. Your on-time rate appears here after the first one.'}</p>
      <div class="mini-stats">
        <div><b class="num">${onTime + late}<small style="font-size:14px;color:var(--muted)">/${total}</small></b><span>Taken</span></div>
        <div><b class="num">${late}</b><span>Taken late</span></div>
        <div><b class="num">${missed}</b><span>Missed</span></div>
        <div><b class="num">${streak}</b><span>${streak === 1 ? 'Day' : 'Days'} without a miss</span></div>
      </div>
    </div>`;

  // today + next dose
  const todays = dosesOn(today);
  const next = [...todays, ...dosesOn(addDays(today, 1))].find(d => d.status === 'upcoming' || d.status === 'due');
  let nextHtml = '';
  if (next) {
    const mins = Math.round((next.sched - now) / 60000);
    const when = next.status === 'due' ? 'Now' : mins < 60 ? `in ${mins} min` : `in ${Math.floor(mins / 60)} h ${mins % 60} min`;
    const firstLine = (next.med.instructions || '').split('.')[0];
    nextHtml = `<div class="next"><div class="slot">${clipIcon(24)}</div>
      <div class="what"><b>${esc(next.med.name)} ${esc(next.med.strength)}</b><span>${key(next.sched) !== key(now) ? 'Tomorrow. ' : ''}${firstLine ? esc(firstLine) + '.' : ''}</span></div>
      <div class="when num">${next.time}<small>${when}</small></div></div>`;
  }
  const takenToday = todays.filter(d => isTaken(d.status)).length;
  $('#todayCard').innerHTML = `
    <div class="card-head"><h2>Today</h2><span class="note num">${takenToday} of ${todays.length} taken</span></div>
    ${nextHtml}
    <ul class="dose-list">${todays.map(d => `<li>
      <span class="swatch" style="background:${medColor(d.med)}"></span>
      <span class="t num">${d.time}</span>
      <span class="m"><b>${esc(d.med.name)}</b> ${esc(d.med.strength)}<small>${d.takenAt ? `${d.byYou ? 'Ticked by you' : 'Clip signal'} at ${hm(d.takenAt)}` : esc(d.med.clipName)}</small></span>
      ${statusTag(d.status)}</li>`).join('') || '<li>No doses scheduled today.</li>'}</ul>`;

  // weekly organizer (Mon–Sun)
  const monday = mondayOf(today);
  $('#legendWeek').innerHTML = legendHtml();
  let org = '';
  for (let i = 0; i < 7; i++) {
    const d = addDays(monday, i), ds = dosesOn(d);
    const taken = ds.filter(x => isTaken(x.status)).length;
    const cls = key(d) === key(today) ? 'today' : d > today ? 'future' : '';
    org += `<button class="comp ${cls}" type="button" data-day="${key(d)}" aria-label="${fmtLong(d)}: ${taken} of ${ds.length} taken">
      <span class="d">${DAYS_SHORT[d.getDay()]}${cls === 'today' ? '<span class="today-tag">Today</span>' : ''}</span>
      <span class="dt">${d.getDate()} ${MONTHS[d.getMonth()].slice(0, 3)}</span>
      <span class="pills">${ds.map(x => marker(x.status, `${x.time} ${x.med.name}: ${STATUS_LABEL[x.status]}`)).join('')}</span>
      <span class="cnt num">${taken}/${ds.length} <small>taken</small></span></button>`;
  }
  $('#organizer').innerHTML = org;
  $('#organizer').querySelectorAll('.comp').forEach(b => { b.onclick = () => {
    state.selDay = parseKey(b.dataset.day); state.weekStart = mondayOf(state.selDay);
    state.calMonth = new Date(state.selDay.getFullYear(), state.selDay.getMonth(), 1); location.hash = 'calendar';
  }; });

  // per-medicine progress
  const weekDoses = dosesBetween(monday, addDays(monday, 6));
  const all30 = dosesBetween(from30, now);
  $('#medTable').innerHTML = `<thead><tr><th>Medicine</th><th>This week</th><th>Last 30 days</th><th class="hide-sm">On time</th></tr></thead><tbody>${
    state.meds.map(m => {
      const w = weekDoses.filter(d => d.med.id === m.id), wt = w.filter(d => isTaken(d.status)).length;
      const wLeft = w.filter(d => d.status === 'upcoming' || d.status === 'due').length;
      const t = all30.filter(d => d.med.id === m.id && isDone(d.status)), tt = t.filter(d => isTaken(d.status)).length;
      const ot = t.filter(d => d.status === 'on_time').length;
      const p = t.length ? tt / t.length * 100 : 0;
      return `<tr data-med="${m.id}" tabindex="0">
        <td><div class="medname"><span class="swatch" style="background:${medColor(m)}"></span><div><b>${esc(m.name)} ${esc(m.strength)}</b><small>${plural(m.times.length * m.days.length, 'dose')} a week, ${daysText(m.days)}</small></div></div></td>
        <td><span class="frac num">${wt}<small> / ${w.length}</small></span><div style="font-size:12px;color:var(--muted)">${wLeft ? wLeft + ' still to take' : 'Week complete'}</div></td>
        <td><span class="frac num">${tt}<small> / ${t.length}</small></span><div class="bar"><i style="width:${p}%"></i></div></td>
        <td class="hide-sm"><span class="pct num">${t.length ? Math.round(ot / t.length * 100) + '%' : '–'}</span></td></tr>`;
    }).join('') || '<tr><td colspan="4">No medicines paired yet.</td></tr>'}</tbody>`;
  $('#medTable').querySelectorAll('tbody tr[data-med]').forEach(r => { r.onclick = () => openMed(r.dataset.med); r.onkeydown = e => { if (e.key === 'Enter') openMed(r.dataset.med); }; });

  renderDailyChart(today);
  renderFeed(today);
}

function legendHtml() { return ['on_time', 'late', 'missed', 'upcoming'].map(s => `<span>${marker(s)}${STATUS_LABEL[s]}</span>`).join(''); }

function renderDailyChart(today) {
  const days = []; for (let i = 14; i >= 1; i--) days.push(addDays(today, -i));
  const data = days.map(d => {
    const ds = dosesOn(d).filter(x => isDone(x.status)); const on = ds.filter(x => x.status === 'on_time').length;
    return { d, total: ds.length, on, late: ds.filter(x => x.status === 'late').length, missed: ds.filter(x => x.status === 'missed').length, pct: ds.length ? on / ds.length * 100 : 0 };
  });
  const W = 640, H = 220, L = 38, Rr = 6, T = 10, B = 28, iw = W - L - Rr, ih = H - T - B, bw = iw / data.length;
  let svg = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Daily on-time rate for the last 14 days">`;
  for (const g of [0, 50, 100]) { const y = T + ih - ih * g / 100; svg += `<line class="grid-line" x1="${L}" x2="${W - Rr}" y1="${y}" y2="${y}"/><text class="axis-label" x="${L - 8}" y="${y + 4}" text-anchor="end">${g}%</text>`; }
  data.forEach((v, i) => {
    const x = L + i * bw, h = Math.max(ih * v.pct / 100, v.total ? 2 : 0), y = T + ih - h, w = bw - 8;
    svg += `<rect class="hit" data-i="${i}" x="${x}" y="${T}" width="${bw}" height="${ih}" fill="transparent"/>`;
    if (h > 0) svg += `<path class="bar-mark" data-i="${i}" d="M${x + 4} ${T + ih} V${y + Math.min(4, h)} q0 -${Math.min(4, h)} 4 -${Math.min(4, h)} h${w - 8} q4 0 4 ${Math.min(4, h)} V${T + ih} Z" pointer-events="none"/>`;
    if (i % 2 === 0) svg += `<text class="axis-label" x="${x + bw / 2}" y="${H - 8}" text-anchor="middle">${v.d.getDate()}</text>`;
  });
  svg += '</svg>';
  const el = $('#dailyChart'); el.innerHTML = svg;
  el.querySelectorAll('.hit').forEach(r => {
    const i = +r.dataset.i, v = data[i], bar = el.querySelector(`.bar-mark[data-i="${i}"]`);
    r.addEventListener('mousemove', e => {
      if (bar) bar.classList.add('hover');
      const body = v.total ? `${v.on} of ${v.total} on time${v.late ? `, ${v.late} late` : ''}${v.missed ? `, ${v.missed} missed` : ''}` : 'No doses scheduled';
      showTip(`<b>${fmtDay(v.d)}: ${Math.round(v.pct)}%</b>${body}`, e.clientX, e.clientY);
    });
    r.addEventListener('mouseleave', () => { if (bar) bar.classList.remove('hover'); hideTip(); });
  });
}

function renderFeed(today) {
  const items = [];
  const WARN = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="#7a5000" stroke-width="2.4" stroke-linecap="round"><path d="M12 8v5M12 17h.01"/><path d="M10.3 3.9L2.4 18a2 2 0 0 0 1.7 3h15.8a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/></svg>';
  for (const c of state.clips.filter(x => !x.medicineId)) { const t = new Date(c.lastSeen || c.firstSeen); items.push({ t, warn: true, title: `${c.name} is ready to pair`, sub: 'Switched on and connected, but not paired with a medicine yet. Pair it in Setup.', ts: ago(t) }); }
  for (const m of state.meds) if (m.stock <= m.refillAt) items.push({ t: state.now, warn: true, title: `${m.name} is running low`, sub: `${m.stock} left in the box. Refill soon.`, ts: 'Now' });
  for (const d of dosesBetween(addDays(today, -2), state.now)) {
    if (isTaken(d.status)) items.push({ t: d.takenAt, status: d.status, title: `${d.med.name} ${d.status === 'late' ? 'taken late' : 'taken'}`, sub: d.byYou ? `Ticked by you at ${hm(d.takenAt)} (scheduled ${d.time})` : `${d.med.clipName} signalled at ${hm(d.takenAt)} (scheduled ${d.time})` });
    else if (d.status === 'missed') items.push({ t: new Date(d.sched.getTime() + HUB_CONFIG.missedAfterMin * 60000), status: 'missed', title: `${d.med.name} missed`, sub: `No signal from ${d.med.clipName} for the ${d.time} dose.` });
  }
  items.sort((a, b) => (b.warn ? 1 : 0) - (a.warn ? 1 : 0) || b.t - a.t);
  $('#feed').innerHTML = items.slice(0, 7).map(it => `<li>
    <span class="ico ${it.warn ? 'warn' : ''}">${it.warn ? WARN : marker(it.status)}</span>
    <span><b>${esc(it.title)}</b><small>${esc(it.sub)}</small></span>
    <span class="ts">${esc(it.ts || (key(it.t) === key(today) ? 'Today' : DAYS_SHORT[it.t.getDay()] + ' ' + it.t.getDate()))}</span></li>`).join('') || '<li>No activity yet. Signals appear here as soon as a chip reports a dose.</li>';
}

/* ========================= CALENDAR ========================= */
/* one tickable dose: ticked when a clip recorded it or the patient ticked it */
function checkItem(x) {
  const canTick = x.sched - state.now <= 60 * 60000;   // allow up to 1 h early
  const st = x.takenAt ? `${x.status === 'late' ? 'Late' : 'Taken'} ${hm(x.takenAt)}${x.byYou ? ', by you' : ''}` : STATUS_LABEL[x.status];
  return `<li><label class="check ${x.status}${canTick ? '' : ' disabled'}" style="--c:${medColor(x.med)}" title="${canTick ? '' : 'Not due yet'}">
    <input type="checkbox" data-k="${esc(x.k)}" ${x.takenAt ? 'checked' : ''} ${canTick ? '' : 'disabled'} aria-label="${esc(`${x.time} ${x.med.name} ${x.med.strength}`)}">
    <span class="box">${TICK}</span>
    <span class="time num">${x.time}</span>
    <span class="ci"><b>${esc(x.med.name)}</b><span>${esc(x.med.strength || x.med.form)}</span><span class="st">${esc(st)}</span></span>
  </label></li>`;
}

document.addEventListener('change', async e => {
  const inp = e.target.closest('input[data-k]'); if (!inp) return;
  const k = inp.dataset.k, sep = k.indexOf('|'), medId = k.slice(0, sep), scheduled = k.slice(sep + 1);
  const sched = new Date(scheduled), now = new Date();
  // ticking today's dose logs "now"; ticking a past day logs the scheduled time
  const takenAt = inp.checked ? (key(sched) === key(now) ? now : sched) : null;
  const prev = state.intakes.get(k), m = medById(medId);
  const wasTaken = !!(prev && prev.takenAt);
  state.now = now;
  state.intakes.set(k, { takenAt, source: 'manual' });
  if (!wasTaken && takenAt) m.stock = Math.max(0, m.stock - 1);
  if (wasTaken && !takenAt) m.stock += 1;
  renderCalendar();
  const again = document.querySelector(`input[data-k="${CSS.escape(k)}"]`); if (again) again.focus();
  try {
    await Api.markTaken(medId, scheduled, takenAt);
    toast(takenAt ? `${m.name} ${scheduled.slice(11)} marked as taken` : `${m.name} ${scheduled.slice(11)} marked as not taken`);
  } catch (ex) {
    if (prev) state.intakes.set(k, prev); else state.intakes.delete(k);
    if (!wasTaken && takenAt) m.stock += 1;
    if (wasTaken && !takenAt) m.stock = Math.max(0, m.stock - 1);
    renderCalendar(); toast(`Couldn't save: ${ex.message}`);
  }
});

function renderCalendar() {
  const today = startOfDay(state.now);
  if (!state.calMonth) state.calMonth = new Date(today.getFullYear(), today.getMonth(), 1);
  if (!state.selDay) state.selDay = today;
  if (!state.weekStart) state.weekStart = mondayOf(state.selDay);
  const week = state.calMode === 'week';
  document.querySelectorAll('.seg button').forEach(b => b.setAttribute('aria-pressed', b.dataset.mode === state.calMode));
  document.querySelector('.cal-wrap').classList.toggle('week-mode', week);
  $('#weekGrid').hidden = !week; $('#calGrid').hidden = week; $('#dayPanel').hidden = week;
  $('#legendCal').innerHTML = week ? '<span>Tick a medicine when you take it. Doses the clips detect are ticked for you.</span>' : legendHtml();
  if (week) { renderWeek(today); return; }

  const m0 = state.calMonth;
  $('#calTitle').textContent = `${MONTHS[m0.getMonth()]} ${m0.getFullYear()}`;
  const start = mondayOf(m0);
  let html = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'].map(d => `<div class="wd">${d}</div>`).join('');
  for (let i = 0; i < 42; i++) {
    const d = addDays(start, i);
    if (i % 7 === 0 && i > 0 && d.getMonth() !== m0.getMonth()) break;
    const ds = dosesOn(d), taken = ds.filter(x => isTaken(x.status)).length;
    const cls = [d.getMonth() !== m0.getMonth() ? 'out' : '', key(d) === key(today) ? 'today' : '', key(d) === key(state.selDay) ? 'sel' : ''].join(' ');
    html += `<button class="cell ${cls}" type="button" data-day="${key(d)}" aria-label="${fmtLong(d)}: ${taken} of ${ds.length} taken">
      <span class="n"><span>${d.getDate()}</span>${ds.length ? `<small class="num">${taken}/${ds.length}</small>` : ''}</span>
      <span class="marks">${ds.map(x => marker(x.status)).join('')}</span></button>`;
  }
  const grid = $('#calGrid'); grid.innerHTML = html;
  grid.querySelectorAll('.cell').forEach(c => { c.onclick = () => {
    state.selDay = parseKey(c.dataset.day); state.weekStart = mondayOf(state.selDay);
    if (state.selDay.getMonth() !== m0.getMonth()) state.calMonth = new Date(state.selDay.getFullYear(), state.selDay.getMonth(), 1);
    renderCalendar();
  }; });
  renderDayPanel();
}
function renderWeek(today) {
  const ws = state.weekStart, we = addDays(ws, 6);
  $('#calTitle').textContent = ws.getMonth() === we.getMonth()
    ? `${ws.getDate()}–${we.getDate()} ${MONTHS[ws.getMonth()]}`
    : `${ws.getDate()} ${MONTHS[ws.getMonth()].slice(0, 3)} – ${we.getDate()} ${MONTHS[we.getMonth()].slice(0, 3)}`;
  let html = '';
  for (let i = 0; i < 7; i++) {
    const d = addDays(ws, i), ds = dosesOn(d), taken = ds.filter(x => isTaken(x.status)).length;
    const cls = key(d) === key(today) ? 'today' : d > today ? 'future' : '';
    html += `<div class="wcol ${cls}">
      <h3><span>${DAYS_SHORT[d.getDay()]} ${d.getDate()}${cls === 'today' ? '<span class="today-tag">Today</span>' : ''}</span><small class="num">${taken}/${ds.length}</small></h3>
      <div class="prog"><i style="width:${ds.length ? taken / ds.length * 100 : 0}%"></i></div>
      <ul class="checks">${ds.map(checkItem).join('') || '<li style="color:var(--muted);font-size:13px">Nothing to take</li>'}</ul></div>`;
  }
  $('#weekGrid').innerHTML = html;
}
function renderDayPanel() {
  const d = state.selDay, ds = dosesOn(d);
  const taken = ds.filter(x => isTaken(x.status)).length, onT = ds.filter(x => x.status === 'on_time').length;
  $('#dayPanel').innerHTML = `<h2>${fmtLong(d)}</h2>
    <p class="sum num">${ds.length ? `${taken} of ${ds.length} doses taken, ${onT} on time` : 'No doses scheduled.'}</p>
    <ul class="checks">${ds.map(checkItem).join('')}</ul>`;
}
document.querySelectorAll('.seg button').forEach(b => { b.onclick = () => {
  state.calMode = b.dataset.mode;
  if (state.calMode === 'month') state.calMonth = new Date(state.weekStart.getFullYear(), state.weekStart.getMonth(), 1);
  else state.weekStart = mondayOf(state.selDay);
  renderCalendar();
}; });
$('#calPrev').onclick = () => {
  if (state.calMode === 'week') state.weekStart = addDays(state.weekStart, -7);
  else { const m = state.calMonth; state.calMonth = new Date(m.getFullYear(), m.getMonth() - 1, 1); }
  renderCalendar();
};
$('#calNext').onclick = () => {
  if (state.calMode === 'week') state.weekStart = addDays(state.weekStart, 7);
  else { const m = state.calMonth; state.calMonth = new Date(m.getFullYear(), m.getMonth() + 1, 1); }
  renderCalendar();
};
$('#calToday').onclick = () => { const t = startOfDay(state.now); state.calMonth = new Date(t.getFullYear(), t.getMonth(), 1); state.selDay = t; state.weekStart = mondayOf(t); renderCalendar(); };

/* ========================= MEDICINES ========================= */
function renderMedicines() {
  const today = startOfDay(state.now);
  const all30 = dosesBetween(addDays(today, -29), state.now);
  $('#medCount').textContent = `${plural(state.meds.length, 'medicine')} with a clip`;
  const label = t => `<span style="display:block;font-size:12px;color:var(--muted);font-weight:600">${t}</span>`;
  $('#medGrid').innerHTML = state.meds.map(m => {
    const t = all30.filter(d => d.med.id === m.id && isDone(d.status)), tt = t.filter(d => isTaken(d.status)).length;
    const low = m.stock <= m.refillAt;
    return `<button class="med-card" type="button" data-med="${m.id}">
      <span class="med-stripe" style="background:${medColor(m)}"></span>
      <span class="top"><span><h3>${esc(m.name)}</h3><span class="str">${esc(m.strength)} ${esc((m.form || '').toLowerCase())}</span></span>${clipBadge(m.clipName)}</span>
      <span>${m.times.map(t => `<span class="pill-chip num"><span class="c" style="background:${medColor(m)}"></span>${t}</span>`).join('')}<span class="pill-chip">${daysText(m.days)}</span></span>
      <span class="kv">
        <span>${label('Taken, 30 days')}<b class="num">${tt}/${t.length}</b></span>
        <span>${label('In box')}<b class="num ${low ? 'low' : ''}">${m.stock} left${low ? ', refill soon' : ''}</b></span>
      </span>
      <span style="color:var(--muted);font-size:14px">${esc(m.purpose)}</span></button>`;
  }).join('') || '<div class="card"><h2>No medicines yet</h2><p style="color:var(--muted)">Pair a clip with a medicine to see it here.</p><a class="btn" href="#setup">Pair a clip</a></div>';
  $('#medGrid').querySelectorAll('.med-card').forEach(c => { c.onclick = () => openMed(c.dataset.med); });
}

let closeDrawer = () => {};
function openDrawer(html) {
  const root = $('#drawerRoot'), prev = document.activeElement;
  root.innerHTML = `<div class="scrim"><div class="drawer" role="dialog" aria-modal="true">${html}</div></div>`;
  const scrim = root.firstElementChild;
  const onKey = e => { if (e.key === 'Escape') closeDrawer(); };
  closeDrawer = () => { root.innerHTML = ''; document.removeEventListener('keydown', onKey); if (prev && prev.focus) prev.focus(); closeDrawer = () => {}; };
  document.addEventListener('keydown', onKey);
  scrim.addEventListener('click', e => { if (e.target === scrim) closeDrawer(); });
  scrim.querySelectorAll('[data-close]').forEach(b => { b.onclick = () => closeDrawer(); });
  scrim.querySelector('button').focus();
  return scrim;
}

function openMed(id) {
  const m = medById(id), today = startOfDay(state.now);
  const days = []; for (let i = 29; i >= 0; i--) days.push(addDays(today, -i));
  let tt = 0, tot = 0, ot = 0;
  const strip = days.map(d => {
    const ds = dosesOn(d).filter(x => x.med.id === m.id && isDone(x.status));
    if (!ds.length) return `<i title="${fmtDay(d)}: none scheduled"></i>`;
    const tk = ds.filter(x => isTaken(x.status)).length; tt += tk; tot += ds.length; ot += ds.filter(x => x.status === 'on_time').length;
    return `<i class="${tk === ds.length ? 'full' : tk ? 'part' : 'none'}" title="${fmtDay(d)}: ${tk} of ${ds.length} taken"></i>`;
  }).join('');
  const monday = mondayOf(today);
  const w = dosesBetween(monday, addDays(monday, 6)).filter(x => x.med.id === m.id);
  const low = m.stock <= m.refillAt;
  const scrim = openDrawer(`
    <div class="drawer-head"><div>${clipBadge(m.clipName)}<h2 style="margin-top:10px">${esc(m.name)} ${esc(m.strength)}</h2><p style="color:var(--muted);margin-top:4px">${esc(m.form)}. ${esc(m.purpose)}</p></div>
      <button class="icon-btn" type="button" data-close aria-label="Close">${CLOSE}</button></div>
    <div class="kv" style="grid-template-columns:repeat(3,1fr)">
      <div><span>This week</span><b class="num">${w.filter(x => isTaken(x.status)).length} of ${w.length}</b></div>
      <div><span>Last 30 days</span><b class="num">${tt}/${tot}</b></div>
      <div><span>On time</span><b class="num">${tot ? Math.round(ot / tot * 100) + '%' : '–'}</b></div>
    </div>
    <h3>Last 30 days</h3>
    <div class="strip">${strip}</div>
    <div class="strip-legend"><span>${fmtDay(days[0])}</span><span>Green: all taken. Amber: some. Red: none.</span><span>Today</span></div>
    <h3>Schedule</h3>
    <p>${m.times.map(t => `<span class="pill-chip num"><span class="c" style="background:${medColor(m)}"></span>${t}</span>`).join('')} ${daysText(m.days)}, since ${fmtDay(parseKey(m.startDate))}</p>
    <h3>Clip</h3>
    <p>${clipBadge(m.clipName)} <span style="color:var(--muted);margin-left:6px">${m.lastSignal ? `Last signal ${ago(new Date(m.lastSignal))}` : 'No signal received yet'}</span></p>
    <h3>How to take it</h3><p>${esc(m.instructions || 'No instructions added.')}</p>
    ${m.sideEffects && m.sideEffects.length ? `<h3>Possible side effects</h3><ul class="plain">${m.sideEffects.map(s => `<li>${esc(s)}</li>`).join('')}</ul>` : ''}
    <h3>Supply</h3>
    <p class="num ${low ? 'low' : ''}"><b>${m.stock} of ${m.stockMax}</b> left in the box${low ? '. Refill soon.' : '.'}</p>
    <div class="bar" style="max-width:260px"><i style="width:${Math.min(100, m.stock / Math.max(1, m.stockMax) * 100)}%;background:${low ? 'var(--late)' : 'var(--accent)'}"></i></div>
    <h3>Prescribed by</h3><p>${esc(m.prescriber || 'Not recorded')}</p>
    ${m.notes ? `<h3>Notes</h3><p>${esc(m.notes)}</p>` : ''}
    <div class="form-actions" style="margin-top:26px"><button class="btn ghost" type="button" id="editInSetup">Edit in Setup</button></div>`);
  scrim.querySelector('#editInSetup').onclick = () => { closeDrawer(); location.hash = 'setup'; fillPairForm(m); };
}

/* ========================= SETUP (pair clips) ========================= */
const pairForm = $('#pairForm');
$('#daysPick').innerHTML = [1, 2, 3, 4, 5, 6, 0].map(d => `<label><input type="checkbox" name="days" value="${d}" checked><span>${DAYS_SHORT[d]}</span></label>`).join('');
const freeClips = () => state.clips.filter(c => !c.medicineId);

/* the row of clips to choose from: every free clip, plus the clip being edited */
function renderClipPick(selected) {
  const editing = state.editingId ? medById(state.editingId) : null;
  const options = freeClips().slice();
  if (editing && !options.some(c => c.clipId === editing.clipId)) {
    options.unshift({ clipId: editing.clipId, name: editing.clipName, lastSeen: editing.lastSignal });
  }
  const current = selected ?? (pairForm.querySelector('input[name=clipId]:checked') || {}).value;
  const pick = options.some(c => c.clipId === current) ? current : (options.length === 1 ? options[0].clipId : null);
  $('#clipPick').innerHTML = options.length
    ? options.map(c => `<label class="clip-opt"><input type="radio" name="clipId" value="${esc(c.clipId)}" ${c.clipId === pick ? 'checked' : ''}>
        <span>${clipIcon(18)}<span>${esc(c.name)}<small>${c.lastSeen ? 'Seen ' + esc(ago(new Date(c.lastSeen))) : 'Connected'}</small></span></span></label>`).join('')
    : `<div class="no-clips">No free clips right now. Switch a clip on: as soon as it joins the Wi-Fi, the hub adds it here.</div>`;
  $('#pairSubmit').disabled = !options.length;
}

function fillPairForm(m, clipId) {
  const f = pairForm.elements;
  state.editingId = m ? m.id : null;
  pairForm.reset();
  $('#pairErr').textContent = '';
  if (m) {
    f.name.value = m.name; f.strength.value = m.strength || ''; f.form.value = m.form || 'Tablet';
    f.purpose.value = m.purpose || ''; f.t1.value = m.times[0] || ''; f.t2.value = m.times[1] || ''; f.t3.value = m.times[2] || '';
    pairForm.querySelectorAll('input[name=days]').forEach(c => { c.checked = m.days.includes(+c.value); });
    f.stock.value = m.stock; f.refillAt.value = m.refillAt; f.instructions.value = m.instructions || '';
    f.sideEffects.value = (m.sideEffects || []).join(', '); f.prescriber.value = m.prescriber || ''; f.notes.value = m.notes || '';
  }
  renderClipPick(m ? m.clipId : (clipId || null));
  $('#moreDetails').open = !!(m && (m.strength || m.instructions || m.prescriber || m.notes));
  $('#pairTitle').textContent = m ? `Edit ${m.name}` : 'Pair a clip';
  $('#pairSubmit').textContent = m ? 'Save changes' : 'Pair clip';
  $('#pairCancel').hidden = !m;
  $('#editingNote').hidden = !m;
  if (m) $('#editingNote').textContent = `Editing ${m.clipName}. Dose history is kept when you change the schedule.`;
  f.name.focus();
}
$('#pairCancel').onclick = () => fillPairForm(null);

pairForm.addEventListener('submit', async e => {
  e.preventDefault();
  const f = new FormData(pairForm), err = $('#pairErr');
  err.textContent = '';
  const clipId = f.get('clipId');
  const name = f.get('name').trim();
  const times = [...new Set([f.get('t1'), f.get('t2'), f.get('t3')].filter(Boolean))].sort();
  const days = f.getAll('days').map(Number);
  const fail = (msg, field) => { err.textContent = msg; if (field) pairForm.elements[field].focus(); };
  if (!clipId) return fail('Pick the clip that is on this medicine box.');
  if (!name) return fail('Enter the medicine name.', 'name');
  if (!times.length) return fail('Set at least one dose time.', 't1');
  if (!days.length) return fail('Pick at least one day.');
  const stock = Math.max(0, parseInt(f.get('stock'), 10) || 0);
  const editing = state.editingId ? medById(state.editingId) : null;
  const used = state.meds.filter(m => m.id !== state.editingId).map(m => Number(m.color));
  let color = editing ? editing.color : 1; if (!editing) { while (used.includes(color) && color < 6) color++; }
  const clip = state.clips.find(c => c.clipId === clipId);
  const clipName = clip ? clip.name : (editing ? editing.clipName : clipId);
  const med = {
    ...(editing ? { id: editing.id, startDate: editing.startDate } : {}),
    clipId, name, color,
    strength: f.get('strength').trim(), form: f.get('form'), purpose: f.get('purpose').trim(),
    times, days, stock, stockMax: Math.max(stock, editing ? editing.stockMax : stock, 1),
    refillAt: Math.max(0, parseInt(f.get('refillAt'), 10) || 0),
    instructions: f.get('instructions').trim(), sideEffects: f.get('sideEffects').split(',').map(s => s.trim()).filter(Boolean),
    prescriber: f.get('prescriber').trim(), notes: f.get('notes').trim()
  };
  const btn = $('#pairSubmit'); btn.disabled = true;
  try {
    await Api.saveMedicine(med);
    toast(editing ? `${name} saved` : `${clipName} paired with ${name}`);
    fillPairForm(null);
    await refresh(true);
  } catch (ex) { fail(`Couldn't save: ${ex.message}.`); }
  finally { btn.disabled = !$('#clipPick input'); }
});

function renderSetup() {
  renderClipPick();
  const free = freeClips().length;
  $('#clipCount').textContent = state.clips.length ? `${plural(state.clips.length, 'clip')}, ${free} free` : '';
  $('#clipList').innerHTML = state.clips.map(c => {
    const m = c.medicineId ? medById(c.medicineId) : null;
    const seen = c.lastSeen ? `Last seen ${esc(ago(new Date(c.lastSeen)))}` : '';
    let actions;
    if (m && state.confirmRemove === m.id) {
      actions = `<button class="btn danger small" type="button" data-unpair-yes="${m.id}">Unpair and delete history</button><button class="btn ghost small" type="button" data-cancel>Keep</button>`;
    } else if (m) {
      actions = `<button class="btn ghost small" type="button" data-edit="${m.id}">Edit</button><button class="btn ghost small danger-text" type="button" data-unpair="${m.id}">Unpair</button>`;
    } else if (state.confirmForget === c.clipId) {
      actions = `<button class="btn danger small" type="button" data-forget-yes="${esc(c.clipId)}">Forget ${esc(c.name)}</button><button class="btn ghost small" type="button" data-cancel>Keep</button>`;
    } else {
      actions = `<button class="btn small" type="button" data-pair="${esc(c.clipId)}">Pair</button><button class="btn ghost small" type="button" data-forget="${esc(c.clipId)}">Forget</button>`;
    }
    return `<div class="clip-row">
      ${m ? `<span class="swatch" style="background:${medColor(m)};height:34px"></span>` : ''}
      ${clipBadge(c.name)}
      <div class="info">${m ? `<b>${esc(m.name)} ${esc(m.strength)}</b><small>${m.times.join(', ')}, ${daysText(m.days)}. ${seen}</small>`
        : `<b>Not paired</b><span class="tag free">Free</span><small>${seen}</small>`}</div>
      <div class="row-actions">${actions}</div></div>`;
  }).join('') || '<p style="color:var(--muted);margin:0">No clips yet. Switch a clip on near the hub and it shows up here within a few seconds.</p>';

  const list = $('#clipList');
  list.querySelectorAll('[data-pair]').forEach(b => { b.onclick = () => { fillPairForm(null, b.dataset.pair); pairForm.scrollIntoView({ behavior: 'smooth', block: 'start' }); }; });
  list.querySelectorAll('[data-edit]').forEach(b => { b.onclick = () => { fillPairForm(medById(b.dataset.edit)); pairForm.scrollIntoView({ behavior: 'smooth', block: 'start' }); }; });
  list.querySelectorAll('[data-unpair]').forEach(b => { b.onclick = () => { state.confirmRemove = b.dataset.unpair; state.confirmForget = null; renderSetup(); list.querySelector('[data-cancel]').focus(); }; });
  list.querySelectorAll('[data-forget]').forEach(b => { b.onclick = () => { state.confirmForget = b.dataset.forget; state.confirmRemove = null; renderSetup(); list.querySelector('[data-cancel]').focus(); }; });
  list.querySelectorAll('[data-cancel]').forEach(b => { b.onclick = () => { state.confirmRemove = null; state.confirmForget = null; renderSetup(); }; });
  list.querySelectorAll('[data-unpair-yes]').forEach(b => { b.onclick = async () => {
    const m = medById(b.dataset.unpairYes);
    try {
      await Api.unpair(m.id); state.confirmRemove = null;
      if (state.editingId === m.id) fillPairForm(null);
      toast(`${m.clipName} is free again`); await refresh(true);
    } catch (ex) { toast(`Couldn't unpair: ${ex.message}`); }
  }; });
  list.querySelectorAll('[data-forget-yes]').forEach(b => { b.onclick = async () => {
    try { await Api.forgetClip(b.dataset.forgetYes); state.confirmForget = null; toast('Clip forgotten'); await refresh(true); }
    catch (ex) { toast(`Couldn't forget clip: ${ex.message}`); }
  }; });
}

/* ========================= LOGIN PAGE ========================= */
$('#pwToggle').onclick = () => {
  const pw = $('#password'), show = pw.type === 'password';
  pw.type = show ? 'text' : 'password';
  $('#pwToggle').textContent = show ? 'Hide' : 'Show';
  $('#pwToggle').setAttribute('aria-pressed', show);
  $('#pwToggle').setAttribute('aria-label', show ? 'Hide password' : 'Show password');
};

/* ========================= BOOT ========================= */
(async function boot() {
  await Api.detect();
  if (DEMO) {
    $('#demoHint').hidden = false;
    $('#email').value = 'patient@medclip.demo'; $('#password').value = 'demo1234';
  }
  const s = store.get();
  if (s && s.user && (DEMO ? s.token === 'demo' : s.token !== 'demo')) {
    state.user = s.user; Api.token = s.token;
    try { await loadData(); setUser(); updateHub(); route(); return; }
    catch { state.user = null; Api.token = null; }
  }
  showLogin();
})();
