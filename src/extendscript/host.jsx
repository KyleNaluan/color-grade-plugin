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
      layerId: selected.length > 0 ? selected[0].id : null,
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
    // `time` is comp-internal (0-based, as returned by comp.time), but the
    // render queue's timeSpanStart lives in the comp's displayStartTime domain:
    // for a timecode-offset comp (displayStartTime != 0) a bare `time` of 0
    // falls outside the render range and produces no frame. Shift into that
    // domain so the requested frame renders regardless of displayStartTime.
    rqItem.timeSpanStart = comp.displayStartTime + time;
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
  // saveFrameToPng takes comp-internal time (0-based), so - unlike the render
  // queue's timeSpanStart - no displayStartTime shift is applied here.
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

/*
 * --- Correct stack: V-Log/standard toggle (Managed effects) ---
 *
 * Decode LUT via Apply Color LUT (matchName 'ADBE Apply Color LUT2'), then
 * Lumetri Color (matchName 'ADBE Lumetri'). Both get a ' [cg]' marker
 * appended to their display name so the panel/user can identify Managed
 * effects (see CONTEXT.md). All color math (the LUT contents) is computed on
 * the panel side; this file only wires up AE DOM effect/file operations.
 */

var CG_MANAGED_MARKER = ' [cg]';
var CG_DECODE_LUT_MATCH_NAME = 'ADBE Apply Color LUT2';
var CG_APPLY_COLOR_LUT_MATCH_NAME = CG_DECODE_LUT_MATCH_NAME;
var CG_LUMETRI_MATCH_NAME = 'ADBE Lumetri';

/** Find a Managed ([cg]-tagged) effect of the given matchName on a layer, or null. */
function CG_findManagedEffect(layer, matchName) {
  var effects = layer.property('ADBE Effect Parade');
  for (var i = 1; i <= effects.numProperties; i++) {
    var fx = effects.property(i);
    if (fx.matchName === matchName && fx.name.indexOf('[cg]') !== -1) return fx;
  }
  return null;
}

/** Ensure a Managed Lumetri Color effect exists on the layer; return it. */
function CG_ensureLumetri(layer) {
  var existing = CG_findManagedEffect(layer, CG_LUMETRI_MATCH_NAME);
  if (existing) return existing;
  var fx = layer.property('ADBE Effect Parade').addProperty(CG_LUMETRI_MATCH_NAME);
  fx.name = 'Lumetri Color' + CG_MANAGED_MARKER;
  return fx;
}

/**
 * Ensure a Managed Apply Color LUT effect exists, points at `lutFile`, and
 * sits directly before Lumetri in the stack (Decode LUT then Lumetri).
 */
function CG_ensureDecodeLut(layer, lutFile) {
  var effects = layer.property('ADBE Effect Parade');
  var fx = CG_findManagedEffect(layer, CG_DECODE_LUT_MATCH_NAME);
  if (!fx) {
    fx = effects.addProperty(CG_DECODE_LUT_MATCH_NAME);
    fx.name = 'Apply Color LUT' + CG_MANAGED_MARKER;
    var lumetri = CG_findManagedEffect(layer, CG_LUMETRI_MATCH_NAME);
    if (lumetri) fx.moveTo(lumetri.propertyIndex);
  }
  // Apply Color LUT's sole property is the LUT file picker.
  fx.property(1).setValue(lutFile);
  return fx;
}

/** The layer in `comp` whose stable id matches `id`, or null if none. */
function CG_findLayerById(comp, id) {
  for (var i = 1; i <= comp.numLayers; i++) {
    if (comp.layer(i).id === id) return comp.layer(i);
  }
  return null;
}

/** Remove the Managed Decode LUT effect from the layer, if present. */
function CG_removeDecodeLut(layer) {
  var existing = CG_findManagedEffect(layer, CG_DECODE_LUT_MATCH_NAME);
  if (existing) existing.remove();
}

/** Guard-delete the panel's staged scratch .cube, ignoring any delete error. */
function CG_deleteStagedLut(decodeLutPath) {
  if (!decodeLutPath) return;
  try {
    var temp = new File(decodeLutPath);
    if (temp.exists) temp.remove();
  } catch (rmErr) {}
}

