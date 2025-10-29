# Sentinel Phase 2 Implementation Plan

**Phase**: Week 2 - Browser Integration (SecurityTap)
**Status**: Ready to Start
**Dependencies**: Phase 1 Complete ✅

---

## Overview

Phase 2 integrates Sentinel into the browser's download pipeline by creating a SecurityTap module that intercepts completed downloads, sends them to Sentinel for YARA scanning, and routes alerts back to the browser UI via IPC.

---

## Architecture Analysis

### Current RequestServer Flow

```
[Browser UI]
    ↓ IPC (start_request)
[ConnectionFromClient]
    ↓
[Request] → DNS → Connect → Fetch → Complete
    ↓ (m_response_buffer contains content)
[handle_complete_state()]
    ↓ IPC (async_request_finished)
[Browser UI]
```

### New Flow with SecurityTap

```
[Browser UI]
    ↓ IPC (start_request)
[ConnectionFromClient]
    ↓
[Request] → DNS → Connect → Fetch → Complete
    ↓ (m_response_buffer contains content)
[handle_complete_state()]
    ↓
[SecurityTap::inspect_download()] ← NEW
    ↓ UNIX socket
[Sentinel Daemon]
    ↓ YARA scan results
[SecurityTap]
    ↓ IPC (security_alert) if threat detected ← NEW
[ConnectionFromClient]
    ↓ IPC
[Browser UI] - shows alert dialog ← Week 3
```

---

## Implementation Tasks

### Day 8-9: SecurityTap Module

#### Task 1.1: Create SecurityTap.h

**File**: `Services/RequestServer/SecurityTap.h`

```cpp
#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibCore/LocalSocket.h>

namespace RequestServer {

class SecurityTap {
public:
    static ErrorOr<NonnullOwnPtr<SecurityTap>> create();
    ~SecurityTap() = default;

    struct DownloadMetadata {
        ByteString url;
        ByteString filename;
        ByteString mime_type;
        ByteString sha256;
        size_t size_bytes;
    };

    struct ScanResult {
        bool is_threat { false };
        Optional<ByteString> alert_json;
    };

    // Main inspection method
    ErrorOr<ScanResult> inspect_download(
        DownloadMetadata const& metadata,
        ReadonlyBytes content
    );

private:
    SecurityTap(NonnullOwnPtr<Core::LocalSocket> socket);

    ErrorOr<ByteString> send_scan_request(
        DownloadMetadata const& metadata,
        ReadonlyBytes content
    );

    NonnullOwnPtr<Core::LocalSocket> m_sentinel_socket;
};

}
```

**Key Design Decisions**:
- Uses `Core::LocalSocket` (same as Sentinel server)
- `DownloadMetadata` struct for clean parameter passing
- `ScanResult` struct for typed responses
- Returns `ErrorOr<>` for proper error handling

#### Task 1.2: Implement SecurityTap.cpp

**File**: `Services/RequestServer/SecurityTap.cpp`

**Implementation Steps**:
1. Connect to `/tmp/sentinel.sock`
2. Compute SHA256 hash of content
3. Construct JSON request (same format as test_sentinel.py)
4. Send to Sentinel
5. Parse JSON response
6. Return ScanResult

