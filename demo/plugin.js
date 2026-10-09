/**
 * Fiche — browser demo.
 *
 * Browsing a microfiche reader (SW Fiche, MF01, an effect). The one idea, from
 * AGENTS.md: a microfiche reader magnifies a hand. The clip is printed in a
 * grid of frames on a card; an operator browses it through a reader's lens,
 * and every movement of the hand on the carriage, every flick of the zoom and
 * every turn of the focus knob arrives on the screen multiplied twenty-four
 * times. The whip pans, the overshoot and correction, the crash zooms that lose
 * focus and the focus hunt are what the hand does, seen through that
 * magnifier.
 *
 * ---------------------------------------------------------------------------
 * What runs here, and what does not
 * ---------------------------------------------------------------------------
 *
 * **This page does not port the plugin. It runs it.** `fiche-core.wasm` is the
 * plugin's own C++ compiled UNMODIFIED with emscripten
 * (demo/tools/build-wasm.sh): `source/Fiche.cpp` — the plugin class, with its
 * constructor and declarations, its clock and seconds-or-milliseconds vote, its
 * own 120 BPM beat, the textures, the five passes and every uniform — and
 * everything it calls: Hand (Fitts's law, minimum jerk, the scatter and the
 * corrections, the crash zoom, the focus hunt, the carriage on its grip, the
 * PCG draws in their fixed order), Reader (the card, the lens, the bow, the
 * lamp, the film stocks), Font, Controls, Shaders and Diag, with the FFGL SDK's
 * CFFGLPlugin, its parameter bookkeeping, CFFGLPluginInfo, FFGLShader,
 * FFGLScreenQuad and the scoped bindings. Its GL calls go to this page's WebGL2
 * context through emscripten's GL library.
 *
 * What is the page's and not the plugin's:
 *
 *   - **The host.** demo/wasm/glue.cpp constructs the plugin, reads its
 *     parameter declarations back through the SDK's host getters — the panel
 *     below is built from them, not from a list typed here — and forwards
 *     SetFloatParameter, SetTextParameter, SetTime and ProcessOpenGL, as
 *     tools/mftest's Rig does. It never calls SetBeatInfo.
 *   - **GL entry points** (demo/wasm/gl_shim.cpp). glShaderSource: the plugin
 *     hands GL desktop GLSL 4.10; `shaderSource()` below refuses any text that
 *     is not byte for byte one of this page's assemblies of its copy
 *     (shaders.js, held to source/Shaders.cpp and Assemble() by
 *     demo/tools/check_shaders.py), then applies the kit's `port()` — the
 *     version line and the ES precision defaults, nothing else.
 *     glEnable / glDisable / glIsEnabled: GL_PROGRAM_POINT_SIZE, which the
 *     plugin saves and restores every frame, does not exist in WebGL2.
 *     glTexImage2D / glTexImage3D: the two textures the plugin renders its
 *     mips into (the clip's RGBA16F chain, the filmed store) are allocated as
 *     immutable storage of the same levels, format and size, because WebGL2
 *     (ES 3.0) will not render into a level of a mutable texture outside its
 *     BASE..MAX range and the plugin's hand-written mips do exactly that; and
 *     the bow's R32F texture becomes R16F only where the browser cannot filter
 *     32-bit floats (no OES_texture_float_linear).
 *   - **The panel's read-outs.** The number beside a slider is Controls.cpp's
 *     conversion, compiled in (glue.cpp's fiche_convert pairs each control with
 *     the function ProcessOpenGL applies to it); the units printed after it,
 *     and every tooltip, are this page's words.
 *   - **The About block's links.** The plugin opens them with system(); the
 *     page opens the same addresses itself, read from the plugin's own table.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Kit types for FFGL types.** The kit has no event or integer control. Jump
 * and the About buttons (FF_TYPE_EVENT) are buttons that send the press and
 * the release, 1 then 0, as the harness's cue sheets do. Columns, Rows and
 * Seed (FF_TYPE_INTEGER) are number fields clamped to the declared range.
 * Title (FF_TYPE_TEXT) is sent when the field is committed (Enter, or leaving
 * it), as a host sends text. The About line is shown read-only.
 *
 * **The About block is on the panel**, because the plugin declares it and
 * Resolume shows it, and its first button is the user guide.
 */

