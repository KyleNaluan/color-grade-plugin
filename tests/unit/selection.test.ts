import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Bridge, SelectionSnapshot } from '../../src/host/bridge';
import {
  type SelectionState,
  createSelectionWatcher,
  describeTarget,
  toSelectionState,
} from '../../src/panel/selection';

/**
 * A scripted fake bridge: each getSelection() call consumes the next entry
 * in the script; the last entry repeats once the script is exhausted.
 * Entries may be snapshots or Errors (rejected calls).
 */
type SelectionBridge = Pick<Bridge, 'getSelection'>;

function fakeBridge(script: Array<SelectionSnapshot | Error>): SelectionBridge {
  let calls = 0;
  return {
    getSelection() {
      const entry = script[Math.min(calls++, script.length - 1)]!;
      return entry instanceof Error ? Promise.reject(entry) : Promise.resolve(entry);
    },
  };
}

const layer = (compName: string, layerName: string | null, selectedCount = layerName ? 1 : 0): SelectionSnapshot => ({
  compName,
  layerName,
  selectedCount,
});

describe('createSelectionWatcher', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  const collect = (bridge: SelectionBridge) => {
    const watcher = createSelectionWatcher(bridge, 500);
    const states: SelectionState[] = [];
    watcher.subscribe((s) => states.push(s));
    watcher.start();
    return { watcher, states };
  };

  it('reports the selected layer of the active comp', async () => {
    const { states } = collect(fakeBridge([layer('Main Comp', 'Interview A')]));
    await vi.advanceTimersByTimeAsync(0);
    expect(states.at(-1)).toEqual({
      kind: 'layer',
      compName: 'Main Comp',
      layerName: 'Interview A',
      selectedCount: 1,
    });
  });

  it('updates the target clip name when the selection changes', async () => {
    const { states } = collect(
      fakeBridge([layer('Main Comp', 'Interview A'), layer('Main Comp', 'B-roll 03')]),
    );
    await vi.advanceTimersByTimeAsync(0);
    await vi.advanceTimersByTimeAsync(500);
    const names = states.filter((s) => s.kind === 'layer').map((s) => s.layerName);
    expect(names).toEqual(['Interview A', 'B-roll 03']);
  });

  it('does not notify when the selection is unchanged', async () => {
    const { states } = collect(fakeBridge([layer('Main Comp', 'Interview A')]));
    await vi.advanceTimersByTimeAsync(0);
    const count = states.length;
    await vi.advanceTimersByTimeAsync(2000);
    expect(states.length).toBe(count);
  });

  it('reports no-comp and no-selection states', async () => {
    const { states } = collect(
      fakeBridge([
        { compName: null, layerName: null, selectedCount: 0 },
        layer('Main Comp', null),
      ]),
    );
    await vi.advanceTimersByTimeAsync(0);
    expect(states.at(-1)).toEqual({ kind: 'no-comp' });
    await vi.advanceTimersByTimeAsync(500);
    expect(states.at(-1)).toEqual({ kind: 'no-selection', compName: 'Main Comp' });
  });

  it('surfaces bridge failures as an error state, then recovers', async () => {
    const { states } = collect(
      fakeBridge([new Error('ExtendScript call failed'), layer('Main Comp', 'Interview A')]),
    );
    await vi.advanceTimersByTimeAsync(0);
    expect(states.at(-1)).toEqual({ kind: 'error', message: 'ExtendScript call failed' });
    await vi.advanceTimersByTimeAsync(500);
    expect(states.at(-1)).toMatchObject({ kind: 'layer', layerName: 'Interview A' });
  });

  it('does not spawn overlapping polls on start/stop/start while a poll is in flight', async () => {
    let resolveInFlight: ((snap: SelectionSnapshot) => void) | undefined;
    let pending = 0;
    const bridge: SelectionBridge = {
      getSelection() {
        pending += 1;
        return new Promise<SelectionSnapshot>((resolve) => {
          if (!resolveInFlight) resolveInFlight = resolve;
          else resolve(layer('Main Comp', 'Interview A'));
        });
      },
    };
    const getSelection = vi.spyOn(bridge, 'getSelection');
    const watcher = createSelectionWatcher(bridge, 500);
    watcher.start();
    expect(pending).toBe(1);

    watcher.stop();
    watcher.start();

    resolveInFlight!(layer('Main Comp', 'Interview A'));
    await vi.advanceTimersByTimeAsync(0);
    await vi.advanceTimersByTimeAsync(500);

    const callsPerInterval: number[] = [];
    for (let i = 0; i < 4; i++) {
      const before = getSelection.mock.calls.length;
      await vi.advanceTimersByTimeAsync(500);
      callsPerInterval.push(getSelection.mock.calls.length - before);
    }
    expect(callsPerInterval).toEqual([1, 1, 1, 1]);
  });

  it('stops polling after stop()', async () => {
    const bridge = fakeBridge([layer('Main Comp', 'Interview A')]);
    const getSelection = vi.spyOn(bridge, 'getSelection');
    const watcher = createSelectionWatcher(bridge, 500);
    watcher.start();
    await vi.advanceTimersByTimeAsync(0);
    watcher.stop();
    const calls = getSelection.mock.calls.length;
    await vi.advanceTimersByTimeAsync(5000);
    expect(getSelection.mock.calls.length).toBe(calls);
  });
});

describe('toSelectionState', () => {
  it('maps a comp with no selection', () => {
    expect(toSelectionState({ compName: 'C', layerName: null, selectedCount: 0 })).toEqual({
      kind: 'no-selection',
      compName: 'C',
    });
  });
});

describe('describeTarget', () => {
  it('shows the target clip name', () => {
    expect(
      describeTarget({ kind: 'layer', compName: 'C', layerName: 'Clip 7', selectedCount: 1 }),
    ).toBe('Target: Clip 7');
  });

  it('flags multi-selection', () => {
    expect(
      describeTarget({ kind: 'layer', compName: 'C', layerName: 'Clip 7', selectedCount: 3 }),
    ).toBe('Target: Clip 7 (+2 more selected)');
  });

  it('describes the empty states', () => {
    expect(describeTarget({ kind: 'no-comp' })).toBe('Target: no active comp');
    expect(describeTarget({ kind: 'no-selection', compName: 'C' })).toBe(
      'Target: no layer selected in "C"',
    );
  });
});
