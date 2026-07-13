/**
 * Read-only display of Footage stats measured from the analyzed frame.
 * Mirrors the spike's printout (scripts/spike.ts) so panel numbers and
 * command-line numbers line up.
 */
import type { AnalyzeResult } from './analyze';

const f3 = (x: number) => x.toFixed(3);
const pct = (x: number) => `${(x * 100).toFixed(2)}%`;

export function StatsView({ result }: { result: AnalyzeResult }) {
  const s = result.stats;
  const p = s.lumaPercentiles;
  const sampled = result.width !== result.sourceWidth || result.height !== result.sourceHeight;
  const dims = sampled
    ? `${result.sourceWidth}×${result.sourceHeight} (sampled ${result.width}×${result.height})`
    : `${result.sourceWidth}×${result.sourceHeight}`;
  return (
    <div class="stats" data-testid="footage-stats">
      <header class="stats-head">
        <strong>Footage stats</strong>
        <span class="stats-meta">
          {dims} · {result.bitDepthOfSource}-bit · {result.profileName} · t={f3(result.time)}s
        </span>
      </header>
      <dl class="stats-grid">
        <dt>Luma pct</dt>
        <dd>
          p1 {f3(p.p1)} · p5 {f3(p.p5)} · p25 {f3(p.p25)} · p50 {f3(p.p50)} · p75 {f3(p.p75)} · p95 {f3(p.p95)} · p99 {f3(p.p99)}
        </dd>
        <dt>LAB mean</dt>
        <dd>
          L {f3(s.lab.mean[0])} · a {f3(s.lab.mean[1])} · b {f3(s.lab.mean[2])}
        </dd>
        <dt>LAB std</dt>
        <dd>
          L {f3(s.lab.std[0])} · a {f3(s.lab.std[1])} · b {f3(s.lab.std[2])}
        </dd>
        <dt>Band chroma</dt>
        <dd>
          shadows {f3(s.bandChroma.shadows)} · mids {f3(s.bandChroma.mids)} · highlights {f3(s.bandChroma.highlights)}
        </dd>
        <dt>Saturation</dt>
        <dd>
          mean {f3(s.saturation.mean)} · std {f3(s.saturation.std)}
        </dd>
        <dt>Skin</dt>
        <dd>{pct(s.skinPresence)}</dd>
        <dt>Clipping</dt>
        <dd>
          low {pct(s.clipping.low)} · high {pct(s.clipping.high)}
        </dd>
      </dl>
    </div>
  );
}
