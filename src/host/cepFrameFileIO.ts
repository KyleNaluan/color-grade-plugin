/**
 * The real, CEP-backed FrameFileStore.
 *
 * It reads temporary render files through CEP's `cep.fs` API (no Node
 * integration required) and deletes them after use. This is the only module
 * besides `cepBridge` that touches the CEP runtime; the render backend above
 * it sees just the `FrameFileStore` interface, so it stays testable.
 */
import { BridgeError } from './bridge';
import type { FrameFileStore } from './frameSource';

/** Minimal shape of the injected `cep.fs` API this store needs. */
interface CepFs {
  readFile(path: string, encoding: string): { err: number; data: string };
  deleteFile(path: string): { err: number };
}
interface CepRuntime {
  fs: CepFs;
  encoding: { Base64: string };
}

/** `cep.fs` error code for "no error". */
const CEP_NO_ERROR = 0;

function getCep(): CepRuntime {
  const cep = (globalThis as { cep?: CepRuntime }).cep;
  if (!cep || !cep.fs) {
    throw new BridgeError('CEP fs API not available (panel not running inside CEP?)');
  }
  return cep;
}

function base64ToBytes(base64: string): Uint8Array {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

export function createCepFrameFileStore(): FrameFileStore {
  const store: FrameFileStore = {
    async read(path: string): Promise<Uint8Array> {
      const cep = getCep();
      const result = cep.fs.readFile(path, cep.encoding.Base64);
      if (result.err !== CEP_NO_ERROR) {
        throw new BridgeError(`cep.fs.readFile failed (err ${result.err}) for ${path}`);
      }
      return base64ToBytes(result.data);
    },
    async remove(path: string): Promise<void> {
      const cep = getCep();
      const result = cep.fs.deleteFile(path);
      if (result.err !== CEP_NO_ERROR) {
        throw new BridgeError(`cep.fs.deleteFile failed (err ${result.err}) for ${path}`);
      }
    },
  };
  return store;
}
