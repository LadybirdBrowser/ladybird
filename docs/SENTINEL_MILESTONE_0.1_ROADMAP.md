# Sentinel Milestone 0.1 - Implementation Roadmap

**Milestone**: Download Vetting MVP
**Timeline**: 4 weeks
**Status**: Ready to Start

---

## Week 1: Sentinel Daemon Foundation

### Day 1-2: Project Setup
**Goal**: Establish development infrastructure

**Tasks**:
- [ ] Create `Services/Sentinel/` directory structure
- [ ] Add Sentinel to CMakeLists.txt build system
- [ ] Configure vcpkg dependency: `yara` library
- [ ] Create initial `main.cpp` with daemon loop skeleton
- [ ] Set up logging infrastructure (AK::DebugLog)

**Deliverables**:
- `Services/Sentinel/main.cpp` (minimal daemon that starts/stops)
- `Services/Sentinel/CMakeLists.txt`
- Updated root `vcpkg.json` with YARA dependency

**Files**:
```
Services/Sentinel/
├── CMakeLists.txt
├── main.cpp
├── Forward.h
└── README.md
```

---

### Day 3-4: UNIX Socket Server
**Goal**: Accept connections from browser

**Tasks**:
- [ ] Implement LocalServer socket listener
- [ ] Define socket path: `/tmp/ladybird-sentinel-{pid}.sock`
- [ ] Handle client connections (one per browser instance)
- [ ] Implement JSON message protocol (send/receive)
- [ ] Add connection state management

**Deliverables**:
- `Services/Sentinel/SocketServer.h/cpp`
- `Services/Sentinel/MessageProtocol.h` (JSON schemas)
- Test client that sends mock artifact

**Socket Message Format**:
```json
{
  "type": "inspect_artifact",
  "request_id": "req_12345",
  "artifact": {
    "url": "https://example.com/file.exe",
    "filename": "installer.exe",
    "mime_type": "application/x-msdownload",
    "sha256": "abc123...",
    "size_bytes": 1024000,
    "content_base64": "TVqQAAMAAAAEAAAA..."
  }
}
```

**Testing**:
```bash
# Start Sentinel daemon
./Sentinel

# In another terminal, send test artifact
echo '{"type":"inspect_artifact","request_id":"test_1",...}' | nc -U /tmp/ladybird-sentinel.sock
```

---

### Day 5-6: YARA Integration
**Goal**: Run YARA rules on artifacts

**Tasks**:
- [ ] Initialize YARA compiler
- [ ] Load rules from directory: `~/.config/ladybird/sentinel/rules/`
- [ ] Implement artifact scanning (Base64 decode → YARA scan)
- [ ] Generate alert JSON from YARA matches
- [ ] Add default ruleset (Windows PE, JS, macro malware)

**Deliverables**:
- `Services/Sentinel/YARAScanner.h/cpp`
- `Services/Sentinel/AlertGenerator.h/cpp`
- Default YARA rules in `Resources/sentinel/rules/`

**Default Rules** (`Resources/sentinel/rules/default.yar`):
```yara
rule EICAR_Test_File {
    meta:
        description = "EICAR anti-virus test file"
        severity = "low"
    strings:
        $eicar = "X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*"
    condition:
        $eicar
}

rule Windows_PE_Suspicious {
    meta:
        description = "Windows executable with suspicious imports"
        severity = "medium"
    strings:
        $mz = {4D 5A}
        $import1 = "CreateRemoteThread"
        $import2 = "VirtualAllocEx"
    condition:
        $mz at 0 and ($import1 or $import2)
}

rule Obfuscated_JavaScript {
    meta:
        description = "Heavily obfuscated JavaScript"
        severity = "medium"
    strings:
        $eval = /eval\s*\(/
        $base64 = /atob\s*\(/
        $long_str = /['"][a-zA-Z0-9+/=]{200,}['"]/
    condition:
        $eval and $base64 and $long_str
}
```

**Testing**:
- Send EICAR test file → verify alert generation
- Send clean PDF → verify no alert
- Send obfuscated JS → verify detection

---

### Day 7: Week 1 Integration & Testing
**Goal**: Standalone Sentinel validation

**Tasks**:
- [ ] End-to-end test: socket → YARA → alert
- [ ] Performance test: scan 100 files, measure latency
- [ ] Memory leak check (Valgrind/ASAN)
- [ ] Error handling: malformed JSON, invalid Base64, etc.

**Acceptance Criteria**:
-  Sentinel starts and listens on UNIX socket
-  Accepts artifact messages from test client
-  YARA scan completes in < 100ms for 1MB files
-  Alert JSON sent back to client
-  No memory leaks or crashes

---

## Week 2: Browser Integration (SecurityTap)

