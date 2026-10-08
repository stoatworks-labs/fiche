# fiche

Browsing a microfiche reader, for Resolume Arena/Avenue, as one FFGL effect:
`SW Fiche` (`MF01`). The clip is printed in a grid of frames on a card, and an
operator browses it through a reader's lens: a hand that obeys Fitts's law on a
carriage with some give in it, a zoom that does not hold focus, a card that is not
flat, and a focus knob turned by someone with a reaction time. C++/GLSL, CMake
MODULE → a universal `.bundle` (macOS) + Windows `.dll`. MIT. Bundle id
`com.stoatworks.ffgl.fiche`.

Read `AGENTS.md` before touching the hand (`Hand.{h,cpp}`), the reader's geometry
(`Reader.{h,cpp}`), the screen pass (`kScreenFragment`), the mips or the parameter
table (`Controls.cpp`).

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (into `~/Documents/Resolume Arena/Extra Effects`)
- Render the harness's card: `./build/mftest --out /tmp/fiche.png --size 1920x1080 --frames 120`
- A frame of a real clip: `ffmpeg -ss 2 -i clip.mov -frames:v 1 -s 1920x1080 -f rawvideo -pix_fmt rgba /tmp/c.rgba && ./build/mftest --clip /tmp/c.rgba --size 1920x1080 --out /tmp/f.png`
- List parameters: `./build/mftest --list`
- Set anything by name: `./build/mftest --set "Film=Diazo Blue" --set "Title=THE TIMES 1987"`
  (an option by its element's name or index, the Title as text; anything else that
  is not wholly a number is refused, exit 2)
- The hand, frame by frame: `./build/mftest --trace --frames 600` (every segment
  it plans, and the carriage, magnification and knob each frame)
- A clip through the effect: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba -s WxH - | ./build/mftest --pipe --size WxH --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s WxH -r 60 -i - out.mp4`
  (Resolume's demo clips are 30 fps; the harness clocks every frame at 1/60 s.)
- The card as video: `./build/mftest --film 600 --moving --script cues.txt | ffmpeg …`
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the units
  of `--set`. Held before the first key and after the last; a standard control is
  LINEAR between keys, and an option or integer STEPS. An unknown name or a value
  that is not a number exits 2 before any frame; a partial frame at EOF ends the
  stream with exit 0; a reader that hangs up ends `--pipe`/`--film` with exit 1
  (SIGPIPE is ignored).
- The Arena gate's expectation, drafted from the declaration: `./build/mftest --expect`

## Verify
- Everything: `tools/verify.sh` (reserved words, no trig in the GLSL, nothing
  WebGL2 lacks, glslc, a fresh universal build, lipo/plist/codesign/oxbow
  probe+selftest, every check at 1280x720 and 320x180, the same on Apple's software
  renderer at 320x180, the offline set, the `--pipe` contract, the negative controls,
  the mutants, the sweep, the bench)
- **The picture**: `--identity`, `--mips`, `--dark`, `--magnify`, `--stock`, `--screen`.
- **The lens**: `--defocus` (a point's disc and its light), `--field` (the bow, read
  out of the picture), `--shutter` (a smear's centre and length).
- **The hand**: `--track` (the picture is where the carriage is), `--carriage` (the
  grip against RK4), `--hunt` (the plan's reversals and the picture's blur), `--sync`;
  no GL: `--fitts`, `--operator-law`.
- **The camera and the machinery**: `--filmed`, `--resize`, `--state`; no GL:
  `--cues`, `--names`.
- One raster only: add `--size WxH`. The software renderer:
  `MFTEST_RENDERER=software ./build/mftest --defocus --size 320x180`.
- **The checks can fail**: `--negative` (23 wrong models), `tools/mutate.sh`
  (11 one-character mutants of the GLSL and the C++).
- What CI runs: `--offline` and `tools/glslc.sh`.
- No dead controls: `python3 tools/sweep.py` (33 parameters); `--bare` lists the
  ones that only act with another control set.
- Cost: `--bench` (720p/1080p/4K, GPU time by GL_TIME_ELAPSED).
- `MFTEST_DEBUG=1` makes `--magnify`, `--track` and `--fitts` say where they looked.

## Notes
- **Millimetres throughout.** The card is 148 mm wide, the screen 300 mm; the
  carriage P is the card point at the screen's centre; `f = P + ( s.x, −s.y ) / M`.
  Card y runs DOWN, screen y UP; GL textures are bottom-up, so a frame's `v` (down)
  is `1 − t`.
- **The clip is held in linear light.** The copy pass converts; the store keeps
  sRGB code in RGBA8 and its mips average in linear. A test clip meant to be read
  back must be linear in LIGHT, not in code (see AGENTS.md).
- **Mips by hand, by area.** Never glGenerateMipmap (its filter is the driver's)
  and never a plain 2 x 2 box (it drops odd rows). BASE = MAX = the level below
  while writing the next.
- **Box coverage from the distances to each edge**, `min( fp/2, hi − p ) +
  min( fp/2, p − lo )`, never `min − max` of positions: `--dark`.
- **Nothing absolute crosses into GLSL**: the CPU reduces the host's clock to dt;
  the shader gets 17 states of the last exposure.
- **No sin/cos/atan in the GLSL** (verify.sh greps): the aperture's directions come
  from the CPU as a uniform array.
- **The taps are a Hammersley set**, n a power of two (bit reversal needs one).
  The defocus check depends on `Σ ( a + 0.5 ) / n = n / 2`; keep the radius
  `sqrt( ( a + 0.5 ) / n )`.
- **The hand is deterministic per seed**: every draw is a PCG hash of ( seed, draw
  count ), taken in a fixed order whether or not it is used (planDwell draws twice
  even when synced).
- Every host parameter is 0..1 except the integers (Columns, Rows, Seed), the Title
  (text) and Jump (an event, which counts a press, never a release). An option
  reads back 0..1 whatever its count: map by element index.
- Negative-control hooks: `shaders::Hook` (the `Hooks` uniform, 0 shipped),
  `hand::HandHook`, and the plugin's `SetShowHandForTest`,
  `SetShutterScaleForTest`, `SetResizeKeepsStoreForTest`,
  `SetCueAtFrameStartForTest`. `SetPrefilterForTest( false )` is not a wrong model:
  it lets the defocus and shutter checks measure the taps' geometry bare.
- Reserved words (`packed`, `smooth`, `half`, `noise1..4` …) must not be
  identifiers. No `M_PI`, no `far`/`near`.
- `fiche_core` is an OBJECT library: the registration is a file-scope constructor
  nothing references.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are PROVISIONAL hand copies until the
  repo is registered; then sync-about.py and sync-attributions.py own them.

## Not done yet
- Never loaded into Resolume, on either platform; Windows never built.
- No OpenFX port, no browser demo, no user guide.
- Not registered, not public: a local v0.1.0 in `~/dev/fiche`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/fiche/fiche.YYYY-MM-DD.log
    %LOCALAPPDATA%\fiche\logs\fiche.YYYY-MM-DD.log