/**
 * The `.colorgrade/` Project-state folder next to the .aep, creating it if
 * needed. Returns null when the project has never been saved (no .aep path
 * to sit next to yet).
 */
function CG_projectStateFolder() {
  if (!app.project || !app.project.file) return null;
  var folder = new Folder(app.project.file.parent.fsName + '/.colorgrade');
  if (!folder.exists) folder.create();
  return folder;
}

/**
 * Flag the selected layer V-Log or standard, building/updating its Correct
 * stack. V-Log: moves the LUT text the panel already wrote to `decodeLutPath`
 * (a temp file - the .cube is transferred out-of-band, not inlined into this
 * script) into the Project-state folder and ensures Apply Color LUT (pointed
 * at that file) then Lumetri, both `[cg]`-tagged. Standard: removes the
 * Managed Decode LUT effect, leaving Lumetri. Returns { decodeLutPath } (null
 * when standard).
 *
 * The layer to mutate is resolved by `targetLayerId` (the id the panel captured
 * when the toggle fired), not by whatever happens to be selected now. If that
 * layer is gone or the selection moved off it mid-flight, the call refuses
 * rather than mutating the wrong clip.
 *
 * All can-fail validation runs before any layer mutation, so a failure path
 * never leaves an orphan Managed effect behind. The panel's staged scratch
 * .cube is always deleted before returning (success or failure), so a failed
 * V-Log attempt never leaves a multi-megabyte orphan in userData.
 */
function CG_setCorrectProfile(isLog, decodeLutPath, targetLayerId) {
  try {
    try {
      var comp = CG_activeComp();
      if (comp === null) return CG_fail('no active comp');
      var selected = comp.selectedLayers;
      if (selected.length === 0) return CG_fail('no layer selected');
      var layer = CG_findLayerById(comp, targetLayerId);
      if (layer === null || selected[0].id !== targetLayerId) {
        return CG_fail('selection changed before the Correct stack could be applied - try again');
      }

      var lutPath = null;
      if (isLog) {
        if (!decodeLutPath) return CG_fail('missing decode LUT for V-Log');
        var folder = CG_projectStateFolder();
        if (folder === null) return CG_fail('save the project before flagging V-Log clips');
        var temp = new File(decodeLutPath);
        if (!temp.exists) return CG_fail('decode LUT temp file not found: ' + decodeLutPath);
        var dest = new File(folder.fsName + '/decode_' + layer.id + '.cube');
        if (dest.exists) dest.remove();
        if (!temp.copy(dest)) return CG_fail('could not copy decode LUT into project-state folder');

        CG_ensureLumetri(layer);
        CG_ensureDecodeLut(layer, dest);
        lutPath = dest.fsName;
      } else {
        CG_ensureLumetri(layer);
        CG_removeDecodeLut(layer);
      }

      return CG_ok({ decodeLutPath: lutPath });
    } finally {
      if (isLog) CG_deleteStagedLut(decodeLutPath);
    }
  } catch (err) {
    return CG_fail(err && err.message ? err.message : err);
  }
}

/*
 * --- Grade: baked .cube on a Managed adjustment layer ---
 *
 * The grade is one Apply Color LUT effect (matchName 'ADBE Apply Color LUT2')
 * on a single Managed ([cg]) adjustment layer at the top of the comp, so it
 * grades every clip below it. The LUT contents are computed on the panel side;
 * this file only wires up AE DOM layer/effect/file operations.
 */

/** The Managed ([cg]-tagged) adjustment layer in `comp`, or null if none. */
function CG_findManagedGradeLayer(comp) {
  for (var i = 1; i <= comp.numLayers; i++) {
    var layer = comp.layer(i);
    if (layer.adjustmentLayer && layer.name.indexOf('[cg]') !== -1) return layer;
  }
  return null;
}

/**
 * Enable/disable the single Managed ([cg]) grade adjustment layer. Returns a
 * scalar `true` when such a layer existed and was toggled, `false` when none
 * exists (a no-op). The panel disables the grade layer around the analysis
 * render so a re-grade measures post-Correct pixels, not already-graded ones.
 */
function CG_setGradeLayerEnabled(enabled) {
  try {
    var comp = CG_activeComp();
    if (comp === null) return CG_okRaw(false);
    var layer = CG_findManagedGradeLayer(comp);
    if (layer === null) return CG_okRaw(false);
    layer.enabled = enabled;
    return CG_okRaw(true);
  } catch (err) {
    return CG_fail(err && err.message ? err.message : err);
  }
}

