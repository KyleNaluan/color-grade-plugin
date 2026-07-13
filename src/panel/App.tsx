import { useEffect, useMemo, useState } from 'preact/hooks';
import type { Bridge, CorrectStackResult } from '../host/bridge';
import type { FrameSource } from '../host/frameSource';
import { PROFILES } from '../core/color/index.js';
import {
  type SelectionState,
  createSelectionWatcher,
  describeTarget,
} from './selection';
import { analyzeCurrentFrame, type AnalyzeResult } from './analyze';
import { setCorrectProfile } from './correctStack';
import { StatsView } from './StatsView';

type Tab = 'correct' | 'grade';

type AnalyzeState =
  | { kind: 'idle' }
  | { kind: 'running' }
  | { kind: 'done'; result: AnalyzeResult }
  | { kind: 'error'; message: string };

type CorrectStackState =
  | { kind: 'idle' }
  | { kind: 'applying' }
  | { kind: 'done'; result: CorrectStackResult }
  | { kind: 'error'; message: string };

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

export function App({ bridge, frameSource }: { bridge: Bridge; frameSource: FrameSource }) {
  const [tab, setTab] = useState<Tab>('correct');
  const selection = useSelection(bridge);
  const [profileKey, setProfileKey] = useState<string>('rec709');
  const [analyze, setAnalyze] = useState<AnalyzeState>({ kind: 'idle' });
  const [isLog, setIsLog] = useState(false);
  const [correctStack, setCorrectStack] = useState<CorrectStackState>({ kind: 'idle' });

  const canAnalyze = selection.kind === 'layer' && analyze.kind !== 'running';
  const canToggleLog = selection.kind === 'layer' && correctStack.kind !== 'applying';

  const runAnalysis = async () => {
    setAnalyze({ kind: 'running' });
    try {
      const result = await analyzeCurrentFrame(bridge, frameSource, PROFILES[profileKey]!);
      setAnalyze({ kind: 'done', result });
    } catch (err) {
      setAnalyze({ kind: 'error', message: err instanceof Error ? err.message : String(err) });
    }
  };

  const toggleLog = async () => {
    const next = !isLog;
    setCorrectStack({ kind: 'applying' });
    try {
      const result = await setCorrectProfile(bridge, next, PROFILES['vlog']!);
      setIsLog(next);
      setCorrectStack({ kind: 'done', result });
    } catch (err) {
      setCorrectStack({ kind: 'error', message: err instanceof Error ? err.message : String(err) });
    }
  };

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
          <section class="analyze">
            <div class="correct-profile">
              <label class="vlog-toggle">
                <input
                  type="checkbox"
                  checked={isLog}
                  disabled={!canToggleLog}
                  data-testid="vlog-toggle"
                  onChange={toggleLog}
                />
                V-Log
              </label>
              {correctStack.kind === 'error' && (
                <p class="correct-stack-error" data-testid="correct-stack-error">
                  Correct stack failed: {correctStack.message}
                </p>
              )}
            </div>
            <div class="analyze-controls">
              <label class="profile-select">
                Footage
                <select
                  value={profileKey}
                  onChange={(e) => setProfileKey((e.target as HTMLSelectElement).value)}
                >
                  {Object.entries(PROFILES).map(([key, p]) => (
                    <option value={key}>{p.name}</option>
                  ))}
                </select>
              </label>
              <button
                class="analyze-btn"
                onClick={runAnalysis}
                disabled={!canAnalyze}
                data-testid="analyze-frame"
              >
                {analyze.kind === 'running' ? 'Analyzing…' : 'Analyze frame'}
              </button>
            </div>
            {analyze.kind === 'idle' && (
              <p class="placeholder">Select a layer, then analyze its current frame.</p>
            )}
            {analyze.kind === 'error' && (
              <p class="analyze-error" data-testid="analyze-error">
                Analyze failed: {analyze.message}
              </p>
            )}
            {analyze.kind === 'done' && <StatsView result={analyze.result} />}
          </section>
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
          <span class="analyzed" data-testid="analyzed-indicator" title="Analyzed-clip indicator">
            {analyze.kind === 'done' ? 'Analyzed: current frame' : 'Analyzed: -'}
          </span>
        </span>
      </footer>
    </div>
  );
}
