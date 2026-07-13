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
    // The panel is a locally-installed CEP extension, so bundle size is
    // irrelevant, but readable output matters: minification renames local
    // functions (e.g. systemPathToNative), which makes it impossible to verify
    // by name that a fix actually shipped in dist/ and hides symbols from CEF
    // DevTools stack traces. Keep the panel unminified for verifiability and
    // debuggability. (`vite build` always recompiles from source; node_modules/
    // .vite is only the dev dep-prebundle cache and never feeds a prod build.)
    minify: false,
  },
});
