// Assembles the installable CEP extension in dist/ after `vite build`:
//   dist/CSXS/manifest.xml   (from cep/)
//   dist/.debug              (from cep/)
//   dist/host/host.jsx       (ExtendScript bridge)
//   dist/panel/**            (already emitted by vite)
import { cpSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(repoRoot, 'dist');

mkdirSync(join(dist, 'host'), { recursive: true });
cpSync(join(repoRoot, 'cep', 'CSXS'), join(dist, 'CSXS'), { recursive: true });
cpSync(join(repoRoot, 'cep', '.debug'), join(dist, '.debug'));
cpSync(join(repoRoot, 'src', 'extendscript', 'host.jsx'), join(dist, 'host', 'host.jsx'));

console.log('CEP extension assembled in dist/');
