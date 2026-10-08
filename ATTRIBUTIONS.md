# Attributions

Fiche is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The effect's shape, the clock, the harness and the tooling — Stoatworks patchwork

<https://github.com/stoatworks-labs/patchwork>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin class's shape, the host-clock unit vote (readout's, by way of pitch), source/GLState.h, source/Hash.h, source/Diag.{h,cpp}, the harness's rig, PNG writer, cue sheets, --pipe/--film, --negative with its Perturb, --state, --names, --cues, --offline and --expect, and tools/verify.sh, tools/mutate.sh, tools/glslc.sh, tools/sweep.py, the CI and release workflows and scripts/release-lib.sh are patchwork's (which carries them from pitch, conway and the fleet before them), adapted.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Microfiche readers, and the people at them

The A6 card with its eye-readable header and a grid of frames (the 14 × 7 computer-output layout), the rear-projection reader with its carriage, zoom lens, focus knob and hotspot, silver, diazo and vesicular duplicating films — and the way anybody who has used one in a library browses: pan, overshoot, correct, zoom out to find the next page, zoom in, chase the focus. Modelled from how the machines work and how hands move; no manufacturer's design, document or firmware is copied.

## Standards and published specifications

What the implementation is measured against.

- **Paul M. Fitts, "The information capacity of the human motor system in controlling the amplitude of movement" (1954)** — in Shannon's form (MacKenzie 1992): every pan's duration, a + b log2( D / W + 1 ).
- **Tamar Flash and Neville Hogan, "The coordination of arm movements" (1985)** — the minimum-jerk profile of every submovement, and its 1.875 D / T peak.
- **Christopher M. Harris and Daniel M. Wolpert, "Signal-dependent noise determines motor planning" (1998)** — endpoint scatter proportional to the distance moved.
- **Digby Elliott, Werner F. Helsen and Romeo Chua, "A century later: Woodworth's (1899) two-component model of goal-directed aiming" (2001)** — primary submovements that undershoot, and the corrections after them.
- **Chris Wyman, Peter-Pike Sloan and Peter Shirley, "Simple Analytic Approximations to the CIE XYZ Color Matching Functions" (2013)** — the observer the lamp's black body is seen through.
- **IEC 61966-2-1 (sRGB)** — the transfer the clip is linearised by and the screen encoded with.
- **J. M. Hammersley (1960) and H. Vogel (1979)** — the taps: a Hammersley set across time and a Vogel spiral across the aperture.
- **Melissa E. O'Neill, PCG (2014)** — the integer hash behind every seeded draw, on the GPU and in the hand.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
