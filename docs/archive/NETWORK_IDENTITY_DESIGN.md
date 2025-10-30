# Network Identity Design - Per-Tab Network Isolation

## Overview

This document describes the design of the NetworkIdentity and ProxyConfig classes for implementing per-tab network isolation in Ladybird browser, supporting Tor integration and future P2P protocols.

## Architecture

### Component Hierarchy

```
NetworkIdentity (LibIPC/NetworkIdentity.h)
    ├─ ProxyConfig (LibIPC/ProxyConfig.h)
    ├─ NetworkAuditEntry (audit trail)
    └─ Cryptographic identity (future P2P support)

ConnectionFromClient (RequestServer)
    └─ NetworkIdentity (per page/tab)
            └─ ProxyConfig (Tor/VPN routing)
```

### Multi-Process Integration

```
Browser UI Process
    └─> WebContent Process (per tab)
            ├─ Page ID: unique identifier
            └─> RequestServer Process
                    └─ ConnectionFromClient
                            └─ NetworkIdentity
                                    └─ ProxyConfig (Tor circuit)
```

## Class Designs

### ProxyConfig

**Purpose**: Configuration for network proxy routing (Tor, VPN, custom SOCKS5/HTTP proxies)

**File**: `Libraries/LibIPC/ProxyConfig.h`

**Key Features**:
- Enumeration of proxy types (None, SOCKS5, SOCKS5H, HTTP, HTTPS)
- libcurl-compatible URL and authentication string generation
- Tor proxy factory method with stream isolation support
- Equality comparison for configuration changes

**Usage Example**:
```cpp
// Create Tor proxy with stream isolation
auto tor_proxy = ProxyConfig::tor_proxy("circuit-page-123");

// Apply to libcurl request
set_option(CURLOPT_PROXY, tor_proxy.to_curl_proxy_url().characters());
set_option(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);

// Add SOCKS5 authentication for stream isolation
if (auto auth = tor_proxy.to_curl_auth_string(); auth.has_value())
    set_option(CURLOPT_PROXYUSERPWD, auth->characters());
```

**Integration with libcurl**:

ProxyConfig generates libcurl-compatible configuration:

| ProxyType | libcurl URL | CURLOPT_PROXYTYPE | DNS Resolution |
|-----------|-------------|-------------------|----------------|
| SOCKS5 | `socks5://host:port` | CURLPROXY_SOCKS5 | Local |
| SOCKS5H | `socks5h://host:port` | CURLPROXY_SOCKS5_HOSTNAME | Via proxy (leak-proof) |
| HTTP | `http://host:port` | CURLPROXY_HTTP | Local |
| HTTPS | `https://host:port` | CURLPROXY_HTTPS | Local |

**Tor Stream Isolation**:

Tor provides stream isolation via SOCKS5 authentication:
- Each unique username gets a separate Tor circuit
- ProxyConfig uses circuit ID as SOCKS5 username
- Different tabs automatically get different Tor circuits

```cpp
// Tab 1: circuit-page-1 → Tor circuit A
auto proxy1 = ProxyConfig::tor_proxy("circuit-page-1");

// Tab 2: circuit-page-2 → Tor circuit B
auto proxy2 = ProxyConfig::tor_proxy("circuit-page-2");

// Tab 1 and Tab 2 now use completely isolated Tor circuits
```

---

### NetworkIdentity

**Purpose**: Per-tab network identity with cryptographic identity, proxy configuration, and audit trail

**Files**:
- Header: `Libraries/LibIPC/NetworkIdentity.h`
- Implementation: `Libraries/LibIPC/NetworkIdentity.cpp`

**Key Features**:
- Per-page/tab identity with unique ID generation
- Proxy configuration management (Tor, VPN, custom)
- Tor circuit rotation (NEWNYM support)
- Audit trail of all network activity
- Statistics tracking (bytes sent/received, request count)
- Cryptographic identity (placeholder for future P2P protocols)
- Memory security (zero out private keys on destruction)

**Factory Methods**:

```cpp
// Create basic network identity
auto identity = TRY(NetworkIdentity::create_for_page(page_id));

// Create identity with Tor circuit
auto identity = TRY(NetworkIdentity::create_with_tor(page_id, "circuit-123"));

// Create identity with custom proxy
ProxyConfig custom_proxy;
custom_proxy.type = ProxyType::SOCKS5H;
custom_proxy.host = "custom-proxy.local";
custom_proxy.port = 1080;
auto identity = TRY(NetworkIdentity::create_with_proxy(page_id, custom_proxy));
```