import { mountDemo } from './vendor/demo.js';
import { port, GLError } from './vendor/gl.js';
import * as S from './shaders.js';
import createFiche from './fiche-core.js';

//===========================================================================
// FFGL.h's parameter types, by number: what GetParamType returns.
//===========================================================================

const FF_TYPE_BOOLEAN = 0;
const FF_TYPE_EVENT = 1;
const FF_TYPE_STANDARD = 10;
const FF_TYPE_OPTION = 11;
const FF_TYPE_INTEGER = 13;
const FF_TYPE_TEXT = 100;

//===========================================================================
// The GLSL: the page's checked copy, assembled stage by stage in the order
// the plugin joins the pieces (shaders.js's ASSEMBLY, read out of Fiche.cpp's
// InitGL and Shaders.cpp's Assemble() by check_shaders.py).
//===========================================================================

const ASSEMBLED = Object.fromEntries(
  Object.entries(S.ASSEMBLY).map(([stage, pieces]) => [stage, pieces.map((piece) => S[piece]).join('')]),
);
const PAGE_SHADERS = {
  vertex: [ASSEMBLED.vertex],
  fragment: Object.entries(ASSEMBLED).filter(([stage]) => stage !== 'vertex').map(([, text]) => text),
};

/** Which stages the compiled plugin handed over, and whether each was ours. */
const shaderCheck = { matched: [], refused: 0 };

/**
 * Called by gl_shim.cpp's glShaderSource with the text the compiled plugin
 * handed to GL. Anything but one of this page's own assemblies is refused (an
 * empty source, so the plugin's InitGL fails and logs which pass) rather than
 * compiled.
 */
function shaderSource(stage, text) {
  const ours = PAGE_SHADERS[stage].indexOf(text);
  if (ours < 0) {
    shaderCheck.refused += 1;
    return '';
  }
  shaderCheck.matched.push(stage === 'vertex' ? 'vertex' : Object.keys(ASSEMBLED).filter((s) => s !== 'vertex')[ours]);
  return port(text, { stage });
}

//===========================================================================
// The module.
//===========================================================================

const printed = [];
let core;
try {
  core = await createFiche({
    ficheShaderSource: shaderSource,
    print: (line) => { printed.push(line); console.log(line); },
    printErr: (line) => { printed.push(line); console.warn(line); },
  });
} catch (error) {
  const root = document.querySelector('#demo');
  root.textContent = `The demo could not start: this browser would not run its WebAssembly (${error.message}). The plugin, its source and its downloads are at https://github.com/stoatworks-labs/fiche.`;
  throw error;
}

const text = (pointer) => core.UTF8ToString(pointer);

/** A C string for one call: allocated, handed over, freed. */
function withCString(value, fn) {
  const pointer = core.stringToNewUTF8(value);
  try {
    return fn(pointer);
  } finally {
    core._free(pointer);
  }
}

// Diag.cpp opens its log in the plugin's constructor, so its directory must
// exist in the page's memory before the first instance below.
core._fiche_prepare_log();

//===========================================================================
// The declarations, read from the plugin's constructor through the SDK.
//===========================================================================

function declarations() {
  const h = core._fiche_new();
  const list = [];
  const n = core._fiche_param_count(h);
  for (let i = 0; i < n; i += 1) {
    const type = core._fiche_param_type(h, i);
    const d = { index: i, name: text(core._fiche_param_name(h, i)), type, group: text(core._fiche_param_group(h, i)) };
    if (type === FF_TYPE_TEXT) d.default = text(core._fiche_param_default_text(h, i));
    else d.default = core._fiche_param_default(h, i);
    if (type === FF_TYPE_OPTION) {
      d.elements = [];
      d.values = [];
      for (let e = 0; e < core._fiche_param_element_count(h, i); e += 1) {
        d.elements.push(text(core._fiche_param_element_name(h, i, e)));
        d.values.push(core._fiche_param_element_value(h, i, e));
      }
    }
    if (type === FF_TYPE_INTEGER) {
      d.min = core._fiche_param_range_min(h, i);
      d.max = core._fiche_param_range_max(h, i);
    }
    list.push(d);
  }
  const maxInputs = core._fiche_max_inputs(h);
  core._fiche_delete(h);
  return { list, maxInputs };
}

const DECLARED = declarations();

