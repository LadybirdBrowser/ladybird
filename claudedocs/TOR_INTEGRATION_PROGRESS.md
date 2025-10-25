# Tor Integration - Implementation Progress

## Overview

Implementation of per-tab Tor/VPN network isolation for Ladybird browser, building on existing IPC security framework.

## Completed Work

### ✅ Phase 1: Research (Todo 1)

**Created**: `claudedocs/TOR_INTEGRATION_RESEARCH.md`

**Key Findings**:
- Recommended approach: SOCKS5 proxy via libcurl
- Ladybird already uses libcurl for all HTTP requests
- NO existing proxy configuration - perfect insertion point
- Each tab already has its own RequestServer process (natural isolation boundary)
- libcurl has excellent SOCKS5 support with stream isolation

**Options Analyzed**:
1. ✅ SOCKS5 proxy via libcurl (RECOMMENDED) - Low complexity, high value
2. Arti Rust library - Medium complexity, more control
3. Tor Browser C++ components - Very high complexity, not recommended

**Timeline**: 1-2 days for SOCKS5 implementation, 1-2 weeks for full feature

---

### ✅ Phase 2: Design (Todo 2)

**Created Files**:
1. `Libraries/LibIPC/ProxyConfig.h` - Proxy configuration class
2. `Libraries/LibIPC/NetworkIdentity.h` - Network identity interface
3. `Libraries/LibIPC/NetworkIdentity.cpp` - Network identity implementation
4. `claudedocs/NETWORK_IDENTITY_DESIGN.md` - Comprehensive design documentation

**ProxyConfig Features**:
- Proxy type enumeration (None, SOCKS5, SOCKS5H, HTTP, HTTPS)
- libcurl-compatible URL generation (`to_curl_proxy_url()`)
- libcurl-compatible authentication generation (`to_curl_auth_string()`)
- Tor proxy factory method with stream isolation (`ProxyConfig::tor_proxy()`)
- DNS leak prevention (SOCKS5H - hostname resolution via proxy)

**Example Usage**:
```cpp
// Create Tor proxy with stream isolation
auto tor_proxy = ProxyConfig::tor_proxy("circuit-page-123");

// Apply to libcurl request
set_option(CURLOPT_PROXY, tor_proxy.to_curl_proxy_url().characters());
set_option(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);  // socks5h://

// Add SOCKS5 authentication for circuit separation
if (auto auth = tor_proxy.to_curl_auth_string(); auth.has_value())
    set_option(CURLOPT_PROXYUSERPWD, auth->characters());
```

**NetworkIdentity Features**:
- Per-page/tab identity with unique ID generation
- Proxy configuration management (Tor, VPN, custom)
- Tor circuit rotation support (`rotate_tor_circuit()`)
- Audit trail of all network activity (bounded to 1000 entries)
- Statistics tracking (bytes sent/received, request count)
- Cryptographic identity placeholders (for future P2P protocols)
- Memory security (zero out private keys on destruction)

**Factory Methods**:
```cpp
// Create basic network identity
auto identity = TRY(NetworkIdentity::create_for_page(page_id));

// Create identity with Tor circuit
auto identity = TRY(NetworkIdentity::create_with_tor(page_id, "circuit-123"));

// Create identity with custom proxy
auto identity = TRY(NetworkIdentity::create_with_proxy(page_id, custom_proxy));
```

**Security Features**:
- Stream isolation via SOCKS5 authentication (each tab gets unique Tor circuit)
- DNS leak prevention via SOCKS5H (hostname resolution through Tor)
- Memory security (explicit zeroing of private keys with `explicit_bzero()`)
- Bounded audit log (prevents unbounded memory growth)

**Build Integration**:
- Updated `Libraries/LibIPC/CMakeLists.txt` to compile NetworkIdentity.cpp

---

### ✅ Phase 3: ConnectionFromClient Integration (Todo 3)

**Modified Files**:
1. `Services/RequestServer/ConnectionFromClient.h` - Added NetworkIdentity support
2. `Services/RequestServer/ConnectionFromClient.cpp` - Implemented network identity methods

**Header Changes** (`ConnectionFromClient.h:14`):
```cpp
#include <LibIPC/NetworkIdentity.h>  // Added import
```

**Header Changes** (`ConnectionFromClient.h:42-46`):
```cpp
// Network identity management
[[nodiscard]] RefPtr<IPC::NetworkIdentity> network_identity() const { return m_network_identity; }
void set_network_identity(RefPtr<IPC::NetworkIdentity> identity) { m_network_identity = move(identity); }
void enable_tor(ByteString circuit_id = {});
void disable_tor();
void rotate_tor_circuit();
```

**Header Changes** (`ConnectionFromClient.h:92`):
```cpp
// Network identity for per-tab routing and audit
RefPtr<IPC::NetworkIdentity> m_network_identity;
```

**Implementation** (`ConnectionFromClient.cpp:434-481`):

**`enable_tor()`**:
- Creates NetworkIdentity if not present
- Generates unique circuit ID (uses identity_id if not provided)
- Configures Tor proxy via ProxyConfig
- Logs Tor activation

