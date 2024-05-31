# Ladybird

[Ladybird](https://github.com/LadybirdWebBrowser/ladybird) is a cross-platform web browser built on the [LibWeb](https://github.com/LadybirdWebBrowser/ladybird/tree/master/Userland/Libraries/LibWeb) and [LibJS](https://github.com/LadybirdWebBrowser/ladybird/tree/master/Userland/Libraries/LibJS) engines.
The Browser UI has a cross-platform GUI in Qt6 and a macOS-specific GUI in AppKit.

[![GitHub Actions Status](https://github.com/LadybirdWebBrowser/ladybird/workflows/Build,%20lint,%20and%20test/badge.svg)](https://github.com/LadybirdWebBrowser/ladybird/actions?query=workflow%3A"Build%2C%20lint%2C%20and%20test")
[![Fuzzing Status](https://oss-fuzz-build-logs.storage.googleapis.com/badges/serenity.svg)](https://bugs.chromium.org/p/oss-fuzz/issues/list?sort=-opened&can=1&q=proj:serenity)
!! FIXME: Add slack badge

[FAQ](Documentation/FAQ.md) | [Documentation](#how-do-i-read-the-documentation) | [Build Instructions](#how-do-i-build-and-run-this)

Ladybird aims to be a standards-compliant, independent web browser with limited third-party dependencies.
Currently, the only dependencies are UI frameworks like Qt6 and AppKit, and low-level platform-specific
libraries like PulseAudio, CoreAudio and OpenGL.

> [!IMPORTANT]
> Ladybird is in a pre-alpha state, and only suitable for use by developers
>

## Features

The Ladybird browser application uses a multiprocess architecture with a main UI process, several WebContent renderer processes,
an ImageDecoder process, a RequestServer process, and a SQLServer process for holding cookies.

Image decoding and network connections are done out of process to be more robust against malicious content.
Each tab has its own renderer process, which is sandboxed from the rest of the system.

Many core library support components are derived from SerenityOS:

- LibWeb: Web Rendering Engine
- LibJS: JavaScript Engine
- LibWasm: WebAssembly implementation
- LibCrypto/LibTLS: Cryptography primitives and Transport Layer Security (rather than OpenSSL)
- LibHTTP: HTTP/1.1 client
- LibGfx: 2D Graphics Library, Image Decoding and Rendering (rather than skia)
- LibArchive: Archive file format support (rather than libarchive, zlib)
- LibUnicode, LibLocale: Unicode and Locale support (rather than libicu)
- LibAudio, LibVideo: Audio and Video playback (rather than libav, ffmpeg)
- LibCore: Event Loop, OS Abstraction layer
- LibIPC: Inter-Process Communication
- ... and more!

## How do I build and run this?

See [build instructions](Documentation/BuildInstructionsLadybird.md) for information on how to build Ladybird.

Ladybird runs on Linux, macOS, Windows (with WSL2), SerenityOS and many other *Nixes.

## How do I read the documentation?

Code-related documentation can be found in the [documentation](Documentation/) folder.

## Get in touch and participate!

Join our Slack server: !!FIXME: Add slack link !!

Before opening an issue, please see the [issue policy](https://github.com/LadybirdWebBrowser/ladybird/blob/master/CONTRIBUTING.md#issue-policy).

A general guide for contributing can be found in [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

Ladybird is licensed under a 2-clause BSD license.

## More Information

For more information about the history of Ladybird, see [this blog post](https://awesomekling.github.io/Ladybird-a-new-cross-platform-browser-project/).

The official website for Ladybird is [ladybird.dev](https://ladybird.dev).
