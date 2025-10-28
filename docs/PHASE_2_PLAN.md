# Phase 2: Advanced Network Privacy and Security

## Overview

Phase 2 builds on the NetworkIdentity foundation from Phase 1 to implement advanced privacy features, automated circuit management, and comprehensive security hardening.

## Goals

1. Automated privacy protection (circuit rotation, DNS leak detection)
2. Enhanced user control (VPN integration, manual circuit rotation)
3. Privacy monitoring and analytics (real-time graphs, privacy scores)
4. Security hardening (WebRTC leak prevention, fingerprinting resistance)
5. Persistent configurations (save/restore proxy settings)

## Proposed Milestones

### Milestone 2.1: Circuit Rotation & Management

**Objective:** Automatic and manual Tor circuit rotation for enhanced privacy

**Implementation:**
- Libraries/LibIPC/NetworkIdentity.h/cpp:
  - Add circuit rotation timestamp tracking
  - Circuit age calculation
  - Auto-rotation policy configuration

- Services/RequestServer/ConnectionFromClient.h/cpp:
  - rotate_circuit(u64 page_id) IPC handler
  - Automatic rotation timer (configurable interval)
  - Per-domain circuit rotation option

- UI/Qt/Tab.h/cpp:
  - "New Circuit" button in toolbar
  - Circuit age indicator
  - Auto-rotation settings in preferences

**Features:**
- Manual circuit rotation button
- Automatic rotation every N minutes (configurable)
- New circuit per domain option (privacy mode)
- Circuit age display in UI
- Force new circuit on specific events (e.g., failed request)

**Acceptance Criteria:**
- Manual rotation creates new circuit ID
- Auto-rotation works on timer
- Per-domain circuits isolate traffic
- UI shows current circuit age
- Old circuits properly cleaned up

### Milestone 2.2: DNS Leak Detection & Monitoring

**Objective:** Real-time DNS leak detection and alerting

**Implementation:**
- Services/RequestServer/ConnectionFromClient.h/cpp:
  - DNS query monitoring and logging
  - Leak detection logic (DNS queries when Tor enabled)
  - Alert generation on leak detection

- Libraries/LibIPC/NetworkIdentity.h/cpp:
  - DNS query audit log
  - Leak event tracking
  - Statistics collection

- UI/Qt/DNSLeakMonitor.h/cpp (NEW):
  - Real-time DNS activity monitor
  - Leak alert dialog
  - DNS query history viewer

**Features:**
- Real-time DNS query monitoring
- Automatic leak detection when Tor enabled
- Visual alerts for DNS leaks
- DNS query history with timestamps
- Leak statistics and trends

**Acceptance Criteria:**
- DNS queries logged when Tor enabled trigger alerts
- No false positives for legitimate queries
- Alert UI clearly explains leak
- DNS history accurate and complete
- Statistics show leak-free periods

### Milestone 2.3: Network Activity Analytics

**Objective:** Visual analytics for network activity and privacy metrics

**Implementation:**
- UI/Qt/NetworkAnalyticsDialog.h/cpp (NEW):
  - Real-time bandwidth graph (sent/received over time)
  - Request count histogram
  - Privacy score calculation
  - Domain distribution chart

- Libraries/LibIPC/NetworkIdentity.h/cpp:
  - Time-series bandwidth tracking
  - Request statistics aggregation
  - Privacy metric calculations

**Features:**
- Live bandwidth graph (updating every second)
- Request distribution by domain
- Privacy score (0-100) based on:
  - Tor usage percentage
  - Circuit rotation frequency
  - DNS leak count
  - Unique domain connections
- Export analytics data to CSV/JSON

**Acceptance Criteria:**
- Graphs update in real-time
- Privacy score accurate and meaningful
- Analytics export works correctly
- Performance impact minimal
- Visual clarity and usability

### Milestone 2.4: Configuration Persistence

**Objective:** Save and restore proxy/Tor settings across browser sessions

**Implementation:**
- Libraries/LibCore/ConfigFile.h usage:
  - NetworkIdentity configuration storage
  - Per-page proxy settings serialization
  - Circuit preferences persistence

- Services/RequestServer/ConnectionFromClient.h/cpp:
  - load_network_config() on startup
  - save_network_config() on shutdown
  - Auto-save on configuration changes

- UI/Qt/Preferences.h/cpp:
  - Network privacy settings page
  - Default proxy configuration
  - Auto-rotation preferences
  - Circuit rotation interval setting