### Day 8-9: SecurityTap Module
**Goal**: Hook into RequestServer download completion

**Tasks**:
- [ ] Create `Services/RequestServer/SecurityTap.h/cpp`
- [ ] Initialize Sentinel socket connection in RequestServer startup
- [ ] Hook `Request::on_headers_received()` for download detection
- [ ] Extract download metadata (URL, filename, MIME, content)
- [ ] Compute SHA256 hash of download content

**Deliverables**:
- `Services/RequestServer/SecurityTap.h/cpp`
- Modified `Services/RequestServer/main.cpp` (init SecurityTap)
- Modified `Services/RequestServer/Request.cpp` (hook download complete)

**Integration Point**:
```cpp
// In Request.cpp, after download completes
void Request::on_data_complete() {
    // Existing code...

    // NEW: Send to Sentinel for inspection
    if (m_security_tap) {
        auto result = m_security_tap->inspect_download(
            m_url.to_byte_string(),
            extract_filename_from_url(m_url),
            m_response_headers.get("Content-Type").value_or("application/octet-stream"),
            compute_sha256(m_received_data),
            m_received_data.bytes()
        );

        if (result.is_error()) {
            dbgln("SecurityTap: Failed to inspect download: {}", result.error());
            // Continue download anyway (fail-open)
        }
    }

    // Continue with existing flow...
}
```

---

### Day 10-11: RequestClient IPC Extension
**Goal**: Pass Sentinel alerts to browser UI

**Tasks**:
- [ ] Add `security_alert` message to `RequestClient.ipc`
- [ ] Implement `ConnectionFromClient::send_security_alert()`
- [ ] Handle alert in `Libraries/LibRequests/RequestClient.cpp`
- [ ] Create callback mechanism for UI to receive alerts

**New IPC Message** (`RequestClient.ipc`):
```
endpoint RequestClient
{
    // Existing messages...

    security_alert(i32 request_id, ByteString alert_json) =|
}
```

**Testing**:
- Download EICAR file → verify alert IPC message sent
- Check LibRequests receives alert callback

---

### Day 12-13: Policy Graph Database
**Goal**: Persistent storage for policies and alerts

**Tasks**:
- [ ] Create `Libraries/LibWebView/PolicyGraph.h/cpp`
- [ ] Initialize SQLite database: `~/.config/ladybird/security/policies.db`
- [ ] Implement schema (alerts, policies, enforcement_log tables)
- [ ] Add CRUD operations for policies
- [ ] Implement policy matching engine

**Deliverables**:
- `Libraries/LibWebView/PolicyGraph.h/cpp`
- SQL schema in `PolicyGraph::initialize_database()`
- Unit tests for policy matching

**Policy Matching Example**:
```cpp
// Check if download should be blocked
Optional<Policy> PolicyGraph::match_download_policy(
    ByteString const& origin_domain,
    ByteString const& filename,
    ByteString const& mime_type
) {
    auto file_ext = extract_extension(filename);

    // Query active policies
    auto stmt = m_db.prepare(
        "SELECT * FROM policies WHERE status = 'active' AND "
        "json_extract(match_pattern, '$.type') = 'download_origin_filetype'"
    );

    while (stmt.step() == SQLITE_ROW) {
        auto policy = Policy::from_row(stmt);
        auto pattern = policy.match_pattern;

        if (pattern.origin_domain == origin_domain &&
            pattern.file_extensions.contains(file_ext)) {
            return policy;
        }
    }

    return {};  // No match
}
```

---

### Day 14: Week 2 Integration Testing
**Goal**: End-to-end flow from download to policy check

**Tasks**:
- [ ] Test: Download file → SecurityTap → Sentinel → alert → IPC
- [ ] Test: Create policy manually in database
- [ ] Test: Policy matching logic with various patterns
- [ ] Performance: measure IPC overhead

**Acceptance Criteria**:
-  Downloads trigger SecurityTap inspection
-  Sentinel alerts sent to browser via IPC
-  PolicyGraph can store and query policies
-  Policy matching works for download patterns

---

## Week 3: UI Integration & Policy Enforcement

### Day 15-16: Security Alert Dialog
**Goal**: Show alerts to user with policy creation options

**Tasks**:
- [ ] Create `UI/Qt/SecurityAlertDialog.h/cpp`
- [ ] Design dialog layout (Qt Quick or QWidget)
- [ ] Display alert details (source, severity, tech info)
- [ ] Add action buttons: [Block Future] [Allow Once] [Quarantine]
- [ ] Handle button clicks → create policy or allow download

**Deliverables**:
- `UI/Qt/SecurityAlertDialog.h/cpp` (or AppKit/UIKit equivalent)
- Modified `UI/Qt/BrowserWindow.cpp` (show dialog on alert)

