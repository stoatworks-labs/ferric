# Attributions

ferric is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

> **This copy is hand-written**, because ferric is not registered in the backend
> yet. The entries below are the ones that apply; re-run the sync once the repo
> is registered and this file goes back to being generated like the rest.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0, and on Windows and Linux through the vcpkg manifest. Not fetched separately on macOS.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls directly — listed because it is present in the checkout.

### zlib

<https://zlib.net>  
Licence: zlib  
Copyright: Jean-loup Gailly and Mark Adler

Linked from the system copy that ships with macOS. Not vendored.

Used only by the offline harness, which writes PNGs so that a check can leave a picture behind. It is why the PNG writer in `tools/frtest` is fifty lines rather than a dependency.

## What is emulated here, and what is not claimed

The two noise-reduction curves this plugin models are the well-known consumer
sliding-band companders, and they are called **Type B** and **Type C**
throughout — in the code, in the dropdown and in the documentation.

Those systems are Dolby Laboratories' and the names are their trademarks. This
is not their product, is not licensed by them, and is not endorsed by them. It
is an emulation of a widely documented signal-processing topology, applied to a
picture rather than to audio, and named the way the rest of the industry names
that topology when it is not licensed to use the trademark.

No original coefficients, tables or reference designs were used. The curves here
are derived from the published description of what a sliding-band compander
does — see `source/Compander.h` — and are tuned by eye against a picture, which
is a thing no audio system was ever asked to do.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
