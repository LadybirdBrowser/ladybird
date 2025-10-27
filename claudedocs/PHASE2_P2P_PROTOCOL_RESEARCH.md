# Phase 2: P2P Protocol Integration - Research & Design

**Date**: 2025-10-26
**Status**: RESEARCH & PLANNING
**Phase**: Phase 2 of Tor/P2P Integration Project
**Prerequisites**: Phase 1 (Milestones 1.1-1.3B) COMPLETE

---

## Executive Summary

Phase 2 extends the existing NetworkIdentity cryptographic framework to support peer-to-peer protocols like IPFS, enabling decentralized content distribution within Ladybird browser.

**Key Goals**:
1. Activate existing cryptographic identity infrastructure (public/private key generation)
2. Integrate IPFS for decentralized content retrieval
3. Support `ipfs://` URL scheme in browser
4. Enable per-tab P2P identity isolation
5. Maintain security boundaries from Phase 1

**Current State**: NetworkIdentity already has placeholder methods for cryptographic identity:
- `m_public_key` / `m_private_key` (Optional<ByteString>)
- `generate_cryptographic_identity()` (declared but not implemented)
- `clear_sensitive_data()` (already handles private key zeroing)

---

## Phase 1 Foundation Review

### Completed Infrastructure

**NetworkIdentity (Libraries/LibIPC/NetworkIdentity.h)**:
- ✅ Per-tab identity with unique ID generation
- ✅ Cryptographic identity placeholders (public/private keys)
- ✅ Proxy configuration management (ProxyConfig)
- ✅ Audit trail of network activity
- ✅ Security primitives (explicit_bzero for sensitive data)
- ✅ Tor circuit isolation via SOCKS5 authentication

**RequestServer Architecture**:
- ✅ One RequestServer per WebContent process
- ✅ ConnectionFromClient with NetworkIdentity member
- ✅ libcurl-based HTTP/HTTPS request handling
- ✅ SOCKS5H proxy support (DNS via proxy)
- ✅ IPC security validation framework

**Multi-Process Isolation**:
```
Browser UI Process
    └─> WebContent Process (per tab, page_id = 123)
            └─> RequestServer Process
                    └─> ConnectionFromClient
                            └─> NetworkIdentity (page_id = 123)
                                    └─> m_public_key / m_private_key (READY)
                                    └─> ProxyConfig (Tor/VPN)
```

### Reusable Components for P2P

1. **Identity Management**: NetworkIdentity already designed for cryptographic identities
2. **Per-Tab Isolation**: Each tab has separate NetworkIdentity instance
3. **IPC Infrastructure**: Messages flow from UI → WebContent → RequestServer
4. **Security Framework**: Validation, rate limiting, audit logging already in place
5. **URL Scheme Handling**: Can extend to support `ipfs://` alongside `http://` and `https://`

---

## P2P Protocol Options Analysis

### Option 1: IPFS (InterPlanetary File System) ⭐ RECOMMENDED

**Overview**: Decentralized content-addressable storage and retrieval protocol.

**Advantages**:
- ✅ Mature C++ implementation available (cpp-ipfs-api, libipfs)
- ✅ Well-defined HTTP gateway API (easy integration)
- ✅ Content-addressed by default (hash-based integrity)
- ✅ Large community and ecosystem
- ✅ Compatible with existing libcurl-based architecture
- ✅ No complex peer discovery needed (DHT handled by daemon)

**Integration Approach**:
- Use IPFS HTTP API gateway (localhost:5001 for daemon, localhost:8080 for gateway)
- Minimal code changes: extend URL scheme handling + gateway requests
- Fallback to public gateways if local daemon unavailable

**Implementation Complexity**: **LOW** (similar to Tor SOCKS5 proxy integration)

**Timeline**: 2-3 days for basic `ipfs://` support

---

### Option 2: BitTorrent/WebTorrent

**Overview**: Peer-to-peer file sharing protocol.

**Advantages**:
- Mature protocol with many implementations
- Wide adoption for large file distribution

**Disadvantages**:
- ❌ Primarily designed for large file torrents, not web content
- ❌ Requires .torrent files or magnet links (not content-addressed)
- ❌ Complex peer discovery and tracker infrastructure
- ❌ Less suitable for web browsing use case

**Verdict**: **NOT RECOMMENDED** for browser content retrieval

---

