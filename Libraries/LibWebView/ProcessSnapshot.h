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
};

// A tab whose page a WebContent process hosts.
struct ProcessTabSnapshot {
    Utf16String title;
    ProcessPageSnapshot page;
};

// A frame whose document a WebContent process hosts for a tab another process displays.
struct ProcessFrameSnapshot {
    URL::URL url;
    bool is_sandboxed { false };
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
    // The tabs whose pages the process hosts, and the frames of other tabs' pages it hosts.
    Vector<ProcessTabSnapshot> tabs;
    Vector<ProcessFrameSnapshot> frames;
};

// Updates the process statistics, and describes every process with the page that best represents it.
WEBVIEW_API Vector<ProcessSnapshot> process_snapshots(ProcessManager&);

// Ends a process, and is satisfied with one that is already gone. Returns false for one that could not be ended.
WEBVIEW_API bool end_process(ProcessManager&, u64 child_id);

}
