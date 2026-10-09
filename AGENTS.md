# AGENTS.md — fiche

Read this before touching the hand (`Hand.{h,cpp}`), the reader's geometry
(`Reader.{h,cpp}`), the screen pass (`kScreenFragment`), the mips or the parameter
table (`Controls.cpp`). `CLAUDE.md` is the command reference; this is the why.

## The one idea

**A microfiche reader magnifies a hand.** The screen shows about a centimetre of
film at 24×, so every movement of the hand on the carriage, every flick of the zoom
and every turn of the focus knob arrives on the screen multiplied by twenty-four. The
look of somebody browsing a fiche — the whip pans, the overshoot and the correction,
the crash zoom that loses focus, the hunt for it — is what a person's hand does,
seen through that magnifier. So the plugin is a model of the hand and a model of the
reader, and the picture is the light through both.

Neighbours: `gaffer` (a simulated lens on a depth map, with a focus puller on marks:
no carriage, no magnification, no search), `tilter` (a plane of focus), `vertigo`
(the dolly zoom), `gate` (a film projector's gate). None of them moves a picture
under a magnifier by hand, and none of them hunts.

## How a frame goes

**The CPU** advances the hand by the host's dt in 1 ms substeps (`Hand::Advance`)
and keeps a quarter-second of its states. The screen pass is given 17 of them,
evenly spaced across the last `Shutter` × dt: the exposure. Each state is the
carriage position (the card point at the screen's centre, mm), the magnification
and the focus knob (mm).

**The GPU**, in `Shaders.h`'s order:

1. **copy** — the clip into the live texture (RGBA16F) in **linear light**, so every
   average after it adds light, as a lens and an exposure do.
2. **mip** — the live texture's mips, by hand: each texel the **area average** of its
   share of the level below (see the traps).
3. **film** / **mip layer** — Content Filmed only: the step-and-repeat camera's
   exposure of the clip into one layer of a 2D array (RGBA8, sRGB-coded, a 256 MB
   budget sets the layer size), and that layer's mips, averaged in linear light.
4. **screen** — per pixel, the reader integrated. The pixel's film point travels a
   path across the exposure and is out of focus by some amount at each state; from
   those the pass picks a power-of-two number of taps n (1 at rest and in focus, up
   to 32) and a prefilter footprint (the taps' spacing). Tap j is at moment
   `( j + 0.5 ) / n` of the exposure and at point `rev( j )` of a Vogel disc on the
   aperture — a **Hammersley set** — so time and aperture are each covered evenly
   and do not move together. Each tap goes through the card: the nearest frame (box
   coverage of its rectangle), the clip by `textureLod` at the footprint, the film's
   print, the scratches and dust in the emulsion, and off the card only glass. The
   taps are averaged in linear light, lit by the lamp, fallen off as cos⁴, grained
   by the screen's diffuser, washed by the room, encoded, mixed; alpha
   `mix( src.a, 1, Mix )` (a screen is opaque).

### The reader (`Reader.h`, the GLSL's `filmPoint`, `defocusAt`, `filmBlur`)

- The card is 148 mm wide (A6); `Columns` × `Rows` frames of the clip's aspect,
  `Gutter` mm apart, 4 mm margins, a 10 mm header with the title. Its height follows
  the grid.
- The screen is 300 mm across; the output raster is mapped onto it.
- Film point under screen point s: `f = P + ( s.x, −s.y ) / M`.
- Defocus `δ = knob − bow( f ) − Parfocal · log2( M / 24 )`. Blur circle on the film
  `r = |δ| M / ( 2 N ( M + 1 ) )`, on the screen M r.
- Depth of focus `δa = 0.4 N ( M + 1 ) / M²`: the defocus at which the screen's blur
  circle is 0.4 mm across, about what an eye resolves at reading distance.
- The bow: heights in ±Flatness on a 25 mm lattice, a uniform cubic B-spline,
  seeded; the GPU reads it bilinearly off a 1 mm grid the CPU writes.
- Falloff `( L² / ( L² + r² ) )²` (cos⁴ of the ray's angle) for a throw L.
- The lamp: a black body through Wyman, Sloan and Shirley's fit of the CIE 1931
  observer, white-balanced so 6504 K is exactly white.
- The film: a unit-gamma print, `T = dense + ( base − dense ) f`, f the exposure
  (1 − exposure on a negative), Rec.709 luminance on a mono stock. Unexposed film —
  gutters, margins, unwritten frames — is dark on a positive card and clear on a
  negative one.

### The hand (`Hand.h`)

Auto is a loop of acts: **focus, dwell, act, travel, settle**.

- **Views.** A frame's centre, or — when a frame is wider or taller than the screen
  at the reading magnification — a tiling of it in screen-sized views, in reading
  order. `Browse` picks the next: Reading (the next view), Skimming (1 + a geometric
  number), Searching (any other view alike), Mixed (15 / 40 / 45%).
- **A pan** is a minimum-jerk submovement, `x(τ) = A + ( B′ − A )( 10τ³ − 15τ⁴ + 6τ⁵ )`,
  lasting `0.08 + b log2( D / W + 1 )` (Fitts), with the target W a tenth of the
  screen's width *on the card* (`30 mm / M`), landing 4% short on average with a
  scatter of k D along the movement and 0.4 k D across it. A miss by more than W/2 is
  corrected after a 0.15 s reaction gap, under the same law, up to four times.
- **A crash zoom.** A move longer than 1.5 frames zooms out first (with probability
  `Crash Zoom`) to `M_out = clamp( 300 / ( D + 3w ), 2, M / 2 )`, pans there — where W
  is larger, so the pan is quicker and rougher — and zooms back in, landing with
  log-noise 0.5 k |Δ log2 M|. A zoom is minimum jerk in log2 M over
  `0.1 + 0.07 |Δ log2 M|` s, scaled with b.
- **The hunt**, after every arrival with `|δ0| > δa`: the knob turns at
  `v0 = |δ0| / 0.35 s + 1.5 mm/s` — towards focus with probability `0.5 + 0.45 Skill`,
  else away until the blur has grown by a quarter and a reaction time more. On a pass
  at speed v, if `v τ ≤ 2 δa` the hand stops a reaction time after the picture first
  looks sharp (inside the band); otherwise it overshoots to `δa + v τ` past focus and
  turns back at `g v`. τ = `0.22 − 0.1 Skill` s, g = `0.5 − 0.25 Skill`. Each pass is
  minimum jerk at average speed v.
- **Dwell**: Gamma(2) with mean `Dwell`; with `Sync` on, until the first beat (of 1, 2
  or 4) at least 0.15 s on, placed **inside the frame** by the bar phase
  interpolated between frames. `Jump` ends a dwell at once.
- **The carriage**: `x″ = ω²( hand − x ) − 2ζω x′`, integrated exactly over each 1 ms
  substep with the hand linear in it (the particular solution is a constant; the
  rest is the damped oscillator's closed form), with **end stops** at the card's
  edges that take the speed out. `Carriage Play` 0 is rigid; above it 40 Hz → 3 Hz
  and ζ 0.5 → 0.2.
- **Manual**: the hand is `Position X/Y` (linear between host frames, through the
  carriage), the lens `Zoom`, and `Focus` is the defocus at the screen's centre —
  measured from best focus, so Parfocal does not act in Manual.

## Decisions taken without asking

- **The name** is fiche (`SW Fiche`, `MF01`, free across the fleet and `~/dev` on
  2026-10-08): the card, not the reader, because the card is what is browsed.
- **The frames are the clip's aspect**, not a microfiche format's portrait pages: a
  VJ's clip letterboxed in a portrait frame would waste the screen. 14 × 7 by
  default, after the 98-frame COM layout.
- **The lens runs 2× to 75×.** A real reader's zoom covers about 2:1; this one
  reaches the whole card so a crash zoom can, and says so in the README.
- **The operator browses by default** (Auto, Mixed, a 0.5 s dwell): the request was
  for the browsing, and Manual is one option away.
- **Live is the default content.** Filmed is physical — a step-and-repeat camera
  writing one frame per Interval, the rest unexposed until it gets there — but a
  fresh instance in Filmed shows one frame of picture on a blank card, which reads
  as broken until the camera has gone round.
- **Defaults tuned for the look, within the physics**: f/2.8, a 0.3 mm bow (plates
  that no longer close), a non-parfocal zoom of 0.5 mm per doubling, a knob that
  starts at 1.5 mm/s, a 4500 K lamp. The first tuning (f/4, 0.15 mm, 0.3 mm/s,
  3200 K) hunted by 5 px and looked sepia: Allan's "try again".
- **The title** is a 5 × 7 dot font, drawn by exact box filtering of square font
  pixels up to a pixel's footprint and by the atlas's box mips beyond. 40 characters,
  upper case, digits and a little punctuation; anything else is a space.
- **Off the card is glass**: the lamp straight through. A reader zoomed out shows the
  card as a dark (positive) or clear (negative) rectangle on a bright screen.
- **Dust is opaque and scratches are clear** (emulsion scratches), both in the
  emulsion, so both blur and smear with the picture. Dust goes up to 150 particles
  per cm²; the first range (40) was too faint at 320 px for the sweep to see.
- **The carriage has end stops.** Without them a pan aimed at a card edge sprang
  0.5 mm past it on the grip — found by `--operator-law`.
- **Room light is additive grey**; the screen's grain is multiplicative value noise
  on a quarter-millimetre lattice of the screen, faded where a pixel is coarser.
- **No audio input** for v0.1.0: Resolume can drive any control, and Sync takes the
  host's beat.
- **StoatworksAbout.h and ATTRIBUTIONS.md are generated** by stoatworks-backend's
  sync-about.py and sync-attributions.py (registered 2026-10-09). The About block
  has four buttons, the user guide's among them, so it is 38 parameters long, and
  `tools/sweep.py` skips its buttons.

## The traps

Ordered by how much time they cost.

**A box coverage written as min − max of card positions cancels in float32.** The
card's inside, a frame's rectangle and a scratch's length were first covered as
`min( p + fp/2, hi ) − max( p − fp/2, lo )`. At 75× a pixel is 0.005 mm of a card
148 mm across, and that subtraction lost a tenth of a percent — enough to let the
glass round the card glow at 0.0009 through the whole picture, found as a defocused
point "carrying 1.9× its light". Written as `min( fp/2, hi − p ) + min( fp/2, p − lo )`
it is exact in the interior. `--dark` holds a black clip at exactly 0, and its
negative reinstates the old formula.

**Box mips of odd sizes drop a row.** 720 → 360 → 180 → 90 → 45 → 22: the plain 2 × 2
box leaves level 5 describing 44 of 45 rows while `[ 0, 1 ]` says all of them, a 2%
shift that grew with the level — found by `--track` at 320 px reading positions 0.06
mm off when zoomed out. Every level is now the area average of its share of the
level below (two and a bit texels where the size is odd); `--mips` checks it and its
negative is the plain box.

**Apple's GPU filters a half-float texture in half precision.** A bilinear reading
came back as an exact half, 1.04 ULP off. The track bound counts a half-float ULP per
store (1 + ⌈LOD⌉ of them) and one for the filter.

**A code ramp is a bad ruler for a light-averaging picture.** The first coordinate
clip was linear in sRGB code; the reader averages light (mips, bilinear, taps), and
an average of a code ramp is bent by the transfer. The clip is linear in light.

**`texelFetch`'s level is relative to `TEXTURE_BASE_LEVEL`** (GL 4.1 §2.11.8), and
Apple honours it: the mips set BASE = MAX = the level below while they write the next,
so no level being written is one being read. `--mips` reads every level back.

**A sub-pixel dot sampled at one offset is not its own light.** The defocus check's
first energy reference was the sharp picture's sum, a few percent off a texel's true
area; the disc is now held to the texel's area in px².

**A check that sits on its bound is not a pass.** `--track` at 320 px landed at
0.0221 mm against a 0.0221 bound; working out why found the filter's rounding above.

**The harness's own measurements, each wrong once:** the 50% crossing between pixel
centres is not where a box-filtered edge is (read it by area); an RK4 reference
compared 5 µs off the frame times was 8e-4 mm "out" at the carriage's speed (step onto
them); a line-spread window that reached the glass beyond the card summed a step to
zero; visit counts cannot tell a biased search from a fair one (any random walk on a
cycle visits every view alike — test the jump offsets); a chi-square point for 11
degrees of freedom is not one for 107 (Wilson–Hilferty, computed); the hand's log
keeps a copy of a synced dwell taken before a beat ends it (the log is now told).

**`Hand::Now()` before the first frame** read an empty ring and crashed the harness.

**A smear of a pixel font is banded, correctly.** A 111 px box blur of glyphs made
of 20 px font pixels steps at every font-pixel column; it looked like a tap artefact
and is not (enlarged, the smear is smooth). A wider prefilter was tried and reverted.

**Sweep contexts.** Shutter acts only on a moving carriage (swept with a slow hand
that is nearly always mid-pan), Rows only shows on the whole card, Dust only at high
magnification on a small raster. `tools/sweep.py --bare` (2026-10-08) lists what is
dead without its context: Position X, Position Y and Focus (Manual), Rows (the whole
card), Interval (Filmed), Title (the header in view), and Parfocal barely alive (it
acts only through the operator's zooms; Manual's Focus is measured from best focus).

## Would this hold on another rasteriser, at another raster?

Every check runs at 1280×720 and 320×180 on this Mac's GPU, and at 320×180 on
Apple's software renderer (`tools/verify.sh`).

- `--identity`: one half-float ULP of linear light, which the sRGB slope shrinks;
  holds whether the GPU rounds or truncates. 8-bit noise card: byte-exact.
- `--mips`: one half-float ULP (a level is rounded once on its store).
- `--dark`: exactly 0: nothing is there to light, so no rasteriser may invent it.
- `--magnify`: 0.01 px; float32 card millimetres at M ≤ 11 put an ULP under 1e-3 px.
- `--defocus`: 1% of R from the Vogel identity `Σ ( a + 0.5 ) / n = n / 2` and the
  sampled dot's phase (< 0.25 px², under 1% of R²/2 for R ≥ 6 px; smaller discs are
  skipped and said). Light to 0.5%.
- `--field`: 0.005 mm at 1280, 0.03 at 320, from the dot moment's phase noise over
  the blur's slope; the 1 mm bow grid adds < 0.002 mm.
- `--track`, `--carriage`: per frame, a half-float ULP per store and one for the
  filter, times the frame size.
- `--shutter`: the centroid exactly (0.01 px); the width on L², from the taps'
  stratification (n ≥ 4) and a sampled edge's phase (≤ ¼ px²).
- `--hunt`: the plan to 1e-12; the picture to 2% (the defocus check's 1% plus the
  bow grid); the sharper-frame order only at 1280, where a pass through focus is
  blurred 3 px or more either side — at 320 none is, and the check says so.
- `--stock`: two half-float ULPs of linear light.
- `--screen`: 2e-4 for the falloff (float32 arithmetic); the grain to 1e-6; the dust
  to 5e-3 (a float control's position against a footprint-wide edge).
- `--filmed`: one 8-bit level (the store is RGBA8 of sRGB code).
- `--sync`: 1e-6 s (a float bar phase is good to ~6e-8 of a bar).
- `--resize`: byte equality in one context.
- `--fitts`, `--operator-law`: no GL; 4 σ of each estimator.

## A check that cannot fail is not a check

`mftest --negative` runs 23 wrong models and requires each check to fail: the lens
2% strong (`--identity`, `--magnify`), plain-box mips (`--mips`), the cancelling
coverage (`--dark`), the blur circle without its ( M + 1 ) and a cone of taps
instead of a disc (`--defocus`), the bow's sign (`--field`), the picture shown the
hand not the carriage (`--track`), forward Euler for the grip (`--carriage`), a
shutter twice as long (`--shutter`), a hunt with no reaction time (`--hunt`), a
negative printed positive (`--stock`), cos³, grain on the card and dust on the
screen (`--screen`), the camera filling columns first (`--filmed`), a beat at its
frame's start (`--sync`), the store's exposures surviving a reallocation
(`--resize`), Fitts without Shannon's + 1, a cubic ease and distance-blind scatter
(`--fitts`), a biased search (`--operator-law`), and every cue ramping (`--cues`).
23 of 23 caught (2026-10-08).

`tools/mutate.sh` changes one character of the shipped code and requires the named
check to fail, on a copy of the tree built from scratch:

| mutant | caught by |
| --- | --- |
| GLSL `M + 1.0` → `M - 1.0` in the blur circle | `--defocus` |
| GLSL `st.w - bowAt` → `st.w + bowAt` | `--field` |
| GLSL `q * q` → `q + q` in the falloff | `--screen` |
| GLSL a mip texel's area `scale.x * scale.y` → `+` | `--mips` |
| GLSL `f = 1.0 - f` → `1.0 + f` on a negative | `--stock` |
| GLSL the filmed frame's index `Grid.x` → `Grid.y` | `--filmed` |
| C++ minimum jerk's 15 → 16 | `--fitts` |
| C++ the grip's particular solution `-2.0 * zeta` → `-3.0` | `--carriage` |
| C++ Shannon's `+ 1.0` → `+ 2.0` | `--fitts` |
| C++ the blur radius `2.0 * fNumber` → `3.0` (in `Reader.cpp`, which the harness also uses) | `--defocus` |
| C++ the camera's clock `-=` → `+=` | `--filmed` |

11 of 11 caught (2026-10-08). The GLSL mutants prove the harness drives the shaders
the plugin ships, not a copy.

## Shape of the code

    source/
      Fiche.{h,cpp}       the FFGL plugin: clock, beat, textures, the passes
      Hand.{h,cpp}        the operator: views, acts, Fitts, the hunt, the grip, the history
      Reader.{h,cpp}      the card, the lens, the bow, the lamp, the film stocks
      Shaders.{h,cpp}     every pass, assembled kVersion + kCommon + pass
      Controls.{h,cpp}    the parameter table (name, group, kind, default) and units
      Font.{h,cpp}        the title's 5 x 7 dot font and its box-mipped atlas
      Hash.h              PCG, the same integer arithmetic as the GLSL
      GLState.h           the host's GL state, captured and put back
      Diag.{h,cpp}        the log file, nothing else
      PluginEntry.cpp     the bundle's own TU and build stamp
      StoatworksAbout*.h  the About block (generated by sync-about.py)
    tools/
      mftest/main.cpp     the harness: render, --pipe/--film/--script/--trace, every check
      verify.sh           everything; mutate.sh, sweep.py, glslc.sh
    demo/                 the browser demo: this C++ as WebAssembly (glue.cpp, gl_shim.cpp), vendor/ (the kit)

## What is genuinely verified, and what is assumed

**Measured** (numbers in README Status): a perfect reader showing one frame is the
clip; the mips are area averages; black is black at every magnification; edges land
at M × card mm about the screen's centre; a point out of focus is a uniform disc of
geometric optics' radius and keeps its light; the card's bow, read out of the
picture, is the model's; the picture is where the hand puts the carriage, through
pans, corrections and crash zooms; the grip is the second-order step response to
2e-11 mm against RK4 with the textbook overshoot; a smear is centred on the
mid-exposure position and as long as the exposure's travel; the hunt turns where the
reaction time puts it and the picture's blur follows the knob frame by frame; every
film's print; cos⁴; the grain is the screen's and the dust the card's; the camera's
frames in reading order, one per Interval; acts on the beat; pans obey Fitts with a
minimum-jerk profile, land with distance-proportional scatter and are corrected
exactly when they miss; the operator stays on the card, reads in order and searches
uniformly; a resize is a fresh card; the host's GL state comes back; every control
moves the picture.

**Assumed, not measured**: that a person at a reader moves like this (the laws are
the motor-control literature's — Fitts 1954, Flash and Hogan 1985, Harris and
Wolpert 1998, Elliott et al. 2001 — but the constants are typical, not one person's,
and the hunt's rule is a model of a person, not a measurement of one); the film
stocks' densities and dye colours (typical, not a datasheet's); the 0.4 mm acceptable
blur; the lens's 2–75× range (a stretch). **Never loaded into Resolume on macOS.**
On Windows the fleet's Arena gate passed 9 of 9 on Arena 7.27.1 (llvmpipe, 2026-10-09;
expectation `plugin-bench/arena/expect/fiche.json`, drafted by `mftest --expect`): 26
of 32 controls moved its picture, 31 under a precondition, none dead, and the six that
steer the operator over seconds (Hand Speed, Accuracy, Crash Zoom, Focus Skill,
Carriage Play, Parfocal) were inconclusive on single grabs of a moving reader, as the
expectation's notes say. No OpenFX port and no browser demo.

## The browser demo (2026-10-09)

`demo/` is <https://fiche-demo.stoatworks-labs.com>, built to the fleet's
`resolume-demo` kit rules by a sub-agent of the release session, polyhedral's way: it
**runs the plugin rather than a port of it**.

- **`fiche-core.wasm` is the plugin's C++, unmodified**: Fiche.cpp, Hand, Reader, Font,
  Controls, Shaders and Diag, with the SDK's CFFGLPluginManager / CFFGLPlugin,
  CFFGLPluginInfo, FFGLLog, FFGLShader, FFGLScreenQuad and scoped bindings. Left out:
  PluginEntry.cpp (the build stamp) and FFGL.cpp (plugMain). `-D__linux__` goes to the
  files that include the SDK only (FFGLPlatform.h). Committed, with
  `demo/wasm/inputs.sha256` pinning every input; rebuild with `demo/tools/build-wasm.sh`.
- **The host** (`demo/wasm/glue.cpp`) constructs the plugin, reads its 38 declarations
  back through the SDK's host getters (the panel, About block included, is built from
  them) and forwards SetFloatParameter, SetTextParameter, SetTime and ProcessOpenGL as
  mftest's Rig does; never SetBeatInfo. `FICHE_LOG_DIR` must be set before the FIRST
  constructor: Diag opens its log there.
- **The shim** (`demo/wasm/gl_shim.cpp`), six entry points. glShaderSource: the text must
  equal one of the page's assemblies of `shaders.js`, then the kit's `port()`.
  glEnable / glDisable / glIsEnabled: GL_PROGRAM_POINT_SIZE (GLState.h) is not in
  WebGL2. glTexImage2D / 3D: (a) a level-0 allocation with no data becomes glTexStorage
  of the full chain, and the plugin's own later level allocations are checked against
  it and skipped; (b) the bow's R32F becomes R16F, only without
  OES_texture_float_linear (the status line says which the browser got).
- **(a) is a portability fact about the plugin, not a bug in it.** The mip passes render
  into level L of the live texture and of the store while BASE = MAX = L - 1. GL 4.1
  allows that for any texture; ES 3.0 (4.4.4.1), so WebGL2, makes a level of a MUTABLE
  texture (glTexImage per level) framebuffer-complete only inside [BASE, MAX]. Every mip
  draw failed with INVALID_FRAMEBUFFER_OPERATION, the levels stayed black, and at 24x
  (LOD about 0.4) the page was 0.6 times as bright as mftest. Immutable storage is
  exempt. Allocating with glTexStorage (GL 4.2, or ARB_texture_storage) would make the
  plugin itself portable to ES; it was not changed.
- **Checked**: `demo/tools/check_shaders.py` (verify.sh) holds the 7 pieces, kVersion and
  Assemble()'s order to the C++, and every input of the .wasm to its manifest. 10 of 10
  negative controls caught: M + 1 to M - 1 in shaders.js; the bow's sign in Shaders.cpp
  only; an extra export; ASSEMBLY.screen without COMMON; a new piece; a character of
  Hand.cpp; a default in Controls.cpp; a new header; a character of glue.cpp; a byte of
  the .wasm. At run time a shaders.js with one space added to a comment was refused.
- **Compared once with mftest** (M4 Max; headless Chrome on ANGLE/Metal; 640 x 360; the
  page's frames gated, the first at t = 0 then one Step of 1/60 s each, with
  performance.now() advanced by the same 1/60 s so the plugin's pre-vote wall clock
  agreed; the clip frames from the page at Mix 0 through `mftest --pipe`). Manual at 24x,
  40 frames: worst 1 level, 291 of 9.2 M pixels differing. Over the header at 2.5x on
  Diazo Blue with a typed Title: worst 1, 131. 0.6 mm out of focus on Colour: worst 1,
  106. Auto, Seed 1, 240 frames: worst 1, 829 of 55 M. Auto, Seed 7, 600 frames of pans,
  corrections, crash zooms and hunts: worst 8 in ONE pixel of one frame (a title glyph's
  edge mid crash-zoom), 4,643 of 138 M by one. Filmed, a 4 x 3 card at 2.9x, Interval
  0.073 s, 120 frames: worst 1, 562 of 28 M. On SwiftShader, and with the bow forced to
  R16F, Manual stayed within 1. The comparer fails: Seed 2 differs in 16% of pixels,
  Position Y 0.03 mm away in 34%, the Auto run one frame out of step in 12%; and before
  (a) the Manual case was worst 46, 83% of pixels.
- **Seen, not a fault**: at an Interval of exactly three frames (0.05 s) Filmed differed on
  18 of 120 frames, by up to 179 levels, each page frame k matching mftest's k + 1 or
  k - 1. The camera's `filmClock >= interval` sits on the boundary and the kit's clock is
  a running sum of 1/60 where mftest's is k / 60, so the last bit picks the frame. Any
  host's clock does the same.
- **Gaps, all said on the page**: no host beat (Sync keeps the plugin's own 120 BPM); the
  browser's clock (Pause is no time passing, so no motion smear; Restart is a backward
  jump); Title is sent on commit and the integers are number fields; the About buttons'
  `std::system()` does nothing in a browser, so the page also opens the plugin's own
  URLs; the filmed store (up to 256 MB) may be refused; the clip is unpadded; the shim.
- **Console**: nothing locally, on Metal or SwiftShader. The live page logs one error that
  is not the page's: the zone's injected `/cdn-cgi/challenge-platform` inline script,
  refused by `script-src`, as on every `*-demo` host.

## Open design questions

- **A beat-locked crash zoom**: Sync starts acts on the beat; a VJ may want the zoom
  itself on the beat and the pan off it.
- **Frames per beat for Filmed**: Interval in beats rather than seconds.
- **A second operator style**: a reader who flicks the carriage and lets it coast
  (Coulomb friction) instead of carrying it.
- **Silver grain** at high magnification; diazo's characteristic speckle.
- **Portrait pages** for an archive look: a Format option with real microfiche
  layouts and the clip fitted into portrait frames.
- **Audio**: a kick could be a Jump.
