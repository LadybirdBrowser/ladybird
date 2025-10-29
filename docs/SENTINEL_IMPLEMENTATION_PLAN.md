# Ladybird Sentinel - Implementation Plan

**Status**: Design Complete - Ready for Implementation
**Version**: 0.1
**Created**: 2025-10-28

## Executive Summary

This document outlines the implementation plan for **Ladybird Sentinel**, a security architecture that transforms Ladybird browser into an emergent security system through live traffic analysis, signature matching, and behavioral learning.

**Core Innovation**: The browser learns security policies from human decisions, creating compound defenses that strengthen over time (Emergent Amplification).

---

## 1. Architecture Overview

### 1.1 Component Ecosystem

```
┌─────────────────────────────────────────────────────────────┐
│                    LADYBIRD BROWSER                         │
│  ┌──────────────────────────────────────────────────────┐  │
│  │              SecurityTap Module                       │  │
│  │  - Post-TLS traffic mirror                           │  │
│  │  - Download artifact capture                         │  │
│  │  - Metadata extraction                               │  │
│  └────────────────┬─────────────────────────────────────┘  │
└──────────────────┼──────────────────────────────────────────┘
                   │ IPC (localhost socket)
                   ▼
┌─────────────────────────────────────────────────────────────┐
│              SENTINEL SECURITY DAEMON                        │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  Flow Inspector (Zeek-like)                          │  │
│  │  - Behavioral event detection                        │  │
│  │  - Credential exfil patterns                         │  │
│  │  - Domain mismatch detection                         │  │
│  └──────────────────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  Signature Inspector (Suricata-like)                 │  │
│  │  - Threat intel matching                             │  │
│  │  - Known exploit signatures                          │  │
│  │  - Malicious script patterns                         │  │
│  └──────────────────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  Artifact Inspector (YARA)                           │  │
│  │  - File/blob fingerprinting                          │  │
│  │  - Malware family detection                          │  │
│  │  - Binary/script analysis                            │  │
│  └──────────────────────────────────────────────────────┘  │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  Alert Normalization & Bus                           │  │
│  └────────────────┬─────────────────────────────────────┘  │
└──────────────────┼──────────────────────────────────────────┘
                   │ IPC (structured alerts)
                   ▼
┌─────────────────────────────────────────────────────────────┐
│                  POLICY GRAPH (SQLite DB)                    │
│  - Alert history                                             │
│  - Approved policies                                         │
│  - Enforcement logs                                          │
│  - Amplification candidates                                  │
└──────────────────┬──────────────────────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────────────────────┐
│              POLICY ENFORCER (in browser)                    │
│  - Autofill gate                                             │
│  - Download quarantine                                       │
│  - Permission blocking                                       │
│  - Script execution control                                  │
└──────────────────────────────────────────────────────────────┘
```

### 1.2 Integration with Existing Ladybird

**Ladybird already has**:
- Independent network stack (RequestServer with CURL backend)
- Multi-process architecture (RequestServer is separate service)
- IPC infrastructure (LibIPC with message passing)
- Per-page network isolation (NetworkIdentity system)
- Custom protocol support (IPFS, IPNS, ENS already implemented)
- Download handling hooks

**What we add**:
- SecurityTap module in RequestServer
- Sentinel daemon (new process)
- Policy Graph database
- Policy Enforcer hooks in browser UI
- Review UI (`ladybird://security`)

---

## 2. Milestone 0.1 - Download Vetting MVP

**Goal**: Prove Emergent Amplification pattern with minimal complexity

**Scope**: Before any downloaded file is saved/opened, fingerprint it, scan it, and learn from user decisions.

### 2.1 Success Criteria

1. Browser intercepts completed downloads
2. Files are sent to Sentinel for YARA scanning
3. YARA detections trigger user prompts with policy options
4. User approval creates persistent Policy Graph entries
5. Subsequent matching downloads auto-enforce without prompts
6. All functionality works without kernel drivers or admin privileges