// The About block: its first index, and the address each button opens, from
// the plugin's own table (StoatworksAboutLinks.h, compiled in). A label that
// does not match the declaration is a page bug, not something to paper over.
const ABOUT_FIRST = core._fiche_about_first();
const ABOUT_LINKS = new Map();
for (const d of DECLARED.list) {
  if (d.index <= ABOUT_FIRST || d.type !== FF_TYPE_EVENT) continue;
  const button = d.index - ABOUT_FIRST - 1;
  const label = text(core._fiche_about_button_label(button));
  if (label !== d.name) throw new Error(`About button ${button} is "${label}" in the plugin's table and "${d.name}" in its declaration`);
  ABOUT_LINKS.set(d.index, text(core._fiche_about_button_url(button)));
}

//===========================================================================
// The read-outs: Controls.cpp's number, this page's unit.
//===========================================================================

const fixed = (n, d) => n.toFixed(d);
const signed = (n, d) => `${n >= 0 ? '+' : '−'}${fixed(Math.abs(n), d)}`;
const UNITS = {
  Dwell: (x) => `${fixed(x, x < 1 ? 2 : 1)} s`,
  'Hand Speed': (x) => `${fixed(1000 * x, 0)} ms/bit`,
  Accuracy: (x) => `k ${fixed(x, 3)}`,
  'Carriage Play': (x, v) => (x === 0 ? 'rigid' : `${fixed(x, 1)} Hz ζ ${fixed(core._fiche_carriage_zeta(v), 2)}`),
  Zoom: (x) => `${fixed(x, 1)}×`,
  Focus: (x) => `${signed(x, 3)} mm`,
  Gutter: (x) => `${fixed(x, 2)} mm`,
  Interval: (x) => `${fixed(x, x < 1 ? 2 : 1)} s`,
  Flatness: (x) => `${fixed(x, 3)} mm`,
  Dust: (x) => `${fixed(x, 1)} /cm²`,
  Scratches: (x) => `${fixed(100 * x, 0)}% of bands`,
  Aperture: (x) => `f/${fixed(x, 1)}`,
  Parfocal: (x) => `${fixed(x, 2)} mm/stop`,
  Hotspot: (x) => (x === 0 ? 'none' : `${fixed(x, 0)} mm`),
  Lamp: (x) => `${fixed(x, 0)} K`,
  'Screen Grain': (x) => `±${fixed(100 * x, 1)}%`,
  'Room Light': (x) => `${fixed(100 * x, 1)}%`,
};

function readout(d) {
  return (v) => {
    const f = Math.fround(v);
    const x = core._fiche_convert(d.index, f);
    if (Number.isNaN(x)) return fixed(v, 2);
    return (UNITS[d.name] ?? ((y) => fixed(y, 3)))(x, f);
  };
}

