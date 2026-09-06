/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <LibCore/File.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ProcessType.h>

namespace Core::CrashReportData {

struct ReportFrame;

}

namespace WebView {

// Only bounded native diagnostics and assertion text cross the helper boundary.
// stderr, page data, and process memory are never saved.
class WEBVIEW_API CrashReport {
public:
    static ErrorOr<NonnullOwnPtr<CrashReport>> create(ProcessType);
    static ByteString directory();
    static bool is_supported();
    static ErrorOr<void> show_directory();
    int fd() const { return m_file->fd(); }
    ErrorOr<void> save(int wait_status, ByteString const& directory = CrashReport::directory());

    explicit CrashReport(NonnullOwnPtr<Core::File> file, ProcessType process_type)
        : m_file(move(file))
        , m_process_type(process_type)
    {
    }

private:
    static ByteString symbolicate_frame(Core::CrashReportData::ReportFrame const&);

    NonnullOwnPtr<Core::File> m_file;
    [[maybe_unused]] ProcessType m_process_type;
    MonotonicTime m_started_at { MonotonicTime::now() };
};

}