**`disable_tor()`**:
- Clears proxy configuration
- Logs Tor deactivation

**`rotate_tor_circuit()`**:
- Validates network identity exists
- Validates Tor is enabled
- Calls NetworkIdentity::rotate_tor_circuit()
- Logs circuit rotation

**Usage Example**:
```cpp
// Enable Tor for this RequestServer client
connection->enable_tor();  // Auto-generates circuit ID

// Or with custom circuit ID
connection->enable_tor("circuit-page-123");

// User clicks "New Identity" button
connection->rotate_tor_circuit();

// Disable Tor
connection->disable_tor();
```

---

## Remaining Work

### 🔄 Phase 4: Per-Tab RequestServer Spawning (Todo 4 - IN PROGRESS)

**Current Status**: RequestServer already spawns per WebContent process (per tab)

**Investigation Needed**:
- Verify current RequestServer spawning behavior
- Determine if page_id is available at RequestServer creation
- Decide how to pass page_id from WebContent to RequestServer

**Possible Approaches**:

**Option A: Pass page_id via IPC during initialization**
```cpp
// In WebContent process
request_server->async_init_transport(peer_pid, page_id);  // Add page_id parameter

// In RequestServer
Messages::RequestServer::InitTransportResponse ConnectionFromClient::init_transport(int peer_pid, u64 page_id)
{
    // Create network identity with real page_id
    m_network_identity = MUST(IPC::NetworkIdentity::create_for_page(page_id));
    // ...
}
```

**Option B: Use client_id as page_id** (current temporary approach)
```cpp
// Already implemented in enable_tor():
m_network_identity = MUST(IPC::NetworkIdentity::create_for_page(client_id()));
```

**Required Changes**:
1. Add page_id parameter to RequestServer initialization IPC messages
2. Update RequestServer constructor to accept page_id
3. Create NetworkIdentity in constructor with proper page_id
4. Remove temporary client_id workaround

---

### ⏳ Phase 5: IPC Message Validation (Todo 5 - PENDING)

**Goal**: Add network identity validation to IPC security framework

**Required Changes**:

**1. Add validation helper in ConnectionFromClient.h**:
```cpp
[[nodiscard]] bool validate_network_identity(SourceLocation location = SourceLocation::current())
{
    if (!m_network_identity) {
        dbgln("Security: RequestServer has no network identity at {}:{}",
            location.filename(), location.line_number());
        track_validation_failure();
        return false;
    }
    return true;
}
```

**2. Add network identity checks to request handling**:
```cpp
void ConnectionFromClient::start_request(...)
{
    // Existing validations...
    if (!check_rate_limit())
        return;
    if (!validate_url(url))
        return;

    // NEW: Validate network identity exists
    if (!validate_network_identity())
        return;

    // Log request in audit trail
    if (m_network_identity)
        m_network_identity->log_request(url, method);

    // Continue with request...
}
```

**3. Add audit logging on response**:
```cpp
size_t ConnectionFromClient::on_data_received(...)
{
    // Existing response handling...

    // NEW: Log response in audit trail
    if (request->client->m_network_identity) {
        long response_code = 0;
        curl_easy_getinfo(request->curl_handle, CURLINFO_RESPONSE_CODE, &response_code);

        request->client->m_network_identity->log_response(
            request->url,
            static_cast<u16>(response_code),
            bytes_sent,
            bytes_received
        );
    }

    return realsize;
}
```

---

## Next Steps

### Immediate (Next Session):

1. **Todo 4: Verify RequestServer spawning**
   - Check how RequestServer is currently spawned
   - Determine if page_id is available at spawn time
   - Decide on page_id passing mechanism

2. **Todo 5: Add IPC validation**
   - Add network identity validation helper
   - Integrate audit logging into request/response cycle
   - Test validation with malformed requests

3. **Apply Proxy to Requests** (Critical for functionality):
   - Modify `issue_network_request()` to apply proxy configuration
   - Add CURLOPT_PROXY configuration when NetworkIdentity has proxy
   - Test with local Tor instance

### Example Integration in `issue_network_request()`:

**Current Code** (`ConnectionFromClient.cpp:689-765`):
```cpp
void ConnectionFromClient::issue_network_request(...)
{
    // ... existing curl setup ...

    set_option(CURLOPT_URL, url.to_string().to_byte_string().characters());
    set_option(CURLOPT_PORT, url.port_or_default());
    set_option(CURLOPT_CUSTOMREQUEST, method.characters());

    // ... headers, body, callbacks ...
}
```