/** Tooltips: this page's words, from the README's and AGENTS.md's account of each control. */
const HINTS = {
  Operator: 'Auto: the hand browses by itself. Manual: the hand is Position X/Y, the lens Zoom and the knob Focus — still through the carriage’s grip, the lens’s blur and the screen.',
  Browse: 'How Auto picks the next view: Reading (the next one), Skimming (a few on), Searching (anywhere on the card), Mixed (15 / 40 / 45%).',
  Dwell: 'The mean pause between acts, 0.1 s to 10 s (Gamma-distributed).',
  Sync: 'Free, or acts start on the beat: Beat, 2 Beats, Bar. This page sends no beat, so the plugin keeps its own 120 BPM clock — what it does in any host that sends none.',
  'Hand Speed': 'Fitts’s slope b, from 0.25 s/bit (slow) to 0.03 s/bit (fast): a pan lasts 0.08 + b log2(D/W + 1) seconds.',
  Accuracy: 'The landing scatter k, as a share of the distance moved: 25% to 1%. A miss by more than half the target is corrected after a reaction gap.',
  'Crash Zoom': 'How often a long move zooms out to travel, pans there, and crashes back in.',
  'Focus Skill': 'The operator’s reaction time, how much each pass through focus slows, and how often the first turn of the knob is the right way.',
  'Carriage Play': 'Rigid at 0; above it the carriage is a mass on the hand’s grip, from 40 Hz down to 3 Hz, so a hard stop overshoots and settles.',
  Jump: 'End the dwell now. FF_TYPE_EVENT: a press, then a release; the plugin counts the press.',
  Zoom: 'Magnification, 2× to 75×. In Auto, what the operator reads at; in Manual, the lens.',
  'Position X': 'Manual only: where the carriage is taken across the card.',
  'Position Y': 'Manual only: where the carriage is taken down the card.',
  Focus: 'Manual only: the defocus at the screen’s centre, ±1 mm, measured from best focus.',
  Layout: 'Card: an A6 card with its title header, margins and the reader’s glass beyond its edges. Endless: the grid of frames repeated for ever in every direction, no header and no edges, so the operator never runs out of page.',
  Columns: 'Frames across the card, 1–16. FF_TYPE_INTEGER in the plugin.',
  Rows: 'Frames down the card, 1–16 (the whole card shows them). FF_TYPE_INTEGER in the plugin.',
  Gutter: 'The clear space between frames, 0 to 3 mm.',
  Content: 'Live: every frame is the clip as it plays. Filmed: a step-and-repeat camera films the clip into the frames, one per Interval, in reading order.',
  Interval: 'Filmed only: seconds between frames filmed, 0.05 to 10.',
  Film: 'The stock the card is printed on: Ideal, Silver, Silver Negative, Diazo Blue, Diazo Black, Vesicular, Colour.',
  Flatness: 'The card’s bow between its glass plates, 0 to 0.5 mm: every frame sits at a slightly different height, so arriving means hunting for focus.',
  Dust: 'Opaque dust in the emulsion, up to 150 particles per square centimetre. It rides with the card.',
  Scratches: 'Emulsion scratches along the card: the chance a 2 mm band carries one.',
  Title: 'The header’s text, in a 5 × 7 dot font: 40 characters, upper case, digits and a little punctuation; anything else is a space. Sent when you press Enter or leave the field.',
  Aperture: 'The projection lens’s f-number, f/2 to f/16. A wider aperture has a shallower depth of focus and hunts longer.',
  Parfocal: 'How far the zoom throws the focus, 0 to 1 mm per doubling: a zoom that does not hold focus.',
  Shutter: 'The exposure, as a share of the frame: the length of the motion smear.',
  Hotspot: 'The projection’s cos⁴ falloff, from none to a 250 mm throw.',
  Lamp: 'The lamp’s colour temperature, 2000 K to 6504 K (white).',
  'Screen Grain': 'The rear-projection screen’s diffuser grain. It stays put while the picture moves.',
  'Room Light': 'The room’s light on the screen, up to 20% of the lamp’s white.',
  Seed: 'The card’s bow, dust, scratches and grain, and every draw the hand makes. The same Seed browses the same way. FF_TYPE_INTEGER, 0–999.',
  Mix: 'The effect against the clip. At 0 the clip is returned exactly.',
  About: 'The plugin’s own About line (FF_TYPE_TEXT, display only).',
};

//===========================================================================
// Kit parameters from the declarations. The id is the name as Arena
// addresses it (lower case, spaces removed).
//===========================================================================

const KIT_TYPES = {
  [FF_TYPE_STANDARD]: 'standard',
  [FF_TYPE_OPTION]: 'option',
  [FF_TYPE_BOOLEAN]: 'boolean',
  [FF_TYPE_EVENT]: 'boolean',
  [FF_TYPE_INTEGER]: 'text',
  [FF_TYPE_TEXT]: 'text',
};

/**
 * The kit reads `embed`, `size`, `clip` and `bg` from the query string itself;
 * a parameter id equal to one would be set by it.
 */
const RESERVED_QUERY = new Set(['embed', 'size', 'clip', 'bg']);
const addressOf = (name) => {
  const address = name.toLowerCase().replace(/\s+/g, '');
  return RESERVED_QUERY.has(address) ? `param-${address}` : address;
};

function kitDefault(d) {
  switch (d.type) {
    case FF_TYPE_OPTION: {
      const i = d.values.findIndex((v) => v === d.default);
      return i >= 0 ? i : Math.round(d.default);
    }
    case FF_TYPE_INTEGER: return String(Math.round(d.default));
    case FF_TYPE_BOOLEAN:
    case FF_TYPE_EVENT: return d.default > 0.5 ? 1 : 0;
    default: return d.default;
  }
}

