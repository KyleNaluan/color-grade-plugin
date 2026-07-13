import { useEffect, useMemo, useState } from 'preact/hooks';
import type { Bridge } from '../host/bridge';
import {
  type SelectionState,
  createSelectionWatcher,
  describeTarget,
} from './selection';

type Tab = 'correct' | 'grade';

function useSelection(bridge: Bridge): SelectionState {
  const watcher = useMemo(() => createSelectionWatcher(bridge), [bridge]);
  const [state, setState] = useState<SelectionState>(watcher.getState());
  useEffect(() => {
    const unsubscribe = watcher.subscribe(setState);
    watcher.start();
    return () => {
      unsubscribe();
      watcher.stop();
    };
  }, [watcher]);
  return state;
}

export function App({ bridge }: { bridge: Bridge }) {
  const [tab, setTab] = useState<Tab>('correct');
  const selection = useSelection(bridge);

  return (
    <div class="panel">
      <nav class="tabs">
        <button
          class={tab === 'correct' ? 'tab active' : 'tab'}
          onClick={() => setTab('correct')}
        >
          Correct
        </button>
        <button
          class={tab === 'grade' ? 'tab active' : 'tab'}
          onClick={() => setTab('grade')}
        >
          Grade
        </button>
      </nav>

      <main class="tab-body">
        {tab === 'correct' ? (
          <p class="placeholder">Correct: Decode LUT + Lumetri controls land here.</p>
        ) : (
          <p class="placeholder">Grade: Theme picker and knobs land here.</p>
        )}
      </main>

      <footer class="footer">
        <span class="target" data-testid="target-clip">
          {describeTarget(selection)}
        </span>
        <span class="footer-stubs">
          <button disabled title="Before/after toggle (coming soon)">
            A/B
          </button>
          <button disabled title="Scopes show/hide (coming soon)">
            Scopes
          </button>
          <span class="analyzed" title="Analyzed-clip indicator (coming soon)">
            Analyzed: —
          </span>
        </span>
      </footer>
    </div>
  );
}
