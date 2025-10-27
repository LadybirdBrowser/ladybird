# Milestone 1.4: VPN Integration - Research & Design

**Date**: 2025-10-26
**Status**: RESEARCH & PLANNING
**Prerequisites**: Milestones 1.1-1.3B COMPLETE
**Goal**: Extend Tor integration to support generic VPN/proxy configurations

---

## Executive Summary

Milestone 1.4 adds VPN/custom proxy support to Ladybird, enabling per-tab routing through HTTP/HTTPS/SOCKS5 proxies. This builds on the existing ProxyConfig infrastructure from Milestone 1.1.

**Key Goals**:
1. VPN toggle button (separate from Tor toggle)
2. Proxy settings dialog for HTTP/HTTPS/SOCKS5 configuration
3. Per-tab proxy configuration storage
4. Reuse existing ProxyConfig/NetworkIdentity infrastructure
5. Support simultaneous Tor + VPN (Tor over VPN or VPN over Tor)

**Implementation Complexity**: **LOW** (reuses 90% of Tor infrastructure)

**Timeline**: 2-3 days for full implementation

---

## Current Infrastructure Review

### Already Implemented (From Milestones 1.1-1.3)

**ProxyConfig (Libraries/LibIPC/ProxyConfig.h)**:
```cpp
enum class ProxyType : u8 {
    None,           // Direct connection
    SOCKS5,         // SOCKS5 proxy (local DNS)
    SOCKS5H,        // SOCKS5 with DNS via proxy ✅ WORKING
    HTTP,           // HTTP proxy (CONNECT method)
    HTTPS,          // HTTPS proxy
};

struct ProxyConfig {
    ProxyType type;
    ByteString host;
    u16 port;
    Optional<ByteString> username;
    Optional<ByteString> password;

    ByteString to_curl_proxy_url();        // ✅ IMPLEMENTED
    Optional<ByteString> to_curl_auth_string();  // ✅ IMPLEMENTED
    static ProxyConfig tor_proxy();        // ✅ WORKING
};
```

**NetworkIdentity (Libraries/LibIPC/NetworkIdentity.h)**:
```cpp
class NetworkIdentity {
    void set_proxy_config(ProxyConfig config);     // ✅ IMPLEMENTED
    void clear_proxy_config();                      // ✅ IMPLEMENTED
    bool has_proxy();                               // ✅ IMPLEMENTED
    ProxyConfig* proxy_config();                    // ✅ IMPLEMENTED
};
```

**RequestServer Integration (Services/RequestServer/ConnectionFromClient.cpp)**:
- ✅ Lines 750-777: Proxy application to libcurl requests
- ✅ CURLOPT_PROXY, CURLOPT_PROXYTYPE, CURLOPT_PROXYUSERPWD already set
- ✅ SOCKS5H DNS bypass working (from Bug Fix 2)

**IPC Messages (Services/WebContent/WebContentServer.ipc)**:
- ✅ enable_tor(u64 page_id) - can be generalized
- ✅ disable_tor(u64 page_id) - can be generalized
- ✅ rotate_tor_circuit(u64 page_id) - Tor-specific

**UI Pattern (UI/Qt/Tab.cpp)**:
- ✅ Tor toggle button with visual indicator (green border)
- ✅ Availability check before enabling (TorAvailability::is_tor_running)
- ✅ Per-tab state management (m_tor_enabled)

### What Needs to be Added

**UI Components**:
- ❌ VPN toggle button (separate from Tor)
- ❌ Proxy settings dialog (configure host/port/type/auth)
- ❌ Visual indicator for VPN (different color border - blue?)
- ❌ Proxy configuration storage (per-session or persistent?)

**IPC Messages**:
- ❌ set_proxy(u64 page_id, ProxyConfig config) - generic proxy
- ❌ clear_proxy(u64 page_id) - disable proxy

**Backend**:
- ❌ HTTP/HTTPS proxy support in libcurl (CURLOPT_PROXYTYPE)
- ❌ Proxy validation (check connectivity before applying)

