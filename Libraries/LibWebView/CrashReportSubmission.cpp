/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NumericLimits.h>
#include <AK/Random.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Platform/ThreadQoS.h>
#include <LibCore/Timer.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/CrashReportDiagnostics.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/CrashReportSubmission.h>

namespace WebView {

static constexpr auto default_endpoint = "https://reports.app.ladybird.org"sv;
static constexpr auto challenge_path = "/api/v1/challenges"sv;
static constexpr auto report_path = "/api/v1/reports"sv;
static constexpr auto challenge_algorithm = "sha256-seed-nonce-le-v1"sv;
static constexpr auto diagnostics_reference = "diagnostics"sv;
static constexpr auto diagnostics_name = "crash-diagnostics.txt"sv;
static constexpr size_t maximum_challenge_token_length = 4096;
static constexpr u64 maximum_expected_work = 100'000'000;
static constexpr int request_timeout_ms = 30'000;
static constexpr int retry_delay_ms = 3'000;
static constexpr int maximum_retry_delay_ms = 60'000;

static String to_string(StringView text)
{
    return String::from_utf8_with_replacement_character(text);
}

static Optional<JsonObject> parse_json_object(StringView text)
{
    auto parsed = JsonValue::from_string(text);
    if (parsed.is_error() || !parsed.value().is_object())
        return {};
    return parsed.value().as_object();
}

static ByteString sha256_hex(ReadonlyBytes bytes)
{
    return encode_hex(Crypto::Hash::SHA256::hash(bytes).bytes());
}

static ByteString build_manifest(CrashReportSubmission::Options const& options, StringView text)
{
    auto diagnostics = CrashReportDiagnostics::parse(text);

    JsonArray fields;
    auto add = [&fields](StringView key, StringView type, JsonValue value) {
        JsonObject typed;
        typed.set("type"sv, to_string(type));
        typed.set("value"sv, move(value));
        JsonObject field;
        field.set("key"sv, to_string(key));
        field.set("value"sv, move(typed));
        fields.must_append(move(field));
    };
    auto add_text = [&add](StringView key, StringView type, StringView value) {
        if (!value.is_empty())
            add(key, type, to_string(value));
    };

    add_text("stack"sv, "stack_trace"sv, diagnostics.stack);
    add_text("process"sv, "text"sv, diagnostics.header("Process"sv));
    add_text("platform"sv, "text"sv, diagnostics.header("Platform"sv));
    add_text("architecture"sv, "text"sv, diagnostics.header("Architecture"sv));
    add_text("git_commit"sv, "text"sv, diagnostics.header("Git commit"sv));
    add_text("build_configuration"sv, "text"sv, diagnostics.header("Build configuration"sv));
    add_text("cpp_compiler"sv, "text"sv, diagnostics.header("C++ compiler"sv));
    for (auto label : { "Verification failed"sv, "Assertion failed"sv, "Rust panic"sv }) {
        auto value = diagnostics.header(label);
        if (value.is_empty())
            continue;
        add_text("failure_reason"sv, "text"sv, ByteString::formatted("{}: {}", label, value));
        break;
    }
    add_text("signal"sv, "text"sv, diagnostics.signal.name);
    if (diagnostics.signal.number.has_value())
        add("signal_number"sv, "number"sv, *diagnostics.signal.number);
    add_text("description"sv, "multiline"sv, options.description);
    add_text("url"sv, "text"sv, options.url);

    JsonObject attachment;
    attachment.set("id"sv, to_string(diagnostics_reference));
    attachment.set("name"sv, to_string(diagnostics_name));
    attachment.set("media_type"sv, "text/plain"_string);
    attachment.set("size"sv, text.length());
    attachment.set("sha256"sv, to_string(sha256_hex(text.bytes())));
    JsonArray attachments;
    attachments.must_append(move(attachment));

    auto version = diagnostics.header("Version"sv);
    auto build = diagnostics.header("Build configuration"sv);

    JsonObject manifest;
    manifest.set("protocol"sv, 1);
    manifest.set("submission_id"sv, generate_random_uuid());
    manifest.set("kind"sv, "crash"_string);
    manifest.set("client_version"sv, version.is_empty() ? "Unknown Ladybird version"_string : to_string(version));
    manifest.set("build"sv, build.is_empty() ? "Unknown build"_string : to_string(build));
    manifest.set("fields"sv, move(fields));
    manifest.set("attachments"sv, move(attachments));
    return manifest.serialized().to_byte_string();
}

static ByteString build_multipart_body(StringView manifest, StringView report_text, StringView boundary)
{
    StringBuilder builder;
    auto append_part = [&](StringView disposition, StringView content_type, StringView content) {
        builder.appendff("--{}\r\nContent-Disposition: form-data; {}\r\nContent-Type: {}\r\n\r\n{}\r\n",
            boundary, disposition, content_type, content);
    };

    append_part("name=\"manifest\""sv, "application/json"sv, manifest);
    append_part(ByteString::formatted("name=\"{}\"; filename=\"{}\"", diagnostics_reference, diagnostics_name),
        "text/plain; charset=utf-8"sv, report_text);

    builder.appendff("--{}--\r\n", boundary);
    return builder.to_byte_string();
}

static Optional<u64> solve_challenge(ByteString const& token, u64 expected_work, Atomic<bool> const& cancelled)
{
    auto seed = Crypto::Hash::SHA256::hash(token.bytes());
    Array<u8, 40> candidate {};
    seed.bytes().copy_to(candidate.span().slice(0, 32));
    auto threshold = NumericLimits<u64>::max() / expected_work;

    for (u64 nonce = 0; nonce < NumericLimits<u64>::max(); ++nonce) {
        if (nonce % 4096 == 0 && cancelled.load())
            return {};
        for (size_t i = 0; i < 8; ++i)
            candidate[32 + i] = static_cast<u8>(nonce >> (i * 8));
        auto digest = Crypto::Hash::SHA256::hash(candidate.span());
        u64 prefix = 0;
        for (size_t i = 0; i < 8; ++i)
            prefix = prefix << 8 | digest.bytes()[i];
        if (prefix <= threshold)
            return nonce;
    }
    return {};
}

// The server would reject anything else again, so only these are worth another attempt. A conflict means the challenge
// expired or was already used, which the fresh challenge of the next attempt resolves.
static bool is_transient_status(u32 status)
{
    return status == 408 || status == 409 || status == 429 || (status >= 500 && status < 600);
}

// Only a delay in seconds is honored. A date falls back to the regular delay between attempts.
static Optional<int> retry_after_ms(HTTP::HeaderList const& headers)
{
    auto value = headers.get("Retry-After"sv);
    if (!value.has_value())
        return {};
    auto seconds = value->view().trim_whitespace().to_number<u32>();
    if (!seconds.has_value())
        return {};
    return static_cast<int>(min<u64>(*seconds * 1000ull, maximum_retry_delay_ms));
}

NonnullRefPtr<CrashReportSubmission> CrashReportSubmission::create(CrashReportStore const& store, Options options)
{
    return adopt_ref(*new CrashReportSubmission(store, move(options)));
}

// A submission builds its manifest, solves the proof-of-work challenge off the main thread, uploads
// the report, and removes the local copy after the server acknowledges it. It can outlive the review that started it,
// and so the store that review was given, which is why it keeps a store of its own.
CrashReportSubmission::CrashReportSubmission(CrashReportStore const& store, Options options)
    : m_store(store)
    , m_options(move(options))
{
}

CrashReportSubmission::~CrashReportSubmission()
{
    m_proof_cancelled.store(true);
    if (m_proof_thread && m_proof_thread->needs_to_be_joined())
        (void)m_proof_thread->join();
}

// A submission keeps itself alive until it is sent or fails for good, so it outlives the review that started it.
void CrashReportSubmission::start()
{
    m_self_while_in_flight = *this;
    begin_attempt();
}

void CrashReportSubmission::begin_attempt()
{
    if (on_progress)
        on_progress(Stage::Preparing);

    if (m_manifest.is_empty()) {
        auto report = m_store.saved_report(m_options.report_name);
        if (report.is_error()) {
            warnln("Could not read the crash report for sending: {}", report.error());
            fail_preparation("Could not prepare this report."_string);
            return;
        }
        if (sha256_hex(report.value().text.bytes()) != m_options.reviewed_text_digest) {
            warnln("Crash report {} changed after it was reviewed", m_options.report_name);
            fail_preparation("This report changed after it was reviewed, so it was not sent."_string);
            return;
        }
        m_report_text = move(report.value().text);
        m_manifest = m_options.manifest.is_empty() ? build_manifest(m_options, m_report_text) : move(m_options.manifest);
    }

    if (on_prepared)
        on_prepared(m_manifest);
    request_challenge();
}

void CrashReportSubmission::fail_preparation(String reason)
{
    // Reporting the failure can release the last reference to this submission.
    NonnullRefPtr protector = *this;

    m_self_while_in_flight = nullptr;
    if (on_failed)
        on_failed(Failure::Preparation, move(reason));
}

void CrashReportSubmission::request_challenge()
{
    if (on_progress)
        on_progress(Stage::RequestingChallenge);

    JsonObject body;
    body.set("manifest_digest"sv, to_string(sha256_hex(m_manifest.bytes())));

    post(challenge_path, "requesting a challenge"sv, "application/json"sv, body.serialized().to_byte_string(), {},
        [this](ByteString response) {
            auto challenge = parse_json_object(response);
            if (!challenge.has_value()) {
                fail("The report server sent an unreadable challenge."_string, ShouldRetry::No);
                return;
            }

            auto algorithm = challenge->get_string("algorithm"sv);
            if (!algorithm.has_value() || *algorithm != challenge_algorithm) {
                fail("Unsupported report challenge."_string, ShouldRetry::No);
                return;
            }

            auto token = challenge->get_string("token"sv);
            auto expected_work = challenge->get_integer<u64>("expected_work"sv);
            if (!token.has_value() || token->bytes().size() > maximum_challenge_token_length
                || !expected_work.has_value() || *expected_work == 0 || *expected_work > maximum_expected_work) {
                fail("The report server supplied an invalid challenge."_string, ShouldRetry::No);
                return;
            }

            solve_proof(token->to_byte_string(), *expected_work);
        });
}

void CrashReportSubmission::solve_proof(ByteString token, u64 expected_work)
{
    if (on_progress)
        on_progress(Stage::SolvingProof);

    // A previous attempt's search has already delivered its result.
    if (m_proof_thread && m_proof_thread->needs_to_be_joined())
        (void)m_proof_thread->join();

    // The server chooses the difficulty, so the search runs on its own thread, at the priority of work that the user
    // follows through a progress indicator.
    auto& main_thread_event_loop = Core::EventLoop::current();
    m_proof_thread = Threading::Thread::construct("Crash report proof"sv,
        [weak_this = make_weak_ptr<CrashReportSubmission>(), &cancelled = m_proof_cancelled, &main_thread_event_loop,
            attempt = m_attempt, token = move(token), expected_work]() mutable -> intptr_t {
            auto nonce = solve_challenge(token, expected_work, cancelled);
            main_thread_event_loop.deferred_invoke(
                [weak_this = move(weak_this), attempt, token = move(token), nonce]() mutable {
                    auto self = weak_this.strong_ref();
                    if (!self || self->m_attempt != attempt)
                        return;
                    if (!nonce.has_value()) {
                        self->fail("Could not compute the report's proof of work."_string, ShouldRetry::No);
                        return;
                    }
                    self->send_report(ProofOfWork { move(token), *nonce });
                });
            return 0;
        });
    m_proof_thread->set_qos(Core::Platform::ThreadQoS::Utility);
    m_proof_thread->start();
}

void CrashReportSubmission::send_report(ProofOfWork proof)
{
    if (on_progress)
        on_progress(Stage::Sending);

    auto boundary = ByteString::formatted("ladybird-{}", generate_random_uuid());
    auto body = build_multipart_body(m_manifest, m_report_text, boundary);
    auto content_type = ByteString::formatted("multipart/form-data; boundary={}", boundary);

    post(report_path, "sending the report"sv, content_type, move(body), move(proof), [this](ByteString response) {
        auto body = parse_json_object(response);
        auto receipt = body.has_value() ? body->get_string("receipt"sv) : Optional<String const&> {};
        if (!receipt.has_value() || receipt->is_empty()) {
            fail("Report server did not provide a receipt."_string, ShouldRetry::No);
            return;
        }
        finish();
    });
}

// A response outside the 2xx range fails this attempt, and the activity names what was going on in that message.
void CrashReportSubmission::post(StringView path, StringView activity, StringView content_type, ByteString body,
    Optional<ProofOfWork> proof, Function<void(ByteString)> on_success)
{
    auto endpoint = Core::Environment::get("LADYBIRD_REPORTS_ENDPOINT"sv).value_or(default_endpoint);
    auto url = URL::Parser::basic_parse(MUST(String::formatted("{}{}{}", endpoint,
        endpoint.ends_with('/') ? ""sv : "/"sv, path.substring_view(1))));
    if (!url.has_value() || (url->scheme() != "https"sv && url->scheme() != "http"sv)) {
        fail("Invalid report server configuration."_string, ShouldRetry::No);
        return;
    }

    auto headers = HTTP::HeaderList::create();
    headers->set({ "Content-Type"sv, ByteString { content_type } });
    if (proof.has_value()) {
        headers->set({ "X-Ladybird-Challenge"sv, proof->token });
        headers->set({ "X-Ladybird-Nonce"sv, ByteString::number(proof->nonce) });
    }

    auto& request_client = Application::request_server_client(IsPrivate::No);
    m_request = request_client.start_request("POST"sv, *url, *headers, body.bytes(),
        HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
    if (!m_request) {
        fail("Could not connect to the report server."_string, ShouldRetry::Yes);
        return;
    }

    m_request_timeout = Core::Timer::create_single_shot(request_timeout_ms,
        [weak_this = make_weak_ptr<CrashReportSubmission>()] {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            auto request = move(self->m_request);
            self->m_request_timeout = nullptr;
            if (request)
                request->stop();
            self->fail("The report server did not respond in time."_string, ShouldRetry::Yes);
        });
    m_request_timeout->start();

    m_request->set_buffered_request_finished_callback(
        [weak_this = make_weak_ptr<CrashReportSubmission>(), activity = ByteString { activity },
            on_success = move(on_success)](u64, Requests::RequestTimingInfo const&,
            Optional<Requests::NetworkError> const& network_error, HTTP::HeaderList const& response_headers,
            Optional<u32> response_code, Optional<String> const&,
            Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CacheState,
            Core::ImmutableBytes payload) {
            auto self = weak_this.strong_ref();
            if (!self || !self->m_request)
                return;
            if (self->m_request_timeout)
                self->m_request_timeout->stop();
            self->m_request_timeout = nullptr;

            // A request cannot be destroyed from inside its own callback.
            Core::deferred_invoke([weak_this] {
                if (auto self = weak_this.strong_ref())
                    self->m_request = nullptr;
            });

            if (network_error.has_value() || !response_code.has_value()) {
                self->fail("Network error while contacting the report server."_string, ShouldRetry::Yes);
                return;
            }
            if (*response_code < 200 || *response_code >= 300) {
                self->fail(MUST(String::formatted("Report server returned {} while {}.", *response_code, activity)),
                    is_transient_status(*response_code) ? ShouldRetry::Yes : ShouldRetry::No,
                    retry_after_ms(response_headers));
                return;
            }
            on_success(ByteString::copy(payload.bytes()));
        });
}

void CrashReportSubmission::finish()
{
    // Reporting success can release the last reference to this submission.
    NonnullRefPtr protector = *this;

    if (m_request_timeout) {
        m_request_timeout->stop();
        m_request_timeout = nullptr;
    }
    if (auto result = m_store.remove_sent_report(m_options.report_name); result.is_error())
        warnln("Could not remove the sent crash report: {}", result.error());
    m_self_while_in_flight = nullptr;
    if (on_sent)
        on_sent();
}

void CrashReportSubmission::fail(String reason, ShouldRetry should_retry, Optional<int> retry_after_ms)
{
    // Reporting the final failure can release the last reference to this submission.
    NonnullRefPtr protector = *this;

    if (m_request_timeout) {
        m_request_timeout->stop();
        m_request_timeout = nullptr;
    }

    if (should_retry == ShouldRetry::No || m_attempt >= maximum_attempts) {
        m_self_while_in_flight = nullptr;
        if (on_failed)
            on_failed(Failure::Sending, move(reason));
        return;
    }

    // A server that asks for more time gets it, but it cannot keep the user waiting for long.
    auto delay_ms = clamp(retry_after_ms.value_or(retry_delay_ms), retry_delay_ms, maximum_retry_delay_ms);
    auto retry_number = m_attempt++;
    if (on_retry)
        on_retry(move(reason), retry_number, static_cast<u32>(delay_ms / 1000));

    m_retry_timer = Core::Timer::create_single_shot(delay_ms,
        [weak_this = make_weak_ptr<CrashReportSubmission>()] {
            if (auto self = weak_this.strong_ref())
                self->begin_attempt();
        });
    m_retry_timer->start();
}

}
