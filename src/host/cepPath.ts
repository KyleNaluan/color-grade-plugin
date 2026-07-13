/**
 * Path helpers for the CEP `cep.fs` boundary.
 *
 * `__adobe_cep__.getSystemPath(...)` returns a `file://` URI on Windows (e.g.
 * `file:///C:/Users/kylen/AppData/Roaming`), but `cep.fs.writeFile`/`readFile`/
 * `deleteFile` require a plain native OS path (`C:/Users/kylen/AppData/Roaming`).
 * Passing the URI straight through is what caused the Windows-AE
 * `cep.fs.writeFile failed (err 1)` on the Correct Decode LUT: the failure was
 * the `file://` scheme, not the payload size or encoding.
 *
 * Pure and host-agnostic so it is unit-testable without the CEP runtime.
 */

/**
 * Convert a `getSystemPath` result to a plain native OS path.
 *
 * - `file:///C:/Users/name/AppData` -> `C:/Users/name/AppData` (Windows: the
 *   URI's leading `/` before the drive letter is dropped).
 * - `file:///home/name/.config`     -> `/home/name/.config` (POSIX: absolute
 *   path's leading `/` is preserved).
 * - `%20`/other percent-escapes in the path (e.g. a username with a space) are
 *   decoded.
 * - An already-plain path (no `file://` scheme) is returned unchanged, so this
 *   is safe to apply to paths that are already native (e.g. `Folder.temp.fsName`).
 */
export function systemPathToNative(systemPath: string): string {
  if (!/^file:\/\//i.test(systemPath)) return systemPath;
  // Strip the scheme and any authority: `file:///C:/x` -> `/C:/x`.
  let rest = systemPath.replace(/^file:\/\/[^/]*/i, '');
  rest = decodeURIComponent(rest);
  // Windows drive path: `/C:/Users` -> `C:/Users`; POSIX `/home/x` is untouched.
  if (/^\/[A-Za-z]:/.test(rest)) rest = rest.slice(1);
  return rest;
}
