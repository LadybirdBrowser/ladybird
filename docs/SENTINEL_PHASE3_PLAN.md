# Sentinel Phase 3: Policy Enforcement & UI Integration

**Timeline**: Week 3 (Days 15-21)
**Status**:  PLANNING
**Prerequisites**:  Phase 1 (Sentinel daemon),  Phase 2 (SecurityTap + IPC)

---

## Executive Summary

Phase 3 transforms Sentinel from a detection-only system to a complete security enforcement platform. This phase adds:
1. **PolicyGraph database** - Persistent storage for security policies
2. **UI security alerts** - User-facing dialogs when threats detected
3. **Policy enforcement** - Block, quarantine, or allow downloads based on policies
4. **Policy management** - Browser UI at `about:security` for managing rules

---

## Architecture Overview

### Current State (Phase 2 Complete)
```
Download → SecurityTap → Sentinel → Alert JSON → IPC → Browser (logs only)
```

### Target State (Phase 3)
```
Download → SecurityTap → Sentinel → Alert JSON → IPC → Browser
    ↓
Check PolicyGraph
    ↓
Match existing policy? → Apply action (allow/block/quarantine)
    ↓
No match? → Show UI dialog → User decision → Create policy → Enforce
```

---

## Day 15-16: PolicyGraph Database

### Goals
- SQLite database for storing security policies
- Schema design for policies and threat history
- Policy matching engine
- CRUD operations API

### Database Schema

#### Table: `policies`
```sql
CREATE TABLE policies (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    rule_name TEXT NOT NULL,           -- YARA rule that matched
    url_pattern TEXT,                  -- Optional: URL pattern (e.g., "*.example.com/*")
    file_hash TEXT,                    -- Optional: SHA256 hash
    mime_type TEXT,                    -- Optional: MIME type filter
    action TEXT NOT NULL,              -- "allow", "block", "quarantine"
    created_at INTEGER NOT NULL,       -- Unix timestamp
    created_by TEXT NOT NULL,          -- "user" or "system"
    expires_at INTEGER,                -- Optional: Unix timestamp for temporary policies
    hit_count INTEGER DEFAULT 0,       -- How many times this policy matched
    last_hit INTEGER                   -- Last time this policy matched (Unix timestamp)
);

CREATE INDEX idx_policies_rule_name ON policies(rule_name);
CREATE INDEX idx_policies_file_hash ON policies(file_hash);
CREATE INDEX idx_policies_url_pattern ON policies(url_pattern);
```

#### Table: `threat_history`
```sql
CREATE TABLE threat_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    detected_at INTEGER NOT NULL,      -- Unix timestamp
    url TEXT NOT NULL,                 -- Download URL
    filename TEXT NOT NULL,            -- Downloaded filename
    file_hash TEXT NOT NULL,           -- SHA256 hash
    mime_type TEXT,                    -- Content-Type
    file_size INTEGER NOT NULL,        -- Size in bytes
    rule_name TEXT NOT NULL,           -- YARA rule that matched
    severity TEXT NOT NULL,            -- "low", "medium", "high", "critical"
    action_taken TEXT NOT NULL,        -- "allowed", "blocked", "quarantined"
    policy_id INTEGER,                 -- Foreign key to policies table
    alert_json TEXT NOT NULL,          -- Full JSON from Sentinel
    FOREIGN KEY (policy_id) REFERENCES policies(id)
);

CREATE INDEX idx_threat_history_detected_at ON threat_history(detected_at);
CREATE INDEX idx_threat_history_rule_name ON threat_history(rule_name);
CREATE INDEX idx_threat_history_file_hash ON threat_history(file_hash);
```

### Implementation Files