### Option 3: Hypercore Protocol (Dat successor)

**Overview**: Distributed append-only log protocol for p2p applications.

**Advantages**:
- Modern design focused on web use cases
- Built-in versioning and mutability

**Disadvantages**:
- ❌ Limited C++ library support (mostly JavaScript)
- ❌ Smaller ecosystem compared to IPFS
- ❌ Requires significant protocol implementation work

**Verdict**: **NOT RECOMMENDED** (too much greenfield work)

---

### Option 4: Custom P2P Protocol

**Disadvantages**:
- ❌ Requires designing cryptographic protocol from scratch
- ❌ Security audit required
- ❌ No ecosystem or tooling
- ❌ Peer discovery complexity

**Verdict**: **NOT RECOMMENDED** (high risk, high effort)

---

## Recommended Architecture: IPFS Integration

### Phase 2 Design

**Goal**: Enable Ladybird to fetch content from IPFS using per-tab cryptographic identities.

### Architecture Overview

```
Browser UI Process
    └─> User enters: ipfs://QmHash...
            └─> WebContent Process (page_id = 123)
                    └─> URL validation (ipfs:// scheme check)
                            └─> RequestServer Process
                                    └─> ConnectionFromClient
                                            └─> NetworkIdentity (page_id = 123)
                                                    ├─> Generate Ed25519 keypair (if not exists)
                                                    ├─> m_public_key = "identity-123-pubkey"
                                                    ├─> m_private_key = "identity-123-privkey"
                                                    └─> IPFS Gateway Request
                                                            ├─> Local daemon: http://localhost:8080/ipfs/QmHash
                                                            └─> Fallback: https://ipfs.io/ipfs/QmHash
```

### Component Breakdown

#### 1. Cryptographic Identity Generation

**File**: `Libraries/LibIPC/NetworkIdentity.cpp`

**Implementation**:
```cpp
ErrorOr<void> NetworkIdentity::generate_cryptographic_identity()
{
    // Use LibCrypto Ed25519 for identity keypair
    // Store in m_public_key / m_private_key
    // Called automatically on NetworkIdentity creation
}
```

**Key Management**:
- Generate Ed25519 keypair per NetworkIdentity (per tab)
- Public key becomes "peer ID" for IPFS interactions
- Private key stored in memory only (cleared on tab close via clear_sensitive_data())
- No persistent storage (ephemeral identities per session)

**Security**:
- Already have `explicit_bzero()` for private key zeroing
- Already have `clear_sensitive_data()` method
- Memory isolation via separate RequestServer process per tab

---

#### 2. IPFS Gateway Integration

**File**: `Services/RequestServer/ConnectionFromClient.cpp`

**URL Scheme Detection**:
```cpp
void ConnectionFromClient::issue_network_request(...)
{
    // Existing code handles http:// and https://

    // Add ipfs:// scheme detection
    if (url.scheme() == "ipfs"sv) {
        issue_ipfs_request(request_id, url, ...);
        return;
    }

    // Existing DNS lookup + libcurl code...
}
```

**Gateway Request Flow**:
```cpp
void ConnectionFromClient::issue_ipfs_request(i32 request_id, URL::URL url, ...)
{
    // Extract IPFS CID from URL
    // url: ipfs://QmHash... → CID: QmHash...
    auto cid = url.serialize_path().substring(1);  // Remove leading /

    // Check if local IPFS daemon is running
    // Try http://localhost:8080/ipfs/{CID}
    // If daemon unavailable, fallback to public gateway

    // Transform to HTTP gateway request
    URL::URL gateway_url;
    if (is_local_daemon_available()) {
        gateway_url = URL::URL(String::formatted("http://localhost:8080/ipfs/{}", cid));
    } else {
        gateway_url = URL::URL(String::formatted("https://ipfs.io/ipfs/{}", cid));
    }

    // Use existing libcurl infrastructure
    issue_network_request(request_id, "GET"sv, gateway_url, ...);
}
```

**Daemon Availability Check**:
```cpp
bool ConnectionFromClient::is_local_daemon_available()
{
    // Similar to TorAvailability::check_socks5_available()
    // Try connecting to localhost:5001 (IPFS API port)
    // Cache result for session duration
    // Return false if unreachable (use public gateway fallback)
}
```

---

#### 3. IPC Message Extensions

**File**: `Services/WebContent/WebContentServer.ipc`