**Dialog Mockup**:
```
┌──────────────────────────────────────────────────┐
│ ⚠ Security Alert                                 │
├──────────────────────────────────────────────────┤
│ This file matches a known malware signature.    │
│                                                  │
│ File: installer.exe                             │
│ Source: https://example-bad-site.ru/payload     │
│ Detection: Win32_Trojan_Generic (YARA)          │
│                                                  │
│ [Show Technical Details ▼]                      │
│                                                  │
│ What would you like to do?                      │
│                                                  │
│ [ Block future downloads from this site ]       │
│ [ Allow this download only               ]       │
│ [ Quarantine for review                  ]       │
│                                                  │
│                       [Cancel]  [Apply Policy]   │
└──────────────────────────────────────────────────┘
```

---

### Day 17-18: Policy Creation Flow
**Goal**: Create PolicyGraph entries from user decisions

**Tasks**:
- [ ] Implement "Block future" → policy creation
- [ ] Generate match pattern from alert context
- [ ] Save policy to PolicyGraph database
- [ ] Show confirmation notification
- [ ] Add policy to in-memory cache for fast lookups

**Example Flow**:
```cpp
// User clicks [Block future downloads from this site]
void SecurityAlertDialog::on_block_future_clicked() {
    auto policy = Policy::create_from_alert(
        m_alert,
        Policy::MatchPattern::download_origin_filetype(
            extract_domain(m_alert.url),
            {extract_extension(m_alert.filename)}
        ),
        Policy::EnforcementAction::quarantine_download
    );

    m_policy_graph.save_policy(policy);

    // Show notification
    show_notification("Policy created: Future downloads from "
                     "{} will be quarantined.", extract_domain(m_alert.url));

    close();
}
```

---

### Day 19-20: Policy Enforcer Hooks
**Goal**: Automatically enforce policies without user prompts

**Tasks**:
- [ ] Create `Libraries/LibWebView/PolicyEnforcer.h/cpp`
- [ ] Hook download save path in RequestServer
- [ ] Query PolicyGraph for matching policies
- [ ] Implement quarantine directory: `~/.config/ladybird/quarantine/`
- [ ] Log enforcement events to database

**Deliverables**:
- `Libraries/LibWebView/PolicyEnforcer.h/cpp`
- Modified RequestServer download handling
- Quarantine directory management

**Enforcement Logic**:
```cpp
// Before saving download to filesystem
ErrorOr<void> Request::save_download(ByteString const& path) {
    auto policy = m_policy_enforcer.match_download_policy(
        extract_domain(m_url),
        extract_filename(path),
        m_mime_type
    );

    if (policy.has_value()) {
        switch (policy->enforcement_action) {
        case Policy::EnforcementAction::quarantine_download:
            return quarantine_file(path, policy->policy_id);
        case Policy::EnforcementAction::block_download:
            return Error::from_string_literal("Blocked by security policy");
        }
    }

    // No policy match, proceed normally
    return save_to_filesystem(path);
}
```

---

### Day 21: Week 3 Integration Testing
**Goal**: Full loop validation

**Tests**:
1. **First download from malicious site**:
   - Download triggers alert dialog
   - User selects "Block future"
   - Policy created in database
   - Download quarantined

2. **Second download from same site**:
   - No alert dialog (policy exists)
   - Download auto-quarantined
   - Notification shown: "Blocked by policy pol_xyz"
   - Enforcement logged to database

**Acceptance Criteria**:
-  Alert dialog appears on first detection
-  User can create policy from dialog
-  Subsequent downloads auto-enforce policy
-  Quarantined files saved to correct directory

---

## Week 4: Review UI & Polish

### Day 22-23: `about:security` WebUI
**Goal**: Policy management interface

**Tasks**:
- [ ] Create `Libraries/LibWebView/WebUI/SecurityUI.h/cpp`
- [ ] Implement HTML/CSS for security dashboard
- [ ] Display pending alerts (Amplification Candidates)
- [ ] Display active policies with edit/disable buttons
- [ ] Show enforcement history (recent auto-blocks)
- [ ] Add statistics: total alerts, policies, threats blocked

**Deliverables**:
- `Libraries/LibWebView/WebUI/SecurityUI.h/cpp`
- Registered URL handler for `about:security`

**UI Sections**:
```html
<div class="security-dashboard">
  <section id="stats">
    <h2>Security Overview</h2>
    <div class="stat">
      <span class="label">Total Alerts</span>
      <span class="value">42</span>
    </div>
    <div class="stat">
      <span class="label">Active Policies</span>
      <span class="value">12</span>
    </div>
    <div class="stat">
      <span class="label">Threats Blocked</span>
      <span class="value">156</span>
    </div>
  </section>

  <section id="pending-alerts">
    <h2>Pending Alerts</h2>
    <!-- List of alerts awaiting user decision -->
  </section>

  <section id="active-policies">
    <h2>Active Policies</h2>
    <!-- Table of policies with edit/disable/delete -->
  </section>

  <section id="enforcement-log">
    <h2>Recent Enforcements</h2>
    <!-- Log of automatic policy applications -->
  </section>
</div>
```

