/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibGfx/Size.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ReportDetail.h>
#include <LibWebView/ReportSubmission.h>

namespace WebView {

class WEBVIEW_API WebCompatReport : public Weakable<WebCompatReport> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr size_t maximum_description_bytes = 16384;
    static constexpr size_t maximum_page_dumps_bytes = 8 * MiB;
    static constexpr size_t maximum_screenshot_bytes = 8 * MiB;
    static constexpr size_t maximum_screenshot_pixels = 8'000'000;

    struct Page {
        URL::URL url;
        String user_agent;
        Gfx::IntSize viewport_size;
        double device_pixel_ratio { 1 };
        double zoom_level { 1 };
    };

    enum class IncludePageDumps : bool {
        No,
        Yes,
    };

    static Page describe_page(ViewImplementation const&);
    static NonnullRefPtr<Core::Promise<String>> request_page_dumps(ViewImplementation&);
    static Optional<String> validate(StringView description);

    explicit WebCompatReport(Page);
    ~WebCompatReport();

    Page const& page() const { return m_page; }
    Vector<ReportDetail> details() const;

    void set_page_dumps(String);
    bool has_page_dumps() const { return m_page_dumps.has_value(); }

    ByteString diagnostics(IncludePageDumps) const;

    Optional<String> send(String const& description, Optional<ByteBuffer> screenshot_png, IncludePageDumps);
    void retry();
    bool is_sending() const { return m_submission; }
    void cancel_sending();

    Function<void(ReportSubmission::Stage)> on_progress;
    Function<void(String reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds)> on_retry;
    Function<void()> on_sent;
    Function<void(ReportSubmission::Failure, String reason)> on_failed;

private:
    void start_submission();

    Page m_page;
    Optional<String> m_page_dumps;
    Optional<ReportSubmission::Payload> m_payload;
    RefPtr<ReportSubmission> m_submission;
};

}