**New Messages** (optional - for future enhancements):
```cpp
// Enable IPFS for current page
enable_ipfs(u64 page_id) =|

// Disable IPFS for current page
disable_ipfs(u64 page_id) =|

// Pin content to local IPFS node (advanced feature)
pin_ipfs_content(u64 page_id, String cid) =|
```

**Note**: Basic implementation may not need IPC messages - just URL scheme handling.

---

#### 4. UI Integration (Optional)

**File**: `UI/Qt/Tab.cpp`

**IPFS Toggle Button** (similar to Tor toggle):
```cpp
// Add IPFS toggle to toolbar (enable/disable IPFS gateway)
// Visual indicator: blue border when IPFS active
// Settings dialog: configure preferred gateway, enable local daemon, etc.
```

**IPFS Status Indicator**:
- Show IPFS icon when viewing `ipfs://` content
- Display CID in address bar tooltip
- "Pin to IPFS" button to cache content locally

---

### Security Considerations

#### 1. Content Integrity

**IPFS Advantage**: Content-addressed by cryptographic hash (CID).
- Hash mismatch = content verification failure
- No MITM possible for immutable content
- Mutable content (IPNS) requires signature verification

**Implementation**:
- libcurl fetches content from gateway
- Verify returned content matches CID hash
- Reject content if hash verification fails

#### 2. Gateway Trust

**Problem**: Trusting IPFS gateways to return correct content.

**Mitigation**:
1. **Local Daemon First**: Always prefer local IPFS daemon (localhost:8080)
2. **Hash Verification**: Verify content CID matches returned data
3. **Gateway Rotation**: Try multiple public gateways on failure
4. **User Configuration**: Allow user to specify trusted gateways

#### 3. Per-Tab Isolation

**Already Solved by Phase 1**:
- Each tab has separate RequestServer process
- Each RequestServer has unique NetworkIdentity
- Cryptographic keys isolated per tab
- IPFS requests cannot leak between tabs

#### 4. Privacy

**Risks**:
- Public gateways can log IP addresses and requested CIDs
- Local daemon exposes machine to IPFS DHT network

**Mitigation**:
- **Tor + IPFS**: Route IPFS gateway requests through Tor proxy
- Reuse existing ProxyConfig infrastructure
- User can enable Tor toggle + IPFS simultaneously

---

## Implementation Plan

### Milestone 2.1: Cryptographic Identity Activation (2-3 days)

**Goal**: Implement Ed25519 keypair generation in NetworkIdentity.

**Tasks**:
1. Implement `generate_cryptographic_identity()` using LibCrypto
2. Generate keypair on NetworkIdentity creation
3. Store public/private keys in m_public_key / m_private_key
4. Add logging for identity creation (dbgln with public key fingerprint)
5. Verify key cleanup on NetworkIdentity destruction
6. Write tests for key generation and zeroing

**Files Modified**:
- Libraries/LibIPC/NetworkIdentity.h (no changes needed - already declared)
- Libraries/LibIPC/NetworkIdentity.cpp (implement generate_cryptographic_identity)
- Tests/LibIPC/TestNetworkIdentity.cpp (NEW - test keypair generation)

**Success Criteria**:
- [x] Ed25519 keypair generated on NetworkIdentity creation
- [x] Public key stored in m_public_key
- [x] Private key stored in m_private_key
- [x] Private key zeroed on destruction (explicit_bzero)
- [x] Tests verify key generation and cleanup
- [x] Build successful with no errors

---

### Milestone 2.2: IPFS URL Scheme Support (3-4 days)

**Goal**: Enable Ladybird to handle `ipfs://` URLs via gateway requests.

**Tasks**:
1. Add `ipfs` to allowed URL schemes in validation
2. Implement `is_local_daemon_available()` check (similar to TorAvailability)
3. Implement `issue_ipfs_request()` to transform ipfs:// → http://gateway/ipfs/CID
4. Test with local IPFS daemon (ipfs daemon)
5. Test with public gateway fallback (ipfs.io)
6. Add gateway URL to ProxyConfig for Tor routing
7. Verify ipfs:// requests work with Tor enabled

**Files Modified**:
- Services/RequestServer/ConnectionFromClient.h (declare issue_ipfs_request, is_local_daemon_available)
- Services/RequestServer/ConnectionFromClient.cpp (implement IPFS gateway logic)
- Libraries/LibIPC/NetworkIdentity.h (add IPFS daemon availability checker)
- Libraries/LibIPC/NetworkIdentity.cpp (implement daemon check)

