# Ladybird Browser - Personal Development Fork

This is a personal fork of [Ladybird Browser](https://github.com/LadybirdBrowser/ladybird) for learning and privacy-focused experimentation.

This fork maintains sync with upstream Ladybird while adding experimental features for educational purposes. **Not intended for production use.**

## Fork Features

This fork extends Ladybird with:

### Network Privacy
- **Tor Integration**: Per-tab Tor support with SOCKS5H proxy, DNS leak prevention, and stream isolation
- **VPN/Proxy Support**: Per-tab proxy configuration with NetworkIdentity management
- **.onion Support**: Native support for Tor hidden services

### P2P Protocols
- **IPFS**: Content-addressed web with `ipfs://` URL scheme
- **IPNS**: Mutable IPFS names with `ipns://` URL scheme
- **ENS**: Ethereum Name Service resolution for `.eth` domains
- **Gateway Fallback**: Automatic failover between local and public IPFS gateways

### Malware Detection (Sentinel)
- **YARA-based Scanning**: Real-time malware detection during downloads
- **Security Alerts**: User-facing dialog for threat notifications
- **Policy Enforcement**: Block, quarantine, or allow downloads based on user decisions
- **PolicyGraph Database**: Persistent storage for security policies and threat history
- **Quarantine System**: Secure isolation of malicious files with metadata tracking
- **Note**: Educational implementation, not security-audited

### Experimental Security (Educational)
- IPC rate limiting and validated decoding
- SafeMath operations for overflow protection
- **Note**: Not security-audited, for learning only

## Documentation

- **[docs/FORK.md](docs/FORK.md)** - Fork overview and quick start
- **[docs/FEATURES.md](docs/FEATURES.md)** - Detailed feature documentation with code examples
- **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)** - Development workflow
- **[docs/TESTING.md](docs/TESTING.md)** - Testing procedures

## Quick Start

```bash
# Build
cmake --preset Release
cmake --build Build/release -j$(nproc)

# Run
./Build/release/bin/Ladybird
```

See [Documentation/BuildInstructionsLadybird.md](Documentation/BuildInstructionsLadybird.md) for platform-specific build instructions.

## Testing Features

**Tor .onion domain:**
```
http://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion
```

**IPFS content:**
```
ipfs://QmXoypizjW3WknFiJnKLwHCnL72vedxjQkDDP1mXWo6uco
ipns://docs.ipfs.tech
```

**ENS domain:**
```
https://vitalik.eth
```

---

## About Ladybird

[Ladybird](https://ladybird.org) is a truly independent web browser using a novel engine based on web standards. Ladybird is in pre-alpha and suitable only for developers.

### Multi-Process Architecture

**Core Processes:**
- **Main UI Process** - Qt/AppKit/Android interface with security dialogs
- **WebContent Process** (per tab, sandboxed) - Rendering and JavaScript execution
- **ImageDecoder Process** (sandboxed) - Safe image decoding
- **RequestServer Process** - Enhanced network requests with:
  - Tor/SOCKS5H proxy support with DNS leak prevention
  - IPFS/IPNS protocol handler with gateway fallback
  - ENS (Ethereum Name Service) resolution
  - Per-tab NetworkIdentity for circuit isolation
  - Real-time malware scanning via SecurityTap
  - Request pause/resume for security policy enforcement

**Fork-Specific Services:**
- **Sentinel Service** - Standalone malware detection daemon with:
  - YARA-based rule engine
  - Unix socket IPC for inter-process communication
  - PolicyGraph SQLite database for security policies
  - Threat history tracking and quarantine management

### Core Libraries

**Base Libraries** (Inherited from SerenityOS):
- **LibWeb** - Web rendering engine
- **LibJS** - JavaScript engine
- **LibWasm** - WebAssembly implementation
- **LibCrypto/LibTLS** - Cryptography and TLS
- **LibHTTP** - HTTP/1.1 client
- **LibGfx** - 2D graphics and image decoding
- **LibUnicode** - Unicode and locale support
- **LibMedia** - Audio and video playback
- **LibCore** - Event loop and OS abstraction
- **LibIPC** - Inter-process communication

**Fork Enhancements:**
- **SecurityTap** - YARA integration for download scanning
- **PolicyGraph** - SQLite-backed security policy database
- **NetworkIdentity** - Per-tab network configuration management
- **Quarantine System** - Secure malicious file isolation

## Build and Run

### Build Instructions

See [Documentation/BuildInstructionsLadybird.md](Documentation/BuildInstructionsLadybird.md) for detailed instructions.

Ladybird runs on Linux, macOS, Windows (WSL2), and other *Nixes.

### Documentation

Code-related documentation is in the [Documentation/](Documentation/) folder.

## Upstream Participation

### Get Involved

Join [Ladybird's Discord server](https://discord.gg/nvfjVJ4Svh) to participate in upstream development.

**Contributing to upstream**: Read [Getting Started Contributing](Documentation/GettingStartedContributing.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

**Reporting issues**: See [issue policy](CONTRIBUTING.md#issue-policy) and [issue guidelines](ISSUES.md).

## Fork Disclaimer

⚠️ This fork contains experimental features for educational purposes:

- Not security-audited
- May contain bugs or vulnerabilities
- Should not be used in production
- Intended for learning and research only

For production use, visit the official [Ladybird Browser](https://github.com/LadybirdBrowser/ladybird).

## License

Ladybird is licensed under a 2-clause BSD license. This fork maintains the same license for all code.

See [LICENSE](LICENSE) for details.
