import { describe, expect, it } from 'vitest';
import { checkCoreFile, extractImports } from '../../scripts/lib/corePurity';

const FILE = 'src/core/engine/engine.ts';

describe('extractImports', () => {
  it('finds static, side-effect, re-export, dynamic and require imports', () => {
    const source = [
      "import { a } from './a';",
      "import './side-effect';",
      "export { b } from '../color/lab';",
      "const c = await import('./lazy');",
      "const d = require('fs');",
    ].join('\n');
    expect(extractImports(source).map((i) => i.specifier)).toEqual([
      './a',
      './side-effect',
      '../color/lab',
      './lazy',
      'fs',
    ]);
  });
});

describe('checkCoreFile', () => {
  it('accepts relative imports that stay inside src/core', () => {
    expect(checkCoreFile(FILE, "import { rgbToLab } from '../color/lab';")).toEqual([]);
  });

  it('rejects Node builtins', () => {
    expect(checkCoreFile(FILE, "import { readFileSync } from 'node:fs';")).toHaveLength(1);
    expect(checkCoreFile(FILE, "import * as fs from 'fs';")).toHaveLength(1);
  });

  it('rejects npm packages and CEP libs', () => {
    expect(checkCoreFile(FILE, "import { h } from 'preact';")).toHaveLength(1);
    expect(checkCoreFile(FILE, "import CSInterface from 'csinterface';")).toHaveLength(1);
  });

  it('rejects panel, host, and extendscript imports', () => {
    for (const target of ['../../panel/App', '../../host/bridge', '../../extendscript/host']) {
      const violations = checkCoreFile(FILE, `import { x } from '${target}';`);
      expect(violations).toHaveLength(1);
      expect(violations[0]!.reason).toContain('outside src/core/');
    }
  });

  it('reports the offending line number', () => {
    const source = "import { a } from './a';\nimport { join } from 'node:path';\n";
    expect(checkCoreFile(FILE, source)).toMatchObject([{ line: 2, specifier: 'node:path' }]);
  });
});

describe('extractImports statement boundaries', () => {
  it('does not chain an unrelated export across code to a later import', () => {
    const source = [
      'export function xyzToLab(v: number) {',
      '  return v * 2;',
      '}',
      "import { join } from 'node:path';",
    ].join('\n');
    expect(extractImports(source)).toEqual([{ specifier: 'node:path', line: 4 }]);
  });

  it('handles multi-line named imports', () => {
    const source = "import {\n  a,\n  b,\n} from './a';";
    expect(extractImports(source)).toEqual([{ specifier: './a', line: 1 }]);
  });
});