**Services/Sentinel/PolicyGraph.h**
```cpp
class PolicyGraph {
public:
    static ErrorOr<NonnullOwnPtr<PolicyGraph>> create(StringView db_path);

    struct Policy {
        i64 id;
        ByteString rule_name;
        Optional<ByteString> url_pattern;
        Optional<ByteString> file_hash;
        Optional<ByteString> mime_type;
        enum Action { Allow, Block, Quarantine } action;
        time_t created_at;
        ByteString created_by;
        Optional<time_t> expires_at;
    };

    struct ThreatMetadata {
        ByteString url;
        ByteString filename;
        ByteString file_hash;
        ByteString mime_type;
        size_t file_size;
        ByteString rule_name;
        ByteString severity;
    };

    // Policy CRUD
    ErrorOr<i64> create_policy(Policy const&);
    ErrorOr<Policy> get_policy(i64 policy_id);
    ErrorOr<Vector<Policy>> list_policies();
    ErrorOr<void> update_policy(i64 policy_id, Policy const&);
    ErrorOr<void> delete_policy(i64 policy_id);

    // Policy matching
    ErrorOr<Optional<Policy>> match_policy(ThreatMetadata const&);

    // Threat history
    ErrorOr<void> record_threat(ThreatMetadata const&, ByteString action_taken,
                                Optional<i64> policy_id, ByteString alert_json);
    ErrorOr<Vector<ThreatRecord>> get_threat_history(Optional<time_t> since);

private:
    PolicyGraph(NonnullRefPtr<SQL::Database>);
    NonnullRefPtr<SQL::Database> m_database;
};
```

**Services/Sentinel/PolicyGraph.cpp**
- SQLite initialization and schema creation
- SQL prepared statements for all operations
- Policy matching algorithm (priority: hash > URL pattern > rule name)
- Expiration handling for temporary policies

### Testing
```bash
# Unit tests
ninja PolicyGraphTests

# Test policy creation
./bin/PolicyGraphTests --test create_policy

# Test policy matching
./bin/PolicyGraphTests --test match_policy
```

---

## Day 17-18: Browser UI Security Alerts

### Goals
- Qt/QML dialog when threat detected
- Display threat details (rule name, severity, description)
- User actions: "Block", "Allow Once", "Always Allow"
- Create policy based on user decision

### UI Components

**Libraries/LibWebView/SecurityAlertDialog.h**
```cpp
class SecurityAlertDialog : public QDialog {
    Q_OBJECT
public:
    struct ThreatDetails {
        QString url;
        QString filename;
        QString rule_name;
        QString severity;
        QString description;
        QString file_hash;
    };

    enum class UserDecision {
        Block,
        AllowOnce,
        AlwaysAllow
    };

    SecurityAlertDialog(ThreatDetails const&, QWidget* parent = nullptr);

    UserDecision decision() const;

signals:
    void userDecided(UserDecision);

private:
    void setup_ui();
    ThreatDetails m_details;
    UserDecision m_decision;
};
```

### Dialog Layout (QML or Qt Widgets)

```
┌─────────────────────────────────────────────────────────┐
│  ⚠  Security Threat Detected                           │
├─────────────────────────────────────────────────────────┤
│                                                           │
│  Sentinel has detected malware in this download:         │
│                                                           │
│  📄 Filename: suspicious.exe                             │
│  🔗 URL: https://malicious.example.com/download          │
│    Rule: EICAR_Test_File                              │
│  ⚡ Severity: Low                                        │
│  📋 Description: EICAR anti-virus test file             │
│                                                           │
│  ───────────────────────────────────────────────────────│
│                                                           │
│  What would you like to do?                              │
│                                                           │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐     │
│  │   🚫 Block  │  │ ✓ Allow Once│  │ ✓ Always Allow│   │
│  └─────────────┘  └─────────────┘  └─────────────┘     │
│                                                           │
│  [ ] Remember this decision (create policy)              │
│                                                           │
└─────────────────────────────────────────────────────────┘
```

### Integration Point

**Libraries/LibRequests/RequestClient.cpp** (modify existing handler)
```cpp
void RequestClient::security_alert(i32 request_id, ByteString alert_json)
{
    // ... existing logging code ...

    // Parse threat details
    auto details = parse_threat_details(alert_json);

    // Check PolicyGraph for existing policy
    auto policy = PolicyGraph::the().match_policy(details);
    if (policy.has_value()) {
        // Apply existing policy
        apply_policy(request_id, policy.value());
        return;
    }

    // No policy - show UI dialog
    auto dialog = SecurityAlertDialog(details);
    auto decision = dialog.exec();

    if (dialog.should_create_policy()) {
        // Create permanent policy from user decision
        PolicyGraph::the().create_policy_from_decision(details, decision);
    }

    // Apply user decision to this download
    apply_decision(request_id, decision);
}
```

