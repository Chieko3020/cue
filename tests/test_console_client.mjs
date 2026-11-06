import assert from 'node:assert/strict';
import { createEventStore, EVENT_LIMIT } from '../web/console/src/event-store.js';
import { getJson } from '../web/console/src/api.js';

const store = createEventStore();
const event = (id, generation = 'A') => ({ id, generation, ev: 'metric.sample' });
assert(store.select('A'));
for (let id = 1; id <= 10000; id++) store.push(event(id));
assert.equal(store.events.length, EVENT_LIMIT);
assert.equal(store.events[0].id, 10000 - EVENT_LIMIT + 1);
assert(!store.push(event(1)));
assert(!store.push(event(10000)));
assert(!store.push(event(Infinity)));
assert(!store.push(event(10001, 'B')));
assert(store.select('B'));
assert.equal(store.events.length, 0);
assert(store.push(event(2, 'B')));
assert(store.push(event(1, 'B')));
assert.deepEqual(store.events.map((e) => e.id), [1, 2]);
assert(!store.select('A'));
assert.equal(store.generation, 'B');
assert(!store.push({ ...event(3, 'B'), ev: 'console.ready' }));

const originalFetch = globalThis.fetch;
try {
  globalThis.fetch = async () => ({ ok: false, status: 503, json: async () => ({ ok: true }) });
  assert.deepEqual(await getJson('/state'), { ok: false, error: 'HTTP 503' });
  globalThis.fetch = async (_url, { signal }) => new Promise((_resolve, reject) => {
    signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
  });
  const started = performance.now();
  assert.equal((await getJson('/state', { timeoutMs: 30 })).ok, false);
  assert(performance.now() - started < 1000);
  console.log('console client: bounded 10000-event ingest, ordering, generation, HTTP error and timeout passed');
} finally { globalThis.fetch = originalFetch; }
