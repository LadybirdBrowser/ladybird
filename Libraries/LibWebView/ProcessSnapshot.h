/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

enum class ProcessPageKind : u8 {
    TopLevel,
    Frame,
};

// The page that a WebContent process is shown with in the task manager.
struct ProcessPageSnapshot {
    u64 view_id { 0 };
    URL::URL url;
    ProcessPageKind kind { ProcessPageKind::TopLevel };
    Optional<String> favicon_hash;
};

struct ProcessSnapshot {
    u64 child_id { 0 };
    pid_t pid { 0 };
    ProcessType type { ProcessType::Browser };
    Optional<Utf16String> title;
    Optional<URL::URL> site_url;
    Optional<u64> owner_child_id;
    float cpu_percent { 0 };
    u64 memory_usage_bytes { 0 };
    Optional<ProcessPageSnapshot> page;
};

// Updates the process statistics, and describes every process with the page that best represents it.
WEBVIEW_API Vector<ProcessSnapshot> process_snapshots(ProcessManager&);

WEBVIEW_API Optional<ByteBuffer> favicon_png_for_page(ProcessPageSnapshot const&);

}