---

## Day 19: Policy Enforcement in RequestServer

### Goals
- Intercept downloads before completion
- Apply policy actions (block, quarantine, allow)
- Quarantine directory structure
- Update Request.cpp to respect policies

### Quarantine Implementation

**Directory Structure**:
```
~/.local/share/Ladybird/Sentinel/quarantine/
    ├── YYYY-MM-DD/
    │   ├── <sha256-hash-1>.bin
    │   ├── <sha256-hash-1>.meta.json
    │   ├── <sha256-hash-2>.bin
    │   └── <sha256-hash-2>.meta.json
```

**Metadata JSON** (`<hash>.meta.json`):
```json
{
  "original_filename": "suspicious.exe",
  "original_url": "https://example.com/download",
  "quarantined_at": 1730217600,
  "file_hash": "abc123...",
  "mime_type": "application/x-msdos-program",
  "file_size": 12345,
  "threat_details": {
    "rule_name": "Windows_PE_Suspicious",
    "severity": "medium",
    "description": "Windows executable with suspicious imports"
  }
}
```

### Enforcement Logic

**Services/RequestServer/Request.cpp** (modify existing SecurityTap integration)
```cpp
// After SecurityTap detects threat (around line 571)
if (!scan_result.is_error() && scan_result.value().is_threat) {
    dbgln("SecurityTap: Threat detected in download: {}", metadata.filename);

    // Send IPC alert to browser (existing)
    m_client.async_security_alert(m_request_id, scan_result.value().alert_json.value());

    // NEW: Wait for policy decision from browser
    // Browser will send back enforcement action via new IPC message
    // For now, add flag to pause download until decision received
    m_awaiting_security_decision = true;
    return; // Don't complete download yet
}
```

**New IPC Message**: `RequestServer.ipc`
```ipc
// Browser → RequestServer: Apply security policy decision
enforce_security_policy(i32 request_id, String action) =|
```

**Implementation**:
```cpp
void ConnectionFromClient::enforce_security_policy(i32 request_id, String action)
{
    auto request = m_requests.get(request_id).value_or(nullptr);
    if (!request) {
        dbgln("RequestServer: Cannot enforce policy for non-existent request {}", request_id);
        return;
    }

    if (action == "block"sv) {
        // Delete downloaded content, send error to client
        request->abort_with_error(NetworkError::Blocked);
    } else if (action == "quarantine"sv) {
        // Move file to quarantine directory
        auto quarantine_path = Quarantine::move_to_quarantine(
            request->download_path(),
            request->metadata()
        );
        request->set_quarantined(quarantine_path);
    } else if (action == "allow"sv) {
        // Complete download normally
        request->complete_download();
    }
}
```

---

## Day 20: Policy Management UI

### Goals
- Browser chrome UI at `about:security`
- View all policies
- View threat history
- Create/edit/delete policies manually
- Export/import policy sets

### UI Layout (`about:security`)

```
┌─────────────────────────────────────────────────────────┐
│    Sentinel Security Center                          │
├─────────────────────────────────────────────────────────┤
│  [Policies] [Threat History] [Quarantine] [Settings]    │
├─────────────────────────────────────────────────────────┤
│                                                           │
│  Security Policies (12 active)                           │
│  ┌─────────────────────────────────────────────────┐   │
│  │ EICAR_Test_File                           [Block]│   │
│  │ All downloads matching this rule               │   │
│  │ Created: 2025-10-29 (User)         [Edit][Delete]│   │
│  ├─────────────────────────────────────────────────┤   │
│  │ Windows_PE_Suspicious                   [Quarantine]│ │
│  │ SHA256: abc123...                              │   │
│  │ Created: 2025-10-28 (User)         [Edit][Delete]│   │
│  └─────────────────────────────────────────────────┘   │
│                                                           │
│  [+ Add New Policy]  [Import Policies]  [Export All]    │
│                                                           │
└─────────────────────────────────────────────────────────┘
```

### Implementation

**UI/Qt/SecurityPage.cpp**
- Qt WebEngineView loading internal `about:security` URL
- JavaScript API for policy management
- Real-time updates when policies change

