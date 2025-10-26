# Tor UI Integration Plan - Milestone 1.2

## Research Summary

### Ladybird UI Architecture

**Multi-Process Design**:
```
Browser Window (Qt/AppKit)
  └─> Tab (UI widget, per-tab)
        └─> WebContentView (UI widget, rendering)
              └─> WebContentClient (IPC client)
                    ↓ (IPC)
                  WebContent Process (per-tab)
                    └─> RequestServer Process (per-tab)
                          └─> ConnectionFromClient
                                └─> NetworkIdentity (our Tor integration)
                                      └─> ProxyConfig (Tor SOCKS5H)
```

### Key UI Components (Qt Implementation)

**Files**:
- `UI/Qt/Tab.h/cpp` - Per-tab UI widget with toolbar
- `UI/Qt/BrowserWindow.h/cpp` - Main browser window managing tabs
- `UI/Qt/WebContentView.h/cpp` - WebContent rendering widget

**UI Architecture Patterns**:

1. **Toolbar Actions** (Tab.cpp:94-99):
   ```cpp
   m_toolbar->addAction(m_navigate_back_action);
   m_toolbar->addAction(m_navigate_forward_action);
   m_toolbar->addAction(m_reload_action);
   m_toolbar->addWidget(m_location_edit);
   m_toolbar->addAction(create_application_action(*m_toolbar, view().reset_zoom_action()));
   m_hamburger_button_action = m_toolbar->addWidget(m_hamburger_button);
   ```

2. **Action Creation Pattern**:
   ```cpp
   m_navigate_back_action = create_application_action(*this, view().navigate_back_action());
   ```

3. **Per-Tab State**:
   - Each `Tab` instance has its own toolbar
   - Each `Tab` has a `WebContentView` (`m_view`)
   - Each `WebContentView` has its own `WebContentClient` (IPC connection)

### IPC Communication Pattern

**RequestServer IPC** (`Services/RequestServer/RequestServer.ipc`):
- Messages sent from WebContent to RequestServer
- Each RequestServer is per-tab (spawned per WebContent process)
- Messages use syntax: `message_name(params) => (return_values)` or `message_name(params) =|`
- Example: `set_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally) =|`

**WebContentServer IPC** (`Services/WebContent/WebContentServer.ipc`):
- Messages sent from UI to WebContent process
- All messages include `u64 page_id` as first parameter
- Example: `set_proxy_mappings(u64 page_id, Vector<ByteString> proxies, HashMap<ByteString, size_t> mappings) =|`

**Current Problem**: UI cannot directly control RequestServer - it must go through WebContent

---

## Implementation Plan

### Phase 1: Add IPC Messages for Tor Control

**Goal**: Enable UI → WebContent → RequestServer communication for Tor controls

**Files to Modify**:

1. **`Services/WebContent/WebContentServer.ipc`** (Add new messages):
   ```cpp
   // Tor network control
   enable_tor(u64 page_id, ByteString circuit_id) =|
   disable_tor(u64 page_id) =|
   rotate_tor_circuit(u64 page_id) =|
   get_tor_status(u64 page_id) => (bool tor_enabled, Optional<ByteString> circuit_id)
   ```

2. **`Services/WebContent/PageClient.cpp`** (Implement handlers):
   ```cpp
   void PageClient::enable_tor(ByteString circuit_id)
   {
       // Get request_server_client() and call enable_tor()
       if (auto* request_server = request_server_client())
           request_server->async_enable_tor(move(circuit_id));
   }

   void PageClient::disable_tor()
   {
       if (auto* request_server = request_server_client())
           request_server->async_disable_tor();
   }

   void PageClient::rotate_tor_circuit()
   {
       if (auto* request_server = request_server_client())
           request_server->async_rotate_tor_circuit();
   }
   ```

3. **`Services/RequestServer/RequestServer.ipc`** (Add new messages):
   ```cpp
   // Tor network control (no response needed)
   enable_tor(ByteString circuit_id) =|
   disable_tor() =|
   rotate_tor_circuit() =|
   ```

4. **`Services/RequestServer/ConnectionFromClient.h`** (Already have these methods):
   - `void enable_tor(ByteString circuit_id = {});` ✅ DONE
   - `void disable_tor();` ✅ DONE
   - `void rotate_tor_circuit();` ✅ DONE

   Just need to expose them as IPC message handlers.

