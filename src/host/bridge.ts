/**
 * The typed command boundary between the panel and the ExtendScript bridge.
 *
 * The bridge carries AE DOM queries only - zero color math. Panel logic
 * depends on the `Bridge` interface, never on CEP directly, so it can be
 * tested against a scripted fake.
 */

/** What is selected in the active comp right now. */
export interface SelectionSnapshot {
  /** Name of the active comp, or null when no comp is active. */
  compName: string | null;
  /** Name of the selected layer, or null when zero layers are selected. */
  layerName: string | null;
  /** How many layers are selected in the active comp (0 when no comp). */
  selectedCount: number;
}

export interface Bridge {
  /** Query the active comp and its selected layer. AE DOM read only. */
  getSelection(): Promise<SelectionSnapshot>;
}

/** Raised when the ExtendScript side returns an error or garbage. */
export class BridgeError extends Error {}

/**
 * Every ExtendScript entry point returns a JSON envelope:
 * `{"ok":true,"value":...}` or `{"ok":false,"error":"message"}`.
 */
export function parseBridgeResult<T>(raw: string): T {
  let envelope: unknown;
  try {
    envelope = JSON.parse(raw);
  } catch {
    throw new BridgeError(`Bridge returned non-JSON: ${raw}`);
  }
  if (typeof envelope !== 'object' || envelope === null || !('ok' in envelope)) {
    throw new BridgeError(`Bridge returned malformed envelope: ${raw}`);
  }
  const e = envelope as { ok: boolean; value?: T; error?: string };
  if (!e.ok) {
    throw new BridgeError(e.error ?? 'Unknown bridge error');
  }
  return e.value as T;
}