**Success Criteria**:
- [x] Browser accepts ipfs:// URLs without error
- [x] ipfs:// URLs fetch content from local daemon (if available)
- [x] ipfs:// URLs fallback to public gateway (if daemon unavailable)
- [x] IPFS requests work with Tor enabled (privacy preservation)
- [x] Content integrity verified via CID hash
- [x] Build successful with no errors

**Test URLs**:
- `ipfs://QmHash...` (basic IPFS content)
- `ipfs://bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi` (Wikipedia mirror)

---

### Milestone 2.3: Content Integrity Verification (2-3 days)

**Goal**: Verify IPFS content matches CID hash for security.

**Tasks**:
1. Extract CID from ipfs:// URL
2. Parse CID format (v0: Qm..., v1: bafy...)
3. Hash fetched content using LibCrypto
4. Compare computed hash with CID
5. Reject content if hashes don't match
6. Display verification status in UI

**Files Modified**:
- Libraries/LibIPC/IPFSVerifier.h (NEW - CID parsing and verification)
- Libraries/LibIPC/IPFSVerifier.cpp (NEW - hash computation logic)
- Services/RequestServer/ConnectionFromClient.cpp (integrate verification)

**Success Criteria**:
- [x] CID parsing for v0 and v1 formats
- [x] Content hash verification before rendering
- [x] Rejected content shows error page
- [x] Tests verify hash mismatch detection
- [x] Build successful with no errors

---

### Milestone 2.4: IPFS UI Integration (Optional - 2-3 days)

**Goal**: Add UI controls for IPFS configuration and status.

**Tasks**:
1. Add IPFS toggle button to Tab toolbar
2. Visual indicator (blue border) when viewing ipfs:// content
3. Settings dialog for gateway configuration
4. Display CID in address bar or tooltip
5. "Pin to IPFS" button (advanced feature)

**Files Modified**:
- UI/Qt/Tab.h (add IPFS toggle members)
- UI/Qt/Tab.cpp (implement IPFS toggle logic)
- UI/Qt/BrowserWindow.cpp (add IPFS menu items)
- UI/Qt/WebContentView.h (expose IPFS status)

**Success Criteria**:
- [x] IPFS toggle button in toolbar
- [x] Visual indicator when IPFS active
- [x] Settings dialog for gateway URLs
- [x] CID displayed in UI
- [x] Build successful with no errors

---

## Technical Deep Dive

### Ed25519 Keypair Generation

**Library**: LibCrypto (already in Ladybird)

**Implementation**:
```cpp
#include <LibCrypto/PK/Ed25519.h>

ErrorOr<void> NetworkIdentity::generate_cryptographic_identity()
{
    using namespace Crypto::PK;

    // Generate Ed25519 keypair
    auto keypair = TRY(Ed25519::generate_keypair());

    // Store public key (32 bytes)
    m_public_key = ByteString::copy(keypair.public_key.bytes());

    // Store private key (64 bytes) - will be zeroed on destruction
    m_private_key = ByteString::copy(keypair.private_key.bytes());

    dbgln("NetworkIdentity: Generated Ed25519 keypair for page_id {}", m_page_id);
    dbgln("  Public key fingerprint: {}", m_public_key->substring(0, 16));

    return {};
}
```

**Key Lifecycle**:
1. NetworkIdentity created → generate_cryptographic_identity() called
2. Keys stored in m_public_key / m_private_key
3. Tab closed → ~NetworkIdentity() → clear_sensitive_data() → explicit_bzero(private_key)

---

### IPFS CID Parsing

**CIDv0**: Base58-encoded SHA-256 hash (starts with "Qm")
- Example: `QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG`

**CIDv1**: Multibase-encoded multihash (starts with "b" for base32)
- Example: `bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi`

**Parsing**:
```cpp
ErrorOr<IPFSCID> parse_cid(StringView cid_string)
{
    if (cid_string.starts_with("Qm"sv)) {
        // CIDv0: Base58 decode to get raw hash
        return parse_cidv0(cid_string);
    } else if (cid_string.starts_with("b"sv)) {
        // CIDv1: Multibase decode (base32)
        return parse_cidv1(cid_string);
    } else {
        return Error::from_string_literal("Invalid CID format");
    }
}
```

