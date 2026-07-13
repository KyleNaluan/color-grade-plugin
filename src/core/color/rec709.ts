import type { LogProfile } from './types.js';

/** BT.709 OETF (scene linear -> encoded). */
export function rec709Encode(linear: number): number {
  return linear < 0.018 ? 4.5 * linear : 1.099 * Math.pow(linear, 0.45) - 0.099;
}

/** Inverse BT.709 OETF (encoded -> scene linear). */
export function rec709Decode(encoded: number): number {
  return encoded < 0.081 ? encoded / 4.5 : Math.pow((encoded + 0.099) / 1.099, 1 / 0.45);
}

const IDENTITY: [[number, number, number], [number, number, number], [number, number, number]] = [
  [1, 0, 0],
  [0, 1, 0],
  [0, 0, 1],
];

export const REC709: LogProfile = {
  name: 'Rec.709',
  decode: rec709Decode,
  encode: rec709Encode,
  gamutToRec709: IDENTITY,
};
