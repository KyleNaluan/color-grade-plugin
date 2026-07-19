import { describe, it, expect } from 'vitest';
import { mat3MulVec } from '../../src/core/color/matrices.js';
import { slog3Encode, slog3Decode, SLOG3 } from '../../src/core/color/slog3.js';
import {
  canonLog2Encode,
  canonLog2Decode,
  canonLog3Encode,
  canonLog3Decode,
  CLOG2,
  CLOG3,
} from '../../src/core/color/canonlog.js';
import { logC3Encode, logC3Decode, logC4Encode, logC4Decode, LOGC3, LOGC4 } from '../../src/core/color/arrilogc.js';
import { dLogEncode, dLogDecode, DLOG } from '../../src/core/color/dlog.js';
import { filmGen5Encode, filmGen5Decode, FILM_GEN5 } from '../../src/core/color/filmgen5.js';
import { flogEncode, flogDecode, flog2Encode, flog2Decode, FLOG, FLOG2 } from '../../src/core/color/flog.js';
import { nLogEncode, nLogDecode, NLOG } from '../../src/core/color/nlog.js';
import { PROFILES, FOOTAGE_PROFILES } from '../../src/core/color/index.js';
import type { LogProfile } from '../../src/core/color/types.js';

/*
 * Each profile is validated against the maker's published reference code values
 * for 0% reflectance, 18% grey, and 90% white (not just round-trip
 * self-consistency), plus decode(encode(x)) == x across the range and the gamut
 * matrix mapping white to white.
 */

interface RefPoints {
  black: number; // encode(0)
  grey18: number; // encode(0.18)
  white90: number; // encode(0.9)
}

function checkEncodeReferences(name: string, encode: (x: number) => number, ref: RefPoints): void {
  describe(`${name} published reference code values`, () => {
    it('encodes 0% reflectance to the published black', () => {
      expect(encode(0)).toBeCloseTo(ref.black, 5);
    });
    it('encodes 18% grey to the published mid-grey', () => {
      expect(encode(0.18)).toBeCloseTo(ref.grey18, 4);
    });
    it('encodes 90% white to the published value', () => {
      expect(encode(0.9)).toBeCloseTo(ref.white90, 4);
    });
  });
}

function checkRoundTrip(name: string, encode: (x: number) => number, decode: (y: number) => number): void {
  it(`${name}: decode inverts encode across the range`, () => {
    for (const x of [0, 0.005, 0.02, 0.09, 0.18, 0.45, 0.9, 1, 2, 6]) {
      expect(decode(encode(x))).toBeCloseTo(x, 5);
    }
  });
}

function checkGamutWhite(name: string, profile: LogProfile): void {
  it(`${name}: gamut -> Rec.709 maps white to white (rows sum to 1)`, () => {
    const [r, g, b] = mat3MulVec(profile.gamutToRec709, [1, 1, 1]);
    expect(r).toBeCloseTo(1, 5);
    expect(g).toBeCloseTo(1, 5);
    expect(b).toBeCloseTo(1, 5);
  });
}

checkEncodeReferences('Sony S-Log3', slog3Encode, { black: 0.0928641, grey18: 0.4105572, white90: 0.5844195 });
checkEncodeReferences('Canon C-Log2', canonLog2Encode, { black: 0.0928641, grey18: 0.398236, white90: 0.562313 });
checkEncodeReferences('Canon C-Log3', canonLog3Encode, { black: 0.1251222, grey18: 0.3433894, white90: 0.5644615 });
checkEncodeReferences('ARRI LogC3', logC3Encode, { black: 0.092809, grey18: 0.391007, white90: 0.55943 });
checkEncodeReferences('ARRI LogC4', logC4Encode, { black: 0.0928641, grey18: 0.278403, white90: 0.4179594 });
checkEncodeReferences('DJI D-Log', dLogEncode, { black: 0.0929, grey18: 0.3987646, white90: 0.5729444 });
checkEncodeReferences('Blackmagic Film Gen5', filmGen5Encode, {
  black: 0.0924658,
  grey18: 0.383546,
  white90: 0.521382,
});
checkEncodeReferences('Fujifilm F-Log', flogEncode, { black: 0.092864, grey18: 0.459324, white90: 0.689526 });
checkEncodeReferences('Fujifilm F-Log2', flog2Encode, { black: 0.092864, grey18: 0.391006, white90: 0.557131 });
checkEncodeReferences('Nikon N-Log', nLogEncode, { black: 0.1243726, grey18: 0.363674, white90: 0.589634 });