/**
 * Ensure the single Managed grade adjustment layer exists at the top of the
 * comp; return it. Created as a full-comp adjustment solid so it affects every
 * clip below.
 */
function CG_ensureGradeLayer(comp) {
  var existing = CG_findManagedGradeLayer(comp);
  if (existing) return existing;
  var layer = comp.layers.addSolid(
    [1, 1, 1],
    'Grade' + CG_MANAGED_MARKER,
    comp.width,
    comp.height,
    comp.pixelAspect
  );
  layer.adjustmentLayer = true;
  layer.moveToBeginning();
  return layer;
}

/** Ensure a Managed Apply Color LUT effect on `layer` pointed at `lutFile`. */
function CG_ensureGradeLut(layer, lutFile) {
  var fx = CG_findManagedEffect(layer, CG_APPLY_COLOR_LUT_MATCH_NAME);
  if (!fx) {
    fx = layer.property('ADBE Effect Parade').addProperty(CG_APPLY_COLOR_LUT_MATCH_NAME);
    fx.name = 'Apply Color LUT' + CG_MANAGED_MARKER;
  }
  // Apply Color LUT's sole property is the LUT file picker.
  fx.property(1).setValue(lutFile);
  return fx;
}

/**
 * Apply a baked grade to the active comp. The panel already analyzed the
 * post-Correct frame, built the transform toward the chosen Theme, and baked
 * the grade into the temp file at `gradeLutPath` (transferred out-of-band, not
 * inlined into this script), with the recipe inputs in `recipePath`. Both are
 * moved into the Project-state folder (named after the analyzed clip) and the
 * single Managed grade adjustment layer's Apply Color LUT is pointed at the
 * .cube. Returns { gradeLutPath, recipePath } of the persisted files.
 *
 * `analyzedLayerId` is the clip whose frame was analyzed; the call refuses if
 * that clip is no longer in the comp, so a stale recipe never lands. All
 * can-fail validation (comp, clip, saved project, temp files, copies) runs
 * before any comp mutation, so a failure path never leaves an orphan Managed
 * layer or effect behind. The panel's staged scratch files are always deleted
 * before returning (success or failure).
 */
function CG_applyGrade(gradeLutPath, recipePath, analyzedLayerId) {
  try {
    try {
      var comp = CG_activeComp();
      if (comp === null) return CG_fail('no active comp');
      var analyzed = CG_findLayerById(comp, analyzedLayerId);
      if (analyzed === null) {
        return CG_fail('the analyzed clip is no longer in the comp - re-analyze and try again');
      }
      if (!gradeLutPath) return CG_fail('missing grade LUT');
      if (!recipePath) return CG_fail('missing grade recipe');
      var folder = CG_projectStateFolder();
      if (folder === null) return CG_fail('save the project before grading');
      var tempCube = new File(gradeLutPath);
      if (!tempCube.exists) return CG_fail('grade LUT temp file not found: ' + gradeLutPath);
      var tempRecipe = new File(recipePath);
      if (!tempRecipe.exists) return CG_fail('grade recipe temp file not found: ' + recipePath);

      var destCube = new File(folder.fsName + '/grade_' + analyzed.id + '.cube');
      if (destCube.exists) destCube.remove();
      if (!tempCube.copy(destCube)) {
        return CG_fail('could not copy grade LUT into project-state folder');
      }
      var destRecipe = new File(folder.fsName + '/grade_' + analyzed.id + '.json');
      if (destRecipe.exists) destRecipe.remove();
      if (!tempRecipe.copy(destRecipe)) {
        return CG_fail('could not copy grade recipe into project-state folder');
      }

      var gradeLayer = CG_ensureGradeLayer(comp);
      CG_ensureGradeLut(gradeLayer, destCube);

      return CG_ok({ gradeLutPath: destCube.fsName, recipePath: destRecipe.fsName });
    } finally {
      CG_deleteStagedLut(gradeLutPath);
      CG_deleteStagedLut(recipePath);
    }
  } catch (err) {
    return CG_fail(err && err.message ? err.message : err);
  }
}
