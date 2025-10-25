# Tor/VPN Integration Research for Ladybird

## Executive Summary

Research completed for integrating per-tab Tor/VPN isolation into Ladybird browser. **Recommended approach: SOCKS5 proxy integration via libcurl** (already used by Ladybird).

## Integration Options Analysis

### Option 1: SOCKS5 Proxy via libcurl ⭐ **RECOMMENDED**

**Pros**:
- Ladybird already uses libcurl extensively for HTTP requests
- libcurl has mature SOCKS5 support (`CURLOPT_PROXY`, `CURLOPT_PROXYTYPE`)
- Minimal code changes required
- Works with any SOCKS5 proxy (Tor, custom VPN, etc.)
- Per-request proxy configuration allows per-tab isolation
- No additional dependencies

**Cons**:
- Requires external Tor/VPN process running
- Less control over Tor circuit management

**Implementation**:
```cpp
// In ConnectionFromClient.cpp - add to request setup:
if (network_identity.has_tor_circuit()) {
    set_option(CURLOPT_PROXY, "socks5h://localhost:9050");
    set_option(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
}
```

**Complexity**: LOW (1-2 days)
**Risk**: LOW
**Maintainability**: HIGH

---

### Option 2: Arti (Rust Tor) Library Integration

**Pros**:
- Pure Rust implementation (1.0.0 released, production-ready)
- Designed for embedding from the start
- Full control over Tor circuits and identity
- No external process dependencies
- Java and Python bindings exist (C++ via FFI possible)

**Cons**:
- Requires Rust dependency in C++23 project
- FFI boundary complexity
- Need to maintain Rust-C++ bridge
- Larger binary size

**Implementation Approach**:
1. Create Rust library with C ABI exports
2. Build arti_client integration
3. Create C++ wrapper classes

**Complexity**: HIGH (2-3 weeks)
**Risk**: MEDIUM (FFI complexity, build system integration)
**Maintainability**: MEDIUM

---

### Option 3: Tor Browser C++ Components

**Pros**:
- Native C++ integration
- Proven in production (Firefox-based Tor Browser)
- Deep integration capabilities

**Cons**:
- Complex codebase to integrate
- Tight coupling with Firefox architecture
- Heavy dependencies
- License compatibility concerns

**Complexity**: VERY HIGH (1-2 months)
**Risk**: HIGH
**Maintainability**: LOW

---

## Recommended Implementation Strategy

### Phase 1: SOCKS5 Proxy Support (Week 1)

**Minimal Viable Feature**: Per-tab proxy configuration

**Files to Modify**:
1. `Services/RequestServer/ConnectionFromClient.h`
   - Add `Optional<ProxyConfig> m_proxy_config`
   - Add `set_proxy(ByteString host, u16 port, ProxyType type)`

2. `Services/RequestServer/ConnectionFromClient.cpp`
   - Modify `issue_network_request()` to check proxy config
   - Add CURLOPT_PROXY configuration when proxy is set

3. `Libraries/LibIPC/ProxyConfig.h` (new file)
   ```cpp
   namespace IPC {

   enum class ProxyType {
       None,
       SOCKS5,
       SOCKS5H,  // Hostname resolution via proxy
       HTTP,
       HTTPS
   };

   struct ProxyConfig {
       ProxyType type;
       ByteString host;
       u16 port;
       Optional<ByteString> username;
       Optional<ByteString> password;
   };

   }
   ```

**Testing**:
```bash
# Start Tor locally
sudo systemctl start tor
# Tor SOCKS5 proxy default: localhost:9050

# Test with proxy-enabled tab
curl --socks5-hostname localhost:9050 https://check.torproject.org
```

### Phase 2: Per-Tab Network Identity (Week 2)

**Goal**: Each tab can have independent network identity

**New Components**:
1. `Libraries/LibIPC/NetworkIdentity.h` - Manages per-tab identity
2. `Services/RequestServer/NetworkIdentityManager.cpp` - Coordinates identities
3. UI controls for enabling/disabling Tor per tab

### Phase 3: Tor Process Management (Week 3)

**Goal**: Automatically spawn and manage Tor instances

**Options**:
- **Option A**: Single Tor process with SOCKSPort isolation
  - Use `SOCKSPort auto IsolateClientAddr IsolateClientProtocol IsolateDestAddr IsolateDestPort`
  - Each connection gets separate circuit automatically

- **Option B**: Multiple Tor instances per tab
  - Spawn separate Tor process per tab
  - Complete isolation but higher resource usage

**Recommended**: Option A (single Tor with stream isolation)

### Phase 4: Circuit Management & Rotation (Week 4)