5. **`Services/RequestServer/ConnectionFromClient.cpp`** (Add IPC handlers):
   ```cpp
   Messages::RequestServer::EnableTorResponse ConnectionFromClient::enable_tor(ByteString circuit_id)
   {
       // Already implemented - just expose via IPC
       this->enable_tor(move(circuit_id));
   }

   Messages::RequestServer::DisableTorResponse ConnectionFromClient::disable_tor()
   {
       this->disable_tor();
   }

   Messages::RequestServer::RotateTorCircuitResponse ConnectionFromClient::rotate_tor_circuit()
   {
       this->rotate_tor_circuit();
   }
   ```

**Testing**: Use gdb to call IPC messages manually and verify communication works.

---

### Phase 2: Design Per-Tab Tor Toggle UI

**Goal**: Design Qt UI components for Tor controls

**UI Components Needed**:

1. **Tor Toggle Button** (QToolButton, checkable):
   - Icon: Onion icon (enabled state) / crossed-out onion (disabled)
   - Tooltip: "Enable Tor for this tab" / "Disable Tor for this tab"
   - Visual state: Shows whether Tor is enabled
   - Click action: Toggle Tor on/off for current tab

2. **New Identity Button** (QAction in menu):
   - Label: "New Tor Identity"
   - Tooltip: "Rotate to new Tor circuit"
   - Only visible when Tor is enabled
   - Click action: Call rotate_tor_circuit()

3. **Tor Status Indicator** (Visual feedback):
   - Location edit styling (green border when Tor active?)
   - Status bar indicator showing Tor exit IP
   - Tooltip showing circuit ID

**Design Decisions**:

**Option A: Toolbar Button** (RECOMMENDED)
- Add `QToolButton* m_tor_toggle_button` to Tab.h
- Add to toolbar between reload and location_edit
- Checkable button (pressed = Tor enabled)
- Icon changes based on state

**Option B: Hamburger Menu Only**
- Add "Enable Tor" / "Disable Tor" menu item
- Less visible but cleaner UI
- No visual indicator of Tor status

**Option C: Both** (Best UX)
- Toolbar toggle button for quick access
- Hamburger menu for "New Identity" action
- Visual indicators in location edit

**Recommendation**: Option C for best user experience

---

### Phase 3: Implement Tor Toggle Button in Tab UI

**Goal**: Add working Tor toggle button to Tab toolbar

**Files to Modify**:

1. **`UI/Qt/Tab.h`** (Add members):
   ```cpp
   class Tab final : public QWidget {
       // ...
   public slots:
       void tor_toggle_clicked();  // NEW

   private:
       QToolButton* m_tor_toggle_button { nullptr };  // NEW
       QAction* m_tor_toggle_action { nullptr };      // NEW
       bool m_tor_enabled { false };                  // NEW
   };
   ```

2. **`UI/Qt/Tab.cpp`** (Constructor - add button):
   ```cpp
   Tab::Tab(BrowserWindow* window, RefPtr<WebView::WebContentClient> parent_client, size_t page_index)
       : QWidget(window)
       , m_window(window)
   {
       // ... existing setup ...

       // Create Tor toggle button (after reload action)
       m_tor_toggle_button = new QToolButton(m_toolbar);
       m_tor_toggle_button->setText("Toggle Tor");
       m_tor_toggle_button->setToolTip("Enable Tor for this tab");
       m_tor_toggle_button->setCheckable(true);  // Toggle state
       m_tor_toggle_button->setChecked(false);   // Initially disabled

       // TODO: Add onion icon when available
       // m_tor_toggle_button->setIcon(load_icon_from_uri("resource://icons/tor-onion.png"));

       QObject::connect(m_tor_toggle_button, &QToolButton::clicked, this, &Tab::tor_toggle_clicked);

       // Add to toolbar (after reload, before location_edit)
       m_toolbar->addAction(m_navigate_back_action);
       m_toolbar->addAction(m_navigate_forward_action);
       m_toolbar->addAction(m_reload_action);
       m_tor_toggle_action = m_toolbar->addWidget(m_tor_toggle_button);  // NEW
       m_toolbar->addWidget(m_location_edit);
       // ...
   }
   ```