### 2.2 Implementation Components

#### Component 1: SecurityTap (in RequestServer)

**File**: `Services/RequestServer/SecurityTap.h`

```cpp
#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/Socket.h>

namespace RequestServer {

class SecurityTap {
public:
    static ErrorOr<NonnullOwnPtr<SecurityTap>> create();

    // Send download artifact to Sentinel
    ErrorOr<void> inspect_download(
        ByteString const& url,
        ByteString const& filename,
        ByteString const& mime_type,
        ByteString const& sha256,
        ReadonlyBytes content
    );

private:
    SecurityTap(NonnullOwnPtr<Core::LocalSocket> sentinel_socket);

    NonnullOwnPtr<Core::LocalSocket> m_sentinel_socket;
};

}
```

**Integration Point**: Hook into Request::on_data_complete()

#### Component 2: Sentinel Daemon

**Process**: New standalone binary `Services/Sentinel/main.cpp`

**Core Loop**:
1. Listen on UNIX socket (`/tmp/ladybird-sentinel.sock`)
2. Receive artifact messages from browser
3. Run YARA rules on content
4. Send alert back to browser via IPC

**YARA Integration**:
- Embed `libyara` as vendored dependency
- Load rules from `~/.config/ladybird/sentinel/rules/`
- Default ruleset: common malware families (PE, JS, macro-based)

**Alert Schema**:
```json
{
  "alert_id": "uuid-v4",
  "source": "yara",
  "severity": "critical",
  "what_happened": "File matches known trojan signature",
  "tech_details": {
    "url": "https://example-bad-site.ru/payload.exe",
    "sha256": "abc123...",
    "yara_rule": "Win32_Trojan_Generic",
    "matched_strings": ["$mz_header", "$suspicious_import_1"]
  },
  "suggested_actions": [
    "quarantine_download",
    "block_future_from_origin",
    "scan_similar_files"
  ],
  "context_fingerprint": "sha256-of-pattern"
}
```

#### Component 3: Policy Graph

**File**: `Libraries/LibWebView/PolicyGraph.h`

**Storage**: SQLite database at `~/.config/ladybird/security/policies.db`

**Schema**:
```sql
CREATE TABLE alerts (
    alert_id TEXT PRIMARY KEY,
    timestamp INTEGER NOT NULL,
    source TEXT NOT NULL,
    severity TEXT NOT NULL,
    details JSON NOT NULL
);

CREATE TABLE policies (
    policy_id TEXT PRIMARY KEY,
    created_at INTEGER NOT NULL,
    created_from_alert_id TEXT REFERENCES alerts(alert_id),
    match_pattern JSON NOT NULL,
    enforcement_action TEXT NOT NULL,
    status TEXT NOT NULL, -- 'active', 'disabled', 'expired'
    times_enforced INTEGER DEFAULT 0
);

CREATE TABLE enforcement_log (
    log_id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,
    policy_id TEXT REFERENCES policies(policy_id),
    context JSON NOT NULL
);

CREATE INDEX idx_policies_active ON policies(status) WHERE status = 'active';
CREATE INDEX idx_enforcement_timestamp ON enforcement_log(timestamp);
```

**Example Policy**:
```json
{
  "policy_id": "pol_20251028_001",
  "match_pattern": {
    "type": "download_origin_filetype",
    "origin_domain": "example-bad-site.ru",
    "file_extension": ["exe", "msi", "bat"]
  },
  "enforcement_action": "quarantine_download",
  "created_at": 1698512400,
  "created_from_alert_id": "alert_xyz",
  "status": "active"
}
```

#### Component 4: Policy Enforcer

**File**: `Libraries/LibWebView/PolicyEnforcer.h`

**Hooks**:
- `check_download_allowed(url, filename, mime) -> PolicyDecision`
- `check_autofill_allowed(domain, form_action) -> PolicyDecision`
- `check_permission_allowed(origin, permission_type) -> PolicyDecision`

**Integration**: Called from WebContentClient before allowing actions

