# 🌐 Ladybird Browser

[Ladybird](https://ladybird.org) is a truly independent web browser in active development, using a novel engine built from scratch based on modern web standards.

> [!IMPORTANT]  
> Ladybird is in a pre-alpha state and only suitable for use by developers.

Part of the [SerenityOS](https://github.com/SerenityOS/serenity) ecosystem.

---

## 📑 Table of Contents

- [About](#about)
- [Why Ladybird?](#-why-ladybird)
- [Features](#features)
- [Architecture](#architecture)
- [Quick Start](#-quick-start)
- [Build Instructions](#build-instructions)
- [Getting Started for Web Developers](#-getting-started-for-web-developers)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [Community & Help](#-community--help)
- [Related Projects](#-related-projects)
- [License](#license)

---

## 📘 About

Ladybird is a cross-platform browser built from scratch using modern C++ and Qt. It reuses SerenityOS’s rendering engine (`LibWeb`) but runs natively on Linux, macOS, Windows (via WSL2), and other Unix-like platforms.

---

## ❓ Why Ladybird?

Ladybird is a rare attempt to build a browser independent of Chromium, WebKit, or Gecko.

**Contributing helps you:**

- Learn browser internals, C++, and rendering engine architecture.
- Gain hands-on experience in a large-scale open-source project.
- Help shape an alternative to today's browser engine monopoly.

---

## ✨ Features

- Custom rendering engine: [`LibWeb`](https://github.com/SerenityOS/serenity/tree/master/Userland/Libraries/LibWeb)
- JavaScript engine: `LibJS`
- WebAssembly via `LibWasm`
- Audio/Video playback using `LibMedia`
- Sandboxed, multi-process architecture
- TLS via `LibTLS`, Unicode via `LibUnicode`
- Active developer community

---

## 🧠 Architecture

Ladybird uses a **multi-process design** for robustness and security:

- **UI Process**: Manages the interface and user interactions
- **WebContent Process**: Renders web pages (1 per tab)
- **RequestServer Process**: Handles networking
- **ImageDecoder Process**: Handles image decoding securely

> Many components are shared with SerenityOS, including:
> - `LibJS`, `LibWeb`, `LibGfx`, `LibHTTP`, `LibCore`, `LibCrypto`, `LibIPC`

---

## 🚀 Quick Start

```bash
git clone https://github.com/LadybirdBrowser/ladybird.git
cd ladybird
cmake -B build
cmake --build build -j$(nproc)
./build/Ladybird/Ladybird
````

> ✅ Make sure your system meets the requirements described in the [Build Instructions](Documentation/BuildInstructionsLadybird.md).

---

## 🔧 Build Instructions

Ladybird builds on:

* Linux
* macOS
* Windows (via WSL2)

See full setup: [Documentation/BuildInstructionsLadybird.md](Documentation/BuildInstructionsLadybird.md)

---

## 👋 Getting Started for Web Developers

Coming from a frontend background? Here's how you can help

🧪 Test HTML/CSS/JS rendering**: Spot inconsistencies and report bugs
🐞 Create minimal repros**: Help isolate rendering bugs with small HTML/CSS test cases
📄 Improve documentation**: Fix typos, clarify setup, or write guides
💡 Suggest UI improvements**: Help with visual/UI feedback via Qt

👉 Browse [Good First Issues](https://github.com/LadybirdBrowser/ladybird/issues?q=label%3A%22good+first+issue%22)


## 📚 Documentation

Find technical docs and contributor guides in the [Documentation/](Documentation/) directory:

* [Getting Started Contributing](Documentation/GettingStartedContributing.md)
* [Build Instructions](Documentation/BuildInstructionsLadybird.md)
* [Issue Reporting Guide](ISSUES.md)

---

## 🤝 Contributing

We welcome your contributions!

Please read the full guidelines in [`CONTRIBUTING.md`](CONTRIBUTING.md).
Before filing a bug, check our [Issue Policy](CONTRIBUTING.md#issue-policy).

---

## 💬 Community & Help

* 💬 Join [our Discord server](https://discord.gg/nvfjVJ4Svh) for discussions and help
* 🛠️ Report issues or suggest features [here](https://github.com/LadybirdBrowser/ladybird/issues)

---

## 🌐 Related Projects

* [SerenityOS](https://github.com/SerenityOS/serenity): Unix-like OS where Ladybird began
* [LibWeb](https://github.com/SerenityOS/serenity/tree/master/Userland/Libraries/LibWeb): Web rendering engine
* [LibJS](https://github.com/SerenityOS/serenity/tree/master/Userland/Libraries/LibJS): JavaScript engine

---

## 📝 License

Ladybird is licensed under the [2-clause BSD license](LICENSE).

```

