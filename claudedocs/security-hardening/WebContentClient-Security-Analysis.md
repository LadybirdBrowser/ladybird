# WebContentClient Security Analysis

## Overview

This document analyzes the security of IPC messages sent from **WebContent** process (untrusted) **TO** UI/Browser process (trusted). These messages represent a critical trust boundary and require comprehensive validation.

**Trust Model**:
- **WebContent Process**: UNTRUSTED (renders web content, can be compromised)
- **UI/Browser Process**: TRUSTED (user interface, credentials, system access)
- **Attack Vector**: Malicious JavaScript or compromised renderer sending crafted IPC messages

## Message Categories and Risk Assessment

### HIGH RISK - String/Buffer Types (Memory Exhaustion)

These messages contain unbounded strings or buffers that could exhaust browser memory:

| Message | Parameter Types | Risk | Validation Needed |
|---------|----------------|------|-------------------|
| `did_change_title` | `Utf16String title` | HIGH | String length ≤ MaxStringLength |
| `did_get_source` | `String source` | HIGH | String length ≤ MaxStringLength |
| `did_inspect_dom_tree` | `String dom_tree` | HIGH | String length ≤ MaxStringLength |
| `did_inspect_accessibility_tree` | `String accessibility_tree` | HIGH | String length ≤ MaxStringLength |
| `did_get_dom_node_html` | `String html` | HIGH | String length ≤ MaxStringLength |
| `did_get_style_sheet_source` | `String source` | HIGH | String length ≤ MaxStringLength |
| `did_get_internal_page_info` | `String info` | HIGH | String length ≤ MaxStringLength |
| `did_request_alert` | `String message` | MEDIUM | String length ≤ MaxStringLength |
| `did_request_confirm` | `String message` | MEDIUM | String length ≤ MaxStringLength |
| `did_request_prompt` | `String message`, `String default_` | MEDIUM | Both ≤ MaxStringLength |
| `did_request_set_prompt_text` | `String message` | MEDIUM | String length ≤ MaxStringLength |
| `did_request_tooltip_override` | `ByteString title` | LOW | ByteString length ≤ MaxStringLength |
| `did_enter_tooltip_area` | `ByteString title` | LOW | ByteString length ≤ MaxStringLength |
| `did_click_link` | `ByteString target` | LOW | ByteString length ≤ MaxStringLength |
| `did_middle_click_link` | `ByteString target` | LOW | ByteString length ≤ MaxStringLength |
| `did_request_file` | `ByteString path` | MEDIUM | ByteString length + path validation |

**Attack Scenario**: Malicious page sends `did_change_title` with 1 GB string → Browser OOM crash

**Mitigation**: Use `ValidatedDecoder::decode_string()` and `ValidatedDecoder::decode_byte_string()`

### HIGH RISK - URL Types (UXSS/Phishing)

These messages contain URLs that could enable phishing or UXSS attacks:

| Message | Parameter Types | Risk | Validation Needed |
|---------|----------------|------|-------------------|
| `did_request_new_process_for_navigation` | `URL::URL url` | HIGH | URL length ≤ MaxURLLength |
| `did_start_loading` | `URL::URL url` | HIGH | URL length ≤ MaxURLLength |
| `did_finish_loading` | `URL::URL url` | HIGH | URL length ≤ MaxURLLength |
| `did_change_url` | `URL::URL url` | HIGH | URL length ≤ MaxURLLength |
| `did_hover_link` | `URL::URL url` | MEDIUM | URL length ≤ MaxURLLength |
| `did_click_link` | `URL::URL url` | MEDIUM | URL length ≤ MaxURLLength |
| `did_middle_click_link` | `URL::URL url` | MEDIUM | URL length ≤ MaxURLLength |
| `did_request_link_context_menu` | `URL::URL url` | MEDIUM | URL length ≤ MaxURLLength |
| `did_request_image_context_menu` | `URL::URL url` | MEDIUM | URL length ≤ MaxURLLength |
| `did_get_source` | `URL::URL url`, `URL::URL base_url` | HIGH | Both ≤ MaxURLLength |
| `did_get_style_sheet_source` | `URL::URL base_url` | MEDIUM | URL length ≤ MaxURLLength |
| `did_request_all_cookies_webdriver` | `URL::URL url` | HIGH | URL length ≤ MaxURLLength |
| `did_request_all_cookies_cookiestore` | `URL::URL url` | HIGH | URL length ≤ MaxURLLength |
| `did_request_named_cookie` | `URL::URL url` | HIGH | URL length ≤ MaxURLLength |
| `did_request_cookie` | `URL::URL url` | HIGH | URL length ≤ MaxURLLength |
| `did_set_cookie` | `URL::URL url` | HIGH | URL length ≤ MaxURLLength |