#### Component 5: Review UI

**URL**: `ladybird://security`

**Sections**:
1. **Pending Alerts**: Awaiting user decision (Amplification Candidates)
2. **Active Policies**: Currently enforced rules with edit/disable options
3. **Enforcement History**: Log of automatic policy applications
4. **Statistics**: Total alerts, policies created, threats blocked

**Implementation**: LibWebView::WebUI extension (similar to existing `ladybird://processes`)

### 2.3 Data Flow - Download Vetting

```
[1] User clicks download link in web page
     ↓
[2] RequestServer completes download (Request.cpp)
     ↓
[3] SecurityTap::inspect_download() called with:
     - URL, filename, MIME type, SHA256, content bytes
     ↓
[4] Artifact sent to Sentinel via UNIX socket
     ↓
[5] Sentinel runs YARA rules on content
     ↓
[6a] No match → Sentinel sends "clean" response
     → Download proceeds normally

[6b] YARA match → Sentinel sends alert JSON
     ↓
[7] Browser UI shows prompt:
     "⚠ This file matches known malware signature [XYZ].

      [Block future from this site] [Allow this one] [Quarantine]"
     ↓
[8] User selects [Block future from this site]
     ↓
[9] PolicyGraph creates entry:
     match: {origin: "bad-site.ru", type: "executable"}
     action: "quarantine_download"
     ↓
[10] Download quarantined to ~/.config/ladybird/quarantine/
     ↓
[FUTURE] Next download from bad-site.ru:
     PolicyEnforcer checks PolicyGraph
     → Automatic quarantine, no prompt
     → Banner: "Auto-blocked per policy pol_20251028_001"
```

### 2.4 Implementation Tasks

**Phase 1: Sentinel Daemon Foundation** (Week 1)
- [ ] Create `Services/Sentinel/` directory structure
- [ ] Implement UNIX socket server (LibCore::LocalServer)
- [ ] Add YARA library integration (vcpkg: `yara` package)
- [ ] Create artifact message protocol (JSON over socket)
- [ ] Implement alert generation and normalization
- [ ] Add default YARA ruleset (common malware signatures)
- [ ] Test standalone: send test files, verify alerts

**Phase 2: SecurityTap Integration** (Week 1-2)
- [ ] Add SecurityTap class to RequestServer
- [ ] Hook Request completion callback
- [ ] Extract download metadata (URL, filename, MIME, hash)
- [ ] Send artifacts to Sentinel socket
- [ ] Handle Sentinel responses (clean/alert)
- [ ] Add RequestClient.ipc message for security alerts

**Phase 3: Policy Graph Database** (Week 2)
- [ ] Create PolicyGraph class (LibWebView)
- [ ] Initialize SQLite database with schema
- [ ] Implement policy CRUD operations
- [ ] Add policy matching engine
- [ ] Add enforcement logging
- [ ] Test: create policies, query, match patterns

**Phase 4: Policy Enforcer Hooks** (Week 2-3)
- [ ] Add PolicyEnforcer class
- [ ] Hook download save path (before filesystem write)
- [ ] Query PolicyGraph for matching policies
- [ ] Implement quarantine directory handling
- [ ] Add enforcement logging to database
- [ ] Test: trigger policies, verify enforcement

**Phase 5: Browser UI** (Week 3)
- [ ] Create security alert dialog component
- [ ] Add "Create Policy" flow from alert
- [ ] Implement policy approval/rejection
- [ ] Add quarantine notification banner
- [ ] Create `ladybird://security` WebUI page
- [ ] Display pending alerts and active policies

**Phase 6: Integration Testing** (Week 4)
- [ ] End-to-end test: download malicious test file
- [ ] Verify YARA detection and alert
- [ ] Create policy via UI
- [ ] Download same file again → verify auto-quarantine
- [ ] Test policy management (disable, re-enable)
- [ ] Performance test: download speed impact

