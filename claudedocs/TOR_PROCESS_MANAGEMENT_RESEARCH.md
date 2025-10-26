# Tor Process Management - Research and Design

## Goal

Implement robust Tor process detection and management to provide clear user feedback when Tor is unavailable and prevent confusing error states.

## Current Behavior (Problem)

**Scenario**: User clicks Tor toggle button when Tor service is not running

**Current Result**:
- Button toggles to "enabled" state
- Green border appears on location bar
- NetworkIdentity configures Tor proxy
- HTTP requests attempt to connect to localhost:9050 (SOCKS5)
- Connection fails silently or times out
- User sees broken behavior with no clear error message

**Why This Is Bad**:
- User thinks Tor is working but it's not
- No indication that Tor service needs to be started
- Silent failures lead to confusion and poor UX

## Desired Behavior

**Scenario**: User clicks Tor toggle when Tor is not running

**New Result**:
1. Detect that Tor is not available BEFORE enabling
2. Show clear error dialog: "Tor is not running. Please start the Tor service first."
3. Button remains in "disabled" state
4. Provide instructions for starting Tor (platform-specific)

**Scenario**: User clicks Tor toggle when Tor IS running

**Result**:
1. Check passes, Tor is available
2. Enable Tor normally (existing behavior)
3. Continue with proxy configuration

## Detection Methods

### Option 1: SOCKS5 Port Check (RECOMMENDED)