3. **`UI/Qt/Tab.cpp`** (Implement toggle handler):
   ```cpp
   void Tab::tor_toggle_clicked()
   {
       m_tor_enabled = !m_tor_enabled;
       m_tor_toggle_button->setChecked(m_tor_enabled);

       if (m_tor_enabled) {
           // Enable Tor for this tab
           m_tor_toggle_button->setToolTip("Disable Tor for this tab");

           // Generate unique circuit ID for this tab
           ByteString circuit_id = ByteString::formatted("tab-{}-{}", tab_index(), time(nullptr));

           // Send IPC message to WebContent
           view().client()->async_enable_tor(circuit_id);

           // Visual feedback: green border on location edit?
           m_location_edit->setStyleSheet("border: 2px solid green;");

           dbgln("Tab {}: Tor enabled with circuit {}", tab_index(), circuit_id);
       } else {
           // Disable Tor for this tab
           m_tor_toggle_button->setToolTip("Enable Tor for this tab");

           view().client()->async_disable_tor();

           // Remove visual feedback
           m_location_edit->setStyleSheet("");

           dbgln("Tab {}: Tor disabled", tab_index());
       }
   }
   ```

**Visual States**:
- Unchecked (Tor disabled): Gray onion icon, "Enable Tor for this tab"
- Checked (Tor enabled): Colored onion icon, green location edit border

---

### Phase 4: Implement New Identity Button

**Goal**: Add "New Identity" option to rotate Tor circuit

**Files to Modify**:

1. **`UI/Qt/Menu.cpp`** (Add menu item to hamburger menu):
   ```cpp
   // In create_application_action or similar menu creation

   auto* tor_menu = menu.addMenu("&Tor");

   auto* new_identity_action = new QAction("New Tor &Identity", &menu);
   new_identity_action->setShortcut(QKeySequence("Ctrl+Shift+U"));
   QObject::connect(new_identity_action, &QAction::triggered, [&tab] {
       if (tab.tor_enabled()) {
           tab.rotate_tor_circuit();
       }
   });
   tor_menu->addAction(new_identity_action);

   // Only enable when Tor is active
   new_identity_action->setEnabled(false);
   QObject::connect(&tab, &Tab::tor_state_changed, [new_identity_action](bool enabled) {
       new_identity_action->setEnabled(enabled);
   });
   ```

2. **`UI/Qt/Tab.h`** (Add methods):
   ```cpp
   class Tab final : public QWidget {
       // ...
   public:
       bool tor_enabled() const { return m_tor_enabled; }
       void rotate_tor_circuit();

   signals:
       void tor_state_changed(bool enabled);  // NEW signal
   };
   ```

3. **`UI/Qt/Tab.cpp`** (Implement rotation):
   ```cpp
   void Tab::rotate_tor_circuit()
   {
       if (!m_tor_enabled) {
           QMessageBox::warning(this, "Tor Not Enabled",
               "Tor is not enabled for this tab. Enable Tor first.");
           return;
       }

       // Send IPC message to rotate circuit
       view().client()->async_rotate_tor_circuit();

       // Show notification
       QMessageBox::information(this, "New Tor Identity",
           "Tor circuit rotated. You now have a new identity.");

       dbgln("Tab {}: Tor circuit rotated", tab_index());
   }
   ```

4. **Update `tor_toggle_clicked()` to emit signal**:
   ```cpp
   void Tab::tor_toggle_clicked()
   {
       m_tor_enabled = !m_tor_enabled;
       // ...
       emit tor_state_changed(m_tor_enabled);  // NEW
   }
   ```

**UX Flow**:
1. User enables Tor → "New Identity" menu item becomes enabled
2. User clicks "New Identity" → Tor circuit rotates
3. User gets new Tor exit IP for this tab
4. User disables Tor → "New Identity" becomes disabled again

---

### Phase 5: Add Visual Indicators for Tor Status

**Goal**: Provide clear visual feedback when Tor is active

**Indicators to Add**:

1. **Location Edit Styling** (DONE in Phase 3):
   ```cpp
   // Green border when Tor enabled
   m_location_edit->setStyleSheet("border: 2px solid green;");

   // Or add prefix icon in location edit
   m_location_edit->setText("🧅 " + url);  // Onion emoji prefix
   ```

