# Ladybird Browser - Personal Development Fork

> [!NOTE]
> **This is a personal fork of [Ladybird Browser](https://github.com/LadybirdBrowser/ladybird)** for learning and IPC security research.
>
> This fork includes experimental security enhancements not intended for upstream contribution.
> For the official project, visit [ladybird.org](https://ladybird.org)

## About This Fork

This fork maintains sync with upstream Ladybird while adding experimental IPC security features for educational purposes:

- **IPC Security Enhancements**: Rate limiting, validated decoding, overflow protection
- **Fuzzing Framework**: Automated IPC message testing infrastructure
- **Development Documentation**: Comprehensive guides for AI-assisted development

See [FORK_README.md](FORK_README.md) for detailed documentation of custom additions.

---

## About Ladybird

[Ladybird](https://ladybird.org) is a truly independent web browser, using a novel engine based on web standards.

> [!IMPORTANT]
> Ladybird is in a pre-alpha state, and only suitable for use by developers

### Features

Ladybird aims to build a complete, usable browser for the modern web.

**Multi-Process Architecture**:
- Main UI process (Qt/AppKit/Android UI)
- WebContent renderer processes (one per tab, sandboxed)
- ImageDecoder process (sandboxed image decoding)
- RequestServer process (network isolation)

Image decoding and network connections are done out of process to be more robust against malicious content.
Each tab has its own renderer process, which is sandboxed from the rest of the system.

**Core Libraries** (inherited from SerenityOS):
- **LibWeb**: Web rendering engine
- **LibJS**: JavaScript engine
- **LibWasm**: WebAssembly implementation
- **LibCrypto/LibTLS**: Cryptography primitives and Transport Layer Security
- **LibHTTP**: HTTP/1.1 client
- **LibGfx**: 2D Graphics Library, Image Decoding and Rendering
- **LibUnicode**: Unicode and locale support
- **LibMedia**: Audio and video playback
- **LibCore**: Event loop, OS abstraction layer
- **LibIPC**: Inter-process communication

## How do I build and run this?

See [build instructions](Documentation/BuildInstructionsLadybird.md) for information on how to build Ladybird.

Ladybird runs on Linux, macOS, Windows (with WSL2), and many other \*Nixes.

## How do I read the documentation?

Code-related documentation can be found in the [documentation](Documentation/) folder.

## Get in touch and participate!

Join [our Discord server](https://discord.gg/nvfjVJ4Svh) to participate in development discussion.

Please read [Getting started contributing](Documentation/GettingStartedContributing.md) if you plan to contribute to Ladybird for the first time.

Before opening an issue, please see the [issue policy](CONTRIBUTING.md#issue-policy) and the [detailed issue-reporting guidelines](ISSUES.md).

The full contribution guidelines can be found in [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

Ladybird is licensed under a 2-clause BSD license.