const PARAMS = DECLARED.list.map((d) => {
  const p = { id: addressOf(d.name), name: d.name, type: KIT_TYPES[d.type], group: d.group, hint: HINTS[d.name], ff: d, default: kitDefault(d) };
  if (p.type === undefined) throw new Error(`no kit control for FFGL type ${d.type} (${d.name})`);
  if (d.type === FF_TYPE_OPTION) p.elements = d.elements;
  if (d.type === FF_TYPE_STANDARD) p.display = readout(d);
  if (ABOUT_LINKS.has(d.index)) p.hint = `Opens ${ABOUT_LINKS.get(d.index)} — the address the plugin’s own button opens.`;
  return p;
});

const query = new URLSearchParams(window.location.search);

//===========================================================================
// The renderer: one plugin instance, driven as a host drives it.
//===========================================================================

/** The value the host would send for a kit control, or null for none. */
function hostValue(p, v) {
  const d = p.ff;
  if (d.index === ABOUT_FIRST) return null;// display only
  switch (d.type) {
    case FF_TYPE_OPTION: return Math.fround(d.values[Math.round(v)] ?? 0);
    case FF_TYPE_BOOLEAN: return v > 0.5 ? 1 : 0;
    case FF_TYPE_EVENT: return null;
    case FF_TYPE_INTEGER: {
      const n = Number.parseInt(String(v).trim(), 10);
      if (!Number.isFinite(n)) return null;
      return Math.min(d.max, Math.max(d.min, n));
    }
    case FF_TYPE_TEXT: return String(v);
    default: return Math.fround(v);
  }
}

function createRenderer(gl) {
  // The clip is held in linear light in an RGBA16F texture the plugin renders
  // into (copy, mip). WebGL2 renders to a float target only with one of these.
  if (!gl.getExtension('EXT_color_buffer_float') && !gl.getExtension('EXT_color_buffer_half_float')) {
    throw new GLError('This browser cannot render to a half-float texture (neither EXT_color_buffer_float nor EXT_color_buffer_half_float), and the plugin holds the clip in linear light in RGBA16F. An 8-bit stand-in would be a plausible wrong picture, so the demo stops here.');
  }

  // emscripten's GL library, pointed at the kit's context. Nothing is turned
  // on behind the kit's back: no extensions are enabled on its behalf.
  const handle = core.GL.registerContext(gl, { majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: false });
  core.GL.makeContextCurrent(handle);

  let instance = 0;
  let pushed = new Map();
  let clipName = 0;
  let clipTexture = null;
  let frames = 0;
  const presses = [];

  /** A kit texture under a GL name the plugin can bind, in emscripten's tables. */
  function glName(texture) {
    if (texture === clipTexture) return clipName;
    if (clipName) core.GL.textures[clipName] = null;
    clipName = core.GL.getNewId(core.GL.textures);
    core.GL.textures[clipName] = texture;
    texture.name = clipName;
    clipTexture = texture;
    return clipName;
  }

  function start(width, height) {
    instance = core._fiche_new();
    pushed = new Map();
    frames = 0;
    if (!core._fiche_init_gl(instance, width, height)) {
      const why = shaderCheck.refused > 0
        ? 'the GLSL the compiled plugin handed to GL is not this page’s checked copy of source/Shaders.cpp, so it was not compiled'
        : 'the plugin’s InitGL failed — its log line under the picture says which pass';
      throw new GLError(`SW Fiche could not start: ${why}.`);
    }
  }

  function push(params) {
    for (const p of PARAMS) {
      const value = hostValue(p, params.get(p.id));
      if (value === null || pushed.get(p.id) === value) continue;
      pushed.set(p.id, value);
      if (typeof value === 'string') withCString(value, (s) => core._fiche_set_text(instance, p.ff.index, s));
      else core._fiche_set_float(instance, p.ff.index, value);
    }
  }

  return {
    render({ input, params, width, height, time }) {
      core.GL.makeContextCurrent(handle);
      if (!instance) start(width, height);
      push(params);
      // An event: the press, then the release, before the frame that honours it.
      while (presses.length) {
        const index = presses.shift();
        core._fiche_set_float(instance, index, 1);
        core._fiche_set_float(instance, index, 0);
      }
      const clip = glName(input.texture);
      if (!core._fiche_process(instance, time, clip, input.width, input.height)) {
        throw new GLError('The plugin’s ProcessOpenGL failed.');
      }
      // For anything checking the page from outside: frames this instance drew.
      frames += 1;
      gl.canvas.dataset.frames = String(frames);
    },
    press(index) {
      presses.push(index);
    },
    /** The newest state the screen pass was given, and the card (harness accessors). */
    state() {
      if (!instance) return null;
      const s = (k) => core._fiche_state(instance, k);
      return { x: s(0), y: s(1), m: s(2), knob: s(3), cardW: s(4), cardH: s(5), written: core._fiche_written(instance) };
    },
    log() {
      try {
        return core.FS.readFile(text(core._fiche_log_path()), { encoding: 'utf8' });
      } catch {
        return '';
      }
    },
  };
}