**With Proxy Support** (TO BE IMPLEMENTED):
```cpp
void ConnectionFromClient::issue_network_request(...)
{
    // ... existing curl setup ...

    set_option(CURLOPT_URL, url.to_string().to_byte_string().characters());
    set_option(CURLOPT_PORT, url.port_or_default());

    // NEW: Apply proxy configuration if present
    if (m_network_identity && m_network_identity->has_proxy()) {
        auto const& proxy = m_network_identity->proxy_config().value();

        // Set proxy URL
        set_option(CURLOPT_PROXY, proxy.to_curl_proxy_url().characters());

        // Set proxy type
        if (proxy.type == IPC::ProxyType::SOCKS5H)
            set_option(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
        else if (proxy.type == IPC::ProxyType::SOCKS5)
            set_option(CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
        else if (proxy.type == IPC::ProxyType::HTTP)
            set_option(CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
        else if (proxy.type == IPC::ProxyType::HTTPS)
            set_option(CURLOPT_PROXYTYPE, CURLPROXY_HTTPS);

        // Set SOCKS5 authentication for stream isolation
        if (auto auth = proxy.to_curl_auth_string(); auth.has_value())
            set_option(CURLOPT_PROXYUSERPWD, auth->characters());

        dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Using proxy {} for request to {}",
            proxy.to_curl_proxy_url(), url);
    }

    set_option(CURLOPT_CUSTOMREQUEST, method.characters());

    // ... rest of setup ...

    // NEW: Log request in audit trail
    if (m_network_identity)
        m_network_identity->log_request(url, method);

    // ... continue with curl_multi_add_handle ...
}
```

### Testing Plan:

**Unit Tests** (`Tests/LibIPC/TestNetworkIdentity.cpp` - TO BE CREATED):
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

TEST_CASE(network_identity_audit_log)
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

**Integration Testing**:
1. Start Tor locally: `sudo systemctl start tor`
2. Build Ladybird: `./Meta/ladybird.py build`
3. Run Ladybird: `./Meta/ladybird.py run`
4. Enable Tor for tab (via debug console or test harness)
5. Navigate to https://check.torproject.org
6. Verify "Congratulations. This browser is configured to use Tor."

---

## Architecture Summary

```
Browser UI Process
    └─> WebContent Process (per tab, page_id = 123)
            └─> RequestServer Process
                    └─> ConnectionFromClient
                            └─> NetworkIdentity (page_id = 123)
                                    └─> ProxyConfig (Tor circuit: "page-123-abc456")
                                            └─> libcurl HTTP request
                                                    └─> Tor SOCKS5 proxy (localhost:9050)
                                                            └─> Tor network
```

**Stream Isolation Flow**:
1. Tab 1 creates NetworkIdentity with ID "page-1-abc123"
2. Tab 1 enables Tor → ProxyConfig with username="page-1-abc123"
3. libcurl connects to Tor SOCKS5 with username "page-1-abc123"
4. Tor sees unique username → allocates Circuit A
5. Tab 2 creates NetworkIdentity with ID "page-2-def456"
6. Tab 2 enables Tor → ProxyConfig with username="page-2-def456"
7. libcurl connects to Tor SOCKS5 with username "page-2-def456"
8. Tor sees different username → allocates Circuit B
9. Tab 1 and Tab 2 now use completely isolated Tor circuits

---

## Files Created/Modified

### Created:
- `Libraries/LibIPC/ProxyConfig.h` - Proxy configuration (248 lines)
- `Libraries/LibIPC/NetworkIdentity.h` - Network identity interface (159 lines)
- `Libraries/LibIPC/NetworkIdentity.cpp` - Network identity implementation (154 lines)
- `claudedocs/TOR_INTEGRATION_RESEARCH.md` - Research document (299 lines)
- `claudedocs/NETWORK_IDENTITY_DESIGN.md` - Design document (530 lines)
- `claudedocs/TOR_INTEGRATION_PROGRESS.md` - This file

### Modified:
- `Libraries/LibIPC/CMakeLists.txt` - Added NetworkIdentity.cpp compilation
- `Services/RequestServer/ConnectionFromClient.h` - Added NetworkIdentity member and methods
- `Services/RequestServer/ConnectionFromClient.cpp` - Implemented enable_tor(), disable_tor(), rotate_tor_circuit()

---

## Timeline

- **Week 1, Day 1-2**: Research and design ✅ COMPLETE
- **Week 1, Day 3**: ConnectionFromClient integration ✅ COMPLETE
- **Week 1, Day 4-5**: RequestServer spawning + IPC validation ⏳ IN PROGRESS
- **Week 1, Day 6-7**: Proxy application to requests + testing ⏳ PENDING
- **Week 2**: UI controls, Tor process management, circuit rotation ⏳ PENDING

**Current Status**: 60% complete (3/5 todos done)

---

## Success Criteria

- [x] Design ProxyConfig class with libcurl integration
- [x] Design NetworkIdentity class with Tor support
- [x] Integrate NetworkIdentity into ConnectionFromClient
- [ ] Apply proxy configuration to HTTP requests via libcurl
- [ ] Verify per-tab Tor circuit isolation
- [ ] Test with local Tor instance (https://check.torproject.org)
- [ ] Implement "New Identity" button (circuit rotation)
- [ ] Add UI controls for per-tab Tor toggle

**Next Milestone**: Working Tor proxy for HTTP requests (requires implementing libcurl proxy application)