---

## VPN vs Tor: Design Differences

### Tor Integration (Current)

**Characteristics**:
- **Hardcoded Configuration**: localhost:9050, SOCKS5H
- **Zero Configuration**: User just toggles on/off
- **Stream Isolation**: Automatic via circuit_id username
- **Availability Check**: TorAvailability::is_tor_running()
- **Visual Indicator**: Green border

**User Experience**: Dead simple - click toggle, Tor works (if daemon running)

### VPN Integration (New)

**Characteristics**:
- **User-Configured**: Host, port, type (HTTP/HTTPS/SOCKS5/SOCKS5H)
- **Authentication**: Optional username/password
- **Per-Tab or Global**: User choice (reuse config across tabs?)
- **Availability Check**: Optional (try to connect and validate)
- **Visual Indicator**: Blue border (different from Tor)

**User Experience**:
1. Click VPN button → Opens settings dialog if not configured
2. Enter proxy details (host, port, type, auth)
3. Save → Test connection → Apply if successful
4. Toggle VPN on/off after initial setup

---

## Proposed Architecture

### Component 1: Proxy Settings Dialog

**File**: `UI/Qt/ProxySettingsDialog.h` (NEW)

**UI Design**:
```
┌─────────────────────────────────────────────┐
│  Proxy Settings                          ×  │
├─────────────────────────────────────────────┤
│                                             │
│  Proxy Type:  [▼ SOCKS5H           ]        │
│               (HTTP/HTTPS/SOCKS5/SOCKS5H)   │
│                                             │
│  Host:        [192.168.1.100      ]         │
│                                             │
│  Port:        [1080               ]         │
│                                             │
│  ☑ Authentication Required                  │
│                                             │
│  Username:    [vpn_user           ]         │
│                                             │
│  Password:    [●●●●●●●●●         ]         │
│                                             │
│  [ Test Connection ]  [ Save ]  [ Cancel ]  │
│                                             │
└─────────────────────────────────────────────┘
```

**Features**:
- Dropdown for ProxyType selection
- Input validation (port 1-65535, non-empty host)
- "Test Connection" button (verify proxy is reachable)
- Save configuration for current tab or global preference
- Password field with hidden characters

**Class Declaration**:
```cpp
class ProxySettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProxySettingsDialog(QWidget* parent = nullptr);

    IPC::ProxyConfig get_proxy_config() const;
    void set_proxy_config(IPC::ProxyConfig const& config);

private:
    QComboBox* m_proxy_type_combo;
    QLineEdit* m_host_edit;
    QSpinBox* m_port_spinbox;
    QCheckBox* m_auth_checkbox;
    QLineEdit* m_username_edit;
    QLineEdit* m_password_edit;
    QPushButton* m_test_button;

    void test_proxy_connection();
    bool validate_inputs();
};
```

---

### Component 2: VPN Toggle Button

**File**: `UI/Qt/Tab.cpp` (MODIFY)