//===========================================================================
// The page.
//===========================================================================

let renderer = null;

const mounted = mountDemo({
  name: 'Fiche',
  pluginId: 'MF01',
  kind: 'effect',
  tagline:
    'Browsing a microfiche reader, for Resolume: your clip printed in a grid of frames on a card, and an operator browsing it through a reader’s lens — whip pans, overshoot and correction, crash zooms that lose focus, and the hunt for it. None of it is drawn. It is a hand that obeys Fitts’s law on a carriage with some give in it, a zoom that does not hold focus, a card that is not flat and a focus knob turned by someone with a reaction time, magnified twenty-four times.',
  repo: 'https://github.com/stoatworks-labs/fiche',
  page: 'https://stoatworks-labs.com/software/fiche/',

  blurb:
    'It is the plugin’s own C++ — the plugin class, the hand, the reader, the title font and the parameter table, with the FFGL SDK classes it uses — compiled unmodified to WebAssembly, drawing with its own GLSL in WebGL2. The host is this page: it sends no beat, so Sync keeps the plugin’s own 120 BPM; the clock is the browser’s; and the clip is generated in the page.',

  sources: ['scene', 'grid', 'bars', 'spot', 'detail'],

  params: PARAMS,

  differences: [
    'This page runs the plugin rather than a port of it. fiche-core.wasm is source/Fiche.cpp — the plugin class: its constructor and declarations, its clock and unit vote, its beat, the textures, the five passes and every uniform — and everything it calls (Hand, Reader, Font, Controls, Shaders, Diag), with the FFGL SDK’s CFFGLPlugin, parameter bookkeeping, CFFGLPluginInfo, FFGLShader, FFGLScreenQuad and scoped bindings, all compiled unmodified by emscripten (demo/tools/build-wasm.sh). Of source/ only PluginEntry.cpp, the bundle’s build stamp, is left out, and of the SDK only plugMain (FFGL.cpp): the page constructs the plugin itself, as the plugin’s harness does. The panel is read back from the plugin’s own declarations through the SDK’s host getters, and the numbers beside the sliders are Controls.cpp’s conversions, compiled in; their units and every tooltip are this page’s.',
    'The GLSL is the plugin’s: demo/shaders.js is spliced from source/Shaders.cpp, with the order Assemble() joins the pieces in, and the repository’s verify script fails if a character differs. At start-up the page also requires each text the compiled plugin hands to glShaderSource to equal its own assembly of that copy, and refuses to compile anything else. The kit’s port() then changes the version line and adds the ES precision defaults. GL is emscripten’s WebGL2 library on this page’s context, not a GL 4.1 driver, and six entry points are the page’s (demo/wasm/gl_shim.cpp). glShaderSource, for that check and port. glEnable, glDisable and glIsEnabled, because GL_PROGRAM_POINT_SIZE — state the plugin saves and restores each frame, as a plugin in Resolume must — does not exist in WebGL2 (it reads as off here; the plugin draws no points). glTexImage2D and glTexImage3D, twice over. First, the plugin writes the mips of the clip’s texture and of the filmed store by hand, rendering into each level while the one below is the only one it samples; GL 4.1 allows that, but WebGL2 (OpenGL ES 3.0) will not render into a level of a texture allocated level by level outside the range it samples, so every mip came out black and the picture 0.6 times as bright — found by comparing this page with the plugin’s harness. Those two textures are therefore allocated as immutable storage of the same levels, format and size, which ES allows to be rendered into that way; the plugin’s own allocation calls are checked against it, and anything that does not fit stops the page. Second, the card’s bow texture is made R16F instead of the plugin’s R32F, but only in a browser that cannot filter 32-bit float textures (no OES_texture_float_linear); the status line under the picture says which this browser got.',
    'The plugin holds the clip in linear light in a half-float (RGBA16F) texture and renders into it, which WebGL2 allows only with EXT_color_buffer_float or EXT_color_buffer_half_float. Without either the page stops rather than run on an 8-bit stand-in.',
    'No host beat. Resolume calls SetBeatInfo every frame; this page never does, so Beat, 2 Beats and Bar under Sync place the operator’s acts on the plugin’s own 120 BPM clock — what the plugin does in any host that sends no beat. In Resolume they follow the composition’s tempo.',
    'The clock is the browser’s: SetTime is handed seconds summed from the display’s frames (a frame capped at 0.1 s), and the plugin’s own vote settles on seconds within four frames, running on its wall clock (here performance.now()) until it does. The hand moves in real seconds, so a slow machine shows the same browsing in fewer frames. Pause stops the host clock and the plugin sees no time pass: the hand holds still, and because its exposure is Shutter × the frame’s time, a paused frame has no motion smear. A control moved while paused redraws that instant. Step advances 1/60 s. Restart is a backward jump, which the plugin takes as no time passing: the operator carries on from where it was.',
    'Title is FF_TYPE_TEXT in the plugin and a text field here, sent when you press Enter or leave it, as a host sends text. Columns, Rows and Seed are FF_TYPE_INTEGER and number fields here, clamped to the declared ranges. Jump is FF_TYPE_EVENT; the button sends the press and the release, 1 then 0, and the plugin counts the press.',
    'The About block (a text line and four buttons, the user guide’s first) is the plugin’s, and is on the panel because Resolume shows it. Pressing a button sends the press to the plugin, as Resolume would, and the plugin’s handler opens the link by asking the operating system (std::system), which a browser does not have — so there nothing happens. The page opens the same address itself, read from the plugin’s own About table (StoatworksAboutLinks.h, compiled in).',
    'Content Filmed makes the plugin allocate its filmed store: a 2D array of the card’s frames with their mips, up to 256 MB. A browser may refuse that much; the plugin’s own fallback then shows the clip live, and its log line under the picture says so.',
    'The clip is the kit’s generated one, or your own image or video, handed to the plugin unpadded (Width = HardwareWidth); a host may pad it. The output is the page’s 8-bit canvas.',
    'Compared once with the plugin’s own harness, mftest, on an Apple Silicon Mac (M4 Max) when this page was first built, from v0.1.0’s sources, before Layout was added; it has not been repeated since. The page’s frames were gated in headless Chrome (ANGLE on Metal): the first at t = 0, then one Step of 1/60 s each, with the browser’s clock advanced by the same 1/60 s; the clip’s frames were taken from the page at Mix 0 and put through mftest --pipe at the same settings, at 640 × 360. In Manual — 40 frames each reading a frame at 24×, over the header at 2.5× on Diazo Blue with a typed Title, and 0.6 mm out of focus on Colour — no pixel differed by more than one level, and at most 291 of 9.2 million by that. The Auto operator on Seed 7 for 600 frames (ten seconds of pans, corrections, crash zooms and hunts): one pixel of one frame differed by 8 levels (the edge of a title glyph in a crash zoom) and 4,643 of 138 million by one; on Seed 1 for 240 frames, within one level. Content Filmed on a 4 × 3 card at 2.9× for 120 frames, a frame filmed every 0.073 s: within one level. On SwiftShader, and with the bow forced to R16F, the Manual case was within one level too. The comparison can fail: mftest on Seed 2 differs in 16% of pixels, with Position Y 0.03 mm of card away in 34%, and the Auto run one frame out of step in 12%. Nothing repeats that comparison; the repository’s verify script holds the page’s shaders and the .wasm’s sources to the plugin’s.',
    'What has and has not been run in Resolume itself is in the plugin’s README, under Status. Its harness, mftest, renders the same classes headlessly and measures each claim — the blur circle, the bow, the carriage against an independent integration, Fitts’s law, the hunt — and that harness, not this page, is the reason to believe it.',
  ],

  createRenderer: (gl) => {
    renderer = createRenderer(gl);
    return renderer;
  },
});