---

### Day 24-25: Notification System
**Goal**: Non-intrusive alerts for auto-enforcements

**Tasks**:
- [ ] Create notification banner component
- [ ] Show banner when policy auto-blocks download
- [ ] Add "View Policy" link → opens `about:security`
- [ ] Implement auto-dismiss (5 seconds) or manual close

**Banner Example**:
```
┌────────────────────────────────────────────────────┐
│  Download blocked by security policy             │
│ installer.exe from example-bad-site.ru             │
│ [View Policy] [Dismiss]                            │
└────────────────────────────────────────────────────┘
```

---

### Day 26: Performance Optimization
**Goal**: Minimize browser impact

**Tasks**:
- [ ] Profile download path with SecurityTap enabled
- [ ] Optimize YARA scanning (stream large files)
- [ ] Cache policy lookups (avoid DB query per download)
- [ ] Async IPC (don't block download UI thread)

**Benchmarks**:
- Download 10MB file: measure time with/without Sentinel
- Target: < 5% overhead
- Download 100MB file: stream scan, don't load entire file

---

### Day 27: Documentation
**Goal**: User-facing guides

**Deliverables**:
- [ ] `docs/SENTINEL_USER_GUIDE.md`
- [ ] `docs/SENTINEL_POLICY_GUIDE.md`
- [ ] `docs/SENTINEL_YARA_RULES.md`
- [ ] Update main README with Sentinel section

**User Guide Contents**:
1. What is Sentinel?
2. How download vetting works
3. Creating policies from alerts
4. Managing policies in `about:security`
5. Writing custom YARA rules
6. Troubleshooting

---

### Day 28: Final Testing & Release Prep
**Goal**: Validate Milestone 0.1 completion

**Test Suite**:
1.  Download EICAR file → alert → create policy → re-download → auto-block
2.  Download clean file → no alert
3.  Disable Sentinel → downloads work normally
4.  Restart browser → policies persist
5.  Create custom YARA rule → verify detection
6.  Policy management in `about:security`
7.  Performance: < 5% overhead for typical downloads

**Bug Fixes**:
- Address any issues found during testing
- Memory leak fixes (ASAN/Valgrind)
- UI polish (button labels, error messages)

**Release Checklist**:
- [ ] All acceptance criteria met
- [ ] Documentation complete
- [ ] No critical bugs
- [ ] Feature branch merged to main
- [ ] Tag release: `v0.1.0-sentinel-mvp`

---

## Success Metrics

### Functional
-  Sentinel daemon compiles and runs
-  Browser intercepts downloads
-  YARA detections trigger alerts
-  User can create policies
-  Policies auto-enforce on subsequent downloads
-  `about:security` UI functional

### Performance
-  Download overhead < 5% for 10MB files
-  YARA scan < 100ms for 1MB files
-  Policy query < 5ms
-  No memory leaks or crashes

### User Experience
-  Alert dialog is clear and non-technical
-  Policy creation is intuitive (one click)
-  Auto-enforcement is transparent (notification banner)
-  Review UI is informative and actionable

---

## Risk Mitigation

### Risk: YARA performance on large files
**Mitigation**: Stream scanning, don't load entire file into memory

### Risk: Sentinel crashes
**Mitigation**: Graceful degradation (browser continues without scanning)

### Risk: Policy database corruption
**Mitigation**: Backup on startup, integrity checks, repair tool

### Risk: User confusion about policies
**Mitigation**: Clear documentation, in-app help, examples in UI

---

## Next Milestones (Preview)

### Milestone 0.2: Credential Exfiltration Detection
- Add Flow Inspector to Sentinel
- Detect login forms posting to mismatched domains
- Policy enforcement: block autofill for suspicious forms

### Milestone 0.3: Suricata Integration
- Add Signature Inspector
- Detect malicious inline scripts
- Block cryptominers, redirect chains

---

## Team Assignments (If Applicable)

**Backend (Sentinel, SecurityTap)**:
- Implement Sentinel daemon
- YARA integration
- IPC protocol

**Frontend (UI, Policy Enforcer)**:
- Security alert dialog
- `about:security` WebUI
- Notification system

**Database (PolicyGraph)**:
- SQLite schema
- Policy matching engine
- Enforcement logging

**QA (Testing)**:
- Test plan execution
- Performance benchmarks
- Bug triage

---

**Roadmap Version**: 1.0
**Last Updated**: 2025-10-28