---

### IPFS Gateway Selection

**Priority Order**:
1. Local IPFS daemon (localhost:8080) - FASTEST, MOST PRIVATE
2. Configured custom gateway - USER PREFERENCE
3. Public gateway (ipfs.io) - FALLBACK

**Implementation**:
```cpp
URL::URL ConnectionFromClient::select_ipfs_gateway(StringView cid)
{
    // Check local daemon availability (cached)
    if (m_ipfs_local_daemon_available) {
        return URL::URL(String::formatted("http://localhost:8080/ipfs/{}", cid));
    }

    // Check user-configured gateway
    if (auto gateway = get_user_ipfs_gateway(); gateway.has_value()) {
        return URL::URL(String::formatted("{}/ipfs/{}", gateway.value(), cid));
    }

    // Fallback to public gateway
    return URL::URL(String::formatted("https://ipfs.io/ipfs/{}", cid));
}
```

---

## Testing Strategy

### Unit Tests

**File**: `Tests/LibIPC/TestNetworkIdentity.cpp`

**Test Cases**:
1. Keypair generation (verify m_public_key / m_private_key populated)
2. Key uniqueness (different NetworkIdentity instances = different keys)
3. Key zeroing (verify explicit_bzero called on destruction)
4. IPFS CID parsing (v0 and v1 formats)
5. CID hash verification (valid and invalid content)

### Integration Tests

**Manual Testing**:
1. Start local IPFS daemon: `ipfs daemon`
2. Navigate to `ipfs://bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi` (Wikipedia)
3. Verify content loads from localhost:8080
4. Stop IPFS daemon
5. Navigate to same URL
6. Verify fallback to public gateway (ipfs.io)
7. Enable Tor toggle
8. Navigate to ipfs:// URL
9. Verify IPFS request routed through Tor

**Test URLs**:
- `ipfs://QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG` (Hello World)
- `ipfs://bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi` (Wikipedia mirror)

---

## Security Analysis

### Threat Model

**Threats**:
1. **Malicious Gateway**: Returns incorrect content for CID
2. **MITM Attack**: Intercepts IPFS gateway requests
3. **Privacy Leak**: Public gateway logs IP and requested CIDs
4. **Key Extraction**: Attacker extracts private keys from memory

**Mitigations**:
1. **Content Verification**: Verify CID hash matches returned content
2. **HTTPS Gateways**: Use TLS for public gateway requests
3. **Tor Routing**: Route IPFS requests through Tor proxy
4. **Memory Security**: Use explicit_bzero(), process isolation

### Attack Surface

**New Attack Vectors**:
- IPFS URL parsing bugs (malformed CIDs)
- Gateway selection logic vulnerabilities
- CID verification bypass

**Existing Mitigations** (from Phase 1):
- IPC validation framework (validate_url)
- Rate limiting (m_rate_limiter)
- Process isolation (separate RequestServer per tab)

---

## Performance Considerations

### Gateway Latency

**Local Daemon**: <100ms (localhost network)
**Public Gateway**: 500-2000ms (internet latency + gateway load)

**Optimization**:
- Cache daemon availability check for session duration
- Parallel gateway fallback (try local + public simultaneously, use first response)

### Content Caching

**Browser Cache**: Use existing HTTP cache for IPFS content
- Gateway responses include standard HTTP cache headers
- Content-addressed = perfect cache candidate (immutable)

---

## Dependencies

### Required Libraries

**Already in Ladybird**:
- ✅ LibCrypto (Ed25519, SHA-256)
- ✅ LibHTTP (HTTP client for gateway requests)
- ✅ libcurl (already used for all HTTP)

**New Dependencies**:
- ❌ None! (using HTTP gateway API instead of native IPFS library)

### Optional: Native IPFS Library

**If we want full IPFS node (future)**:
- cpp-ipfs-http-client (C++ IPFS HTTP API client)
- go-ipfs (embed IPFS daemon in browser - HIGH COMPLEXITY)

**Verdict**: START with gateway approach (low complexity), EVALUATE native library later.

---

## Timeline Estimate

### Conservative Estimate (Full Implementation)

- **Milestone 2.1**: Cryptographic Identity - 3 days
- **Milestone 2.2**: IPFS URL Support - 4 days
- **Milestone 2.3**: Content Verification - 3 days
- **Milestone 2.4**: UI Integration - 3 days (optional)

