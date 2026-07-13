import preact from '@preact/preset-vite';
import { defineConfig } from 'vite';

// The panel is loaded by CEP from the filesystem, so assets must resolve
// relative to index.html (base './'). Output lands in dist/panel; the
// assemble-cep script adds the CEP manifest and ExtendScript host around it.
export default defineConfig({
  root: 'src/panel',
  base: './',
  plugins: [preact()],
  build: {
    outDir: '../../dist/panel',
    emptyOutDir: true,
    target: 'chrome99',
  },
});
