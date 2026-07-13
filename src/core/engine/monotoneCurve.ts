/**
 * Monotone piecewise-cubic (Fritsch-Carlson PCHIP) interpolation.
 * Used for the tone curve: guaranteed no overshoot/reversal between control points.
 */
export interface MonotoneCurve {
  (x: number): number;
}

export function makeMonotoneCurve(xsIn: number[], ysIn: number[]): MonotoneCurve {
  if (xsIn.length !== ysIn.length || xsIn.length < 2) {
    throw new Error('makeMonotoneCurve: need >= 2 matching points');
  }
  // Sort by x and drop points that are not strictly increasing in x.
  const pts = xsIn.map((x, i) => [x, ysIn[i]!] as const).sort((a, b) => a[0] - b[0]);
  const xs: number[] = [pts[0]![0]];
  const ys: number[] = [pts[0]![1]];
  for (const [x, y] of pts.slice(1)) {
    if (x > xs[xs.length - 1]! + 1e-6) {
      xs.push(x);
      ys.push(Math.max(y, ys[ys.length - 1]!)); // keep y monotone too
    }
  }
  const n = xs.length;
  if (n < 2) return (x) => x;

  const h: number[] = [];
  const delta: number[] = [];
  for (let i = 0; i < n - 1; i++) {
    h.push(xs[i + 1]! - xs[i]!);
    delta.push((ys[i + 1]! - ys[i]!) / h[i]!);
  }
  const m: number[] = new Array(n).fill(0);
  m[0] = delta[0]!;
  m[n - 1] = delta[n - 2]!;
  for (let i = 1; i < n - 1; i++) {
    m[i] = delta[i - 1]! * delta[i]! <= 0 ? 0 : (delta[i - 1]! + delta[i]!) / 2;
  }
  // Fritsch-Carlson limiter.
  for (let i = 0; i < n - 1; i++) {
    if (delta[i] === 0) {
      m[i] = 0;
      m[i + 1] = 0;
    } else {
      const a = m[i]! / delta[i]!;
      const b = m[i + 1]! / delta[i]!;
      const s = a * a + b * b;
      if (s > 9) {
        const t = 3 / Math.sqrt(s);
        m[i] = t * a * delta[i]!;
        m[i + 1] = t * b * delta[i]!;
      }
    }
  }

  return (x: number): number => {
    if (x <= xs[0]!) return ys[0]!;
    if (x >= xs[n - 1]!) return ys[n - 1]!;
    let i = 0;
    let lo = 0;
    let hi = n - 2;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (xs[mid + 1]! < x) lo = mid + 1;
      else if (xs[mid]! > x) hi = mid - 1;
      else {
        i = mid;
        break;
      }
      i = Math.min(Math.max(lo, 0), n - 2);
    }
    const t = (x - xs[i]!) / h[i]!;
    const t2 = t * t;
    const t3 = t2 * t;
    return (
      ys[i]! * (2 * t3 - 3 * t2 + 1) +
      h[i]! * m[i]! * (t3 - 2 * t2 + t) +
      ys[i + 1]! * (-2 * t3 + 3 * t2) +
      h[i]! * m[i + 1]! * (t3 - t2)
    );
  };
}