**Phase 7: Documentation** (Week 4)
- [ ] User guide: How Sentinel works
- [ ] YARA rule authoring guide
- [ ] Policy management documentation
- [ ] Security architecture documentation
- [ ] Deployment guide (enable/disable Sentinel)

---

## 3. Milestone 0.2 - Credential Exfiltration Detection

**Goal**: Expand to behavioral analysis (Zeek-style)

**New Detection**: Login forms that exfiltrate credentials to mismatched domains

### 3.1 Flow Inspector Integration

**Zeek Alternative**: Lightweight custom protocol analyzer

**Detection Logic**:
```cpp
// Pseudo-code for credential exfil detection
if (request.method == "POST" &&
    request.content_type.contains("urlencoded") &&
    request.body.contains_credential_fields() &&
    extract_domain(request.url) != extract_domain(page_origin)) {

    emit_alert({
        type: "credential_exfil",
        severity: "high",
        message: "Login form POST to different domain",
        expected_domain: page_origin_domain,
        actual_domain: request_domain,
        suggested_action: "block_autofill_for_this_domain"
    });
}
```

**Integration Point**: SecurityTap intercepts POST requests, not just downloads

**Policy Example**:
```json
{
  "match_pattern": {
    "type": "credential_post_mismatch",
    "page_domain": "paypal.com",
    "post_domain": "paypa1-login.ru"
  },
  "enforcement_action": "disable_autofill_and_warn"
}
```

---

## 4. Milestone 0.3 - Suricata Signature Integration

**Goal**: Add IDS-style signature matching for scripts and network patterns

### 4.1 Signature Inspector

**Suricata Alternative**: Custom signature engine or embedded Suricata library

**Signatures Target**:
- Inline `<script>` tags with obfuscation patterns
- Cross-origin iframe injection attempts
- Known cryptominer JavaScript fingerprints
- Malicious redirect chains

**Example Signature**:
```
alert http any any -> any any (
  msg:"Cryptominer detected - CoinHive variant";
  flow:established,to_server;
  content:"coinhive.min.js";
  classtype:trojan-activity;
  sid:1000001;
)
```

**Integration**: SecurityTap sends HTTP response bodies to Sentinel for signature matching

---

## 5. Long-Term Vision (Post-MVP)

### 5.1 In-Process Deep Hooks

**Migration Path**: Move security analysis inline for IPS-level enforcement

**Changes**:
- Zeek-style protocol analyzers in RequestServer network stack
- Suricata signature matching on socket read path
- YARA scanning before blob execution (not just download)

**Benefits**:
- Block threats before content reaches renderer
- Inline policy enforcement (IPS vs IDS)
- Lower latency (no IPC round-trip)

**Risks**:
- Browser stability (crash in security code = crash browser)
- Performance impact (must be async, non-blocking)
- Complexity (harder to debug, update)

### 5.2 ML-Based Anomaly Detection

**Future Enhancement**: Add unsupervised learning for zero-day threats

**Approach**:
- Train model on "normal" browsing patterns
- Detect statistical anomalies (unusual POST sizes, rare domains, etc.)
- Generate Amplification Candidates from anomalies

**Not in MVP**: Requires extensive data collection and training infrastructure

### 5.3 Federated Threat Intelligence

**Concept**: Optional sharing of Policy Graph fingerprints

**Privacy-Preserving**: Share only hashed patterns, not raw data

**Benefit**: Community-driven threat detection (browser learns from all users)

---

## 6. Technical Specifications

### 6.1 IPC Protocol - SecurityTap → Sentinel

**Transport**: UNIX domain socket (LocalSocket)

**Message Format**: JSON Lines (newline-delimited JSON)

**Artifact Message**:
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

**Alert Response**:
```json
{
  "type": "alert",
  "request_id": "req_12345",
  "alert": {
    "alert_id": "alert_uuid",
    "source": "yara",
    "severity": "critical",
    "what_happened": "Malware detected",
    "tech_details": { ... },
    "suggested_actions": [...]
  }
}
```