**Services/RequestServer/SecurityProtocol.cpp** (new)
- Custom protocol handler for `about:security`
- Serves HTML/CSS/JS for policy management UI
- REST-like API endpoints:
  - `GET /api/policies` - List all policies
  - `POST /api/policies` - Create new policy
  - `PUT /api/policies/:id` - Update policy
  - `DELETE /api/policies/:id` - Delete policy
  - `GET /api/threats` - List threat history
  - `GET /api/quarantine` - List quarantined files

---

## Day 21: Integration Testing & Refinement

### Goals
- End-to-end testing with real malware samples (EICAR, test payloads)
- Performance profiling
- Documentation updates
- Bug fixes from testing

### Test Scenarios

#### Test 1: Block Policy Enforcement
1. Download EICAR file
2. User selects "Block" + "Remember this decision"
3. Policy created in database
4. Download second EICAR file
5. **Expected**: Automatically blocked without user prompt

#### Test 2: Quarantine Workflow
1. Download suspicious Windows PE
2. User selects "Quarantine"
3. File moved to quarantine directory
4. Visit `about:security/quarantine`
5. View quarantined file details
6. Restore or permanently delete

#### Test 3: Allow Whitelist
1. Download legitimate file flagged by overly-broad rule
2. User selects "Always Allow" for this file hash
3. Policy created
4. Future downloads of same file (same hash) allowed automatically

#### Test 4: Policy Expiration
1. Create temporary policy with 1-hour expiration
2. Verify policy active initially
3. Fast-forward system clock 2 hours
4. Verify policy expired and no longer applied

#### Test 5: URL Pattern Matching
1. Create policy: "Block all .exe from *.malicious.com"
2. Download `http://evil.malicious.com/payload.exe`
3. **Expected**: Blocked by URL pattern policy
4. Download `http://legitimate.com/program.exe`
5. **Expected**: Allowed (different domain)

### Performance Targets
- PolicyGraph lookup: < 5ms per threat
- UI dialog display: < 200ms from IPC message
- Quarantine move: < 100ms for 10MB file
- Database queries: < 10ms for typical policy list

### Documentation Updates
- User guide: How to manage security policies
- Developer guide: PolicyGraph API reference
- Architecture diagram: Complete Sentinel system
- Security best practices for policy creation

---

## Success Criteria

### Phase 3 Complete When:
-  PolicyGraph database operational with all CRUD operations
-  Security alert dialog appears when threat detected
-  User decisions create policies correctly
-  Policies enforced automatically on subsequent threats
-  Quarantine directory functional
-  `about:security` UI functional
-  All integration tests pass
-  Performance targets met

---

## Risk Mitigation

### Risk 1: Database Corruption
- **Mitigation**: Regular backups, SQLite write-ahead logging (WAL)
- **Recovery**: Database schema includes migrations for version upgrades

### Risk 2: UI Blocking Main Thread
- **Mitigation**: Show dialog asynchronously, timeout after 30 seconds
- **Fallback**: Default to "Block" if user doesn't respond

### Risk 3: Quarantine Storage Exhaustion
- **Mitigation**: Automatic cleanup of quarantine older than 30 days
- **Warning**: Alert user when quarantine exceeds 1GB

### Risk 4: Policy Conflicts
- **Mitigation**: Policy priority system (hash > URL > rule name)
- **UI**: Show warning when conflicting policies detected

---

## Out of Scope (Future Phases)

- ❌ Cloud-based threat intelligence feeds
- ❌ Machine learning threat detection
- ❌ Sandboxed execution analysis
- ❌ Network traffic analysis
- ❌ Real-time file system monitoring
- ❌ Multi-user policy management
- ❌ Centralized policy distribution (enterprise)

---

## Next Steps

1. **Day 15**: Begin PolicyGraph database implementation
2. Review schema design with stakeholders
3. Implement SQLite backend with LibDatabase
4. Write unit tests for policy CRUD operations
5. Create sample policies for testing

---

**Prepared By**: Claude Code
**Date**: 2025-10-29
**Status**: Ready to begin Phase 3 implementation
**Estimated Duration**: 7 days (Days 15-21)
