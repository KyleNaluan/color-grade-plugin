/**
 * The real CEP-backed Bridge: forwards typed commands to ExtendScript via
 * `evalScript` and decodes the JSON envelope. This is the only module that
 * touches the CEP runtime; everything above it sees the `Bridge` interface.
 */
import {
  type Bridge,
  BridgeError,
  type CorrectStackResult,
  type GradeResult,
  type SelectionSnapshot,
  parseBridgeResult,
} from './bridge';
import type { RenderedFrameRef } from './frameSource';

/** The subset of the injected CEP API the bridge needs. */
interface AdobeCep {
  evalScript(script: string, callback: (result: string) => void): void;
  /** Absolute path of a CEP SystemPath (e.g. 'userData'), used for scratch files. */
  getSystemPath(type: string): string;
}

/** Minimal shape of the injected `cep.fs` API used to stage the Decode LUT. */
interface CepFs {
  writeFile(path: string, data: string): { err: number };
  deleteFile(path: string): { err: number };
}

/** CEP's sentinel result when the ExtendScript call itself failed. */
const EVAL_SCRIPT_ERROR = 'EvalScript error.';

/** `cep.fs` error code for "no error". */
const CEP_NO_ERROR = 0;

function getAdobeCep(): AdobeCep {
  const cep = (globalThis as { __adobe_cep__?: AdobeCep }).__adobe_cep__;
  if (!cep) {
    throw new BridgeError('CEP runtime not available (panel not running inside CEP?)');
  }
  return cep;
}

function getCepFs(): CepFs {
  const cep = (globalThis as { cep?: { fs?: CepFs } }).cep;
  if (!cep || !cep.fs) {
    throw new BridgeError('CEP fs API not available (panel not running inside CEP?)');
  }
  return cep.fs;
}

/**
 * Stage a payload (baked .cube text, recipe JSON, ...) in a temp file and
 * return its path, so only the path - not the multi-megabyte body - crosses the
 * `evalScript` boundary, the same out-of-band transport the render path uses.
 * ExtendScript moves the file into the Project-state folder and deletes this
 * scratch copy.
 */
function stageTempFile(text: string, ext: string): string {
  const dir = getAdobeCep().getSystemPath('userData');
  const stamp = `${Date.now()}_${Math.floor(Math.random() * 1_000_000)}`;
  const path = `${dir}/cg_${stamp}${ext}`;
  const result = getCepFs().writeFile(path, text);
  if (result.err !== CEP_NO_ERROR) {
    throw new BridgeError(`cep.fs.writeFile failed (err ${result.err}) for ${path}`);
  }
  return path;
}

/**
 * Best-effort delete of a staged scratch file. ExtendScript deletes it once its
 * body runs, but if `evalScript` rejects before that (dispatch/parse failure)
 * the file would leak, so the panel cleans up too.
 */
function unstageTempFile(path: string): void {
  try {
    getCepFs().deleteFile(path);
  } catch {
    // nothing more we can do; never let cleanup mask the original failure
  }
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
      targetLayerId: number,
    ): Promise<CorrectStackResult> {
      const stagedPath = decodeLutCube === null ? null : stageTempFile(decodeLutCube, '.cube');
      const pathArg = stagedPath === null ? 'null' : JSON.stringify(stagedPath);
      try {
        return parseBridgeResult<CorrectStackResult>(
          await evalScript(`CG_setCorrectProfile(${isLog}, ${pathArg}, ${targetLayerId})`),
        );
      } catch (err) {
        if (stagedPath !== null) unstageTempFile(stagedPath);
        throw err;
      }
    },
    async applyGrade(
      gradeLutCube: string,
      recipeJson: string,
      analyzedLayerId: number,
    ): Promise<GradeResult> {
      // Both the .cube and its recipe cross out-of-band as temp files, so the
      // multi-megabyte LUT text never inlines into the evalScript string.
      const cubePath = stageTempFile(gradeLutCube, '.cube');
      let recipePath: string | null = null;
      try {
        recipePath = stageTempFile(recipeJson, '.json');
        return parseBridgeResult<GradeResult>(
          await evalScript(
            `CG_applyGrade(${JSON.stringify(cubePath)}, ${JSON.stringify(recipePath)}, ${analyzedLayerId})`,
          ),
        );
      } catch (err) {
        unstageTempFile(cubePath);
        if (recipePath !== null) unstageTempFile(recipePath);
        throw err;
      }
    },
  };
}
