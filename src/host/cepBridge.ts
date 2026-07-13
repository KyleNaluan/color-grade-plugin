/**
 * The real CEP-backed Bridge: forwards typed commands to ExtendScript via
 * `evalScript` and decodes the JSON envelope. This is the only module that
 * touches the CEP runtime; everything above it sees the `Bridge` interface.
 */
import {
  type Bridge,
  BridgeError,
  type CorrectStackResult,
  type SelectionSnapshot,
  parseBridgeResult,
} from './bridge';
import type { RenderedFrameRef } from './frameSource';

/** The subset of the injected CEP API the bridge needs. */
interface AdobeCep {
  evalScript(script: string, callback: (result: string) => void): void;
}

/** CEP's sentinel result when the ExtendScript call itself failed. */
const EVAL_SCRIPT_ERROR = 'EvalScript error.';

function getAdobeCep(): AdobeCep {
  const cep = (globalThis as { __adobe_cep__?: AdobeCep }).__adobe_cep__;
  if (!cep) {
    throw new BridgeError('CEP runtime not available (panel not running inside CEP?)');
  }
  return cep;
}

function evalScript(script: string): Promise<string> {
  return new Promise((resolve, reject) => {
    try {
      getAdobeCep().evalScript(script, (result) => {
        if (result === EVAL_SCRIPT_ERROR) {
          reject(new BridgeError(`ExtendScript call failed: ${script}`));
        } else {
          resolve(result);
        }
      });
    } catch (err) {
      reject(err);
    }
  });
}

export function createCepBridge(): Bridge {
  return {
    async getSelection(): Promise<SelectionSnapshot> {
      return parseBridgeResult<SelectionSnapshot>(await evalScript('CG_getSelection()'));
    },
    async getCurrentTime(): Promise<number | null> {
      return parseBridgeResult<number | null>(await evalScript('CG_getCurrentTime()'));
    },
    async renderFrame(time: number): Promise<RenderedFrameRef> {
      return parseBridgeResult<RenderedFrameRef>(await evalScript(`CG_renderFrame(${time})`));
    },
    async setCorrectProfile(
      isLog: boolean,
      decodeLutCube: string | null,
    ): Promise<CorrectStackResult> {
      const cubeArg = decodeLutCube === null ? 'null' : JSON.stringify(decodeLutCube);
      return parseBridgeResult<CorrectStackResult>(
        await evalScript(`CG_setCorrectProfile(${isLog}, ${cubeArg})`),
      );
    },
  };
}
