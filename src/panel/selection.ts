/**
 * The selection model: the panel always operates on the selected layer in
 * the active comp, and always shows the target clip name. This watcher
 * polls the bridge and publishes changes; it is pure panel logic, tested
 * against a scripted fake bridge.
 */
import type { Bridge, SelectionSnapshot } from '../host/bridge';

export type SelectionState =
  | { kind: 'loading' }
  | { kind: 'no-comp' }
  | { kind: 'no-selection'; compName: string }
  | { kind: 'layer'; compName: string; layerName: string; selectedCount: number }
  | { kind: 'error'; message: string };

export function toSelectionState(snapshot: SelectionSnapshot): SelectionState {
  if (snapshot.compName === null) return { kind: 'no-comp' };
  if (snapshot.layerName === null) return { kind: 'no-selection', compName: snapshot.compName };
  return {
    kind: 'layer',
    compName: snapshot.compName,
    layerName: snapshot.layerName,
    selectedCount: snapshot.selectedCount,
  };
}

/** The footer's target-clip line for a given selection state. */
export function describeTarget(state: SelectionState): string {
  switch (state.kind) {
    case 'loading':
      return 'Target: …';
    case 'no-comp':
      return 'Target: no active comp';
    case 'no-selection':
      return `Target: no layer selected in "${state.compName}"`;
    case 'layer':
      return state.selectedCount > 1
        ? `Target: ${state.layerName} (+${state.selectedCount - 1} more selected)`
        : `Target: ${state.layerName}`;
    case 'error':
      return `Target: bridge error (${state.message})`;
  }
}

export interface SelectionWatcher {
  getState(): SelectionState;
  /** Immediately calls back with the current state, then on every change. */
  subscribe(listener: (state: SelectionState) => void): () => void;
  start(): void;
  stop(): void;
}

function statesEqual(a: SelectionState, b: SelectionState): boolean {
  return JSON.stringify(a) === JSON.stringify(b);
}

/**
 * Polls `bridge.getSelection()` every `intervalMs`. Polls never overlap:
 * the next tick is scheduled only after the previous query settles, so a
 * slow bridge cannot pile up requests or deliver answers out of order.
 */
export function createSelectionWatcher(bridge: Bridge, intervalMs = 500): SelectionWatcher {
  let state: SelectionState = { kind: 'loading' };
  let listeners: Array<(state: SelectionState) => void> = [];
  let running = false;
  let timer: ReturnType<typeof setTimeout> | undefined;

  const setState = (next: SelectionState) => {
    if (statesEqual(state, next)) return;
    state = next;
    for (const listener of listeners) listener(state);
  };

  const tick = async () => {
    try {
      setState(toSelectionState(await bridge.getSelection()));
    } catch (err) {
      setState({ kind: 'error', message: err instanceof Error ? err.message : String(err) });
    }
    if (running) timer = setTimeout(tick, intervalMs);
  };

  return {
    getState: () => state,
    subscribe(listener) {
      listeners.push(listener);
      listener(state);
      return () => {
        listeners = listeners.filter((l) => l !== listener);
      };
    },
    start() {
      if (running) return;
      running = true;
      void tick();
    },
    stop() {
      running = false;
      if (timer !== undefined) clearTimeout(timer);
      timer = undefined;
    },
  };
}