**Features:**
- Proxy settings persist across restarts
- Per-tab Tor state restored on session restore
- Default proxy configuration (apply to all new tabs)
- Circuit rotation preferences saved
- Import/export configuration files

**Acceptance Criteria:**
- Settings survive browser restart
- Session restore maintains proxy state
- Default configuration applies to new tabs
- Configuration files valid and readable
- Import/export works correctly

### Milestone 2.5: VPN Integration (Optional - Advanced)

**Objective:** Per-tab VPN support with provider integration

**Implementation:**
- Libraries/LibIPC/VPNConfig.h (NEW):
  - VPN configuration structure
  - Provider authentication
  - Connection state tracking

- Services/RequestServer/ConnectionFromClient.h/cpp:
  - enable_vpn(u64 page_id, VPNConfig) IPC handler
  - VPN connection management
  - Fallback to Tor on VPN failure

- UI/Qt/VPNDialog.h/cpp (NEW):
  - VPN provider selection
  - Authentication input
  - Connection status display

**Features:**
- Multiple VPN provider support (WireGuard, OpenVPN)
- Per-tab VPN assignment
- VPN + Tor layering option
- Kill switch (block traffic if VPN disconnects)
- Connection status monitoring

**Acceptance Criteria:**
- VPN connections establish successfully
- Per-tab VPN isolation works
- Kill switch prevents leaks on disconnect
- VPN + Tor combination functional
- Provider authentication secure

### Milestone 2.6: WebRTC Leak Prevention

**Objective:** Prevent WebRTC from leaking real IP address

**Implementation:**
- Libraries/LibWeb/WebRTC/:
  - IP address enumeration blocking
  - STUN/TURN request interception
  - mDNS hostname obfuscation

- Services/WebContent/:
  - WebRTC policy enforcement
  - IP leak detection and blocking

- UI/Qt/Preferences.h/cpp:
  - WebRTC policy settings (block/proxy/allow)
  - Per-site WebRTC permissions

**Features:**
- Block WebRTC IP enumeration
- Force WebRTC through proxy
- mDNS obfuscation (hide local IPs)
- Per-site WebRTC policy
- IP leak testing integration

**Acceptance Criteria:**
- Real IP not exposed via WebRTC
- WebRTC functionality preserved when allowed
- Policy settings enforced correctly
- No leaks detected in testing
- User control over WebRTC behavior

### Milestone 2.7: Fingerprinting Resistance

**Objective:** Reduce browser fingerprinting surface area

**Implementation:**
- Libraries/LibWeb/:
  - Canvas fingerprinting protection (noise injection)
  - Font enumeration blocking/limiting
  - WebGL fingerprinting mitigation
  - Audio context fingerprinting protection

- Services/WebContent/:
  - User agent normalization
  - Timezone spoofing
  - Screen resolution reporting control

**Features:**
- Canvas noise injection (subtle randomization)
- Limited font list (common fonts only)
- WebGL parameter normalization
- Audio fingerprint mitigation
- Configurable fingerprint resistance levels

**Acceptance Criteria:**
- Fingerprinting tests show reduced uniqueness
- Website compatibility maintained
- Resistance levels adjustable
- No obvious visual artifacts
- Privacy/usability balance achieved

## Phase 2 Architecture Considerations

### Performance Impact
- Real-time monitoring should use efficient data structures
- Analytics graphs should not impact browsing performance
- Circuit rotation should be seamless (no visible delays)
- Configuration persistence should be async (non-blocking)

### Security Considerations
- VPN credentials stored securely (encrypted)
- Configuration files validated on load
- DNS leak detection has no false negatives
- WebRTC blocking cannot be bypassed
- Fingerprinting resistance transparent to user

### User Experience
- Privacy features easy to understand
- Sensible defaults (secure by default)
- Clear visual feedback on privacy state
- Non-intrusive alerts
- Settings organized logically

### Testing Requirements
- Automated DNS leak tests
- Circuit rotation verification
- WebRTC leak tests
- Fingerprinting resistance validation
- Performance regression tests

## Implementation Priority (Recommended)

1. **High Priority:**
   - Milestone 2.1: Circuit Rotation (immediate privacy benefit)
   - Milestone 2.2: DNS Leak Detection (security critical)
   - Milestone 2.4: Configuration Persistence (user convenience)