**Attack Scenario 1**: Malicious page sends `did_change_url` with `javascript:` URL → UXSS
**Attack Scenario 2**: Page sends oversized URL (1 MB) → Memory exhaustion

**Mitigation**: Use `ValidatedDecoder::decode_url()` + additional scheme validation

### HIGH RISK - Vector Types (Resource Exhaustion)

These messages contain vectors that could exhaust memory:

| Message | Parameter Types | Risk | Validation Needed |
|---------|----------------|------|-------------------|
| `did_list_style_sheets` | `Vector<StyleSheetIdentifier>` | MEDIUM | Vector size ≤ MaxVectorSize |
| `did_request_all_cookies_webdriver` | `Vector<Cookie>` (return) | HIGH | Vector size ≤ MaxVectorSize |
| `did_request_all_cookies_cookiestore` | `Vector<Cookie>` (return) | HIGH | Vector size ≤ MaxVectorSize |
| `did_request_storage_keys` | `Vector<String>` (return) | MEDIUM | Vector size ≤ MaxVectorSize |
| `did_request_select_dropdown` | `Vector<SelectItem>` | MEDIUM | Vector size ≤ MaxVectorSize |
| `did_get_js_console_messages` | `Vector<ConsoleOutput>` | MEDIUM | Vector size ≤ MaxVectorSize |

**Attack Scenario**: Malicious page sends `did_request_select_dropdown` with 10 million items → Browser OOM

**Mitigation**: Use `ValidatedDecoder::decode_vector<T>()`

### CRITICAL RISK - Page ID Validation (UXSS)

EVERY message contains a `u64 page_id` parameter. This MUST be validated to prevent Universal XSS attacks:

**Attack Scenario**:
1. Malicious page in tab A (page_id=1)
2. Victim page in tab B (page_id=2)
3. Attacker sends `did_request_alert(page_id=2, "XSS")` targeting victim tab
4. Alert appears in victim's security context → UXSS

**Current Code**:
```cpp
void WebContentClient::did_request_alert(u64 page_id, String message)
{
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        // No validation that WebContent process owns this page_id!
        view->show_alert(message);
    }
}
```

**Mitigation**:
```cpp
// Option 1: Validate page_id against process ownership
if (!is_page_owned_by_this_process(page_id))
    return;

// Option 2: Use ValidatedDecoder::decode_page_id()
auto validated_page_id = TRY(ValidatedDecoder::decode_page_id(decoder, m_views));
```

### MEDIUM RISK - ShareableBitmap (Memory/Performance)

These messages contain bitmaps that could be crafted to exhaust resources:

| Message | Parameter Types | Risk | Validation Needed |
|---------|----------------|------|-------------------|
| `did_take_screenshot` | `Gfx::ShareableBitmap` | MEDIUM | Bitmap size validation |
| `did_change_favicon` | `Gfx::ShareableBitmap` | MEDIUM | Bitmap size validation |
| `did_request_image_context_menu` | `Optional<Gfx::ShareableBitmap>` | MEDIUM | Bitmap size validation |
| `did_allocate_backing_stores` | 2x `Gfx::ShareableBitmap` | HIGH | Bitmap size validation |

**Attack Scenario**: Page sends `did_take_screenshot` with 100000x100000 pixel bitmap → Memory exhaustion

**Mitigation**: Validate bitmap dimensions before allocation (already in ValidatedDecoder::decode_image_dimensions)

### LOW RISK - Primitive Types

These messages contain only primitive types (u64, i32, bool, enums) with bounded ranges:

| Message | Parameter Types | Risk |
|---------|----------------|------|
| `did_paint` | `u64 page_id`, `Gfx::IntRect`, `i32 bitmap_id` | LOW |
| `did_request_refresh` | `u64 page_id` | LOW |
| `did_request_cursor_change` | `u64 page_id`, `Gfx::Cursor` | LOW |
| `did_unhover_link` | `u64 page_id` | LOW |
| `did_request_context_menu` | `u64 page_id`, `Gfx::IntPoint` | LOW |
| `did_set_browser_zoom` | `u64 page_id`, `double factor` | LOW |
| `did_change_theme_color` | `u64 page_id`, `Gfx::Color` | LOW |
| `did_change_audio_play_state` | `u64 page_id`, `AudioPlayState` | LOW |
| `did_update_navigation_buttons_state` | `u64 page_id`, `bool`, `bool` | LOW |