2. **Status Bar Indicator** (Optional):
   ```cpp
   // Add to BrowserWindow status bar
   auto* tor_status_label = new QLabel(this);
   tor_status_label->setText("Tor: Inactive");
   tor_status_label->setVisible(false);

   // Update when tab changes Tor state
   QObject::connect(&tab, &Tab::tor_state_changed, [tor_status_label](bool enabled) {
       if (enabled) {
           tor_status_label->setText("Tor: Active");
           tor_status_label->setStyleSheet("color: green;");
           tor_status_label->setVisible(true);
       } else {
           tor_status_label->setVisible(false);
       }
   });
   ```

3. **Toolbar Button Icon** (When icon available):
   ```cpp
   // Unchecked state: gray onion
   QIcon tor_icon_disabled = load_icon_from_uri("resource://icons/tor-onion-disabled.png");

   // Checked state: colored onion
   QIcon tor_icon_enabled = load_icon_from_uri("resource://icons/tor-onion-enabled.png");

   m_tor_toggle_button->setIcon(m_tor_enabled ? tor_icon_enabled : tor_icon_disabled);
   ```

4. **Tooltip with Circuit ID**:
   ```cpp
   void Tab::update_tor_tooltip()
   {
       if (m_tor_enabled && m_network_identity) {
           auto circuit_id = m_network_identity->tor_circuit_id().value_or("unknown");
           m_tor_toggle_button->setToolTip(ByteString::formatted(
               "Tor enabled\nCircuit: {}", circuit_id));
       } else {
           m_tor_toggle_button->setToolTip("Enable Tor for this tab");
       }
   }
   ```

**Visual Design**:
- **Inactive**: Gray button, no border
- **Active**: Colored button, green location edit border
- **Rotating**: Brief animation or "Rotating..." tooltip

---

### Phase 6: Testing & Validation

**Goal**: Verify all Tor UI controls work correctly

**Test Cases**:

1. **Basic Toggle**:
   - Open tab, click Tor toggle → Tor enables
   - Visit https://check.torproject.org → Shows "Tor enabled"
   - Click toggle again → Tor disables
   - Reload page → Shows "Not using Tor"

2. **Per-Tab Isolation**:
   - Open Tab 1, enable Tor → Circuit A
   - Open Tab 2, enable Tor → Circuit B
   - Both tabs visit check.torproject.org → Different exit IPs
   - Disable Tor on Tab 1 → Only Tab 2 still using Tor

3. **New Identity**:
   - Enable Tor on tab
   - Note exit IP from check.torproject.org
   - Click "New Identity" menu → Circuit rotates
   - Reload check.torproject.org → Different exit IP

4. **Visual Indicators**:
   - Enable Tor → Green border appears on location edit
   - Disable Tor → Green border disappears
   - Toggle button shows checked/unchecked state correctly

5. **Error Handling**:
   - Tor service not running → Show error message
   - Click "New Identity" when Tor disabled → Show warning

**Manual Testing Checklist**:
- [ ] Tor toggle button appears in toolbar
- [ ] Button is checkable (toggle state)
- [ ] Clicking button enables/disables Tor
- [ ] IPC messages sent correctly (verify with REQUESTSERVER_DEBUG)
- [ ] NetworkIdentity created with correct circuit ID
- [ ] Proxy applied to HTTP requests (verify with Tor check site)
- [ ] Green border appears when Tor enabled
- [ ] "New Identity" menu item enabled only when Tor active
- [ ] Circuit rotation works (new exit IP after rotation)
- [ ] Multiple tabs have independent Tor states
- [ ] Different tabs show different exit IPs (stream isolation)

---

## Implementation Order

**Week 1**:
1. Add IPC messages (RequestServer.ipc, WebContentServer.ipc)
2. Implement IPC handlers (ConnectionFromClient, PageClient)
3. Test IPC communication with gdb

**Week 2**:
4. Add Tor toggle button to Tab UI
5. Implement tor_toggle_clicked() handler
6. Add visual indicators (green border, tooltip)
7. Test basic toggle functionality

**Week 3**:
8. Add "New Identity" menu item
9. Implement rotate_tor_circuit() handler
10. Test circuit rotation
11. Final integration testing with multiple tabs

---

## Success Criteria