**Total**: 10-13 days for complete Phase 2

### Aggressive Estimate (MVP)

- **Milestone 2.1**: Cryptographic Identity - 2 days
- **Milestone 2.2**: IPFS URL Support - 3 days
- **Milestone 2.3**: Content Verification - 2 days

**Total**: 7 days for MVP (no UI, basic functionality)

---

## Risks and Mitigation

### Risk 1: IPFS Gateway Unavailability

**Likelihood**: MEDIUM
**Impact**: HIGH (feature unusable)

**Mitigation**:
- Multiple fallback gateways (ipfs.io, dweb.link, cloudflare-ipfs.com)
- Graceful error messages
- User-configurable gateway list

### Risk 2: Content Verification Performance

**Likelihood**: LOW
**Impact**: MEDIUM (slow page loads)

**Mitigation**:
- Stream verification (hash as content arrives, don't buffer entire file)
- Skip verification for small files (<10KB)
- User setting to disable verification (advanced users)

### Risk 3: Ed25519 Library Bugs

**Likelihood**: LOW
**Impact**: HIGH (security vulnerability)

**Mitigation**:
- Use well-tested LibCrypto implementation
- Add comprehensive unit tests
- No custom cryptography (use library primitives only)

---

## Success Metrics

### Functional Success

- [x] User can navigate to ipfs:// URLs
- [x] Content loads from local daemon or public gateway
- [x] Content integrity verified via CID hash
- [x] Per-tab cryptographic identities generated
- [x] IPFS works with Tor enabled (privacy preservation)

### Performance Success

- [x] Local daemon: <500ms page load time
- [x] Public gateway: <3s page load time
- [x] No browser hang during IPFS requests

### Security Success

- [x] No private key leaks across tabs
- [x] Content verification prevents malicious gateways
- [x] IPFS requests route through Tor when enabled
- [x] No crashes from malformed IPFS URLs

---

## Future Enhancements (Post-Phase 2)

### Phase 3: Advanced IPFS Features

1. **IPNS Support**: Mutable content via IPNS names (ipns://)
2. **Content Pinning**: Pin important content to local IPFS node
3. **DHT Participation**: Run full IPFS node in-browser (advanced)
4. **Pubsub Messaging**: Real-time communication via IPFS pubsub

### Phase 4: Other P2P Protocols

1. **Dat/Hypercore**: If demand emerges
2. **WebRTC Data Channels**: Browser-to-browser P2P
3. **Blockchain Integration**: Ethereum Name Service (ENS) resolution

---

## Conclusion

**Recommendation**: PROCEED with IPFS integration via gateway approach.

**Rationale**:
- ✅ Low implementation complexity (similar to Tor integration)
- ✅ Reuses existing infrastructure (ProxyConfig, NetworkIdentity, libcurl)
- ✅ No new dependencies required
- ✅ Clear security model (content-addressed + hash verification)
- ✅ Incremental delivery (MVP in 7 days, full feature in 13 days)

**Next Step**: Begin Milestone 2.1 - Cryptographic Identity Activation

---

## Appendix: IPFS Resources

**Documentation**:
- IPFS Whitepaper: https://ipfs.io/ipfs/QmR7GSQM93Cx5eAg6a6yRzNde1FQv7uL6X1o4k7zrJa3LX/ipfs.draft3.pdf
- IPFS HTTP Gateway Spec: https://docs.ipfs.tech/reference/http/gateway/
- CID Specification: https://github.com/multiformats/cid

**Example Implementations**:
- Brave Browser: IPFS support via gateway + embedded Kubo node
- Opera Browser: IPFS support via gateway only
- IPFS Desktop: Full IPFS node with GUI

**Public Gateways**:
- https://ipfs.io/ipfs/CID
- https://dweb.link/ipfs/CID
- https://cloudflare-ipfs.com/ipfs/CID

**Local Daemon Setup**:
```bash
# Install IPFS (Kubo implementation)
wget https://dist.ipfs.tech/kubo/v0.22.0/kubo_v0.22.0_linux-amd64.tar.gz
tar -xvzf kubo_v0.22.0_linux-amd64.tar.gz
cd kubo
sudo bash install.sh

# Initialize and start daemon
ipfs init
ipfs daemon

# Gateway available at: http://localhost:8080
# API available at: http://localhost:5001
```