**Audit Trail**:

Every network request is logged with:
- Timestamp (MonotonicTime for precision)
- URL (full URL including query parameters)
- HTTP method (GET, POST, etc.)
- Response code (when response received)
- Bytes sent/received

```cpp
// Log outgoing request
identity->log_request(url, "GET"sv);

// Later, log response
identity->log_response(url, 200, bytes_sent, bytes_received);

// Query statistics
dbgln("Total requests: {}", identity->total_requests());
dbgln("Total data sent: {} bytes", identity->total_bytes_sent());
dbgln("Total data received: {} bytes", identity->total_bytes_received());
```

**Circuit Rotation**:

Tor circuit rotation (request new identity):

```cpp
// User clicks "New Identity" button
TRY(identity->rotate_tor_circuit());

// Generates new circuit ID and updates proxy config
// Subsequent requests use new Tor circuit
```

**Lifecycle Management**:

```cpp
// Created when tab is created
auto identity = TRY(NetworkIdentity::create_for_page(page_id));

// Configured by user action (enable Tor)
identity->set_proxy_config(ProxyConfig::tor_proxy(identity->identity_id()));

// Used for every network request in that tab
if (identity->has_proxy()) {
    apply_proxy_to_curl_request(identity->proxy_config());
}

// Destroyed when tab is closed (automatically clears sensitive data)
// NetworkIdentity destructor calls clear_sensitive_data()
```

---

## Security Considerations

### DNS Leak Prevention

**Problem**: DNS queries reveal browsing activity even if HTTP traffic is proxied

**Solution**: SOCKS5H (hostname resolution via proxy)

```cpp
ProxyType::SOCKS5H  // DNS resolution happens through Tor, not locally
```

**Implementation**:
- ProxyConfig defaults to SOCKS5H for Tor
- libcurl CURLPROXY_SOCKS5_HOSTNAME sends hostname to Tor
- Tor resolves hostname and proxies connection
- No local DNS queries = no DNS leaks

### Stream Isolation

**Problem**: Multiple tabs using same Tor circuit can be correlated

**Solution**: SOCKS5 authentication for circuit separation

```cpp
// Each tab gets unique circuit ID
tor_proxy.username = "page-123-abc456";  // Unique per tab

// Tor sees different SOCKS5 username → allocates different circuit
```

**Verification**:
```bash
# Monitor Tor circuits
watch -n1 'sudo -u debian-tor tor-control GETINFO circuit-status'

# Should see multiple circuits for different SOCKS5 usernames
```

### Memory Security

**Problem**: Private keys should not remain in memory after use

**Solution**: Explicit zeroing of sensitive data

```cpp
void NetworkIdentity::clear_sensitive_data()
{
    if (m_private_key.has_value()) {
        // Zero out private key memory before deallocation
        explicit_bzero(const_cast<char*>(m_private_key->characters()), m_private_key->length());
        m_private_key = {};
    }
    m_proxy_config = {};
    m_tor_circuit_id = {};
}
```

Called automatically in destructor, ensuring keys don't leak through:
- Memory dumps
- Swap files
- Heap analysis

### Audit Trail Limitations

**Problem**: Unbounded audit log consumes memory

**Solution**: Fixed-size circular buffer

```cpp
static constexpr size_t MaxAuditEntries = 1000;

// When limit reached, remove oldest entry
if (m_audit_log.size() >= MaxAuditEntries) {
    m_audit_log.remove(0);  // Remove oldest
}
```