**Clean Response**:
```json
{
  "type": "clean",
  "request_id": "req_12345"
}
```

### 6.2 Policy Matching Algorithm

**Input**: Download/action context (URL, filename, MIME, domain, etc.)

**Algorithm**:
```
FOR EACH active_policy IN PolicyGraph:
    pattern = active_policy.match_pattern

    IF pattern.type == "download_origin_filetype":
        IF context.origin_domain MATCHES pattern.origin_domain AND
           context.file_extension IN pattern.file_extensions:
            RETURN policy_match(active_policy)

    ELIF pattern.type == "credential_post_mismatch":
        IF context.page_domain == pattern.page_domain AND
           context.post_domain == pattern.post_domain:
            RETURN policy_match(active_policy)

    // ... other pattern types

IF no_match_found:
    RETURN allow_with_sentinel_check()
```

### 6.3 YARA Rule Management

**Rule Directories**:
- System: `/usr/share/ladybird/sentinel/rules/` (default ruleset)
- User: `~/.config/ladybird/sentinel/rules/` (custom rules)

**Auto-Update**: Optional fetch from threat intel feeds (e.g., YARAify)

**Rule Format**: Standard YARA syntax

**Example Rule**:
```yara
rule Win32_Trojan_Generic {
    meta:
        description = "Generic Windows trojan pattern"
        severity = "high"
    strings:
        $mz_header = { 4D 5A }
        $sus_import_1 = "CreateRemoteThread"
        $sus_import_2 = "WriteProcessMemory"
    condition:
        $mz_header at 0 and 2 of ($sus_import*)
}
```

---

## 7. Security Considerations

### 7.1 Sentinel Process Isolation

**Sandboxing**: Sentinel runs as unprivileged user process

**Resource Limits**:
- Max memory: 256MB
- Max CPU: 50% of one core
- Max file descriptors: 1024

**Failure Mode**: If Sentinel crashes, browser continues without security scanning (graceful degradation)

### 7.2 Policy Graph Integrity

**Threat**: Malware modifies policy database to disable protections

**Mitigation**:
- SQLite database with filesystem permissions (600)
- Optional: Sign policies with browser keypair
- Detect tampering on startup (checksum validation)

### 7.3 Privacy

**Local-Only**: All analysis happens on user's machine, no cloud services

**No Telemetry**: Sentinel does not phone home by default

**Optional Sharing**: User can opt-in to federated threat intel (future feature)

---

## 8. Performance Requirements

### 8.1 Download Vetting Latency

**Target**: < 100ms overhead for typical downloads (< 10MB)

**Breakdown**:
- IPC communication: < 10ms
- YARA scanning: < 80ms (for 10MB file)
- Policy query: < 5ms
- UI rendering: < 5ms

