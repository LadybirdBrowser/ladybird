# Ladybird Fork - Feature Documentation

This document describes the custom features implemented in this personal fork of Ladybird Browser.

## Table of Contents

1. [Network Privacy Features](#network-privacy-features)
2. [P2P Protocol Support](#p2p-protocol-support)
3. [Security Enhancements](#security-enhancements)
4. [Architecture](#architecture)

---

## Network Privacy Features

### Tor Integration (Complete)

Full Tor integration with per-tab network isolation and DNS leak prevention.

**Features:**
- **SOCKS5H Proxy Support**: DNS queries routed through Tor (no leaks)
- **Stream Isolation**: Each tab can use a unique Tor circuit via SOCKS5 authentication
- **DNS Bypass**: Automatic DNS bypass for .onion domains
- **Circuit Rotation**: Support for changing circuits per tab
- **Per-Tab Configuration**: NetworkIdentity manages proxy settings per page

**Implementation Files:**
- `Libraries/LibIPC/NetworkIdentity.h` - Per-page network identity management
- `Libraries/LibIPC/ProxyConfig.h` - Proxy configuration structures
- `Services/RequestServer/Request.cpp` - Proxy application and DNS bypass logic
- `Services/RequestServer/ConnectionFromClient.cpp` - NetworkIdentity integration

**Key Components:**

```cpp
// NetworkIdentity - Per-page proxy configuration
class NetworkIdentity {
    Optional<ProxyConfig> m_proxy_config;
    Optional<ByteString> m_circuit_id;  // For Tor stream isolation

    static ProxyConfig for_tor_circuit(ByteString const& circuit_id);
};

// ProxyConfig - Proxy settings
struct ProxyConfig {
    ProxyType type;  // SOCKS5H, SOCKS5, HTTP, HTTPS
    ByteString host;
    u16 port;
    Optional<ByteString> username;  // For circuit isolation
    Optional<ByteString> password;
};
```

**Usage Example:**

```cpp
// Configure Tor for a specific tab
auto network_identity = NetworkIdentity::create();
network_identity->set_proxy_config(
    ProxyConfig::for_tor_circuit("unique-circuit-id")
);

// DNS queries for this tab will route through Tor
// Each unique circuit_id gets a separate Tor circuit
```

**DNS Leak Prevention:**
- When SOCKS5H proxy is configured, DNS lookup is skipped
- CURLOPT_RESOLVE is not applied (proxy handles DNS)
- CURLOPT_PROXYTYPE set to CURLPROXY_SOCKS5_HOSTNAME
- Hostnames sent to proxy for resolution

**Commits:**
- `3cfbe1897b7` - Fix critical Tor DNS regression: Restore SOCKS5H support

---

## P2P Protocol Support

### IPFS Integration (Complete)

Full IPFS (InterPlanetary File System) protocol support with gateway fallback.

**Supported Protocols:**
- `ipfs://` - Content-addressed IPFS resources
- `ipns://` - Mutable IPFS name system
- `.eth` domains - Ethereum Name Service (ENS) resolution

**Features:**
- **Gateway Fallback Chain**: Automatic failover between multiple IPFS gateways
- **Content Verification**: CID verification for IPFS content (optional)
- **Local Daemon Support**: Preference for local IPFS node when available
- **Public Gateway Fallback**: Falls back to public gateways (ipfs.io, dweb.link, cloudflare-ipfs.com)
- **Protocol Detection**: Automatic protocol type detection and URL transformation

**Implementation Files:**
- `Services/RequestServer/Request.h` - Protocol type enum and callbacks
- `Services/RequestServer/ConnectionFromClient.cpp` - Protocol detection and gateway transformation
- `Libraries/LibIPC/Multibase.cpp` - Base encoding support for CIDs
- `Libraries/LibIPC/Multihash.cpp` - Cryptographic hash support for CIDs
- `Libraries/LibIPC/IPFSAPIClient.cpp` - IPFS API client implementation

**Architecture:**

```cpp
// Request class supports different protocol types
enum class ProtocolType : u8 {
    HTTP,  // Standard HTTP/HTTPS
    IPFS,  // IPFS content-addressed
    IPNS,  // IPFS mutable names
    ENS,   // Ethereum Name Service
};

// Extension via callbacks (not inheritance)
void set_content_verification_callback(Function<ErrorOr<bool>(ReadonlyBytes)>);
void set_gateway_fallback_callback(Function<void()>);
```

**URL Transformation Examples:**

```
ipfs://QmHash...          → https://127.0.0.1:8080/ipfs/QmHash...
                            (local daemon)
                          → https://ipfs.io/ipfs/QmHash...
                            (fallback)

ipns://domain.com         → https://127.0.0.1:8080/ipns/domain.com
                          → https://ipfs.io/ipns/domain.com

https://vitalik.eth       → https://cloudflare-eth.com/v1/mainnet/resolve/vitalik.eth
                            (ENS resolution)
```

**Gateway Configuration:**

```cpp
// Gateway fallback chain
static Vector<ByteString> s_ipfs_gateways = {
    "http://127.0.0.1:8080",     // Local daemon (preferred)
    "https://ipfs.io",            // IPFS.io gateway
    "https://dweb.link",          // Distributed web gateway
    "https://cloudflare-ipfs.com" // Cloudflare gateway
};

// Gateway timeouts
static long s_gateway_connect_timeout_seconds = 10L;
static long s_gateway_request_timeout_seconds = 30L;
```

**Content Verification:**

IPFS content verification ensures downloaded content matches the requested CID:

```cpp
// Verify IPFS content integrity
auto verify_callback = [expected_cid](ReadonlyBytes content) -> ErrorOr<bool> {
    auto computed_cid = compute_cid(content);
    return computed_cid == expected_cid;
};

request->set_content_verification_callback(verify_callback);
```

**Commits:**
- `db8935a30e8` - Complete IPFS integration: Refactor ConnectionFromClient.cpp
- `afa266bc06b` - feat: Add IPFS protocol hooks to Request class (Phase 1-7.2)

---

## Security Enhancements

### IPC Security Features

The fork includes experimental IPC security enhancements for research and learning.

**Rate Limiting:**
- Per-connection message rate limiting
- Sliding window algorithm
- Configurable limits per message type

**Validated Decoding:**
- Bounds-checked IPC message decoding
- Type-safe parameter validation
- Automatic overflow detection

**SafeMath Operations:**
- Overflow-safe arithmetic operations
- Compile-time and runtime checks
- Prevents integer overflow vulnerabilities

**Implementation Files:**
- `Libraries/LibIPC/Limits.h` - IPC constants and limits
- `Libraries/LibIPC/RateLimiter.h` - Rate limiting implementation
- `Libraries/LibIPC/SafeMath.h` - Safe arithmetic operations
- `Libraries/LibIPC/ValidatedDecoder.h` - Validated message decoding

**Note:** These security features are experimental and for educational purposes only. They have not been security-audited and should not be used in production.

See [docs/SECURITY_AUDIT.md](SECURITY_AUDIT.md) for detailed analysis.

---

## Architecture

### Multi-Process Design

Ladybird uses a multi-process architecture for security and stability:

```
Main Process (UI)
├── WebContent Process (per tab, sandboxed)
│   └── Communicates via LibIPC
├── ImageDecoder Process (sandboxed)
│   └── Safe image decoding
└── RequestServer Process
    ├── Network requests
    ├── Tor/VPN proxy handling
    └── IPFS gateway management
```

### NetworkIdentity System

**Per-Page Network Configuration:**

Each browser tab has an associated NetworkIdentity that controls:
- Proxy settings (Tor, VPN, SOCKS5, HTTP)
- Circuit isolation (unique ID per tab)
- DNS routing preferences
- Network activity audit logging

**Data Flow:**

```
User Request
    ↓
WebContent Process
    ↓ (with page_id)
RequestServer
    ↓ (lookup NetworkIdentity by page_id)
NetworkIdentity
    ↓ (apply proxy config)
Request Class
    ↓
libcurl (with proxy settings)
    ↓
Network / Tor / IPFS Gateway
```

### Extension Pattern

The fork uses a **callback-based extension pattern** rather than inheritance:

**Benefits:**
- No changes to core Request class structure
- Easy to maintain during upstream syncs
- Clean separation of concerns
- Optional feature activation

**Example:**

```cpp
// Request provides hooks
class Request {
    Function<ErrorOr<bool>(ReadonlyBytes)> m_content_verification_callback;
    Function<void()> m_gateway_fallback_callback;

    void handle_complete_state() {
        if (m_content_verification_callback) {
            // Call callback if provided
            auto verified = m_content_verification_callback(response_data);
            if (!verified.value()) {
                // Trigger fallback if verification fails
                if (m_gateway_fallback_callback)
                    m_gateway_fallback_callback();
            }
        }
    }
};

// ConnectionFromClient implements callbacks
void setup_ipfs_verification(Request& request, String const& cid) {
    request.set_content_verification_callback([cid](ReadonlyBytes data) {
        return verify_ipfs_content(data, cid);
    });
}
```

---

## Testing

### Tor Testing

Test Tor integration with .onion domains:

```bash
# Run Ladybird
~/ladybird/Build/release/bin/Ladybird

# Test .onion domain
# Navigate to: http://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion

# Expected log output:
# RequestServer: Skipping DNS lookup for '...onion' (using SOCKS5H proxy - DNS via Tor)
```

### IPFS Testing

Test IPFS protocol support:

```bash
# Test IPFS URL
ipfs://QmXoypizjW3WknFiJnKLwHCnL72vedxjQkDDP1mXWo6uco

# Test IPNS URL
ipns://docs.ipfs.tech

# Test ENS domain
https://vitalik.eth
```

### Build and Test

```bash
# Build
cd ~/ladybird
cmake --preset Release
cmake --build Build/release -j$(nproc)

# Run
./Build/release/bin/Ladybird

# Run specific tests
./Meta/ladybird.py test LibIPC
```

---

## Development

### Sync with Upstream

This fork regularly synchronizes with upstream Ladybird:

```bash
cd ~/ladybird
git fetch upstream
git merge upstream/master
git push origin master
```

### Build Instructions

See [Documentation/BuildInstructionsLadybird.md](../Documentation/BuildInstructionsLadybird.md) for detailed build instructions.

### Contributing

This fork contains experimental features not intended for upstream contribution. For contributing to the official Ladybird project, see the upstream repository at https://github.com/LadybirdBrowser/ladybird

---

## License

This fork maintains the same 2-clause BSD license as upstream Ladybird. All custom additions are also BSD-licensed.

See [LICENSE](../LICENSE) for details.