**Approach**: Attempt to connect to localhost:9050 (Tor's SOCKS5 port)

**Advantages**:
- Platform-independent
- Tests actual SOCKS5 availability (what we need)
- Fast (connection attempt times out quickly)
- No external dependencies

**Implementation**:
```cpp
bool is_tor_available()
{
    // Try to connect to Tor SOCKS5 proxy
    auto socket = Core::TCPSocket::construct();
    auto result = socket->connect("127.0.0.1", 9050, Core::Duration::from_milliseconds(1000));

    if (result.is_error()) {
        dbgln("Tor availability check failed: {}", result.error());
        return false;
    }

    // Connection successful - Tor is listening
    socket->close();
    return true;
}
```

**Drawbacks**:
- Doesn't distinguish between "Tor not running" and "firewall blocking"
- Could theoretically false positive if another service uses port 9050

---

### Option 2: systemctl/Process Check

**Approach**: Check if Tor process is running via system tools

**Linux**:
```cpp
// Check systemctl status
auto result = Core::command("systemctl is-active tor");
bool tor_running = (result.exit_code == 0);

// Or check process list
auto result = Core::command("pgrep -x tor");
bool tor_running = (result.exit_code == 0);
```

**macOS**:
```cpp
auto result = Core::command("launchctl list | grep tor");
bool tor_running = (result.exit_code == 0);
```

**Windows**:
```cpp
auto result = Core::command("tasklist /FI \"IMAGENAME eq tor.exe\" | find \"tor.exe\"");
bool tor_running = (result.exit_code == 0);
```

**Advantages**:
- Directly checks if Tor process exists
- Can provide more detailed status info

**Drawbacks**:
- Platform-specific implementations
- Requires executing shell commands (security considerations)
- Process might be running but SOCKS5 not available
- Different Tor installations (system package vs Tor Browser)

---

### Option 3: Tor Control Port (9051)

**Approach**: Connect to Tor's control port and query status

**Implementation**:
```cpp
bool check_tor_control_port()
{
    auto socket = Core::TCPSocket::construct();
    auto result = socket->connect("127.0.0.1", 9051);

    if (result.is_error())
        return false;

    // Send AUTHENTICATE command
    socket->write("AUTHENTICATE\r\n");
    auto response = socket->read_until("\r\n");

    // Check for "250 OK" response
    return response.starts_with("250");
}
```

**Advantages**:
- More sophisticated check
- Can query Tor configuration
- Can verify Tor is actually Tor (not another service)

**Drawbacks**:
- Requires control port to be enabled (not always the case)
- More complex implementation
- Overkill for simple availability check

---

## Recommendation: Hybrid Approach

**Phase 1: SOCKS5 Port Check** (Simple, fast, sufficient for most cases)
```cpp
ErrorOr<void> check_tor_availability()
{
    auto socket = TRY(Core::TCPSocket::construct());
    auto connect_result = socket->connect("127.0.0.1", 9050, Core::Duration::from_milliseconds(1000));

    if (connect_result.is_error())
        return Error::from_string_literal("Tor SOCKS5 proxy not available on localhost:9050");

    socket->close();
    return {};
}
```

**Phase 2: Enhanced Detection** (Future improvement)
- Try control port if SOCKS5 check fails
- Provide platform-specific instructions for starting Tor
- Auto-detect Tor Browser vs system Tor

## Implementation Plan

### 1. Add Tor Availability Check Function

**File**: `Libraries/LibIPC/NetworkIdentity.h`

```cpp
namespace IPC {

class TorAvailability {
public:
    static ErrorOr<void> check_socks5_available(ByteString host = "127.0.0.1", u16 port = 9050);
    static bool is_tor_running(); // Convenience wrapper
};

}
```

**File**: `Libraries/LibIPC/NetworkIdentity.cpp`

```cpp
ErrorOr<void> TorAvailability::check_socks5_available(ByteString host, u16 port)
{
    auto socket = TRY(Core::TCPSocket::construct());

    // Try to connect with 1 second timeout
    auto connect_result = socket->connect(host, port, Core::Duration::from_milliseconds(1000));

    if (connect_result.is_error()) {
        return Error::from_string_literal("Cannot connect to Tor SOCKS5 proxy. Is Tor running?");
    }

    // Successfully connected - Tor is available
    socket->close();
    return {};
}

bool TorAvailability::is_tor_running()
{
    auto result = check_socks5_available();
    return !result.is_error();
}
```

---

### 2. Add Availability Check Before Enabling Tor

**File**: `UI/Qt/Tab.cpp`

**Current Code** (lines 94-116):
```cpp
QObject::connect(m_tor_toggle_action, &QAction::triggered, this, [this](bool checked) {
    m_tor_enabled = checked;
    if (checked) {
        // Enable Tor for this tab
        dbgln("Tab: Enabling Tor for page_id {}", view().page_id());
        m_tor_toggle_action->setToolTip("Disable Tor for this tab (currently using Tor)");
        m_location_edit->setStyleSheet("QLineEdit { border: 2px solid #00C851; }");
        view().client().async_enable_tor(view().page_id(), {});
    } else {
        // Disable Tor for this tab
        dbgln("Tab: Disabling Tor for page_id {}", view().page_id());
        m_tor_toggle_action->setToolTip("Enable Tor for this tab");
        m_location_edit->setStyleSheet("");
        view().client().async_disable_tor(view().page_id());
    }
});
```

**Modified Code** (with Tor availability check):
```cpp
QObject::connect(m_tor_toggle_action, &QAction::triggered, this, [this](bool checked) {
    if (checked) {
        // Check if Tor is available BEFORE enabling
        if (!IPC::TorAvailability::is_tor_running()) {
            // Tor not available - show error and revert toggle
            QMessageBox::warning(this, "Tor Not Available",
                "Cannot enable Tor: The Tor service is not running.\n\n"
                "Please start Tor first:\n"
                "  Linux: sudo systemctl start tor\n"
                "  macOS: brew services start tor\n"
                "  Windows: Start Tor Browser or tor.exe");

            // Revert toggle state
            m_tor_toggle_action->setChecked(false);
            m_tor_enabled = false;
            return;
        }

        // Tor is available - proceed with enabling
        m_tor_enabled = true;
        dbgln("Tab: Enabling Tor for page_id {}", view().page_id());
        m_tor_toggle_action->setToolTip("Disable Tor for this tab (currently using Tor)");
        m_location_edit->setStyleSheet("QLineEdit { border: 2px solid #00C851; }");
        view().client().async_enable_tor(view().page_id(), {});
    } else {
        // Disable Tor for this tab
        m_tor_enabled = false;
        dbgln("Tab: Disabling Tor for page_id {}", view().page_id());
        m_tor_toggle_action->setToolTip("Enable Tor for this tab");
        m_location_edit->setStyleSheet("");
        view().client().async_disable_tor(view().page_id());
    }
});
```

---

### 3. Add Tor Status Indicator

**Option A: Status Bar Indicator**

Add to BrowserWindow status bar showing global Tor availability:
```
[Tor: Available] or [Tor: Unavailable]
```

**Option B: Toolbar Icon**

Add small icon next to Tor toggle showing service status:
```
🟢 (green) = Tor running
🔴 (red) = Tor not running
```

**Option C: Tooltip Enhancement**

Update Tor toggle button tooltip to show status:
```
"Enable Tor for this tab (Tor service: Running)"
"Enable Tor for this tab (Tor service: Not Running - click for help)"
```

**Recommendation**: Option C (Tooltip) + Option B (Visual indicator)

---

### 4. Handle Connection Failures Gracefully

**File**: `Services/RequestServer/ConnectionFromClient.cpp`

Add error detection when curl fails to connect to SOCKS5 proxy:

```cpp
void ConnectionFromClient::issue_network_request(...)
{
    // ... existing setup ...

    // Apply proxy if configured
    if (m_network_identity && m_network_identity->has_proxy()) {
        auto const& proxy = m_network_identity->proxy_config().value();

        set_option(CURLOPT_PROXY, proxy.to_curl_proxy_url().characters());
        // ... proxy setup ...

        // Set connection timeout for proxy failures
        set_option(CURLOPT_CONNECTTIMEOUT, 5L); // 5 second timeout
    }

    // ... rest of request setup ...
}

// In error callback:
void ConnectionFromClient::on_request_failed(...)
{
    if (m_network_identity && m_network_identity->has_proxy()) {
        // Check if failure was due to proxy connection
        long response_code = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);

        if (response_code == 0) {
            // No response - likely proxy connection failure
            dbgln("RequestServer: Request failed - cannot connect to proxy (is Tor running?)");

            // Send IPC message back to UI to show error
            async_request_failed(request_id, "Cannot connect to Tor proxy. Is Tor running?");
            return;
        }
    }

    // ... normal error handling ...
}
```

---

## Testing Plan

### Test 1: Tor Not Running
**Steps**:
1. Stop Tor: `sudo systemctl stop tor`
2. Open Ladybird
3. Click Tor toggle button

**Expected**:
- Warning dialog appears: "Tor Not Available"
- Button remains unchecked
- No green border appears
- No requests are sent

**Actual**: (to be tested)

---

### Test 2: Tor Running
**Steps**:
1. Start Tor: `sudo systemctl start tor`
2. Open Ladybird
3. Click Tor toggle button

**Expected**:
- No error dialog
- Button becomes checked
- Green border appears
- Requests use Tor

**Actual**: (to be tested)

---

### Test 3: Tor Stops While Enabled
**Steps**:
1. Enable Tor in tab
2. Stop Tor service: `sudo systemctl stop tor`
3. Navigate to new URL

**Expected**:
- Request fails with error message
- Option to disable Tor or restart Tor service

**Actual**: (to be tested)

---

### Test 4: Status Indicator Updates
**Steps**:
1. Open Ladybird with Tor stopped
2. Hover over Tor button
3. Start Tor service
4. Hover over Tor button again

**Expected**:
- Tooltip updates to show "Running"
- Status indicator changes from red to green

**Actual**: (to be tested)

---

## Files to Modify

1. **Libraries/LibIPC/NetworkIdentity.h** - Add TorAvailability class
2. **Libraries/LibIPC/NetworkIdentity.cpp** - Implement SOCKS5 port check
3. **UI/Qt/Tab.cpp** - Add availability check before enabling Tor
4. **UI/Qt/Tab.h** - Add status indicator members
5. **Services/RequestServer/ConnectionFromClient.cpp** - Add proxy failure detection
6. **Services/WebContent/ConnectionFromClient.cpp** - Forward proxy errors to UI

**Estimated Lines**: ~200 lines of new code

---

## Error Messages

### User-Facing Messages

**Tor Not Available**:
```
Cannot enable Tor: The Tor service is not running.

Please start Tor first:
  Linux: sudo systemctl start tor
  macOS: brew services start tor
  Windows: Start Tor Browser or tor.exe

Need help? See: https://www.torproject.org/download/
```

**Tor Connection Failed** (during request):
```
Failed to connect to Tor proxy.

The Tor service may have stopped. Please check:
  1. Tor service is running: systemctl status tor
  2. Tor is listening on port 9050
  3. No firewall is blocking localhost:9050

[Disable Tor] [Retry] [Help]
```

---

## Platform-Specific Instructions

### Linux (systemd)
```bash
# Start Tor
sudo systemctl start tor

# Enable Tor on boot
sudo systemctl enable tor

# Check status
systemctl status tor
```

### macOS (Homebrew)
```bash
# Install Tor
brew install tor

# Start Tor
brew services start tor

# Check if running
brew services list | grep tor
```

### Windows
```
Option 1: Tor Browser
  - Download from torproject.org
  - Run Tor Browser (includes Tor service)

Option 2: Tor Expert Bundle
  - Download tor.exe
  - Run: tor.exe -f torrc
```

---

## Future Enhancements

### Auto-Start Tor (Phase 2)
**Goal**: Optionally start Tor automatically when user enables it

**Challenges**:
- Requires system permissions (sudo on Linux)
- Platform-specific implementations
- Security considerations (running system commands)

**Approach**:
```cpp
ErrorOr<void> TorProcessManager::start_tor_service()
{
#ifdef AK_OS_LINUX
    // Try systemctl (user service first, then system service)
    auto result = Core::command("systemctl --user start tor");
    if (result.exit_code != 0)
        result = Core::command("pkexec systemctl start tor"); // Ask for sudo
#elif AK_OS_MACOS
    auto result = Core::command("brew services start tor");
#elif AK_OS_WINDOWS
    // Look for Tor Browser or tor.exe
    auto result = start_tor_browser();
#endif

    if (result.exit_code != 0)
        return Error::from_string_literal("Failed to start Tor service");

    return {};
}
```

**UI**:
- "Tor is not running. [Start Tor Automatically] [Cancel]"
- Progress dialog while starting
- Success/failure notification

---

### Tor Status Monitor (Phase 3)
**Goal**: Continuously monitor Tor availability and update UI

**Approach**:
```cpp
class TorStatusMonitor : public Core::Timer {
public:
    TorStatusMonitor()
    {
        set_interval(5000); // Check every 5 seconds
        on_timeout = [this] {
            bool available = IPC::TorAvailability::is_tor_running();
            if (available != m_last_status) {
                m_last_status = available;
                emit status_changed(available);
            }
        };
    }

signals:
    void status_changed(bool available);

private:
    bool m_last_status { false };
};
```

**Benefits**:
- Real-time status updates
- Detect when Tor stops/starts
- Update UI accordingly

---

## Security Considerations

1. **Port Check Safety**: Connecting to localhost:9050 is safe, no external network access
2. **Timeout**: Use short timeout (1 second) to avoid hanging
3. **No Authentication**: SOCKS5 check doesn't send credentials, just tests connectivity
4. **Process Execution**: If implementing auto-start, carefully validate commands to prevent injection

---

## Success Criteria

- [x] Research Tor detection methods
- [ ] Implement SOCKS5 port check function
- [ ] Add availability check before enabling Tor
- [ ] Show clear error dialog when Tor unavailable
- [ ] Add Tor status indicator to UI
- [ ] Handle proxy connection failures gracefully
- [ ] Test with Tor running and stopped
- [ ] Document implementation

---

## Next Steps

1. Implement `TorAvailability::check_socks5_available()` in LibIPC
2. Add availability check to Tab.cpp toggle handler
3. Create error dialog with platform-specific instructions
4. Add status indicator to Tor button
5. Test with Tor service stopped/started
6. Document Milestone 1.3 completion