**Features**:
- "New Identity" button per tab (sends NEWNYM signal)
- Automatic circuit rotation
- Circuit lifetime tracking
- Identity fingerprinting prevention

---

## Current Ladybird Architecture Analysis

### Existing libcurl Usage

**Location**: `Services/RequestServer/ConnectionFromClient.cpp`

**Current CURL Options Used**:
- `CURLOPT_URL`, `CURLOPT_PORT` - Request destination
- `CURLOPT_CUSTOMREQUEST` - HTTP method
- `CURLOPT_HTTPHEADER` - Custom headers
- `CURLOPT_POSTFIELDS`, `CURLOPT_POSTFIELDSIZE` - Request body
- `CURLOPT_WRITEFUNCTION`, `CURLOPT_HEADERFUNCTION` - Callbacks
- `CURLOPT_RESOLVE` - Custom DNS resolution
- `CURLOPT_CAINFO` - SSL certificates
- `CURLOPT_ACCEPT_ENCODING` - Compression support

**Proxy Configuration Hooks**:
Currently NO proxy configuration exists. Perfect opportunity to add SOCKS5 support!

### Multi-Process Architecture

```
Browser UI Process
    └─> WebContent Process (per tab)
            └─> RequestServer Process (per WebContent)
```

**Key Insight**: Each tab already has its own RequestServer process! This makes per-tab proxy configuration trivial.

### IPC Message Flow

```
WebContentClient (UI)
    --[IPC: start_request()]-->
ConnectionFromClient (RequestServer)
    --[libcurl: curl_multi_add_handle()]-->
Network
```

**Integration Point**: Add proxy config to `start_request()` IPC message or store per-connection.

---

## Implementation Recommendation

### START HERE: Simple SOCKS5 Proxy Support

**Effort**: 1-2 days
**Value**: Immediate Tor support with minimal risk

**Steps**:
1. Add `ProxyConfig` struct to LibIPC
2. Modify `ConnectionFromClient` to accept proxy configuration
3. Apply `CURLOPT_PROXY` when proxy is configured
4. Add UI toggle: "Enable Tor for this tab"
5. Test with local Tor instance

**This gives you**:
- Per-tab Tor routing (manual configuration)
- Foundation for automatic Tor management
- Proof of concept for zero-trust architecture
- Immediate security value

**Future Enhancement Path**:
- Phase 2: Automatic Tor process management
- Phase 3: Circuit rotation and identity management
- Phase 4: Arti integration for embedded Tor (if needed)
- Phase 5: P2P protocol support (IPFS, Hypercore)

---

## Security Considerations

### Stream Isolation

Tor's stream isolation ensures different tabs can't be correlated:
```
# Per-tab isolation via SOCKS authentication
Tab 1: SOCKS5 with username "tab-<page-id-1>"
Tab 2: SOCKS5 with username "tab-<page-id-2>"
```

libcurl supports SOCKS5 authentication:
```cpp
set_option(CURLOPT_PROXYUSERPWD, "tab-<page-id>:password");
```

### Identity Leaks to Prevent

- **DNS Leaks**: Use `CURLOPT_PROXYTYPE = CURLPROXY_SOCKS5_HOSTNAME` (socks5h://)
  - Hostname resolution happens via Tor, not locally
- **WebRTC Leaks**: Disable WebRTC IP discovery (separate feature)
- **HTTP Headers**: Strip identifying headers (User-Agent fingerprinting)
- **Cookies**: Already isolated per tab in Ladybird

---

## Next Steps

1. ✅ Complete this research (DONE)
2. Create `ProxyConfig` class in LibIPC
3. Extend `ConnectionFromClient` with proxy support
4. Add libcurl SOCKS5 configuration
5. Test with local Tor instance
6. Add UI controls for per-tab Tor toggle

---

## Resources

- [libcurl SOCKS proxy documentation](https://everything.curl.dev/usingcurl/proxies/socks)
- [Arti Tor implementation](https://blog.torproject.org/arti_100_released/)
- [Tor stream isolation](https://www.whonix.org/wiki/Stream_Isolation)
- Ladybird source: `Services/RequestServer/ConnectionFromClient.cpp`

---

## Conclusion

**Recommendation**: Start with SOCKS5 proxy support via libcurl. This is:
- Low risk, high value
- Builds on existing infrastructure
- Provides immediate Tor capability
- Foundation for advanced features (circuit rotation, identity management)
- Compatible with future P2P integration

**Timeline**: Working prototype in 1-2 days, production-ready feature in 1-2 weeks.

**Success Criteria**:
- Tab can route through Tor via local SOCKS5 proxy
- Different tabs can have different network identities
- No DNS leaks (hostname resolution via proxy)
- UI clearly shows when Tor is active

