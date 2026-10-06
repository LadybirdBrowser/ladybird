/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/Hex.h>
#include <AK/WeakPtr.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibWebView/CrashReportDiagnostics.h>
#include <LibWebView/CrashReportReview.h>

namespace WebView {

static constexpr auto failure_headers = Array { "Verification failed"sv, "Assertion failed"sv, "Rust panic"sv, "Exit code"sv };

static String to_string(StringView text)
{
    return String::from_utf8_with_replacement_character(text);
}

static ByteString sha256_hex(ReadonlyBytes bytes)
{
    return encode_hex(Crypto::Hash::SHA256::hash(bytes).bytes());
}

CrashReportReview::CrashReportReview(CrashReportStore& store)
    : m_store(store)
{
}

CrashReportReview::~CrashReportReview() = default;

ErrorOr<CrashReportReview::Report> CrashReportReview::open(Optional<ByteString> const& name)
{
    VERIFY(!m_submission);

    m_report_name = {};
    m_text_digest = {};
    m_prepared_manifest = {};

    auto report_name = name.value_or({});
    if (report_name.is_empty()) {
        auto pending = TRY(m_store.pending_report_names());
        if (pending.is_empty())
            return Error::from_string_literal("There is no crash report to review");
        report_name = pending.first();
    }

    auto saved_report = TRY(m_store.saved_report(report_name));

    // Without this, a report closed rather than answered would come back on every launch.
    if (auto result = m_store.mark_seen(report_name); result.is_error())
        warnln("Could not mark crash report as seen: {}", result.error());

    m_report_name = report_name;
    m_text_digest = sha256_hex(saved_report.text.bytes());

    // A submission sends what this same parse produces, so the user cannot be shown one reading of a report and send
    // another.
    auto diagnostics = CrashReportDiagnostics::parse(saved_report.text);

    Report report;
    report.name = report_name;

    auto add_field = [&](StringView label, StringView value, bool is_code = false) {
        if (!value.is_empty())
            report.fields.append({ to_string(label), to_string(value), is_code });
    };

    // A signal reads best as its name followed by its number.
    auto signal = [&](StringView prefix) {
        auto name = diagnostics.header(prefix);
        auto number = diagnostics.header(ByteString::formatted("{} number", prefix));
        if (name.is_empty() || number.is_empty())
            return name;
        return ByteString::formatted("{} ({})", name, number);
    };

    // Shown in local time.
    if (diagnostics.crashed_at.has_value()) {
        if (auto crashed_at = diagnostics.crashed_at->to_string("%Y-%m-%d %H:%M:%S"sv, UnixDateTime::LocalTime::Yes); !crashed_at.is_error())
            add_field("Crash date"sv, crashed_at.value().bytes_as_string_view());
    }
    for (auto header : failure_headers) {
        if (auto value = diagnostics.header(header); !value.is_empty()) {
            add_field(header, value, true);
            break;
        }
    }
    add_field("Termination signal"sv, signal("Termination signal"sv));
    add_field("Ladybird version"sv, diagnostics.header("Version"sv), true);
    add_field("Platform"sv, diagnostics.header("Platform"sv));
    add_field("Process"sv, diagnostics.header("Process"sv));
    add_field("Architecture"sv, diagnostics.header("Architecture"sv));
    add_field("Git commit"sv, diagnostics.header("Git commit"sv), true);

    return report;
}

bool CrashReportReview::has_next_report() const
{
    auto pending = m_store.pending_report_names();
    if (pending.is_error())
        return false;
    return any_of(pending.value(), [&](auto const& name) { return name != m_report_name; });
}

ErrorOr<void> CrashReportReview::show_report() const
{
    VERIFY(!m_report_name.is_empty());
    return m_store.show_report(m_report_name);
}

Optional<String> CrashReportReview::validate(StringView description, Optional<StringView> url)
{
    if (description.length() > maximum_description_bytes)
        return "This description is too long to include in a report."_string;
    if (!url.has_value())
        return {};
    if (url->is_empty())
        return "Enter a website URL or leave the option unchecked."_string;
    if (url->length() > maximum_url_bytes)
        return "This website URL is too long to include in a report."_string;
    if (!url->starts_with("http://"sv, CaseSensitivity::CaseInsensitive)
        && !url->starts_with("https://"sv, CaseSensitivity::CaseInsensitive))
        return "Enter an http or https URL."_string;
    return {};
}

Optional<String> CrashReportReview::send(String const& description, Optional<String> const& url)
{
    VERIFY(!m_report_name.is_empty());
    VERIFY(!m_submission);

    Optional<StringView> url_view;
    if (url.has_value())
        url_view = url->bytes_as_string_view();
    if (auto error = validate(description, url_view); error.has_value())
        return error;

    auto submission = CrashReportSubmission::create(m_store,
        {
            .report_name = m_report_name,
            .description = description.to_byte_string(),
            .url = url.has_value() ? url->to_byte_string() : ByteString {},
            .reviewed_text_digest = m_text_digest,
            .manifest = m_prepared_manifest,
        });

    submission->on_progress = [weak_this = make_weak_ptr()](auto stage) {
        if (auto* self = weak_this.ptr(); self && self->on_progress)
            self->on_progress(stage);
    };
    submission->on_prepared = [weak_this = make_weak_ptr()](ByteString const& manifest) {
        if (auto* self = weak_this.ptr())
            self->m_prepared_manifest = manifest;
    };
    submission->on_retry = [weak_this = make_weak_ptr()](String reason, u32 retry_number, u32 delay_seconds) {
        if (auto* self = weak_this.ptr(); self && self->on_retry)
            self->on_retry(move(reason), retry_number, CrashReportSubmission::maximum_attempts - 1, delay_seconds);
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
    return {};
}

}
