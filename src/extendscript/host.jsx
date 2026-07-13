/*
 * ExtendScript bridge for the color-grade panel.
 *
 * AE DOM operations only - zero color math or business logic lives here.
 * Every entry point returns a JSON envelope string:
 *   {"ok":true,"value":...} or {"ok":false,"error":"message"}
 * matching parseBridgeResult() on the panel side.
 *
 * This file targets the ES3-era ExtendScript engine: no JSON object,
 * no modern syntax.
 */

function CG_escapeJsonString(s) {
  var out = '';
  for (var i = 0; i < s.length; i++) {
    var c = s.charAt(i);
    var code = s.charCodeAt(i);
    if (c === '"') out += '\\"';
    else if (c === '\\') out += '\\\\';
    else if (code < 0x20) {
      var hex = code.toString(16);
      while (hex.length < 4) hex = '0' + hex;
      out += '\\u' + hex;
    } else out += c;
  }
  return out;
}

function CG_jsonValue(v) {
  if (v === null || v === undefined) return 'null';
  if (typeof v === 'number' || typeof v === 'boolean') return String(v);
  return '"' + CG_escapeJsonString(String(v)) + '"';
}

function CG_ok(pairs) {
  var body = '';
  for (var key in pairs) {
    if (body !== '') body += ',';
    body += '"' + key + '":' + CG_jsonValue(pairs[key]);
  }
  return '{"ok":true,"value":{' + body + '}}';
}

function CG_fail(message) {
  return '{"ok":false,"error":"' + CG_escapeJsonString(String(message)) + '"}';
}

/**
 * Selection query: the active comp's name plus the selected layer.
 * Returns a SelectionSnapshot (see src/host/bridge.ts).
 */
function CG_getSelection() {
  try {
    var comp = app.project ? app.project.activeItem : null;
    if (comp === null || !(comp instanceof CompItem)) {
      return CG_ok({ compName: null, layerName: null, selectedCount: 0 });
    }
    var selected = comp.selectedLayers;
    return CG_ok({
      compName: comp.name,
      layerName: selected.length > 0 ? selected[0].name : null,
      selectedCount: selected.length
    });
  } catch (err) {
    return CG_fail(err && err.message ? err.message : err);
  }
}