**Large Files**: Stream YARA scanning (don't load entire file into memory)

### 8.2 Memory Footprint

**Sentinel Daemon**: < 100MB resident memory (idle)

**YARA Rules**: ~10MB for comprehensive ruleset

**Policy Graph**: < 1MB for typical user (1000 policies)

### 8.3 Browser Impact

**Target**: < 2% CPU overhead during normal browsing

**Measurement**: Profile with large downloads and complex policies

---

## 9. Testing Strategy

### 9.1 Unit Tests

- YARA rule matching (known malware samples)
- Policy matching algorithm (various patterns)
- Alert serialization/deserialization
- SQLite operations (CRUD, queries)

### 9.2 Integration Tests

- End-to-end download vetting flow
- Policy creation from alert
- Automatic enforcement on subsequent downloads
- Sentinel daemon lifecycle (start, stop, restart)

### 9.3 Performance Tests

- Download throughput with/without Sentinel
- YARA scanning speed vs file size
- Policy query speed vs database size
- Memory usage under load

### 9.4 Security Tests

- Bypass attempts (malformed messages, socket injection)
- Privilege escalation (Sentinel accessing browser memory)
- Policy tampering detection

---

## 10. Deployment Plan

### 10.1 Feature Flags

**Compile-Time**:
```cmake
option(ENABLE_SENTINEL "Enable Sentinel security subsystem" ON)
```

**Runtime**:
- `ladybird://flags` toggle for Sentinel
- Sentinel auto-starts when browser launches (if enabled)
- Graceful fallback if Sentinel unavailable

### 10.2 User Experience

**First Launch**:
- Sentinel starts automatically
- No user intervention required
- Status indicator in browser chrome (optional)

**Alerts**:
- Non-blocking notifications
- User can dismiss or create policy
- History in `ladybird://security`

**Power Users**:
- Custom YARA rules
- Policy export/import
- Sentinel logs and debugging

---

## 11. Success Metrics

### 11.1 Milestone 0.1 Acceptance Criteria

1. ✅ Sentinel daemon compiles and runs on Linux, macOS, Windows
2. ✅ Browser intercepts downloads and sends to Sentinel
3. ✅ YARA detection triggers alert in browser UI
4. ✅ User can create policy from alert
5. ✅ Subsequent matching downloads auto-quarantine
6. ✅ `ladybird://security` displays policies and enforcement history
7. ✅ No crashes or hangs during normal browsing
8. ✅ Download speed impact < 5% for files < 10MB

### 11.2 Real-World Validation

**Test Scenarios**:
1. Download EICAR test file → verify YARA detection
2. Download from known malware distribution site → verify blocking
3. Create policy for `.exe` from specific domain → verify enforcement
4. Disable Sentinel → verify browser still functional
5. Restart browser → verify policies persist

---

## 12. Documentation Deliverables

1. **Architecture Document** (this file)
2. **User Guide**: How to use Sentinel features
3. **Developer Guide**: How to extend Sentinel (custom analyzers)
4. **YARA Rule Guide**: Writing effective rules
5. **Policy Management Guide**: Creating, editing, sharing policies
6. **Troubleshooting Guide**: Common issues and solutions

---

## 13. Open Questions

1. **YARA Licensing**: Confirm BSD-3-Clause compatibility with Ladybird
2. **Sentinel Lifecycle**: Auto-restart on crash? Watchdog process?
3. **Mobile Support**: Android Sentinel implementation (different IPC?)
4. **Rule Updates**: Auto-fetch from threat feeds? User opt-in?
5. **Policy Conflicts**: What if multiple policies match? Priority system?

---

## 14. Next Steps

**Immediate (This Week)**:
1. ✅ Get stakeholder approval on architecture
2. Create feature branch: `feature/sentinel-mvp`
3. Set up Sentinel directory structure
4. Add YARA dependency to vcpkg manifest
5. Begin Phase 1 implementation (Sentinel daemon)

**Short-Term (Month 1)**:
- Complete Milestone 0.1 implementation
- Internal testing and iteration
- Performance profiling and optimization

**Medium-Term (Month 2-3)**:
- Public beta release
- Community YARA rule contributions
- Milestone 0.2 planning (credential detection)

**Long-Term (Month 6+)**:
- Milestone 0.3 (Suricata integration)
- ML-based anomaly detection research
- Federated threat intelligence design

---

## 15. References

### 15.1 External Projects

- **YARA**: https://virustotal.github.io/yara/
- **Zeek**: https://zeek.org/
- **Suricata**: https://suricata.io/
- **Ladybird**: https://github.com/LadybirdBrowser/ladybird

### 15.2 Related Research

- "Automatic Yara Rule Generation Using Biclustering" (arXiv:2009.03779)
- "YARA: Yet Another Recursive Acronym" (VirusTotal documentation)
- "Zeek: A Powerful Network Analysis Framework" (Zeek documentation)

### 15.3 Existing Ladybird Features

- NetworkIdentity system (per-page circuit isolation)
- RequestServer IPC architecture
- Custom protocol handlers (IPFS, ENS)
- LibIPC infrastructure

---

**Document Revision History**:
- v0.1 (2025-10-28): Initial design and Milestone 0.1 specification
