/**
 * Lint gate for core purity: fails (exit 1) if any file under src/core/
 * imports CEP, Node, npm packages, or panel/host/extendscript modules.
 * Rule logic lives in scripts/lib/corePurity.ts and is unit-tested.
 *
 * Run via `npm run lint`.
 */
import { readFileSync, readdirSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { checkCoreFile, type Violation } from './lib/corePurity';

const repoRoot = join(fileURLToPath(new URL('.', import.meta.url)), '..');
const coreDir = join(repoRoot, 'src', 'core');

function walk(dir: string): string[] {
  return readdirSync(dir, { withFileTypes: true }).flatMap((entry) => {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) return walk(full);
    return /\.(ts|tsx|js|jsx|mjs|cjs)$/.test(entry.name) ? [full] : [];
  });
}

const violations: Violation[] = walk(coreDir).flatMap((path) => {
  const repoRelative = relative(repoRoot, path).split(sep).join('/');
  return checkCoreFile(repoRelative, readFileSync(path, 'utf8'));
});

if (violations.length > 0) {
  for (const v of violations) {
    console.error(`${v.file}:${v.line} imports '${v.specifier}' - ${v.reason}`);
  }
  console.error(`\ncore purity check failed: ${violations.length} violation(s)`);
  process.exit(1);
}

console.log('core purity check passed');
