<script setup>
import { ref, reactive, computed, onMounted, onUnmounted } from "vue";

// The inspector always connects over SSE to the host's trace channel (the bridge URL
// is injected as window.__HULL_BRIDGE__ by `hull dev`). It only listens — no invoke.
const BRIDGE = (typeof globalThis !== "undefined" && globalThis.__HULL_BRIDGE__) || "";

const MAX = 500;
const calls = reactive([]);   // { id, name, args, status, durMs, result, time }
const events = reactive([]);  // { event, payload, time }
const byId = new Map();
const filter = ref("all");
const paused = ref(false);
const selected = ref(null);
const mode = ref(BRIDGE ? "connecting" : "none");

const now = () => new Date().toLocaleTimeString();
function cap(list) { while (list.length > MAX) list.pop(); }

function onTrace(p) {
  if (paused.value || !p) return;
  if (p.type === "call") {
    const e = reactive({ id: p.id, name: p.name, args: p.args, status: "pending", durMs: null, result: null, time: now() });
    byId.set(p.id, e);
    calls.unshift(e); cap(calls);
  } else if (p.type === "reply") {
    const e = byId.get(p.id);
    if (e) { e.status = p.ok ? "ok" : "error"; e.durMs = p.durMs; e.result = p.result; }
  } else if (p.type === "event") {
    events.unshift({ event: p.event, payload: p.payload, time: now() }); cap(events);
  }
}

let es = null;
onMounted(() => {
  if (!BRIDGE) return;
  es = new EventSource(`${BRIDGE}/bridge/events`);
  es.onopen = () => { mode.value = "live"; };
  es.onerror = () => { mode.value = "reconnecting"; };
  es.onmessage = (e) => {
    let msg; try { msg = JSON.parse(e.data); } catch { return; }
    if (msg && msg.event === "__trace") onTrace(msg.payload);
  };
});
onUnmounted(() => es && es.close());

const GROUPS = {
  db: (n) => n.startsWith("db"),
  files: (n) => n.startsWith("file"),
  http: (n) => n.startsWith("http"),
};
const counts = computed(() => ({
  all: calls.length,
  db: calls.filter((c) => GROUPS.db(c.name)).length,
  files: calls.filter((c) => GROUPS.files(c.name)).length,
  http: calls.filter((c) => GROUPS.http(c.name)).length,
  errors: calls.filter((c) => c.status === "error").length,
  events: events.length,
}));
const rows = computed(() => {
  if (filter.value === "events") return [];
  if (filter.value === "all") return calls;
  if (filter.value === "errors") return calls.filter((c) => c.status === "error");
  const g = GROUPS[filter.value];
  return g ? calls.filter((c) => g(c.name)) : calls;
});
const summary = computed(() => {
  const m = {};
  for (const c of calls) {
    if (c.durMs == null) continue;
    (m[c.name] ??= { n: 0, sum: 0, max: 0 });
    const s = m[c.name]; s.n++; s.sum += c.durMs; s.max = Math.max(s.max, c.durMs);
  }
  return Object.entries(m)
    .map(([name, s]) => ({ name, n: s.n, avg: s.sum / s.n, max: s.max }))
    .sort((a, b) => b.max - a.max).slice(0, 8);
});
const ms = (v) => (v == null ? "" : v < 1 ? "<1ms" : `${Math.round(v)}ms`);
const short = (v) => { const s = JSON.stringify(v); return s && s.length > 80 ? s.slice(0, 80) + "…" : s; };
function toggle(e) { selected.value = selected.value === e ? null : e; }
function clearAll() { calls.length = 0; events.length = 0; byId.clear(); selected.value = null; }
</script>

<template>
  <div class="wrap">
    <header>
      <h1>⛵ Hull <span class="b">Inspector</span></h1>
      <span class="badge" :class="{ live: mode !== 'none' }">{{ mode }}</span>
      <span class="spacer"></span>
      <button :class="{ on: paused }" @click="paused = !paused">{{ paused ? "paused" : "live" }}</button>
      <button @click="clearAll">clear</button>
    </header>

    <div class="chips">
      <span v-for="f in ['all','db','files','http','errors','events']" :key="f"
            class="chip" :class="{ active: filter === f }" @click="filter = f">
        {{ f }}<span class="n">{{ counts[f] }}</span>
      </span>
    </div>

    <main>
      <!-- Events view -->
      <table v-if="filter === 'events'">
        <thead><tr><th class="t">time</th><th>event</th><th>payload</th></tr></thead>
        <tbody>
          <tr v-for="(e, i) in events" :key="i" class="row">
            <td class="t">{{ e.time }}</td>
            <td class="ev">{{ e.event }}</td>
            <td class="args">{{ short(e.payload) }}</td>
          </tr>
          <tr v-if="!events.length"><td colspan="3" class="empty">No C++ → UI events yet.</td></tr>
        </tbody>
      </table>

      <!-- Calls view -->
      <table v-else>
        <thead><tr><th class="t">time</th><th>binding</th><th>args</th><th class="dur">dur</th><th>status</th></tr></thead>
        <tbody>
          <template v-for="e in rows" :key="e.id">
            <tr class="row" @click="toggle(e)">
              <td class="t">{{ e.time }}</td>
              <td class="name">{{ e.name }}</td>
              <td class="args">{{ short(e.args) }}</td>
              <td class="dur">{{ ms(e.durMs) }}</td>
              <td class="st" :class="e.status">{{ e.status }}</td>
            </tr>
            <tr v-if="selected === e" class="detail">
              <td colspan="5">
                <div class="muted">args</div>
                <pre>{{ JSON.stringify(e.args, null, 2) }}</pre>
                <div class="muted">result</div>
                <pre>{{ e.result == null ? "(pending)" : JSON.stringify(e.result, null, 2) }}</pre>
              </td>
            </tr>
          </template>
          <tr v-if="!rows.length"><td colspan="5" class="empty">No calls yet — interact with the app.</td></tr>
        </tbody>
      </table>
    </main>

    <div class="summary">
      <h2>slowest bindings</h2>
      <div v-for="s in summary" :key="s.name" class="srow">
        <span class="name">{{ s.name }}</span>
        <span>×{{ s.n }}</span>
        <span>avg {{ ms(s.avg) }}</span>
        <span>max {{ ms(s.max) }}</span>
      </div>
      <div v-if="!summary.length" class="muted" style="font-size:.72rem">No timings yet.</div>
    </div>
  </div>
</template>