**Edge Cases to Handle**:
- Sentinel not running (graceful fail-open)
- Socket timeout (don't block downloads)
- Large files (stream hash computation)
- Invalid JSON responses

#### Task 1.3: Add SHA256 Hashing

**Dependencies**: LibCrypto (already in Ladybird)

```cpp
#include <LibCrypto/Hash/SHA2.h>

ErrorOr<ByteString> compute_sha256(ReadonlyBytes data) {
    Crypto::Hash::SHA256 sha256;
    sha256.update(data);
    auto digest = sha256.digest();

    StringBuilder hex_builder;
    for (auto byte : digest.bytes())
        hex_builder.appendff("{:02x}", byte);

    return hex_builder.to_byte_string();
}
```

#### Task 1.4: Hook into Request::handle_complete_state()

**File**: `Services/RequestServer/Request.cpp` (modify)

**Location**: After line 544 (after audit logging)

```cpp
// Line 545 - NEW: SecurityTap integration
if (m_security_tap && should_inspect_download()) {
    auto metadata = extract_download_metadata();

    // Read content from response buffer
    auto buffer_size = m_response_buffer.used_buffer_size();
    if (buffer_size > 0) {
        auto content_buffer = TRY(ByteBuffer::create_uninitialized(buffer_size));
        TRY(m_response_buffer.read_until_filled(content_buffer));

        // Scan the content
        auto scan_result = m_security_tap->inspect_download(metadata, content_buffer.bytes());

        if (!scan_result.is_error() && scan_result.value().is_threat) {
            // Send security alert to client via IPC (Week 2 Day 10-11)
            m_client.async_security_alert(m_request_id, scan_result.value().alert_json.value());
        }

        // Rewind buffer for client consumption
        m_response_buffer.seek(0, SeekMode::SetPosition);
    }
}
```

**Helper Methods to Add**:

```cpp
bool Request::should_inspect_download() const {
    // Only inspect actual downloads, not page navigations
    auto content_disposition = m_response_headers.get("Content-Disposition");
    if (content_disposition.has_value() && content_disposition->contains("attachment"))
        return true;

    // Check for common download MIME types
    auto content_type = m_response_headers.get("Content-Type");
    if (content_type.has_value()) {
        if (content_type->starts_with("application/"))
            return true;
        if (content_type->contains("executable"))
            return true;
    }

    return false;
}

SecurityTap::DownloadMetadata Request::extract_download_metadata() const {
    // Extract filename from Content-Disposition or URL
    ByteString filename = "unknown";
    auto disposition = m_response_headers.get("Content-Disposition");
    if (disposition.has_value()) {
        // Parse: Content-Disposition: attachment; filename="file.exe"
        if (auto start = disposition->find("filename="); start.has_value()) {
            filename = disposition->substring_view(*start + 9).trim("\""sv);
        }
    }
    if (filename == "unknown") {
        // Fallback: extract from URL path
        filename = m_url.serialize_path();
    }

    auto mime_type = m_response_headers.get("Content-Type").value_or("application/octet-stream");

    return SecurityTap::DownloadMetadata {
        .url = m_url.to_byte_string(),
        .filename = filename,
        .mime_type = mime_type,
        .sha256 = "", // Computed in SecurityTap
        .size_bytes = m_response_buffer.used_buffer_size()
    };
}
```

#### Task 1.5: Initialize SecurityTap in RequestServer

**File**: `Services/RequestServer/main.cpp` (modify)

Add global SecurityTap instance initialization at startup.

---

### Day 10-11: IPC Extension

#### Task 2.1: Add security_alert Message

**File**: `Services/RequestServer/RequestClient.ipc` (modify)

Add after line ~20:

```
security_alert(i32 request_id, ByteString alert_json) =|
```

**Effect**: Generates `async_security_alert()` method in ConnectionFromClient

#### Task 2.2: Handle Alert in ConnectionFromClient

**File**: `Services/RequestServer/ConnectionFromClient.cpp` (modify)

Implementation auto-generated by IPC system. Alert will be sent to browser.

#### Task 2.3: Receive Alert in Browser

**File**: `Libraries/LibRequests/RequestClient.cpp` (modify)

Add handler for `security_alert` message:

```cpp
void RequestClient::security_alert(i32 request_id, ByteString const& alert_json) {
    dbgln("RequestClient: Security alert for request {}: {}", request_id, alert_json);

    // TODO Week 3: Show UI dialog
    // For now, just log it
}
```

---

### Day 12-13: Policy Graph Database (Optional for Phase 2 Testing)

**Decision**: Defer to Week 3

For Phase 2 testing, we'll:
1. Just log alerts (no user prompts)
2. Verify SecurityTap → Sentinel → IPC flow works
3. Implement PolicyGraph in Week 3 with UI

---

### Day 14: Integration Testing

#### Test 1: End-to-End Download → Alert Flow

**Setup**:
1. Start Sentinel daemon
2. Start Ladybird browser
3. Create test server with EICAR file

**Test**:
```bash
# Test server
python3 -m http.server 8000 &
echo 'X5O!P%@AP[4\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*' > /tmp/test_eicar.txt
cp /tmp/test_eicar.txt .
```

**Expected**:
1. Navigate to `http://localhost:8000/test_eicar.txt`
2. SecurityTap intercepts download
3. Sends to Sentinel
4. Sentinel detects EICAR
5. Alert sent via IPC
6. Browser logs alert (or shows dialog in Week 3)

#### Test 2: Clean File Download

**Test**: Download legitimate file (e.g., PDF, image)

**Expected**:
1. No alert generated
2. Download completes normally
3. No performance impact

#### Test 3: Sentinel Unavailable

**Test**: Stop Sentinel daemon, download file

**Expected**:
1. SecurityTap fails gracefully
2. Download still completes (fail-open)
3. Warning logged but no user disruption

#### Test 4: Performance Validation

**Metrics**:
- Download overhead: < 100ms for 10MB file
- Memory impact: < 50MB additional
- CPU usage: < 5% during scan

---

## File Changes Summary

### New Files (2)
- `Services/RequestServer/SecurityTap.h` (~50 lines)
- `Services/RequestServer/SecurityTap.cpp` (~200 lines)

### Modified Files (5)
- `Services/RequestServer/Request.h` (+20 lines)
- `Services/RequestServer/Request.cpp` (+80 lines)
- `Services/RequestServer/RequestClient.ipc` (+1 line)
- `Services/RequestServer/main.cpp` (+10 lines)
- `Libraries/LibRequests/RequestClient.cpp` (+15 lines)

### Build System
- Update `Services/RequestServer/CMakeLists.txt` (add SecurityTap.cpp)

---

## Risk Mitigation

### Risk: Downloads hang waiting for Sentinel
**Mitigation**:
- Socket timeout: 5 seconds max
- Fail-open: If Sentinel unavailable, allow download

### Risk: Large file performance impact
**Mitigation**:
- Skip scanning for files > 100MB
- Stream hashing (don't load entire file into memory)

### Risk: Sentinel crash during scan
**Mitigation**:
- Socket error detection
- Log error, continue download

### Risk: IPC message flooding
**Mitigation**:
- Rate limit alerts (max 10/second)
- Queue alerts if browser busy

---

## Success Criteria

Phase 2 is complete when:

1. ✅ SecurityTap compiles and links
2. ✅ Downloads trigger SecurityTap inspection
3. ✅ EICAR test file generates alert
4. ✅ Clean files pass through without alerts
5. ✅ IPC message delivered to browser
6. ✅ Performance overhead < 5%
7. ✅ Sentinel unavailable handled gracefully

---

## Next Phase Preview

**Week 3: UI Integration & Policy Enforcement**
- Security alert dialog
- Policy creation from alerts
- Policy Graph database
- Enforcement hooks (quarantine, block)
- `ladybird://security` management UI

---

**Document Version**: 1.0
**Created**: 2025-10-29
**Ready to Implement**: Yes ✅