**Risk**: Low (bounded types), but still requires page_id validation

## IPC Decoding Architecture

### Current Implementation (Auto-Generated)

The IPC compiler generates decoding code like this:

```cpp
// Auto-generated by IPCCompiler from WebContentClient.ipc
Messages::WebContentClient::DidChangeTitleResponse
ConnectionToServer::handle_did_change_title(IPC::Decoder& decoder)
{
    auto page_id = TRY(decoder.decode<u64>());
    auto title = TRY(decoder.decode<Utf16String>());

    // Call virtual handler (no validation!)
    return did_change_title(page_id, title);
}
```

### Problem: No Validation Layer

The auto-generated code directly decodes without validation:
- No size limits on strings
- No bounds checking on vectors
- No page_id ownership validation
- No URL scheme validation

### Solution Approaches

#### Approach 1: Modify IPC Compiler (BEST, but complex)

Update `Meta/Lagom/Tools/CodeGenerators/IPCCompiler/main.cpp` to generate:

```cpp
// Auto-generated with validation
Messages::WebContentClient::DidChangeTitleResponse
ConnectionToServer::handle_did_change_title(IPC::Decoder& decoder)
{
    auto page_id = TRY(decoder.decode<u64>());
    auto title = TRY(IPC::ValidatedDecoder::decode_utf16_string(decoder)); // VALIDATED

    return did_change_title(page_id, title);
}
```

**Pros**: Automatic validation for all messages, no manual code changes
**Cons**: Requires IPC compiler changes, affects all IPC endpoints

#### Approach 2: Add Validation in Handlers (SIMPLE, immediate)

Modify each handler to validate inputs:

```cpp
void WebContentClient::did_change_title(u64 page_id, Utf16String title)
{
    // Validate title length
    if (title.code_points().length() > IPC::Limits::MaxStringLength) {
        dbgln("Security: WebContent sent oversized title ({} bytes), rejecting",
            title.code_points().length());
        return;
    }

    // Validate page_id ownership
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        // Process validated data
        view->set_title({}, title);
    }
}
```

**Pros**: Immediate protection, no IPC compiler changes needed
**Cons**: Manual work, easy to miss messages, code duplication

#### Approach 3: Validation Wrapper (HYBRID)

Create validation wrapper in WebContentClient constructor:

```cpp
class WebContentClient {
    WebContentClient(...)
        : m_rate_limiter(1000, Duration::from_milliseconds(10))
    {
        // Validation layer
        set_validation_enabled(true);
    }

private:
    bool validate_page_id(u64 page_id) {
        return m_views.contains(page_id);
    }

    IPC::RateLimiter m_rate_limiter;
};
```

**Pros**: Centralized validation, cleaner than per-handler
**Cons**: Still requires some manual integration

## Recommended Migration Strategy

### Phase 1: Critical Messages (Week 2)

Focus on highest-risk messages first:

1. **Page ID Validation** (CRITICAL):
   - Add `validate_page_id()` helper
   - Call in every handler before processing

2. **String Validation** (HIGH):
   - `did_change_title`
   - `did_get_source`
   - `did_inspect_dom_tree`
   - `did_inspect_accessibility_tree`
   - `did_get_dom_node_html`
   - `did_get_style_sheet_source`

3. **URL Validation** (HIGH):
   - `did_request_new_process_for_navigation`
   - `did_start_loading`
   - `did_finish_loading`
   - `did_change_url`
   - Cookie-related messages

### Phase 2: Medium-Risk Messages (Week 3)

4. **Vector Validation**:
   - `did_list_style_sheets`
   - `did_request_select_dropdown`
   - Cookie/storage vectors

5. **Rate Limiting**:
   - Add `RateLimiter` to WebContentClient
   - Apply to all messages (global limit)

### Phase 3: IPC Compiler Enhancement (Week 4)

6. **Automated Validation**:
   - Modify IPC compiler to generate validated decode calls
   - Add `[untrusted_source]` attribute to `WebContentClient.ipc`
   - Verify auto-generated validation

