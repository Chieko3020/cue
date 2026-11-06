// A bounded, ordered window shared by SSE and snapshot ingestion. The eviction
// floor prevents recurring old snapshots from resurrecting discarded events.
export const EVENT_LIMIT = 800;
export function createEventStore(limit = EVENT_LIMIT) {
  let generation = null;
  let floor = 0;
  let events = [];
  const retired = new Set();
  return {
    get generation() { return generation; },
    get events() { return events; },
    select(next) {
      if (typeof next !== 'string' || !next || retired.has(next)) return false;
      if (generation === next) return true;
      if (generation) retired.add(generation);
      // Only in-flight generations matter; keep the bookkeeping bounded too.
      while (retired.size > 8) retired.delete(retired.values().next().value);
      generation = next;
      floor = 0;
      events = [];
      return true;
    },
    push(msg) {
      if (!msg || msg.generation !== generation || !Number.isSafeInteger(msg.id) ||
          msg.id <= floor || !msg.ev || msg.ev === 'console.ready') return false;
      if (events.some((e) => e.id === msg.id)) return false;
      events = events.concat(msg).sort((a, b) => a.id - b.id);
      if (events.length > limit) {
        floor = events[events.length - limit - 1].id;
        events = events.slice(-limit);
      }
      return true;
    },
  };
}
