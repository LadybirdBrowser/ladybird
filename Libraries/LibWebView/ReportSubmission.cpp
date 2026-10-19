/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
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
#include <LibWebView/ReportSubmission.h>

namespace WebView {

static constexpr auto default_endpoint = "https://reports.app.ladybird.org"sv;
static constexpr auto challenge_path = "/api/v1/challenges"sv;
static constexpr auto report_path = "/api/v1/reports"sv;
static constexpr auto challenge_algorithm = "sha256-seed-nonce-le-v1"sv;
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

void ReportSubmission::Contents::add(StringView key, StringView type, JsonValue value)
{
    fields.append({ key, type, move(value) });
}

// Empty values are left out, as the server has nothing to show for them.
void ReportSubmission::Contents::add_text(StringView key, StringView type, StringView value)
{
    if (!value.is_empty())
        add(key, type, to_string(value));
}

ByteString ReportSubmission::build_manifest(Contents const& contents, ReadonlySpan<Attachment> attachments)
{
    JsonArray fields;
    for (auto const& field : contents.fields) {
        JsonObject typed;
        typed.set("type"sv, to_string(field.type));
        typed.set("value"sv, field.value);
        JsonObject entry;
        entry.set("key"sv, to_string(field.key));
        entry.set("value"sv, move(typed));
        fields.must_append(move(entry));
    }

    JsonArray described_attachments;
    for (auto const& attachment : attachments) {
        JsonObject described;
        described.set("id"sv, to_string(attachment.id));
        described.set("name"sv, to_string(attachment.name));
        described.set("media_type"sv, to_string(attachment.media_type));
        described.set("size"sv, attachment.content.size());
        described.set("sha256"sv, to_string(sha256_hex(attachment.content.bytes())));
        described_attachments.must_append(move(described));
    }

    JsonObject manifest;
    manifest.set("protocol"sv, 1);
    manifest.set("submission_id"sv, generate_random_uuid());
    manifest.set("kind"sv, to_string(contents.kind));
    manifest.set("client_version"sv, to_string(contents.client_version));
    manifest.set("build"sv, to_string(contents.build));
    manifest.set("fields"sv, move(fields));
    manifest.set("attachments"sv, move(described_attachments));
    return manifest.serialized().to_byte_string();
}

static ByteString build_multipart_body(StringView manifest, ReadonlySpan<ReportSubmission::Attachment> attachments,
    StringView boundary)
{
    StringBuilder builder;
    auto append_part = [&](StringView disposition, StringView content_type, ReadonlyBytes content) {
        builder.appendff("--{}\r\nContent-Disposition: form-data; {}\r\nContent-Type: {}\r\n\r\n",
            boundary, disposition, content_type);
        builder.append(StringView { content });
        builder.append("\r\n"sv);
    };

    append_part("name=\"manifest\""sv, "application/json"sv, manifest.bytes());
    for (auto const& attachment : attachments) {
        auto content_type = attachment.media_type == "text/plain"sv ? "text/plain; charset=utf-8"sv : attachment.media_type.view();
        append_part(ByteString::formatted("name=\"{}\"; filename=\"{}\"", attachment.id, attachment.name),
            content_type, attachment.content.bytes());
    }

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

// A submission prepares its payload, solves the proof-of-work challenge off the main thread, and uploads the report.
// It can outlive whatever started it.
ReportSubmission::ReportSubmission() = default;

// A submission of a payload prepared in advance sends that payload, so a retry of it keeps its submission ID.
NonnullRefPtr<ReportSubmission> ReportSubmission::create(Payload payload)
{
    auto submission = adopt_ref(*new ReportSubmission);
    submission->m_payload = move(payload);
    return submission;
}

ErrorOr<ReportSubmission::Payload, String> ReportSubmission::prepare()
{
    VERIFY_NOT_REACHED();
}

ReportSubmission::~ReportSubmission()
{
    m_proof_cancelled.store(true);
    if (m_proof_thread && m_proof_thread->needs_to_be_joined())
        (void)m_proof_thread->join();
}

// A submission keeps itself alive until it is sent or fails for good, so it outlives the review that started it.
void ReportSubmission::start()
{
    m_self_while_in_flight = *this;
    begin_attempt();
}

// A canceled submission stops wherever it is and reports nothing more. A report that reached the server before then may
// still be stored there.
void ReportSubmission::cancel()
{
    // Letting go of the submission can release the last reference to it.
    NonnullRefPtr protector = *this;

    on_progress = nullptr;
    on_prepared = nullptr;
    on_retry = nullptr;
    on_sent = nullptr;
    on_failed = nullptr;

    m_proof_cancelled.store(true);
    if (m_retry_timer) {
        m_retry_timer->stop();
        m_retry_timer = nullptr;
    }
    if (m_request_timeout) {
        m_request_timeout->stop();
        m_request_timeout = nullptr;
    }
    if (auto request = move(m_request))
        request->stop();
    m_self_while_in_flight = nullptr;
}

void ReportSubmission::begin_attempt()
{
    if (on_progress)
        on_progress(Stage::Preparing);

    if (!m_payload.has_value()) {
        auto payload = prepare();
        if (payload.is_error()) {
            fail_preparation(payload.release_error());
            return;
        }
        m_payload = payload.release_value();
    }

    if (on_prepared)
        on_prepared(m_payload->manifest);
    request_challenge();
}

void ReportSubmission::fail_preparation(String reason)
{
    // Reporting the failure can release the last reference to this submission.
    NonnullRefPtr protector = *this;

    m_self_while_in_flight = nullptr;
    if (on_failed)
        on_failed(Failure::Preparation, move(reason));
}

void ReportSubmission::request_challenge()
{
    if (on_progress)
        on_progress(Stage::RequestingChallenge);

    JsonObject body;
    body.set("manifest_digest"sv, to_string(sha256_hex(m_payload->manifest.bytes())));

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

void ReportSubmission::solve_proof(ByteString token, u64 expected_work)
{
    if (on_progress)
        on_progress(Stage::SolvingProof);

    // A previous attempt's search has already delivered its result.
    if (m_proof_thread && m_proof_thread->needs_to_be_joined())
        (void)m_proof_thread->join();

    // The server chooses the difficulty, so the search runs on its own thread, at the priority of work that the user
    // follows through a progress indicator. A submission on its way keeps itself alive, so quitting the browser does not
    // wait for the search, whose result only goes to the main thread's event loop while that loop is still there.
    m_proof_thread = Threading::Thread::construct("Report proof"sv,
        [weak_this = make_weak_ptr<ReportSubmission>(), &cancelled = m_proof_cancelled,
            main_thread_event_loop = Core::EventLoop::current_weak(), attempt = m_attempt, token = move(token),
            expected_work]() mutable -> intptr_t {
            auto nonce = solve_challenge(token, expected_work, cancelled);
            auto event_loop = main_thread_event_loop->take();
            if (!event_loop)
                return 0;
            event_loop->deferred_invoke(
                [weak_this = move(weak_this), attempt, token = move(token), nonce]() mutable {
                    auto self = weak_this.strong_ref();
                    if (!self || self->m_attempt != attempt || self->m_proof_cancelled.load())
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

void ReportSubmission::send_report(ProofOfWork proof)
{
    if (on_progress)
        on_progress(Stage::Sending);

    auto boundary = ByteString::formatted("ladybird-{}", generate_random_uuid());
    auto body = build_multipart_body(m_payload->manifest, m_payload->attachments, boundary);
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
void ReportSubmission::post(StringView path, StringView activity, StringView content_type, ByteString body,
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

    auto request_client = Application::request_server_client(IsPrivate::No);
    if (request_client.is_error()) {
        fail("Could not start the report request."_string, ShouldRetry::Yes);
        return;
    }
    m_request = request_client.value()->start_request("POST"sv, *url, *headers, body.bytes(),
        HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
    if (!m_request) {
        fail("Could not connect to the report server."_string, ShouldRetry::Yes);
        return;
    }

    m_request_timeout = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(request_timeout_ms),
        [weak_this = make_weak_ptr<ReportSubmission>()] {
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
        [weak_this = make_weak_ptr<ReportSubmission>(), activity = ByteString { activity },
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

void ReportSubmission::finish()
{
    // Reporting success can release the last reference to this submission.
    NonnullRefPtr protector = *this;

    if (m_request_timeout) {
        m_request_timeout->stop();
        m_request_timeout = nullptr;
    }
    did_send();
    m_self_while_in_flight = nullptr;
    if (on_sent)
        on_sent();
}

void ReportSubmission::fail(String reason, ShouldRetry should_retry, Optional<int> retry_after_ms)
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
        on_retry(move(reason), retry_number, maximum_attempts - 1, static_cast<u32>(delay_ms / 1000));

    m_retry_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(delay_ms),
        [weak_this = make_weak_ptr<ReportSubmission>()] {
            if (auto self = weak_this.strong_ref())
                self->begin_attempt();
        });
    m_retry_timer->start();
}

}
