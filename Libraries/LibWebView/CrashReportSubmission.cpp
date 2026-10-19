/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <LibCore/ImmutableBytes.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibWebView/CrashReportDiagnostics.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/CrashReportSubmission.h>

namespace WebView {

static constexpr auto diagnostics_reference = "diagnostics"sv;
static constexpr auto diagnostics_name = "crash-diagnostics.txt"sv;

static ByteString sha256_hex(ReadonlyBytes bytes)
{
    return encode_hex(Crypto::Hash::SHA256::hash(bytes).bytes());
}

static ReportSubmission::Contents describe_report(CrashReportSubmission::Options const& options, StringView text)
{
    auto diagnostics = CrashReportDiagnostics::parse(text);

    ReportSubmission::Contents contents;
    contents.kind = "crash"sv;

    contents.add_text("stack"sv, "stack_trace"sv, diagnostics.stack);
    contents.add_text("process"sv, "text"sv, diagnostics.header("Process"sv));
    contents.add_text("platform"sv, "text"sv, diagnostics.header("Platform"sv));
    contents.add_text("architecture"sv, "text"sv, diagnostics.header("Architecture"sv));
    contents.add_text("git_commit"sv, "text"sv, diagnostics.header("Git commit"sv));
    contents.add_text("build_configuration"sv, "text"sv, diagnostics.header("Build configuration"sv));
    contents.add_text("cpp_compiler"sv, "text"sv, diagnostics.header("C++ compiler"sv));
    for (auto label : { "Verification failed"sv, "Assertion failed"sv, "Rust panic"sv }) {
        auto value = diagnostics.header(label);
        if (value.is_empty())
            continue;
        auto failure_reason = ByteString::formatted("{}: {}", label, value);
        contents.add_text("failure_reason"sv, "text"sv, failure_reason);
        break;
    }
    contents.add_text("signal"sv, "text"sv, diagnostics.signal.name);
    if (diagnostics.signal.number.has_value())
        contents.add("signal_number"sv, "number"sv, *diagnostics.signal.number);
    contents.add_text("description"sv, "multiline"sv, options.description);
    contents.add_text("url"sv, "text"sv, options.url);

    auto version = diagnostics.header("Version"sv);
    auto build = diagnostics.header("Build configuration"sv);
    contents.client_version = version.is_empty() ? ByteString { "Unknown Ladybird version"sv } : move(version);
    contents.build = build.is_empty() ? ByteString { "Unknown build"sv } : move(build);
    return contents;
}

NonnullRefPtr<CrashReportSubmission> CrashReportSubmission::create(CrashReportStore const& store, Options options)
{
    return adopt_ref(*new CrashReportSubmission(store, move(options)));
}

// A crash report submission removes the local copy after the server acknowledges it. It can outlive the review that
// started it, and so the store that review was given, which is why it keeps a store of its own.
CrashReportSubmission::CrashReportSubmission(CrashReportStore const& store, Options options)
    : m_store(store)
    , m_options(move(options))
{
}

CrashReportSubmission::~CrashReportSubmission() = default;

ErrorOr<ReportSubmission::Payload, String> CrashReportSubmission::prepare()
{
    auto report = m_store.saved_report(m_options.report_name);
    if (report.is_error()) {
        warnln("Could not read the crash report for sending: {}", report.error());
        return "Could not prepare this report."_string;
    }
    if (sha256_hex(report.value().text.bytes()) != m_options.reviewed_text_digest) {
        warnln("Crash report {} changed after it was reviewed", m_options.report_name);
        return "This report changed after it was reviewed, so it was not sent."_string;
    }

    auto const& text = report.value().text;
    Vector<Attachment> attachments;
    attachments.append({
        .id = diagnostics_reference,
        .name = diagnostics_name,
        .media_type = "text/plain"sv,
        .content = MUST(Core::ImmutableBytes::copy(text.bytes())),
    });

    auto manifest = m_options.manifest.is_empty() ? build_manifest(describe_report(m_options, text), attachments) : move(m_options.manifest);
    return Payload { move(manifest), move(attachments) };
}

void CrashReportSubmission::did_send()
{
    if (auto result = m_store.remove_sent_report(m_options.report_name); result.is_error())
        warnln("Could not remove the sent crash report: {}", result.error());
}

}
