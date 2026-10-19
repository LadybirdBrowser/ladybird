/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StringBuilder.h>
#include <AK/WeakPtr.h>
#include <LibCore/ImmutableBytes.h>
#include <LibCore/Promise.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebCommon/WebView/PageInfo.h>
#include <LibWebView/Application.h>
#include <LibWebView/BuildInformation.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebCompatReport.h>

namespace WebView {

static constexpr auto diagnostics_reference = "diagnostics"sv;
static constexpr auto diagnostics_name = "webcompat-diagnostics.txt"sv;
static constexpr auto screenshot_reference = "screenshot"sv;
static constexpr auto screenshot_name = "screenshot.png"sv;

static constexpr auto browser_version = BROWSER_VERSION ""sv;
static constexpr auto platform = OS_STRING ""sv;
static constexpr auto architecture = CPU_STRING ""sv;

#ifdef NDEBUG
static constexpr auto build_configuration = "release"sv;
#else
static constexpr auto build_configuration = "debug"sv;
#endif

static String to_string(StringView text)
{
    return String::from_utf8_with_replacement_character(text);
}

static String format_viewport_size(Gfx::IntSize size)
{
    return MUST(String::formatted("{}x{}", size.width(), size.height()));
}

static String format_zoom_level(double zoom_level)
{
    return MUST(String::formatted("{}%", round_to<int>(zoom_level * 100)));
}

WebCompatReport::Page WebCompatReport::describe_page(ViewImplementation const& view)
{
    // The viewport in CSS pixels, as the page sees it.
    auto device_size = view.viewport_size();
    auto scale = view.device_pixel_ratio() * view.zoom_level();
    Gfx::IntSize viewport_size {
        round_to<int>(static_cast<double>(device_size.width().value()) / scale),
        round_to<int>(static_cast<double>(device_size.height().value()) / scale),
    };

    return {
        .url = view.url(),
        .user_agent = Application::the().user_agent_string(),
        .viewport_size = viewport_size,
        .device_pixel_ratio = view.device_pixel_ratio(),
        .zoom_level = view.zoom_level(),
    };
}

// The dumps of how the page was laid out and painted, taken together so they describe the same moment. Each one starts
// with what it is: the layout tree with its viewport, the stacking context tree with "SC for", and the display list with
// its visual context tree.
NonnullRefPtr<Core::Promise<String>> WebCompatReport::request_page_dumps(ViewImplementation& view)
{
    return view.request_internal_page_info(PageInfoType::LayoutTree | PageInfoType::StackingContextTree | PageInfoType::DisplayList);
}

Optional<String> WebCompatReport::validate(StringView description)
{
    if (description.trim_whitespace().is_empty())
        return "Describe what is wrong with this website."_string;
    if (description.length() > maximum_description_bytes)
        return "This description is too long to include in a report."_string;
    return {};
}

WebCompatReport::WebCompatReport(Page page)
    : m_page(move(page))
{
}

WebCompatReport::~WebCompatReport() = default;

Vector<ReportDetail> WebCompatReport::details() const
{
    Vector<ReportDetail> details;
    details.append({ "Website"_string, m_page.url.serialize(), false });
    details.append({ "User agent"_string, m_page.user_agent, true });
    details.append({ "Viewport size"_string, format_viewport_size(m_page.viewport_size), false });
    details.append({ "Device pixel ratio"_string, MUST(String::formatted("{}", m_page.device_pixel_ratio)), false });
    details.append({ "Zoom level"_string, format_zoom_level(m_page.zoom_level), false });
    details.append({ "Ladybird version"_string, to_string(browser_version), true });
    details.append({ "Platform"_string, to_string(platform), false });
    details.append({ "Architecture"_string, to_string(architecture), false });
    details.append({ "Git commit"_string, to_string(LADYBIRD_COMMIT), true });
    return details;
}

// Dumps that exceed the limit are cut at a character boundary, and say so.
void WebCompatReport::set_page_dumps(String dumps)
{
    auto bytes = dumps.bytes();
    if (bytes.size() <= maximum_page_dumps_bytes) {
        m_page_dumps = move(dumps);
        return;
    }

    auto length = maximum_page_dumps_bytes;
    while (length > 0 && (bytes[length] & 0xC0) == 0x80)
        --length;
    m_page_dumps = MUST(String::formatted("{}\n(Truncated)\n", StringView { bytes.slice(0, length) }));
}

ByteString WebCompatReport::diagnostics(IncludePageDumps include_page_dumps) const
{
    StringBuilder builder;
    builder.append("Ladybird web compatibility report, format 1\n"sv);
    builder.appendff("Version: {}\nPlatform: {}\nArchitecture: {}\n", browser_version, platform, architecture);
    builder.appendff("Build configuration: {}\n", build_configuration);
    append_build_information_for_process(builder, ProcessType::Browser);
    builder.appendff("URL: {}\n", m_page.url.serialize());
    builder.appendff("User agent: {}\n", m_page.user_agent);
    builder.appendff("Viewport size: {}\n", format_viewport_size(m_page.viewport_size));
    builder.appendff("Device pixel ratio: {}\n", m_page.device_pixel_ratio);
    builder.appendff("Zoom level: {}\n", format_zoom_level(m_page.zoom_level));

    if (include_page_dumps == IncludePageDumps::Yes && m_page_dumps.has_value())
        builder.appendff("\n{}", *m_page_dumps);

    return builder.to_byte_string();
}

Optional<String> WebCompatReport::send(String const& description, Optional<ByteBuffer> screenshot_png,
    IncludePageDumps include_page_dumps)
{
    VERIFY(!m_submission);

    if (auto error = validate(description); error.has_value())
        return error;
    if (screenshot_png.has_value() && screenshot_png->size() > maximum_screenshot_bytes)
        return "This screenshot is too large to include in a report."_string;

    auto diagnostics_text = diagnostics(include_page_dumps);
    Vector<ReportSubmission::Attachment> attachments;
    attachments.append({
        .id = diagnostics_reference,
        .name = diagnostics_name,
        .media_type = "text/plain"sv,
        .content = MUST(Core::ImmutableBytes::copy(diagnostics_text.bytes())),
    });
    if (screenshot_png.has_value()) {
        attachments.append({
            .id = screenshot_reference,
            .name = screenshot_name,
            .media_type = "image/png"sv,
            .content = Core::ImmutableBytes::adopt(screenshot_png.release_value()),
        });
    }

    auto url = m_page.url.serialize();
    auto viewport_size = format_viewport_size(m_page.viewport_size);

    ReportSubmission::Contents contents;
    contents.kind = "web_compat"sv;
    contents.client_version = browser_version;
    contents.build = build_configuration;
    contents.add_text("description"sv, "multiline"sv, description);
    contents.add_text("url"sv, "text"sv, url);
    contents.add_text("user_agent"sv, "text"sv, m_page.user_agent);
    contents.add_text("viewport_size"sv, "text"sv, viewport_size);
    contents.add("device_pixel_ratio"sv, "number"sv, m_page.device_pixel_ratio);
    contents.add("zoom_level"sv, "number"sv, m_page.zoom_level);
    contents.add_text("platform"sv, "text"sv, platform);
    contents.add_text("architecture"sv, "text"sv, architecture);
    contents.add_text("git_commit"sv, "text"sv, LADYBIRD_COMMIT);
    contents.add_text("build_configuration"sv, "text"sv, build_configuration);

    auto manifest = ReportSubmission::build_manifest(contents, attachments);
    m_payload = ReportSubmission::Payload { move(manifest), move(attachments) };
    start_submission();
    return {};
}

void WebCompatReport::retry()
{
    VERIFY(m_payload.has_value());
    VERIFY(!m_submission);
    start_submission();
}

void WebCompatReport::cancel_sending()
{
    if (auto submission = move(m_submission))
        submission->cancel();
}

// A report on its way keeps being sent if this report is destroyed, which then only stops hearing about it.
void WebCompatReport::start_submission()
{
    auto submission = ReportSubmission::create(*m_payload);

    submission->on_progress = [weak_this = make_weak_ptr()](auto stage) {
        if (auto* self = weak_this.ptr(); self && self->on_progress)
            self->on_progress(stage);
    };
    submission->on_retry = [weak_this = make_weak_ptr()](String reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds) {
        if (auto* self = weak_this.ptr(); self && self->on_retry)
            self->on_retry(move(reason), retry_number, maximum_retries, delay_seconds);
    };
    submission->on_sent = [weak_this = make_weak_ptr()] {
        if (auto* self = weak_this.ptr()) {
            self->m_submission = nullptr;
            if (self->on_sent)
                self->on_sent();
        }
    };
    submission->on_failed = [weak_this = make_weak_ptr()](auto failure, String reason) {
        if (auto* self = weak_this.ptr()) {
            self->m_submission = nullptr;
            if (self->on_failed)
                self->on_failed(failure, move(reason));
        }
    };

    m_submission = submission;
    submission->start();
}

}
