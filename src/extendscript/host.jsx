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

/** Envelope for a single scalar (number/boolean/null/string) value. */
function CG_okRaw(v) {
  return '{"ok":true,"value":' + CG_jsonValue(v) + '}';
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

/** The active comp, or null when the active item is not a comp. */
function CG_activeComp() {
  var item = app.project ? app.project.activeItem : null;
  return item !== null && item instanceof CompItem ? item : null;
}

/**
 * Current-time indicator of the active comp, in seconds (null if no comp).
 * Returns a scalar envelope so the panel can pass it back to CG_renderFrame.
 */
function CG_getCurrentTime() {
  try {
    var comp = CG_activeComp();
    return CG_okRaw(comp === null ? null : comp.time);
  } catch (err) {
    return CG_fail(err && err.message ? err.message : err);
  }
}

/**
 * Name of the 16-bit TIFF output-module template used for the primary render
 * path. It must exist in the AE install and be configured for 16-bit TIFF
 * stills; the manual smoke checklist (docs/cep-install.md) verifies this and
 * documents creating it. If applying it fails, CG_renderFrame falls back to
 * an 8-bit PNG via saveFrameToPng.
 */
var CG_TIFF_TEMPLATE = 'CG 16-bit TIFF';

/** A unique temp-file path (Folder.temp) with the given extension. */
function CG_tempFramePath(ext) {
  var stamp = String(new Date().getTime()) + '_' + String(Math.floor(Math.random() * 1000000));
  return new File(Folder.temp.fsName + '/cg_frame_' + stamp + ext);
}

/**
 * Resolve the file the render queue actually wrote. AE may append a frame
 * number to a single-frame render, so if the exact file is absent we look for
 * a sibling that starts with the same stem.
 */
function CG_resolveRenderedFile(requested) {
  if (requested.exists) return requested;
  var stem = requested.name.replace(/\.[^.]+$/, '');
  var parent = requested.parent;
  var siblings = parent.getFiles(stem + '*');
  if (siblings && siblings.length > 0) return siblings[0];
  return requested;
}

/**
 * Guard-delete a temp render target and any frame-numbered siblings AE may have
 * written for it. Used on the failure path only; each remove is isolated so a
 * delete failure never masks the original render error.
 */
function CG_deleteFrameFiles(requested) {
  try {
    if (requested.exists) requested.remove();
  } catch (removeErr) {}
  try {
    var stem = requested.name.replace(/\.[^.]+$/, '');
    var siblings = requested.parent.getFiles(stem + '*');
    if (siblings) {
      for (var k = 0; k < siblings.length; k++) {
        try {
          siblings[k].remove();
        } catch (siblingErr) {}
      }
    }
  } catch (globErr) {}
}

/** Render one frame at `time` to 16-bit TIFF via the render queue. */
function CG_renderFrameToTiff(comp, time) {
  var requested = CG_tempFramePath('.tif');
  var rq = app.project.renderQueue;
  var rqItem = rq.items.add(comp);
  // Unqueue every OTHER queued item so render() only renders our frame item;
  // remember them so they can be restored afterward.
  var suspended = [];
  for (var i = 1; i <= rq.numItems; i++) {
    var other = rq.item(i);
    if (other !== rqItem && other.status === RQItemStatus.QUEUED) {
      suspended.push(other);
      other.render = false;
    }
  }
  try {
    rqItem.timeSpanStart = time;
    rqItem.timeSpanDuration = comp.frameDuration; // exactly one frame
    var om = rqItem.outputModule(1);
    om.applyTemplate(CG_TIFF_TEMPLATE);
    om.file = requested;
    rq.render();
  } catch (renderErr) {
    CG_deleteFrameFiles(requested);
    throw renderErr;
  } finally {
    for (var j = 0; j < suspended.length; j++) {
      try {
        suspended[j].render = true;
      } catch (restoreErr) {}
    }
    try {
      rqItem.remove();
    } catch (removeErr) {}
  }
  return CG_resolveRenderedFile(requested);
}

/** Render one frame at `time` to 8-bit PNG (fallback path). */
function CG_renderFrameToPng(comp, time) {
  var file = CG_tempFramePath('.png');
  comp.saveFrameToPng(time, file);
  return file;
}

/**
 * Render the active comp at `time` (seconds) to a temp file and return
 * { path, format }. Primary path is 16-bit TIFF through the render queue;
 * on any failure it falls back to 8-bit PNG. The panel reads the file back
 * through cep.fs and deletes it (see src/host/renderFrameSource.ts).
 */
function CG_renderFrame(time) {
  try {
    var comp = CG_activeComp();
    if (comp === null) return CG_fail('no active comp to render');
    var file, format;
    try {
      file = CG_renderFrameToTiff(comp, time);
      format = 'tiff';
    } catch (tiffErr) {
      file = CG_renderFrameToPng(comp, time);
      format = 'png';
    }
    if (!file || !file.exists) return CG_fail('render produced no file');
    return CG_ok({ path: file.fsName, format: format });
  } catch (err) {
    return CG_fail(err && err.message ? err.message : err);
  }
}
