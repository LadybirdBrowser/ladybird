/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <LibCore/File.h>
#include <LibWebView/CrashReportStore.h>
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
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<CrashReport>> create(ProcessType);
    static bool is_supported();

    int fd() const { return m_file->fd(); }
    ByteString const& saved_name() const { return m_saved_name; }

    // What a report says of the build and system that crashed. A browser records it with its record, since a later
    // launch formats the report, and that launch may be another build.
    static ByteString describe_current_build(ProcessType);
    ErrorOr<void> record_build_description(StringView description);

    ErrorOr<void> save(int wait_status, ByteString const& directory = CrashReportStore::default_directory(),
        Optional<UnixDateTime> crashed_at = {});

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
    ByteString m_saved_name;
};

}