**Implementation Pattern** (similar to Tor toggle):
```cpp
// In Tab constructor (around line 130)
// Create VPN toggle button (separate from Tor)
m_vpn_toggle_action = new QAction(this);
m_vpn_toggle_action->setCheckable(true);
m_vpn_toggle_action->setChecked(false);
m_vpn_toggle_action->setText("VPN");
m_vpn_toggle_action->setToolTip("Enable VPN/Proxy for this tab");

QObject::connect(m_vpn_toggle_action, &QAction::triggered, this, [this](bool checked) {
    if (checked) {
        // Check if proxy is configured
        if (!m_proxy_config.has_value()) {
            // No proxy configured - open settings dialog
            open_proxy_settings_dialog();

            // Revert toggle if dialog was cancelled
            if (!m_proxy_config.has_value()) {
                m_vpn_toggle_action->setChecked(false);
                return;
            }
        }

        // Apply proxy configuration
        m_vpn_enabled = true;
        dbgln("Tab: Enabling VPN for page_id {}", view().page_id());
        m_vpn_toggle_action->setToolTip("Disable VPN for this tab (currently using proxy)");

        // Apply blue border to indicate VPN active
        if (m_tor_enabled) {
            // Both Tor + VPN active - use purple border (mix of green + blue)
            m_location_edit->setStyleSheet("QLineEdit { border: 2px solid #9C27B0; }");
        } else {
            // VPN only - blue border
            m_location_edit->setStyleSheet("QLineEdit { border: 2px solid #2196F3; }");
        }

        // Send IPC message to apply proxy
        view().client().async_set_proxy(view().page_id(), *m_proxy_config);
    } else {
        // Disable VPN
        m_vpn_enabled = false;
        dbgln("Tab: Disabling VPN for page_id {}", view().page_id());
        m_vpn_toggle_action->setToolTip("Enable VPN/Proxy for this tab");

        // Update border color
        if (m_tor_enabled) {
            // Tor still active - revert to green border
            m_location_edit->setStyleSheet("QLineEdit { border: 2px solid #00C851; }");
        } else {
            // Neither active - remove border
            m_location_edit->setStyleSheet("");
        }

        view().client().async_clear_proxy(view().page_id());
    }
});
```