- [x] Research Ladybird UI architecture ✅
- [ ] IPC messages added for Tor control
- [ ] Tor toggle button visible in toolbar
- [ ] Clicking toggle enables/disables Tor
- [ ] Green border shows when Tor enabled
- [ ] "New Identity" menu rotates circuit
- [ ] Per-tab Tor state works correctly
- [ ] Different tabs show different Tor exit IPs
- [ ] All manual tests pass

---

## Next Steps

After this milestone is complete:
- **Milestone 1.3**: Tor Process Management (auto-start Tor if not running)
- **Milestone 1.4**: VPN Integration (similar pattern for VPN support)
- **Milestone 1.5**: Network Identity Audit UI (view request logs per tab)
- **Milestone 1.6**: Advanced Features (Tor bridges, custom circuits, exit node selection)

---

## Technical Notes

### IPC Message Syntax

**Ladybird IPC format**:
```
// One-way message (no response)
message_name(Type param1, Type param2) =|

// Two-way message (with response)
message_name(Type param1) => (ReturnType result)
```

**Example**:
```cpp
// RequestServer.ipc
enable_tor(ByteString circuit_id) =|  // Fire-and-forget

// WebContentServer.ipc
get_tor_status(u64 page_id) => (bool enabled, Optional<ByteString> circuit_id)  // Returns data
```

### Qt Signals & Slots

**Signal declaration** (Tab.h):
```cpp
signals:
    void tor_state_changed(bool enabled);
```

**Emit signal** (Tab.cpp):
```cpp
emit tor_state_changed(m_tor_enabled);
```

**Connect signal** (Tab.cpp):
```cpp
QObject::connect(&tab, &Tab::tor_state_changed, [this](bool enabled) {
    // Handle Tor state change
});
```

### Debug Output

**Enable RequestServer debug**:
```cpp
#define REQUESTSERVER_DEBUG 1  // In ConnectionFromClient.cpp

dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Tor enabled with circuit {}", circuit_id);
```

**Enable WebContent debug**:
```cpp
dbgln("WebContent: Sending enable_tor IPC message");
```

**View debug output**:
```bash
./Meta/ladybird.py run 2>&1 | grep -E "(RequestServer|WebContent|Tor)"
```

---

## Security Considerations

1. **Circuit ID Generation**:
   - Use timestamp + tab index for uniqueness
   - Format: `tab-{index}-{timestamp}`
   - Ensures no circuit reuse across tabs

2. **DNS Leak Prevention**:
   - Already implemented via SOCKS5H (hostname resolution via Tor)
   - No changes needed in UI layer

3. **Visual Indicators**:
   - Clear distinction between Tor/non-Tor tabs
   - Prevent user confusion about which tabs are protected

4. **Error Handling**:
   - Show clear error if Tor service not running
   - Don't silently fail to enable Tor

5. **State Persistence** (Future):
   - Currently: Tor state not saved across sessions
   - Future: Option to remember Tor preference per domain

---

## Icon Design Requirements

**Tor Onion Icon** (16x16 toolbar icon):
- **Disabled state**: Gray onion outline
- **Enabled state**: Purple/colored onion
- **Format**: PNG or SVG
- **Location**: `Base/res/icons/16x16/tor-onion.png`

**Alternative**: Use existing Qt icons temporarily:
```cpp
// Temporary: Use lock icon until custom icon available
m_tor_toggle_button->setIcon(style()->standardIcon(QStyle::SP_DialogYesButton));
```

---

## File Change Summary

**New Files**: None (all modifications to existing files)

**Modified Files**:
1. `Services/RequestServer/RequestServer.ipc` - Add enable_tor, disable_tor, rotate_tor_circuit messages
2. `Services/RequestServer/ConnectionFromClient.cpp` - Add IPC message handlers
3. `Services/WebContent/WebContentServer.ipc` - Add Tor control messages
4. `Services/WebContent/PageClient.h` - Add Tor control methods
5. `Services/WebContent/PageClient.cpp` - Implement Tor control forwarding
6. `UI/Qt/Tab.h` - Add Tor toggle button members
7. `UI/Qt/Tab.cpp` - Implement Tor toggle UI and handlers
8. `UI/Qt/Menu.cpp` - Add "New Identity" menu item

**Total Estimated Changes**: ~300 lines of new code

---

## Completion Status: PLANNED

**Ready to Begin Implementation**: ✅

**Next Task**: Phase 1 - Add IPC Messages for Tor Control