//===========================================================================
// The controls the kit has no type for, the About block, and the plugin's
// state and log.
//
// By inline style, not the `hidden` attribute: kit.css gives these elements a
// `display` of their own, which beats the attribute's user-agent rule.
//===========================================================================

if (mounted) {
  const params = mounted.params;
  const embed = query.has('embed') && query.get('embed') !== '0';

  if (!embed) {
    const button = (label, title) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'btn';
      b.textContent = label;
      if (title) b.title = title;
      return b;
    };
    const rowOf = (p) => document.getElementById(`p-${p.id}`)?.closest('.prow')
      ?? [...document.querySelectorAll('.prow')].find((row) => row.querySelector('.prow__name')?.textContent === p.name);

    for (const p of PARAMS) {
      const row = rowOf(p);
      if (!row) continue;

      // FF_TYPE_EVENT: a press and a release. An About button also opens the
      // address the plugin's own press would have opened.
      if (p.ff.type === FF_TYPE_EVENT) {
        const toggle = row.querySelector('.prow__toggle');
        const url = ABOUT_LINKS.get(p.ff.index);
        let press;
        if (url) {
          press = document.createElement('a');
          press.href = url;
          press.target = '_blank';
          press.rel = 'noopener';
          press.textContent = 'Open';
          press.title = p.hint;
        } else {
          press = button(p.name, p.hint);
        }
        press.className = 'prow__toggle';
        press.style.textAlign = 'center';
        press.style.textDecoration = 'none';
        press.addEventListener('click', () => {
          renderer?.press(p.ff.index);
          mounted.redraw();
        });
        toggle?.replaceWith(press);
        continue;
      }

      const field = row.querySelector('.prow__text');
      if (!field) continue;

      // The About line: the plugin's, display only.
      if (p.ff.index === ABOUT_FIRST) {
        field.readOnly = true;
        continue;
      }

      // FF_TYPE_INTEGER: a number field in the declared range.
      if (p.ff.type === FF_TYPE_INTEGER) {
        field.type = 'number';
        field.min = String(p.ff.min);
        field.max = String(p.ff.max);
        field.step = '1';
        field.inputMode = 'numeric';
        continue;
      }

      // FF_TYPE_TEXT: sent when committed, as a host sends text.
      if (p.ff.type === FF_TYPE_TEXT) {
        field.maxLength = 40;
        field.addEventListener('input', (event) => event.stopImmediatePropagation(), true);
        field.addEventListener('change', () => params.set(p.id, field.value));
      }
    }

    //-----------------------------------------------------------------------
    // What the reader is doing (the newest state the screen pass was given,
    // through the harness's accessor) and the plugin's own log (Diag.cpp).
    // Both report; neither measures.
    //-----------------------------------------------------------------------
    const stage = document.querySelector('.stage');
    if (stage) {
      const status = document.createElement('p');
      status.className = 'stage__status';
      status.id = 'fiche-state';
      status.setAttribute('aria-live', 'off');
      const log = document.createElement('p');
      log.className = 'stage__status';
      log.id = 'fiche-log';
      log.setAttribute('aria-live', 'off');
      log.style.fontFamily = 'ui-monospace, SFMono-Regular, Menlo, monospace';
      log.style.fontSize = '12px';
      log.style.whiteSpace = 'pre-wrap';
      stage.append(status, log);
      let lastLog = '';
      setInterval(() => {
        const s = renderer?.state();
        if (s && Number.isFinite(s.x)) {
          const filmed = params.option('content') === 1 ? ` · ${s.written} frame${s.written === 1 ? '' : 's'} filmed` : '';
          status.textContent = `The carriage at ${fixed(s.x, 1)}, ${fixed(s.y, 1)} mm on a ${fixed(s.cardW, 0)} × ${fixed(s.cardH, 1)} mm card · ${fixed(s.m, 1)}× · focus knob ${signed(s.knob, 3)} mm${filmed} · the bow texture is ${core.ficheBowFormat ?? '…'} in this browser`;
        }
        const all = renderer?.log() ?? '';
        if (all === lastLog) return;
        lastLog = all;
        const lines = all.trim().split('\n').filter(Boolean).map((l) => l.replace(/^\S+ (INFO |WARN |ERROR) fiche: /, '$1 '));
        log.textContent = lines.length
          ? `The plugin’s own log (Diag.cpp, in this page’s memory):\n${lines.slice(-2).join('\n')}`
          : '';
      }, 250);
    }
  }
}