**Trade-off**:
- Pro: Bounded memory usage (1000 entries ≈ 100KB)
- Con: Oldest entries lost after 1000 requests
- Acceptable for per-tab audit (most tabs don't exceed 1000 requests)

---

## Integration Points

### ConnectionFromClient (RequestServer)

**File**: `Services/RequestServer/ConnectionFromClient.h`

**Integration Steps**:

1. Add NetworkIdentity member:
```cpp
class ConnectionFromClient final : public IPC::ConnectionFromClient<...> {
    // ...
private:
    RefPtr<IPC::NetworkIdentity> m_network_identity;
};
```

2. Initialize in constructor:
```cpp
ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<...>(move(transport))
    , m_network_identity(MUST(IPC::NetworkIdentity::create_for_page(/* page_id */)))
{
}
```

3. Apply proxy in `issue_network_request()`:
```cpp
void ConnectionFromClient::issue_network_request(...)
{
    // ... existing curl setup ...

    // Apply proxy configuration if present
    if (m_network_identity && m_network_identity->has_proxy()) {
        auto const& proxy = m_network_identity->proxy_config().value();
        set_option(CURLOPT_PROXY, proxy.to_curl_proxy_url().characters());

        if (proxy.type == IPC::ProxyType::SOCKS5H)
            set_option(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
        else if (proxy.type == IPC::ProxyType::SOCKS5)
            set_option(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);

        if (auto auth = proxy.to_curl_auth_string(); auth.has_value())
            set_option(CURLOPT_PROXYUSERPWD, auth->characters());
    }

    // Log request in audit trail
    if (m_network_identity)
        m_network_identity->log_request(url, method);

    // ... existing curl_multi_add_handle ...
}
```

4. Log responses:
```cpp
size_t ConnectionFromClient::on_data_received(void* buffer, size_t size, size_t nmemb, void* user_data)
{
    auto* request = static_cast<ActiveRequest*>(user_data);

    // ... existing response handling ...

    // Log response in audit trail
    if (request->client->m_network_identity) {
        long response_code = 0;
        curl_easy_getinfo(request->curl_handle, CURLINFO_RESPONSE_CODE, &response_code);

        request->client->m_network_identity->log_response(
            request->url,
            static_cast<u16>(response_code),
            request->bytes_sent,
            request->bytes_received
        );
    }

    return realsize;
}
```

---

### WebContent Process

**File**: `Services/WebContent/ConnectionFromClient.h`

**Integration for UI Controls**:

1. Add IPC message for Tor toggle:
```cpp
// In WebContentServer.ipc
Messages::WebContentServer::SetNetworkIdentityProxyResponse set_network_identity_proxy(u64 page_id, IPC::ProxyConfig proxy_config) =|
Messages::WebContentServer::ClearNetworkIdentityProxyResponse clear_network_identity_proxy(u64 page_id) =|
Messages::WebContentServer::RotateTorCircuitResponse rotate_tor_circuit(u64 page_id) =|
```

2. Forward to RequestServer:
```cpp
void WebContentClient::set_network_identity_proxy(u64 page_id, IPC::ProxyConfig proxy_config)
{
    // Forward to RequestServer for this page
    auto* request_server = get_request_server_for_page(page_id);
    request_server->async_set_proxy(proxy_config);
}
```

---

### Browser UI (Qt/AppKit)

**UI Elements**:

1. Per-tab Tor toggle button (toolbar or tab context menu)
2. "New Identity" button (rotate Tor circuit)
3. Network activity indicator (shows when Tor is active)
4. Audit log viewer (developer tools panel)

**Implementation** (Qt example):
```cpp
// In Tab class
void Tab::on_tor_toggle_clicked()
{
    if (m_tor_enabled) {
        // Disable Tor
        m_web_content_client->async_clear_network_identity_proxy(page_id());
        m_tor_enabled = false;
    } else {
        // Enable Tor
        auto tor_proxy = IPC::ProxyConfig::tor_proxy();
        m_web_content_client->async_set_network_identity_proxy(page_id(), tor_proxy);
        m_tor_enabled = true;
    }
}

void Tab::on_new_identity_clicked()
{
    m_web_content_client->async_rotate_tor_circuit(page_id());
}
```

---

## Future Enhancements

### Phase 2: P2P Protocol Support

**IPFS Integration**:

NetworkIdentity already has cryptographic identity placeholders:

```cpp
Optional<ByteString> m_public_key;
Optional<ByteString> m_private_key;
```

**Implementation path**:
1. Replace placeholder key generation with real Ed25519/secp256k1 keys
2. Add IPFS CID resolution to RequestServer
3. Use NetworkIdentity keys for IPFS peer identity
4. Implement DHT-based content retrieval

**Example**:
```cpp
// User navigates to ipfs://Qm...
auto identity = m_network_identity;
auto ipfs_client = IPFSClient::create(identity->public_key(), identity->private_key());
auto content = TRY(ipfs_client.fetch_content(ipfs_cid));
```

### Phase 3: Multi-VPN Support

**VPN interface routing**:

```cpp
// Route tab traffic through specific VPN interface
identity->set_vpn_interface("wg0");  // WireGuard interface

// Apply to requests
set_option(CURLOPT_INTERFACE, identity->vpn_interface()->characters());
```

### Phase 4: Tor Process Management

**Automatic Tor spawning**:

```cpp
class TorProcessManager {
    static ErrorOr<NonnullOwnPtr<TorProcessManager>> create();

    ErrorOr<void> start_tor_daemon();
    ErrorOr<void> send_newnym_signal();
    ErrorOr<Vector<TorCircuit>> get_active_circuits();
};
```

**Integration**:
- Browser spawns single Tor process on startup
- Configure with stream isolation: `SOCKSPort 9050 IsolateClientAddr IsolateClientProtocol`
- NetworkIdentity uses SOCKS5 authentication for circuit separation
- "New Identity" button sends NEWNYM via control port

---

## Testing Strategy

### Unit Tests

**File**: `Tests/LibIPC/TestNetworkIdentity.cpp`

```cpp
TEST_CASE(proxy_config_tor_factory)
{
    auto proxy = IPC::ProxyConfig::tor_proxy("test-circuit");
    EXPECT_EQ(proxy.type, IPC::ProxyType::SOCKS5H);
    EXPECT_EQ(proxy.host, "localhost");
    EXPECT_EQ(proxy.port, 9050);
    EXPECT_EQ(proxy.username.value(), "test-circuit");
    EXPECT_EQ(proxy.to_curl_proxy_url(), "socks5h://localhost:9050");
}

TEST_CASE(network_identity_creation)
{
    auto identity = MUST(IPC::NetworkIdentity::create_for_page(123));
    EXPECT_EQ(identity->page_id(), 123);
    EXPECT(!identity->has_proxy());
    EXPECT(!identity->has_tor_circuit());
}

TEST_CASE(network_identity_tor_circuit)
{
    auto identity = MUST(IPC::NetworkIdentity::create_with_tor(123));
    EXPECT(identity->has_proxy());
    EXPECT(identity->has_tor_circuit());
    EXPECT(identity->tor_circuit_id().has_value());
}

TEST_CASE(audit_log_bounded_size)
{
    auto identity = MUST(IPC::NetworkIdentity::create_for_page(123));

    // Add 1500 requests (exceeds MaxAuditEntries = 1000)
    for (size_t i = 0; i < 1500; i++) {
        identity->log_request(URL::URL("http://example.com"), "GET");
    }

    // Should only keep last 1000 entries
    EXPECT_EQ(identity->total_requests(), 1000);
}
```

### Integration Tests

**Manual Testing with Tor**:

```bash
# Start Tor locally
sudo systemctl start tor

# Verify Tor is listening on SOCKS5 port
netstat -tulpn | grep 9050

# Run Ladybird with Tor-enabled tab
./Build/release/bin/Ladybird

# Enable Tor for tab (via UI toggle)
# Navigate to https://check.torproject.org
# Should show "Congratulations. This browser is configured to use Tor."

# Test stream isolation
# Open multiple tabs with Tor enabled
# Each tab should show different Tor exit IP
```

**Automated Testing**:

```cpp
// Tests/LibWeb/TestTorIntegration.cpp
TEST_CASE(tor_proxy_isolation)
{
    // Requires Tor running on localhost:9050

    auto identity1 = MUST(IPC::NetworkIdentity::create_with_tor(1, "circuit-1"));
    auto identity2 = MUST(IPC::NetworkIdentity::create_with_tor(2, "circuit-2"));

    // Make requests through different circuits
    auto ip1 = fetch_current_ip(identity1);
    auto ip2 = fetch_current_ip(identity2);

    // Different circuits should (usually) have different exit IPs
    // Note: Not guaranteed, but statistically very likely
    dbgln("Circuit 1 exit IP: {}", ip1);
    dbgln("Circuit 2 exit IP: {}", ip2);
}
```

---

## Implementation Timeline

**Current Status**: Todo 2 (Design) - COMPLETE

**Next Steps**:
1.  Create ProxyConfig.h - DONE
2.  Create NetworkIdentity.h/cpp - DONE
3.  Create design documentation - DONE
4. Update CMakeLists.txt to compile new files
5. Write unit tests
6. Integrate with ConnectionFromClient (Todo 3)
7. Add IPC messages for proxy configuration (Todo 4)
8. Implement UI controls (Todo 5)
9. Test with local Tor instance

**Estimated Completion**: 1-2 weeks for full integration

---

## References

- Tor Stream Isolation: https://www.whonix.org/wiki/Stream_Isolation
- libcurl SOCKS proxy: https://everything.curl.dev/usingcurl/proxies/socks
- SOCKS5 RFC: https://www.rfc-editor.org/rfc/rfc1928
- Tor Control Protocol: https://spec.torproject.org/control-spec/
- Research document: `claudedocs/TOR_INTEGRATION_RESEARCH.md`