describe('log profile round-trips', () => {
  checkRoundTrip('S-Log3', slog3Encode, slog3Decode);
  checkRoundTrip('C-Log2', canonLog2Encode, canonLog2Decode);
  checkRoundTrip('C-Log3', canonLog3Encode, canonLog3Decode);
  checkRoundTrip('LogC3', logC3Encode, logC3Decode);
  checkRoundTrip('LogC4', logC4Encode, logC4Decode);
  checkRoundTrip('D-Log', dLogEncode, dLogDecode);
  checkRoundTrip('Film Gen5', filmGen5Encode, filmGen5Decode);
  checkRoundTrip('F-Log', flogEncode, flogDecode);
  checkRoundTrip('F-Log2', flog2Encode, flog2Decode);
  checkRoundTrip('N-Log', nLogEncode, nLogDecode);
});

describe('log profile gamut matrices', () => {
  checkGamutWhite('S-Gamut3.Cine', SLOG3);
  checkGamutWhite('Cinema Gamut (C-Log2)', CLOG2);
  checkGamutWhite('Cinema Gamut (C-Log3)', CLOG3);
  checkGamutWhite('ARRI Wide Gamut 3', LOGC3);
  checkGamutWhite('ARRI Wide Gamut 4', LOGC4);
  checkGamutWhite('D-Gamut', DLOG);
  checkGamutWhite('Blackmagic Wide Gamut Gen5', FILM_GEN5);
  checkGamutWhite('F-Gamut (F-Log)', FLOG);
  checkGamutWhite('F-Gamut (F-Log2)', FLOG2);
  checkGamutWhite('N-Gamut', NLOG);
});

describe('PROFILES registry and FOOTAGE_PROFILES catalog', () => {
  it('registers all shipped profile keys', () => {
    expect(Object.keys(PROFILES).sort()).toEqual(
      [
        'arri-logc3',
        'arri-logc4',
        'bmd-filmgen5',
        'canon-clog2',
        'canon-clog3',
        'dji-dlog',
        'fuji-flog',
        'fuji-flog2',
        'nikon-nlog',
        'rec709',
        'sony-slog3',
        'vlog',
      ].sort(),
    );
  });

  it('catalog is contiguously 1-indexed, Standard first, cameras alphabetical', () => {
    FOOTAGE_PROFILES.forEach((e, i) => expect(e.index).toBe(i + 1));
    expect(FOOTAGE_PROFILES[0]!.key).toBe('rec709');
    expect(FOOTAGE_PROFILES[0]!.camera).toBe('');
    const cameras = FOOTAGE_PROFILES.slice(1).map((e) => e.camera);
    const uniqueInOrder = cameras.filter((c, i) => cameras.indexOf(c) === i);
    expect(uniqueInOrder).toEqual([...uniqueInOrder].sort());
  });

  it('every catalog entry maps to a registered profile', () => {
    for (const e of FOOTAGE_PROFILES) expect(PROFILES[e.key]).toBeDefined();
  });

  it('preserves the original V-Log key and name (Lumix behavior unchanged)', () => {
    expect(PROFILES.vlog!.name).toBe('V-Log');
    expect(FOOTAGE_PROFILES.find((e) => e.key === 'vlog')?.flatLabel).toBe('Panasonic - V-Log');
  });
});
