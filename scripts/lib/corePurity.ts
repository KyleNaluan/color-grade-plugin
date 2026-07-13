/**
 * The core-purity rule: `src/core/` may import only relative modules that
 * resolve inside `src/core/`. That whitelist bans CEP, Node builtins, npm
 * packages, and panel/host/extendscript modules in one stroke, keeping all
 * color math runnable from any plain JS environment.
 */
import { posix } from 'node:path';

export interface Violation {
  /** Repo-relative path of the offending file. */
  file: string;
  /** 1-indexed line of the offending import. */
  line: number;
  specifier: string;
  reason: string;
}

// The `[^'";]` spans keep a match inside one statement: an import/export-from
// clause never contains a quote or semicolon before its specifier, while any
// intervening code does, so `export function …` cannot chain to a later `from`.
const IMPORT_PATTERNS = [
  /\bimport\s+[^'";]*?from\s*(['"])([^'"]+)\1/g, // import x from '...'
  /\bimport\s*(['"])([^'"]+)\1/g, // bare side-effect import '...'
  /\bexport\s+[^'";]*?from\s*(['"])([^'"]+)\1/g, // re-export
  /\bimport\s*\(\s*(['"])([^'"]+)\1\s*\)/g, // dynamic import
  /\brequire\s*\(\s*(['"])([^'"]+)\1\s*\)/g, // CommonJS require
];

/** Extract all static import specifiers with their line numbers. */
export function extractImports(source: string): Array<{ specifier: string; line: number }> {
  const found: Array<{ specifier: string; line: number }> = [];
  const seen = new Set<string>();
  for (const pattern of IMPORT_PATTERNS) {
    for (const match of source.matchAll(pattern)) {
      const key = `${match.index}:${match[2]}`;
      if (seen.has(key)) continue;
      seen.add(key);
      const line = source.slice(0, match.index).split('\n').length;
      found.push({ specifier: match[2]!, line });
    }
  }
  return found.sort((a, b) => a.line - b.line);
}

/**
 * Check one core source file. `file` is the repo-relative posix path
 * (e.g. `src/core/color/lab.ts`).
 */
export function checkCoreFile(file: string, source: string): Violation[] {
  const violations: Violation[] = [];
  for (const { specifier, line } of extractImports(source)) {
    if (!specifier.startsWith('./') && !specifier.startsWith('../')) {
      violations.push({
        file,
        line,
        specifier,
        reason: 'core/ must not import packages or builtins (CEP, Node, npm)',
      });
      continue;
    }
    const resolved = posix.normalize(posix.join(posix.dirname(file), specifier));
    if (!resolved.startsWith('src/core/')) {
      violations.push({
        file,
        line,
        specifier,
        reason: `core/ must not import outside src/core/ (resolves to ${resolved})`,
      });
    }
  }
  return violations;
}