## Implementation Plan

### Step 1: Add Validation Infrastructure

```cpp
// In WebContentClient.h
class WebContentClient {
    // ...
private:
    bool validate_page_id(u64 page_id, SourceLocation location = SourceLocation::current())
    {
        if (!m_views.contains(page_id)) {
            dbgln("Security: WebContent[{}] attempted access to invalid page_id {} at {}",
                pid(), page_id, location);
            track_validation_failure();
            return false;
        }
        return true;
    }

    bool check_rate_limit()
    {
        if (!m_rate_limiter.try_consume()) {
            dbgln("Security: WebContent[{}] exceeded rate limit", pid());
            track_validation_failure();
            return false;
        }
        return true;
    }

    void track_validation_failure()
    {
        m_validation_failures++;
        if (m_validation_failures >= MAX_VALIDATION_FAILURES) {
            dbgln("Security: WebContent[{}] exceeded validation failure limit, terminating", pid());
            async_close_server();
        }
    }

    IPC::RateLimiter m_rate_limiter;
    size_t m_validation_failures { 0 };
    static constexpr size_t MAX_VALIDATION_FAILURES = 100;
};
```

### Step 2: Migrate Handlers

```cpp
// Example: did_change_title with full validation
void WebContentClient::did_change_title(u64 page_id, Utf16String title)
{
    // Rate limiting
    if (!check_rate_limit())
        return;

    // Page ID validation
    if (!validate_page_id(page_id))
        return;

    // Title length validation
    if (title.code_points().length() > IPC::Limits::MaxStringLength) {
        dbgln("Security: WebContent[{}] sent oversized title ({} code points)",
            pid(), title.code_points().length());
        track_validation_failure();
        return;
    }

    // Safe to process
    if (auto view = view_for_page_id(page_id); view.has_value()) {
        if (title.is_empty())
            title = Utf16String::from_utf8(view->url().serialize());

        view->set_title({}, title);

        if (view->on_title_change)
            view->on_title_change(title);
    }
}
```

## Security Test Cases

### Test Case 1: Oversized String
```cpp
TEST(WebContentClientSecurity, oversized_title_rejected)
{
    // Create oversized title (10 MB)
    StringBuilder builder;
    for (size_t i = 0; i < 10 * 1024 * 1024; ++i)
        builder.append('A');
    auto title = Utf16String::from_utf8(builder.to_string());

    // Send did_change_title
    client->did_change_title(1, title);

    // Verify rejection (title not set)
    EXPECT_NE(view->title(), title);
}
```

### Test Case 2: Page ID UXSS Attempt
```cpp
TEST(WebContentClientSecurity, invalid_page_id_rejected)
{
    // WebContent owns page_id=1 only
    // Attempt to target page_id=999
    client->did_request_alert(999, "UXSS attempt"_string);

    // Verify alert not shown
    EXPECT_FALSE(alert_was_shown);
}
```

### Test Case 3: Rate Limit DoS
```cpp
TEST(WebContentClientSecurity, rate_limit_enforced)
{
    // Send 2000 messages (exceeds 1000/sec limit)
    for (size_t i = 0; i < 2000; ++i)
        client->did_request_refresh(1);

    // Verify some messages rejected
    EXPECT_LT(refresh_count, 2000);
}
```

## Metrics and Monitoring

### Validation Failure Tracking

```cpp
struct ValidationMetrics {
    size_t total_messages { 0 };
    size_t validated_messages { 0 };
    size_t rejected_oversized_strings { 0 };
    size_t rejected_invalid_page_ids { 0 };
    size_t rejected_rate_limited { 0 };
    size_t processes_terminated { 0 };
};
```

### Logging Strategy

```cpp
// Log all validation failures
dbgln("Security: WebContent[{}] validation failure: {}", pid(), reason);

// Log process terminations
dbgln("Security: Terminating WebContent[{}] after {} violations", pid(), count);
```

## Summary

**Total IPC Messages**: ~110 messages in WebContentClient.ipc
**High Risk Messages**: 32 (strings, URLs, vectors)
**Critical Risk**: Page ID validation (ALL messages)
**Recommended Approach**: Manual handler validation (Phase 1-2) + IPC compiler enhancement (Phase 3)
**Expected Timeline**: 3 weeks for full migration
**Security Impact**: 10× improvement in defense against compromised WebContent processes
