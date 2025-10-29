# Ladybird Browser - Personal Development Fork

This is a personal fork of the [Ladybird Browser](https://github.com/LadybirdBrowser/ladybird) project for learning, experimentation, and privacy-focused development.

## Fork Status

- **Upstream**: Synchronized with [LadybirdBrowser/ladybird](https://github.com/LadybirdBrowser/ladybird)
- **Purpose**: Personal learning, privacy research, and P2P protocol experimentation
- **Contribution Policy**: This fork contains experimental features not intended for upstream contribution

## Feature Overview

This fork extends Ladybird with privacy and P2P protocol features:

### ✅ Completed Features

1. **Tor Integration** - Full per-tab Tor support with DNS leak prevention
   - SOCKS5H proxy with hostname resolution via Tor
   - Stream isolation (unique circuits per tab)
   - DNS bypass for .onion domains
   - NetworkIdentity system for per-page proxy configuration

2. **IPFS Protocol Support** - Decentralized web content
   - `ipfs://` URL scheme support
   - `ipns://` URL scheme support
   - `.eth` ENS domain resolution
   - Gateway fallback chain (local daemon → public gateways)
   - Optional content verification via CID validation

3. **IPC Security Enhancements** - Educational security features
   - Rate limiting for IPC messages
   - Validated decoding with bounds checking
   - SafeMath operations for overflow protection

For detailed feature documentation, see [FEATURES.md](FEATURES.md)

## Documentation Structure

```
docs/
├── FEATURES.md          # Comprehensive feature documentation
├── FORK.md             # This file - fork overview
├── DEVELOPMENT.md      # Development workflow and setup
├── SECURITY_AUDIT.md   # IPC security analysis
├── TESTING.md          # Testing procedures
└── archive/            # Archived development notes
    ├── TOR_INTEGRATION_COMPLETE.md
    └── NETWORK_IDENTITY_DESIGN.md
```

## Quick Start

### Building

```bash
# Clone and build
git clone https://github.com/quanticsoul4772/ladybird.git
cd ladybird
cmake --preset Release
cmake --build Build/release -j$(nproc)
```

See [Documentation/BuildInstructionsLadybird.md](../Documentation/BuildInstructionsLadybird.md) for platform-specific instructions.

### Running

```bash
# Run Ladybird
./Build/release/bin/Ladybird
```

### Testing Tor

```bash
# Navigate to a .onion domain:
http://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion

# Check logs for DNS bypass message:
# "RequestServer: Skipping DNS lookup for '...onion' (using SOCKS5H proxy - DNS via Tor)"
```

### Testing IPFS

```bash
# Navigate to IPFS content:
ipfs://QmXoypizjW3WknFiJnKLwHCnL72vedxjQkDDP1mXWo6uco

# Or IPNS:
ipns://docs.ipfs.tech

# Or ENS:
https://vitalik.eth
```

## Key Implementation Files

### Network Privacy
- `Libraries/LibIPC/NetworkIdentity.h` - Per-page network identity
- `Libraries/LibIPC/ProxyConfig.h` - Proxy configuration
- `Services/RequestServer/Request.cpp` - Proxy application & DNS bypass
- `Services/RequestServer/ConnectionFromClient.cpp` - NetworkIdentity integration

### P2P Protocols
- `Services/RequestServer/Request.h` - Protocol type enum & callbacks
- `Services/RequestServer/ConnectionFromClient.cpp` - IPFS URL transformation
- `Libraries/LibIPC/Multibase.cpp` - Base encoding for CIDs
- `Libraries/LibIPC/Multihash.cpp` - Cryptographic hashes for CIDs
- `Libraries/LibIPC/IPFSAPIClient.cpp` - IPFS API client

### IPC Security (Experimental)
- `Libraries/LibIPC/Limits.h` - IPC constants
- `Libraries/LibIPC/RateLimiter.h` - Rate limiting
- `Libraries/LibIPC/SafeMath.h` - Safe arithmetic
- `Libraries/LibIPC/ValidatedDecoder.h` - Validated decoding

## Architecture Highlights

### Extension Pattern

This fork uses **callbacks instead of inheritance** for extension:

```cpp
// Request class provides optional hooks
class Request {
    ProtocolType m_protocol_type { ProtocolType::HTTP };
    Function<ErrorOr<bool>(ReadonlyBytes)> m_content_verification_callback;
    Function<void()> m_gateway_fallback_callback;
};

// ConnectionFromClient implements protocol-specific logic
void setup_ipfs_verification(Request& request, String const& cid);
void setup_gateway_fallback(Request& request, GatewayConfig config);
```

**Benefits:**
- Clean separation of concerns
- Easy upstream synchronization
- Optional feature activation
- No core class modifications

### NetworkIdentity System

Per-page network configuration enables:
- Different proxy settings per tab
- Tor circuit isolation
- VPN tunneling per page
- Network activity audit logging

```cpp
// Each tab has a NetworkIdentity
auto network_identity = NetworkIdentity::create();
network_identity->set_proxy_config(ProxyConfig::for_tor_circuit("unique-id"));

// RequestServer applies configuration
auto request = Request::fetch(/* ... */, network_identity);
```

## Sync Strategy

This fork regularly synchronizes with upstream:

```bash
git fetch upstream
git merge upstream/master
# Resolve any conflicts
git push origin master
```

**Merge Strategy:**
- Keep fork features isolated in specific files
- Minimize changes to core upstream files
- Use callbacks/hooks for extensibility
- Document all custom changes

## Learning Focus Areas

1. **Browser Architecture** - Multi-process design and IPC
2. **Network Privacy** - Tor integration and DNS leak prevention
3. **P2P Protocols** - IPFS, IPNS, ENS implementation
4. **Security Patterns** - IPC validation and rate limiting
5. **C++ Patterns** - Modern C++ in large-scale projects

## Disclaimer

This fork contains experimental features for educational purposes. The implementations:

- ❌ Are not security-audited
- ❌ May contain bugs or vulnerabilities
- ❌ Should not be used in production
- ✅ Are intended for learning and research only

For production use, please use the official [Ladybird Browser](https://github.com/LadybirdBrowser/ladybird).

## Upstream Project

Ladybird is a truly independent web browser with a novel engine based on web standards.

- **Website**: https://ladybird.org
- **Repository**: https://github.com/LadybirdBrowser/ladybird
- **Discord**: https://discord.gg/nvfjVJ4Svh

## License

This fork maintains the same 2-clause BSD license as the upstream Ladybird project.

All custom additions in this fork are also released under the 2-clause BSD license.

See [LICENSE](../LICENSE) for details.