2. **Medium Priority:**
   - Milestone 2.3: Network Analytics (visibility and trust)
   - Milestone 2.6: WebRTC Leak Prevention (common leak vector)

3. **Lower Priority:**
   - Milestone 2.5: VPN Integration (complex, optional)
   - Milestone 2.7: Fingerprinting Resistance (advanced privacy)

## Resources Required

### Development Time Estimate
- Milestone 2.1: 2-3 days
- Milestone 2.2: 2-3 days
- Milestone 2.3: 3-4 days
- Milestone 2.4: 1-2 days
- Milestone 2.5: 5-7 days (optional)
- Milestone 2.6: 3-4 days
- Milestone 2.7: 4-5 days

**Total: 20-28 days** (excluding VPN integration)

### Technical Dependencies
- Qt for UI components (charts, graphs)
- libcurl for VPN integration (if implemented)
- LibWeb WebRTC implementation understanding
- Configuration file format design

### Documentation Needs
- User guide for privacy features
- Circuit rotation explanation
- DNS leak detection guide
- VPN setup instructions (if implemented)
- Fingerprinting resistance trade-offs

## Success Metrics

### Privacy Metrics
- DNS leak detection accuracy: 100% (no false negatives)
- Circuit rotation uptime: >99%
- WebRTC leak prevention: 100% when enabled
- Fingerprinting uniqueness reduction: >50%

### User Experience Metrics
- Settings discovery: <3 clicks to any privacy feature
- Configuration persistence: 100% reliability
- Alert clarity: User understands privacy status
- Performance impact: <5% overhead

### Technical Metrics
- Code coverage: >80% for privacy-critical code
- Build time impact: <10% increase
- Memory footprint: <20MB additional
- Response time: <100ms for privacy operations

## Risk Analysis

### Technical Risks
1. **WebRTC Integration Complexity:** LibWeb WebRTC may require extensive refactoring
   - Mitigation: Start with simple IP enumeration blocking

2. **VPN Provider Variability:** Different VPN protocols have different requirements
   - Mitigation: Focus on one protocol (WireGuard) initially

3. **Performance Overhead:** Analytics and monitoring may impact browsing
   - Mitigation: Use efficient data structures, sample where possible

### User Experience Risks
1. **Feature Complexity:** Too many settings may overwhelm users
   - Mitigation: Sensible defaults, progressive disclosure

2. **False Leak Alerts:** May cause user alarm unnecessarily
   - Mitigation: Clear explanations, confidence scoring

### Security Risks
1. **Configuration Storage:** Credentials in config files may leak
   - Mitigation: Encrypt sensitive data, secure file permissions

2. **Bypass Mechanisms:** Privacy features may be circumventable
   - Mitigation: Comprehensive testing, defense in depth

## Open Questions for User

1. **VPN Priority:** Is VPN integration essential for Phase 2, or can it be deferred?
2. **Fingerprinting Scope:** Which fingerprinting vectors are highest priority?
3. **Analytics Detail:** How detailed should network analytics be?
4. **Circuit Rotation:** What should default rotation interval be?
5. **DNS Leak Alerts:** How intrusive should DNS leak alerts be?

## Next Steps

1. Review Phase 2 plan with user
2. Confirm milestone priorities
3. Answer open questions
4. Create detailed implementation plan for Milestone 2.1
5. Begin implementation

## Appendix: Alternative Approaches

### Circuit Rotation Alternatives
- **Time-based:** Rotate every N minutes (simple, predictable)
- **Request-based:** Rotate every N requests (traffic-adaptive)
- **Domain-based:** New circuit per domain (maximum isolation)
- **Hybrid:** Combination of above (configurable strategy)

### DNS Leak Detection Alternatives
- **Passive Monitoring:** Log all DNS queries, alert on unexpected
- **Active Testing:** Periodic DNS leak tests to known domains
- **Network Layer:** Intercept DNS at network stack level

### Analytics Alternatives
- **Minimal:** Just bandwidth totals (low overhead)
- **Standard:** Bandwidth + request counts + domains (balanced)
- **Comprehensive:** Full statistics with graphs (high detail)

## References

- Phase 1 Review: docs/PHASE_1_REVIEW.md
- Tor Integration Complete: claudedocs/TOR_INTEGRATION_COMPLETE.md
- Security Audit: docs/SECURITY_AUDIT.md
- Development Guide: docs/DEVELOPMENT.md
