# 0004. Multi-camera log/color-profile selector (not Lumix-only)

Date: 2026-07-19
Status: Accepted

## Context

The decode stage was Lumix-only: the footage popup offered just `Rec.709 (standard)` and `V-Log`.
A log profile is fully described by two published, parametric pieces of data: a per-channel transfer curve (`decode`) and a scene-linear camera-gamut -> Rec.709 matrix (`gamutToRec709`).
Everything downstream (Correct decode LUT bake, stats, grade) already flows through the pluggable `LogProfile` seam (`src/core/color`, ADR 0001), so widening camera support is a data problem, not an engine problem.
UX decision recorded in `firstmate/data/cg-camera-profiles-decision.md` (captain-approved 2026-07-19).

## Decision

- **Profiles shipped** (each transcribed once in the TS core from the maker's published spec, then mirrored bit-exact into the native C++ port): Standard (Rec.709 passthrough), Panasonic V-Log, Sony S-Log3/S-Gamut3.Cine, Canon C-Log2 & C-Log3/Cinema Gamut, ARRI LogC3 (EI800)/AWG3 & LogC4/AWG4, DJI D-Log/D-Gamut, Blackmagic Film Gen5/BMD Wide Gamut Gen5, Fujifilm F-Log & F-Log2/F-Gamut (=BT.2020), Nikon N-Log/N-Gamut (=BT.2020). None skipped - every constant was sourced from an authoritative published-spec transcription (colour-science) and validated against the spec's documented 0%/18%/90% code values.
- **Registry**: `PROFILES` gains all keys; `FOOTAGE_PROFILES` (`src/core/color/index.ts`) is the ordered, UI-facing catalog and the single oracle the native `core/FootageCatalog.h` mirrors. Ordering: Standard first, cameras alphabetical.
- **AE Effect Controls**: one flat grouped popup ("Sony - S-Log3"), Standard first. The choice string + count are built from the catalog at `ParamsSetup`, so adding a profile needs no popup edit.
- **Editor window**: a Camera -> Profile cascade (both dropdowns derived from the same catalog). The Profile dropdown is never empty - every camera leads with "Standard (Rec.709)"; picking a camera auto-selects it. Both resolve to the same flat 1-based index the effect stores.
- **Fresh-apply default**: Standard/Rec.709 (index 1, no decode) - safe for non-log users; log shooters make one deliberate selection.
- **Ordering freedom**: this popup has no append-only/project-compat constraint (captain, same rationale as the theme popup's none-first reorder). AE keys it by index; reorder freely for best UX pre-release.

## Consequences

- The single-trilinear-sample and LUT-baking invariants are untouched: a profile only changes what the Decode LUT is baked from. V-Log behavior for a Lumix clip is unchanged (same `vlog` key/name, same decode).
- Correctness is gated two ways: `tests/unit/logProfiles.test.ts` checks each curve against its published reference code values (not just round-trip), and `npm run native:core-parity` bakes every profile's Decode LUT in both engines - achieved **bit-exact** (0 error across 11.2M samples), so any native transcription drift fails the harness. The editor cascade logic is pure and headless-tested in `native:editor-test`.
- Adding a future profile = one TS module + one `Themes.h`-style native header + a `getProfile` entry + a `FOOTAGE_PROFILES`/`FootageCatalog.h` row. No engine, type, or UI-plumbing change.
- Projects saved before this change have their footage selection permuted once (V-Log moved from index 2 to 11); accepted as pre-release, re-pick once.