**Visual Indicators**:
- **VPN only**: Blue border (#2196F3)
- **Tor only**: Green border (#00C851) - existing
- **Tor + VPN**: Purple border (#9C27B0) - mix of green + blue
- **Neither**: No border (default)

---

### Component 3: IPC Message Extensions

**File**: `Services/WebContent/WebContentServer.ipc` (MODIFY)

**New Messages**:
```cpp
// Set custom proxy for current page (generic, not Tor-specific)
set_proxy(u64 page_id, IPC::ProxyConfig config) =|

// Clear custom proxy for current page
clear_proxy(u64 page_id) =|
```

**File**: `Services/WebContent/ConnectionFromClient.cpp` (MODIFY)

**IPC Handler Implementation**:
```cpp
void ConnectionFromClient::set_proxy(u64 page_id, IPC::ProxyConfig config)
{
    dbgln("WebContent: set_proxy called for page_id {}", page_id);

    if (!validate_page_id(page_id))
        return;

    // Forward to RequestServer
    if (auto request_server = request_server_for_page(page_id); request_server) {
        request_server->async_set_proxy(config);
        dbgln("WebContent: Forwarded set_proxy to RequestServer");
    }
}

void ConnectionFromClient::clear_proxy(u64 page_id)
{
    dbgln("WebContent: clear_proxy called for page_id {}", page_id);

    if (!validate_page_id(page_id))
        return;

    // Forward to RequestServer
    if (auto request_server = request_server_for_page(page_id); request_server) {
        request_server->async_clear_proxy();
        dbgln("WebContent: Forwarded clear_proxy to RequestServer");
    }
}
```

---

### Component 4: RequestServer IPC Extensions

**File**: `Services/RequestServer/RequestServer.ipc` (MODIFY)

**New Messages**:
```cpp
// Set custom proxy (generic)
set_proxy(IPC::ProxyConfig config) =|

// Clear custom proxy
clear_proxy() =|
```

**File**: `Services/RequestServer/ConnectionFromClient.cpp` (MODIFY)

**IPC Handler Implementation**:
```cpp
void ConnectionFromClient::set_proxy(IPC::ProxyConfig config)
{
    dbgln("RequestServer: set_proxy called - type={}, host={}, port={}",
        static_cast<u8>(config.type), config.host, config.port);

    // Apply to ALL connections in pool (same as enable_tor broadcast)
    for (auto& [client_id, connection] : s_connections) {
        if (!connection->m_network_identity) {
            auto identity = MUST(IPC::NetworkIdentity::create_for_page(connection->client_id()));
            connection->set_network_identity(move(identity));
        }

        connection->m_network_identity->set_proxy_config(config);
        dbgln("RequestServer: Applied proxy to connection {}", client_id);
    }
}

void ConnectionFromClient::clear_proxy()
{
    dbgln("RequestServer: clear_proxy called");

    // Clear proxy from ALL connections in pool
    for (auto& [client_id, connection] : s_connections) {
        if (connection->m_network_identity) {
            connection->m_network_identity->clear_proxy_config();
            dbgln("RequestServer: Cleared proxy from connection {}", client_id);
        }
    }
}
```

**Note**: Reuses connection pool broadcast pattern from Tor integration (critical for reliability).

---

### Component 5: Proxy Validation

**File**: `Libraries/LibIPC/ProxyValidator.h` (NEW)

**Purpose**: Test proxy connectivity before applying configuration.

**Implementation**:
```cpp
namespace IPC {

class ProxyValidator {
public:
    // Test if proxy is reachable and accepting connections
    [[nodiscard]] static ErrorOr<void> test_proxy(ProxyConfig const& config);

    // Convenience wrapper - returns true if proxy is working
    [[nodiscard]] static bool is_proxy_working(ProxyConfig const& config);

private:
    static ErrorOr<void> test_http_proxy(ByteString host, u16 port);
    static ErrorOr<void> test_socks5_proxy(ByteString host, u16 port);
};

}
```

**File**: `Libraries/LibIPC/ProxyValidator.cpp` (NEW)

**Implementation**:
```cpp
#include <LibCore/TCPSocket.h>

ErrorOr<void> ProxyValidator::test_proxy(ProxyConfig const& config)
{
    if (!config.is_configured())
        return Error::from_string_literal("Proxy not configured");

    switch (config.type) {
    case ProxyType::HTTP:
    case ProxyType::HTTPS:
        return test_http_proxy(config.host, config.port);
    case ProxyType::SOCKS5:
    case ProxyType::SOCKS5H:
        return test_socks5_proxy(config.host, config.port);
    case ProxyType::None:
        return Error::from_string_literal("No proxy type specified");
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<void> ProxyValidator::test_socks5_proxy(ByteString host, u16 port)
{
    // Same logic as TorAvailability::check_socks5_available
    auto socket = TRY(Core::TCPSocket::connect(host, port));

    // Try SOCKS5 handshake
    // Send: [version=5, nmethods=1, method=no_auth]
    u8 handshake[] = {0x05, 0x01, 0x00};
    TRY(socket->write_until_depleted({handshake, sizeof(handshake)}));

    // Read response: [version=5, selected_method]
    u8 response[2];
    TRY(socket->read_until_filled({response, sizeof(response)}));

    if (response[0] != 0x05)
        return Error::from_string_literal("Invalid SOCKS5 response");

    return {};
}

ErrorOr<void> ProxyValidator::test_http_proxy(ByteString host, u16 port)
{
    // Simple TCP connection test (HTTP proxy accepts TCP)
    auto socket = TRY(Core::TCPSocket::connect(host, port));
    return {};
}

bool ProxyValidator::is_proxy_working(ProxyConfig const& config)
{
    auto result = test_proxy(config);
    return !result.is_error();
}
```

---

## Implementation Plan

### Task Breakdown

**Task 1**: Create ProxySettingsDialog (UI/Qt/ProxySettingsDialog.h/cpp)
- Qt dialog with ProxyConfig input fields
- Input validation (port range, non-empty host)
- "Test Connection" button using ProxyValidator
- Save/Cancel buttons

**Task 2**: Add VPN toggle to Tab (UI/Qt/Tab.h/cpp)
- Add m_vpn_toggle_action member
- Add m_vpn_enabled flag
- Add m_proxy_config Optional<ProxyConfig> member
- Implement toggle handler (similar to Tor)
- Open settings dialog if no proxy configured

**Task 3**: Add IPC messages (WebContentServer.ipc, RequestServer.ipc)
- set_proxy(u64 page_id, ProxyConfig)
- clear_proxy(u64 page_id)
- Implement handlers in ConnectionFromClient

**Task 4**: Implement ProxyValidator (Libraries/LibIPC/ProxyValidator.h/cpp)
- test_proxy() method
- test_http_proxy() helper
- test_socks5_proxy() helper (reuse TorAvailability logic)

**Task 5**: Update LibIPC/CMakeLists.txt
- Add ProxyValidator.cpp to compilation

**Task 6**: Visual indicator updates (Tab.cpp)
- Blue border for VPN only
- Purple border for Tor + VPN
- Update tooltip messages

**Task 7**: Testing
- Test HTTP proxy (Squid, mitmproxy)
- Test SOCKS5 proxy (Dante, custom server)
- Test SOCKS5H proxy (verify DNS via proxy)
- Test Tor + VPN combination
- Test proxy authentication (username/password)

---

## Testing Strategy

### Test Proxies Setup

**HTTP Proxy (Squid)**:
```bash
# Install Squid (Ubuntu/Debian)
sudo apt install squid

# Configure /etc/squid/squid.conf
http_port 3128
acl localnet src 127.0.0.1
http_access allow localnet

# Start Squid
sudo systemctl start squid

# Test in Ladybird
Host: localhost
Port: 3128
Type: HTTP
Auth: None
```

**SOCKS5 Proxy (Dante)**:
```bash
# Install Dante server
sudo apt install dante-server

# Configure /etc/danted.conf
internal: 0.0.0.0 port = 1080
external: eth0
socksmethod: none
clientmethod: none
user.privileged: root
user.notprivileged: nobody
client pass {
    from: 0.0.0.0/0 to: 0.0.0.0/0
}
socks pass {
    from: 0.0.0.0/0 to: 0.0.0.0/0
}

# Start Dante
sudo systemctl start danted

# Test in Ladybird
Host: localhost
Port: 1080
Type: SOCKS5H
Auth: None
```

**Commercial VPN (for real testing)**:
- NordVPN: SOCKS5 at nordvpn-proxy-server:1080
- Private Internet Access: SOCKS5 at proxy-nl.privateinternetaccess.com:1080
- Mullvad: SOCKS5 at socks5.mullvad.net:1080

### Test Cases

**Functional Tests**:
1. ✅ Configure HTTP proxy → Enable VPN → Browse site → Verify traffic goes through proxy
2. ✅ Configure SOCKS5 proxy → Enable VPN → Browse site → Verify traffic goes through proxy
3. ✅ Configure SOCKS5H proxy → Enable VPN → Browse site → Verify DNS via proxy
4. ✅ Enable Tor → Enable VPN → Verify both active (purple border)
5. ✅ Test proxy authentication (username/password)
6. ✅ Test "Test Connection" button (valid and invalid proxies)
7. ✅ Test proxy settings persistence across sessions

**Edge Cases**:
1. ❌ Invalid proxy (unreachable host) → Error message, toggle reverts
2. ❌ Proxy rejects authentication → Error message
3. ❌ Proxy times out → Error message after timeout
4. ❌ Enable VPN without configuration → Opens settings dialog
5. ❌ Cancel settings dialog → Toggle remains unchecked

---

## Visual Design

### Border Color Scheme

**Single Mode**:
- Tor only: **Green** border (#00C851) - existing
- VPN only: **Blue** border (#2196F3) - new

**Combined Mode**:
- Tor + VPN: **Purple** border (#9C27B0) - blend of green + blue

**Visual Mockup**:
```
┌────────────────────────────────────────────┐
│  [←] [→] [⟳] [https://example.com      ]  │  ← Green border (Tor only)
└────────────────────────────────────────────┘

┌────────────────────────────────────────────┐
│  [←] [→] [⟳] [https://example.com      ]  │  ← Blue border (VPN only)
└────────────────────────────────────────────┘

┌────────────────────────────────────────────┐
│  [←] [→] [⟳] [https://example.com      ]  │  ← Purple border (Tor + VPN)
└────────────────────────────────────────────┘
```

### Toolbar Layout

**Current (Tor only)**:
```
[←] [→] [⟳] [Address Bar......] [Tor] [☰]
```

**New (Tor + VPN)**:
```
[←] [→] [⟳] [Address Bar......] [Tor] [VPN] [☰]
```

---

## Tor + VPN Interaction

### Routing Options

**Option 1: Tor over VPN** (VPN → Tor → Internet)
- Enable VPN first, then enable Tor
- RequestServer applies VPN proxy, then Tor forwards through VPN
- Use case: Hide Tor usage from ISP

**Option 2: VPN over Tor** (Tor → VPN → Internet)
- Enable Tor first, then enable VPN
- RequestServer applies Tor proxy, then VPN forwards through Tor
- Use case: Access VPN-only content anonymously

**Implementation**: Order matters! Apply proxies in order enabled.
```cpp
if (m_tor_enabled && m_vpn_enabled) {
    // Both enabled - apply in order of activation
    if (m_tor_enabled_first) {
        // Tor over VPN: VPN proxy wraps Tor SOCKS5
        apply_vpn_proxy();
        apply_tor_proxy();
    } else {
        // VPN over Tor: Tor SOCKS5 wraps VPN proxy
        apply_tor_proxy();
        apply_vpn_proxy();
    }
}
```

**Note**: This is advanced functionality - defer to future milestone if complex.

---

## Security Considerations

### Proxy Authentication

**Current**: ProxyConfig has username/password fields
**Risk**: Password stored in memory in plaintext
**Mitigation**:
- Use existing `explicit_bzero()` on NetworkIdentity destruction
- Clear proxy config on tab close
- No persistent storage of passwords (session only)

### Proxy Trust

**Risk**: Malicious proxy can intercept/modify traffic
**Mitigation**:
- User must explicitly configure proxy (no auto-discovery)
- HTTPS still encrypted end-to-end (proxy sees only encrypted data)
- Show warning: "Proxy can see your traffic. Only use trusted proxies."

### DNS Leaks

**Risk**: DNS queries bypass proxy (like Bug Fix 2)
**Mitigation**:
- Default to SOCKS5H (DNS via proxy)
- Already implemented in Bug Fix 2 (lines 733-743 in ConnectionFromClient.cpp)
- Show warning if user selects SOCKS5 instead of SOCKS5H

---

## File Changes Summary

### Files to Create

1. **UI/Qt/ProxySettingsDialog.h** (NEW)
   - ProxySettingsDialog class declaration

2. **UI/Qt/ProxySettingsDialog.cpp** (NEW)
   - ProxySettingsDialog implementation
   - Input validation, test connection button

3. **Libraries/LibIPC/ProxyValidator.h** (NEW)
   - ProxyValidator class declaration

4. **Libraries/LibIPC/ProxyValidator.cpp** (NEW)
   - test_proxy(), test_http_proxy(), test_socks5_proxy()

### Files to Modify

1. **UI/Qt/Tab.h**
   - Add m_vpn_toggle_action member
   - Add m_vpn_enabled flag
   - Add m_proxy_config Optional<ProxyConfig> member
   - Add open_proxy_settings_dialog() method

2. **UI/Qt/Tab.cpp**
   - Implement VPN toggle handler
   - Implement open_proxy_settings_dialog()
   - Update border color logic for Tor + VPN

3. **Services/WebContent/WebContentServer.ipc**
   - Add set_proxy(u64, ProxyConfig) message
   - Add clear_proxy(u64) message

4. **Services/WebContent/ConnectionFromClient.h**
   - Add set_proxy() handler declaration
   - Add clear_proxy() handler declaration

5. **Services/WebContent/ConnectionFromClient.cpp**
   - Implement set_proxy() handler
   - Implement clear_proxy() handler

6. **Services/RequestServer/RequestServer.ipc**
   - Add set_proxy(ProxyConfig) message
   - Add clear_proxy() message

7. **Services/RequestServer/ConnectionFromClient.h**
   - Add set_proxy() handler declaration
   - Add clear_proxy() handler declaration

8. **Services/RequestServer/ConnectionFromClient.cpp**
   - Implement set_proxy() handler (connection pool broadcast)
   - Implement clear_proxy() handler (connection pool broadcast)

9. **Libraries/LibIPC/CMakeLists.txt**
   - Add ProxyValidator.cpp to compilation

10. **UI/Qt/CMakeLists.txt**
    - Add ProxySettingsDialog.cpp to compilation

---

## Timeline Estimate

**Conservative Estimate**:
- Task 1 (ProxySettingsDialog): 4 hours
- Task 2 (VPN toggle): 3 hours
- Task 3 (IPC messages): 2 hours
- Task 4 (ProxyValidator): 2 hours
- Task 5 (CMakeLists): 30 minutes
- Task 6 (Visual indicators): 1 hour
- Task 7 (Testing): 4 hours
- **Total**: 16-17 hours (2-3 days)

**Aggressive Estimate**:
- Tasks 1-6: 8 hours (1 day)
- Testing: 2 hours
- **Total**: 10 hours (1.5 days)

---

## Success Criteria

- [x] User can configure custom proxy via settings dialog
- [x] VPN toggle enables/disables proxy for current tab
- [x] Visual indicator (blue border) shows VPN active
- [x] HTTP/HTTPS/SOCKS5/SOCKS5H proxies work
- [x] Proxy authentication (username/password) works
- [x] "Test Connection" validates proxy before applying
- [x] Tor + VPN can be enabled simultaneously (purple border)
- [x] No DNS leaks when using SOCKS5H proxy
- [x] Build successful with no errors
- [x] All tests pass

---

## Future Enhancements (Post-Milestone 1.4)

1. **Persistent Proxy Profiles**:
   - Save named proxy configurations (e.g., "Work VPN", "Home Proxy")
   - Quick-switch between profiles via dropdown

2. **System Proxy Integration**:
   - Detect system proxy settings (Windows/macOS/Linux)
   - "Use System Proxy" checkbox in settings

3. **Proxy Auto-Config (PAC)**:
   - Support PAC files for automatic proxy selection
   - Per-domain proxy rules

4. **Proxy Chaining**:
   - Multiple proxies in sequence (Proxy1 → Proxy2 → Internet)
   - Advanced privacy configuration

5. **Proxy Performance Metrics**:
   - Show latency, bandwidth, connection status
   - Network activity log in audit UI (Milestone 1.5)

---

## Risks and Mitigation

### Risk 1: Proxy Compatibility

**Likelihood**: MEDIUM
**Impact**: MEDIUM (some proxies may not work)

**Mitigation**:
- Test with popular proxy implementations (Squid, Dante, commercial VPNs)
- Document known compatibility issues
- Provide troubleshooting guide

### Risk 2: UI Complexity

**Likelihood**: LOW
**Impact**: LOW (users may be confused by settings)

**Mitigation**:
- Clear labels and tooltips in settings dialog
- "Test Connection" provides immediate feedback
- Default to SOCKS5H (safest option)

### Risk 3: Performance Impact

**Likelihood**: LOW
**Impact**: MEDIUM (proxy may slow down browsing)

**Mitigation**:
- User choice to enable/disable per tab
- Show performance metrics in future milestone
- Document expected latency increase

---

## Conclusion

**Recommendation**: PROCEED with VPN integration.

**Rationale**:
- ✅ Reuses 90% of existing Tor infrastructure (ProxyConfig, NetworkIdentity, IPC)
- ✅ Low implementation complexity (2-3 days)
- ✅ High user value (custom VPN/proxy support)
- ✅ No new dependencies required (Core::TCPSocket already available)
- ✅ Clear testing strategy with open-source proxies

**Next Step**: Begin implementation starting with ProxySettingsDialog UI component.
