# Milestone 1.5: Network Identity Audit UI

> **STATUS: NOT IMPLEMENTED**
>
> This milestone was implemented on 2025-10-26 but was subsequently lost due to a catastrophic error in a previous development session (git checkout without committing changes). The implementation described in this document represents planned/designed functionality that will need to be re-implemented.
>
> The NetworkIdentity audit logging infrastructure (Milestone 1.4) remains intact and functional.

## Overview

This milestone implements a user-facing interface for viewing network activity audit logs tracked by the NetworkIdentity system. Users can click a toolbar button to view a comprehensive audit log of all network requests made by a tab, with filtering and CSV export capabilities.

## Original Implementation Date

**Implemented**: 2025-10-26
**Status**: Lost in development session, not re-implemented

## Features

### User Interface Components

1. **Network Activity Button**
   - Location: Tab toolbar, positioned after VPN toggle button
   - Icon: Network icon (resource://icons/16x16/network.png)
   - Tooltip: "View network activity audit log"
   - Action: Opens NetworkAuditDialog

2. **NetworkAuditDialog**
   - **Table View**:
     - 6 sortable columns: Timestamp, Method, URL, Status, Sent, Received
     - Displays all network requests made by the tab
     - Click column headers to sort

   - **Filtering**:
     - Real-time text search across Method, URL, and Status fields
     - Case-insensitive matching
     - Clear filter button for quick reset

   - **Statistics Display**:
     - Total number of requests
     - Total bytes sent (formatted as B/KB/MB)
     - Total bytes received (formatted as B/KB/MB)

   - **CSV Export**:
     - Exports filtered results to CSV file
     - Proper field escaping for commas, quotes, and newlines
     - File dialog for save location selection
     - Success/failure feedback

### Data Flow Architecture

```
Tab UI (user clicks button)
    ↓
Tab::open_network_audit_dialog()
    ↓
WebContentClient::get_network_audit(page_id) [IPC]
    ↓
WebContent::ConnectionFromClient::get_network_audit()
    ↓
RequestServer::ConnectionFromClient::get_network_audit() [IPC]
    ↓
NetworkIdentity::audit_log()
    ↓
Serialization (pipe-delimited format)
    ↓
Return through IPC chain
    ↓
NetworkAuditDialog::set_audit_data()
    ↓
Display in table
```

## Technical Implementation

### IPC Messages

#### RequestServer.ipc
```cpp
// Services/RequestServer/RequestServer.ipc:37
get_network_audit() => (Vector<ByteString> audit_entries, size_t total_bytes_sent, size_t total_bytes_received)
```

#### WebContentServer.ipc
```cpp
// Services/WebContent/WebContentServer.ipc:143
get_network_audit(u64 page_id) => (Vector<ByteString> audit_entries, size_t total_bytes_sent, size_t total_bytes_received)
```

### Serialization Format

Audit entries are serialized as pipe-delimited strings for IPC transmission:

```
timestamp_ms|method|url|response_code|bytes_sent|bytes_received
```

**Example**:
```
1698764532123|GET|https://example.com|200|512|2048
1698764533456|POST|https://api.example.com/data|201|1024|512
```

This format was chosen for:
- Simple parsing (split on '|')
- Efficient transmission (no complex struct encoding)
- Human-readable for debugging

### Key Implementation Files

#### Services/RequestServer/ConnectionFromClient.cpp (lines 605-640 - Audit Retrieval)

Handler for get_network_audit IPC message:

```cpp
Messages::RequestServer::GetNetworkAuditResponse ConnectionFromClient::get_network_audit()
{
    Vector<ByteString> audit_entries;
    size_t total_bytes_sent = 0;
    size_t total_bytes_received = 0;

    if (!m_network_identity) {
        dbgln("RequestServer: Cannot get audit - no network identity");
        return { move(audit_entries), total_bytes_sent, total_bytes_received };
    }

    // Serialize audit log entries as CSV-like strings for easy parsing
    for (auto const& entry : m_network_identity->audit_log()) {
        auto timestamp_ms = entry.timestamp.milliseconds();
        auto response_code = entry.response_code.has_value() ? ByteString::number(*entry.response_code) : ByteString("0");

        auto serialized = ByteString::formatted("{}|{}|{}|{}|{}|{}",
            timestamp_ms,
            entry.method,
            entry.url.to_byte_string(),
            response_code,
            entry.bytes_sent,
            entry.bytes_received);

        audit_entries.append(move(serialized));
    }

    total_bytes_sent = m_network_identity->total_bytes_sent();
    total_bytes_received = m_network_identity->total_bytes_received();

    dbgln("RequestServer: Returning {} audit entries, {} bytes sent, {} bytes received",
        audit_entries.size(), total_bytes_sent, total_bytes_received);

    return { move(audit_entries), total_bytes_sent, total_bytes_received };
}
```

**Error Handling**:
- Returns empty vector if no NetworkIdentity exists
- Handles missing response codes (uses "0" for pending/failed requests)
- Debug logging for troubleshooting

#### Services/RequestServer/ConnectionFromClient.cpp (lines 1168-1181 - Bandwidth Tracking)

Bandwidth tracking in request completion handler:

```cpp
// Extract bandwidth information for audit logging
curl_off_t bytes_sent = 0;
curl_off_t bytes_received = 0;
curl_easy_getinfo(msg->easy_handle, CURLINFO_SIZE_UPLOAD_T, &bytes_sent);
curl_easy_getinfo(msg->easy_handle, CURLINFO_SIZE_DOWNLOAD_T, &bytes_received);

// Log response with bandwidth in NetworkIdentity audit trail
if (m_network_identity && request->http_status_code.has_value()) {
    m_network_identity->log_response(
        request->url,
        static_cast<u16>(*request->http_status_code),
        static_cast<size_t>(bytes_sent),
        static_cast<size_t>(bytes_received));
}
```

**Implementation Details**:
- Called in `check_active_requests()` when curl requests complete (CURLMSG_DONE)
- Extracts actual bytes transferred from curl using `CURLINFO_SIZE_UPLOAD_T` and `CURLINFO_SIZE_DOWNLOAD_T`
- Calls `NetworkIdentity::log_response()` to update the audit entry with bandwidth data
- Only logs if NetworkIdentity exists and HTTP status code is available

#### UI/Qt/NetworkAuditDialog.cpp

**Key Features**:

1. **Data Parsing** (lines 91-107):
```cpp
for (auto const& entry_str : entries) {
    auto parts = entry_str.split('|');
    if (parts.size() != 6)
        continue;  // Skip malformed entries

    AuditEntry entry;
    entry.timestamp_ms = parts[0].to_number<u64>().value_or(0);
    entry.method = parts[1];
    entry.url = parts[2];
    entry.response_code = parts[3].to_number<u16>().value_or(0);
    entry.bytes_sent = parts[4].to_number<size_t>().value_or(0);
    entry.bytes_received = parts[5].to_number<size_t>().value_or(0);

    m_entries.append(entry);
}
```

2. **Filtering** (lines 152-171):
```cpp
void NetworkAuditDialog::apply_filter()
{
    auto filter_text = m_filter_edit->text().toLower();
    if (filter_text.isEmpty()) {
        m_filtered_entries = m_entries;
    } else {
        m_filtered_entries.clear();
        for (auto const& entry : m_entries) {
            auto method_lower = entry.method.to_lowercase();
            auto url_lower = entry.url.to_lowercase();
            auto status_str = ByteString::number(entry.response_code);

            if (method_lower.contains(filter_text.toUtf8().data()) ||
                url_lower.contains(filter_text.toUtf8().data()) ||
                status_str.contains(filter_text.toUtf8().data())) {
                m_filtered_entries.append(entry);
            }
        }
    }

    populate_table();
}
```

3. **CSV Export** (lines 183-226):
```cpp
void NetworkAuditDialog::on_export_button_clicked()
{
    auto filename = QFileDialog::getSaveFileName(this, "Export Network Audit", "network_audit.csv", "CSV Files (*.csv)");
    if (filename.isEmpty())
        return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed", "Could not open file for writing.");
        return;
    }

    QTextStream out(&file);

    // Write CSV header
    out << "Timestamp,Method,URL,Status,Bytes Sent,Bytes Received\\n";

    // Write data rows with proper CSV escaping
    for (auto const& entry : m_filtered_entries) {
        auto timestamp_str = format_timestamp(entry.timestamp_ms);
        auto status_str = entry.response_code == 0 ? "-"sv : ByteString::number(entry.response_code);

        // Escape CSV fields that contain commas, quotes, or newlines
        auto escape_csv = [](ByteString const& field) -> ByteString {
            if (field.contains(',') || field.contains('"') || field.contains('\\n')) {
                auto escaped = field;
                escaped = escaped.replace("\""sv, "\"\""sv, ReplaceMode::All);
                return ByteString::formatted("\"{}\"", escaped);
            }
            return field;
        };

        out << QString::fromUtf8(escape_csv(timestamp_str).characters()) << ","
            << QString::fromUtf8(escape_csv(entry.method).characters()) << ","
            << QString::fromUtf8(escape_csv(entry.url).characters()) << ","
            << QString::fromUtf8(status_str.characters()) << ","
            << entry.bytes_sent << ","
            << entry.bytes_received << "\\n";
    }

    file.close();

    QMessageBox::information(this, "Export Successful", QString("Exported %1 entries to %2").arg(m_filtered_entries.size()).arg(filename));
}
```

**CSV Escaping Logic**:
- Fields containing commas, quotes, or newlines are wrapped in quotes
- Internal quotes are doubled ("" becomes """")
- Proper RFC 4180 CSV formatting

4. **Formatting Utilities** (lines 228-242):
```cpp
ByteString NetworkAuditDialog::format_bytes(size_t bytes) const
{
    if (bytes < 1024)
        return ByteString::formatted("{} B", bytes);
    else if (bytes < 1024 * 1024)
        return ByteString::formatted("{:.2f} KB", bytes / 1024.0);
    else
        return ByteString::formatted("{:.2f} MB", bytes / (1024.0 * 1024.0));
}

ByteString NetworkAuditDialog::format_timestamp(u64 timestamp_ms) const
{
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(timestamp_ms);
    return ByteString(dt.toString("yyyy-MM-dd HH:mm:ss").toUtf8().data());
}
```

### UI Integration (Tab.cpp)

**Button Creation** (lines 206-211):
```cpp
// Create Network Audit button
m_network_audit_action = new QAction(this);
m_network_audit_action->setIcon(load_icon_from_uri("resource://icons/16x16/network.png"sv));
m_network_audit_action->setText("Network Activity");
m_network_audit_action->setToolTip("View network activity audit log");
QObject::connect(m_network_audit_action, &QAction::triggered, this, &Tab::open_network_audit_dialog);
```

**Toolbar Integration** (line 227):
```cpp
m_toolbar->addAction(m_network_audit_action);  // Add Network Audit button
```

**Dialog Invocation** (lines 670-681):
```cpp
void Tab::open_network_audit_dialog()
{
    // Get audit data via IPC
    auto response = view().client().get_network_audit(view().page_id());

    // Create and show dialog
    auto* dialog = new NetworkAuditDialog(this);
    dialog->set_audit_data(response.audit_entries(), response.total_bytes_sent(), response.total_bytes_received());
    dialog->exec();

    delete dialog;
}
```

## Files Modified

1. **Services/RequestServer/RequestServer.ipc** - Added IPC message definition
2. **Services/RequestServer/ConnectionFromClient.h** - Added handler declaration
3. **Services/RequestServer/ConnectionFromClient.cpp** - Implemented audit retrieval and serialization
4. **Services/WebContent/WebContentServer.ipc** - Added IPC message definition with page_id
5. **Services/WebContent/ConnectionFromClient.h** - Added forwarding handler declaration
6. **Services/WebContent/ConnectionFromClient.cpp** - Implemented IPC forwarding to RequestServer
7. **UI/Qt/NetworkAuditDialog.h** (NEW) - Dialog interface definition
8. **UI/Qt/NetworkAuditDialog.cpp** (NEW) - Complete dialog implementation
9. **UI/Qt/Tab.h** - Added network audit button and method declarations
10. **UI/Qt/Tab.cpp** - Button creation, toolbar integration, dialog invocation
11. **UI/Qt/CMakeLists.txt** - Added NetworkAuditDialog.cpp to build sources

## Usage

1. **Opening the Audit Dialog**:
   - Click the "Network Activity" button in the tab toolbar (inspector icon)
   - Dialog appears showing all network requests for that tab

2. **Filtering Results**:
   - Type in the filter text box to search across method, URL, and status
   - Results update in real-time
   - Click "Clear" to reset filter

3. **Sorting Data**:
   - Click any column header to sort by that column
   - Click again to reverse sort order

4. **Exporting Data**:
   - Click "Export to CSV" button
   - Choose save location in file dialog
   - CSV file contains filtered results

5. **Viewing Statistics**:
   - Statistics label at top shows:
     - Total number of requests
     - Total bytes sent
     - Total bytes received

## Testing Recommendations

### Manual Testing

1. **Basic Functionality**:
   - Navigate to multiple pages in a tab
   - Click Network Activity button
   - Verify all requests appear in table
   - Check timestamps are correct and formatted properly

2. **Filtering**:
   - Enter various search terms (URL fragments, methods like "GET", status codes)
   - Verify filtering works across all fields
   - Test case-insensitive matching
   - Clear filter and verify all entries return

3. **Sorting**:
   - Click each column header
   - Verify sorting works correctly (ascending/descending)
   - Check timestamp sorting orders chronologically

4. **CSV Export**:
   - Export audit log with various filters applied
   - Open CSV in spreadsheet application
   - Verify proper formatting and escaping
   - Test with URLs containing commas, quotes, or special characters

5. **Statistics**:
   - Verify total request count matches table rows
   - Check byte count formatting (B/KB/MB transitions)
   - Navigate to different pages and verify counts update

6. **Edge Cases**:
   - Open dialog when no requests have been made (empty table)
   - Test with very long URLs (check column width handling)
   - Test with special characters in URLs
   - Verify pending requests (no status code) display correctly

### Integration Testing

1. **Multi-Process IPC**:
   - Verify data flows correctly through WebContent → RequestServer
   - Test with multiple tabs (each should have independent audit logs)
   - Check memory cleanup when dialog is closed

2. **Network Identity Integration**:
   - Verify audit log matches NetworkIdentity internal state
   - Test with Tor enabled/disabled (separate identities)
   - Test with VPN enabled/disabled

3. **Performance**:
   - Test with large audit logs (100+ entries)
   - Verify filtering remains responsive
   - Check export performance with large datasets

## Known Limitations

1. **Audit Log Size**: Limited by NetworkIdentity::MaxAuditEntries (1000 entries)
2. **No Real-Time Updates**: Dialog shows snapshot at open time, doesn't live-update
3. **No Request Details**: Only shows summary information, not headers/body
4. **No Network Timeline**: Data is tabular, not visualized over time

## Future Enhancements (Not in Scope)

1. Live-updating table as new requests occur
2. Request/response headers and body inspection
3. Timeline visualization
4. Network performance metrics (DNS time, connection time, etc.)
5. Filtering by request type (XHR, image, script, etc.)
6. HAR (HTTP Archive) format export
7. Request replay functionality

## Design Patterns Used

### Qt Patterns
- **Modal Dialog**: NetworkAuditDialog extends QDialog with exec()
- **Model-View**: QTableWidget for data display
- **Signal-Slot**: QAction::triggered connected to handler method
- **RAII**: Dialog created on stack, deleted after exec()

### Ladybird Patterns
- **IPC Message Chain**: Tab → WebContent → RequestServer
- **Synchronous IPC**: Using `=>` return syntax for immediate data retrieval
- **AK Containers**: Vector, ByteString throughout implementation
- **Debug Logging**: dbgln() at key points for troubleshooting

### Data Patterns
- **Serialization**: Simple pipe-delimited format for IPC efficiency
- **Filtering**: In-memory filtering with separate filtered list
- **Formatting**: Lazy formatting (on display, not on storage)

## Compilation Notes

### Build Errors Fixed

1. **MonotonicTime API** (RequestServer/ConnectionFromClient.cpp:619): Changed `to_milliseconds()` to `milliseconds()` (correct method name)
2. **Type Ambiguity in Response Code** (RequestServer/ConnectionFromClient.cpp:620): Changed `"0"sv` to `ByteString("0")` to avoid StringView/ByteString conversion ambiguity
3. **StringView Type Mismatch in Filter** (NetworkAuditDialog.cpp:164): Changed `filter_text.toUtf8().data()` (char*) to `ByteString(filter_text.toUtf8().data())` for `contains()` method
4. **Type Ambiguity in CSV Export** (NetworkAuditDialog.cpp:203): Changed `"-"sv` to `ByteString("-")` to match ByteString type in ternary operator

### Runtime Issues Fixed

1. **Icon Path Error** (Tab.cpp:208): Changed `inspector-object.png` to `network.png` (icon file exists)
2. **Bandwidth Tracking Bug** (RequestServer/ConnectionFromClient.cpp:1168-1181): Implemented `log_response()` call in `check_active_requests()` to populate bytes_sent and bytes_received from curl

The bandwidth tracking bug was caused by NetworkIdentity's `log_response()` method never being called. The system correctly logged requests via `log_request()` but didn't update the audit entries with actual bandwidth data when requests completed. Fixed by adding curl info extraction in the request completion handler:

```cpp
// Extract bandwidth information for audit logging
curl_off_t bytes_sent = 0;
curl_off_t bytes_received = 0;
curl_easy_getinfo(msg->easy_handle, CURLINFO_SIZE_UPLOAD_T, &bytes_sent);
curl_easy_getinfo(msg->easy_handle, CURLINFO_SIZE_DOWNLOAD_T, &bytes_received);

// Log response with bandwidth in NetworkIdentity audit trail
if (m_network_identity && request->http_status_code.has_value()) {
    m_network_identity->log_response(
        request->url,
        static_cast<u16>(*request->http_status_code),
        static_cast<size_t>(bytes_sent),
        static_cast<size_t>(bytes_received));
}
```

### Build Command
```bash
./Meta/ladybird.py build
```

### Build Time
Incremental build: ~2-5 minutes (after IPC code generation)

## Code Review Checklist

- [x] IPC messages defined correctly in .ipc files
- [x] Handler declarations match IPC definitions
- [x] Error handling for missing NetworkIdentity
- [x] Proper CSV escaping for special characters
- [x] Memory management (dialog deletion, Vector moves)
- [x] Consistent naming conventions (snake_case for methods)
- [x] Debug logging for troubleshooting
- [x] Qt best practices (signal-slot connections, layout management)
- [x] No memory leaks (RAII, proper deletion)
- [x] Thread safety (all operations on main thread)

## Related Documentation

- **Milestone 1.3**: NetworkIdentity Per-Tab Implementation
- **Milestone 1.4**: VPN/Proxy Integration (UI patterns reference)
- **Phase 2 Security Enhancements**: Future audit log security analysis
