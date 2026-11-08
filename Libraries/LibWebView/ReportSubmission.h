/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/JsonValue.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibCore/ImmutableBytes.h>
#include <LibHTTP/Forward.h>
#include <LibHTTP/Header.h>
#include <LibRequests/Forward.h>
#include <LibThreading/Forward.h>
#include <LibURL/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API ReportSubmission
    : public RefCounted<ReportSubmission>
    , public Weakable<ReportSubmission> {
public:
    AK_ALLOC_WITH_KMALLOC;

    enum class Stage : u8 {
        Preparing,
        RequestingChallenge,
        SolvingProof,
        Sending,
    };

    enum class Failure : u8 {
        Preparation,
        Sending,
    };

    static constexpr u32 maximum_attempts = 6;

    struct Field {
        StringView key;
        StringView type;
        JsonValue value;
    };

    struct Attachment {
        ByteString id;
        ByteString name;
        ByteString media_type;
        Core::ImmutableBytes content;
    };

    struct Contents {
        void add(StringView key, StringView type, JsonValue value);
        void add_text(StringView key, StringView type, StringView value);
        void add_build_information(StringView platform, StringView architecture, StringView git_commit, StringView build_configuration);

        StringView kind;
        ByteString client_version;
        ByteString build;
        Vector<Field> fields;
    };

    struct Payload {
        ByteString manifest;
        Vector<Attachment> attachments;
    };

    static ByteString build_manifest(Contents const&, ReadonlySpan<Attachment>);
    static NonnullRefPtr<ReportSubmission> create(Payload);

    struct PostResponse {
        bool had_network_error { false };
        Optional<u32> response_code;
        Vector<HTTP::Header> headers;
        ByteString body;
    };
    using PostFunction = Function<Function<void()>(URL::URL const&, Vector<HTTP::Header>, ByteString body, Function<void(PostResponse)>)>;
    // A process with no RequestServer of its own posts its reports through this.
    static void set_post_function(PostFunction);
    // Whether the URL is one that reports are posted to.
    static bool is_report_server_url(URL::URL const&);

    virtual ~ReportSubmission();

    void start();
    void cancel();

    // What another attempt needs to count as the same submission, once this one has been prepared.
    ByteString prepared_manifest() const { return m_payload.has_value() ? m_payload->manifest : ByteString {}; }

    Function<void(Stage)> on_progress;
    Function<void(ByteString const& manifest)> on_prepared;
    Function<void(String reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds)> on_retry;
    Function<void()> on_sent;
    Function<void(Failure, String reason)> on_failed;

    // However it ended, even if it was canceled, and even if nothing is left of whoever started it.
    Function<void()> on_finished;

protected:
    ReportSubmission();

    virtual ErrorOr<Payload, String> prepare();
    virtual void did_send() { }

private:
    void did_finish();

    struct ProofOfWork {
        ByteString token;
        u64 nonce { 0 };
    };

    enum class ShouldRetry {
        No,
        Yes,
    };

    void begin_attempt();
    void fail_preparation(String reason);
    void request_challenge();
    void solve_proof(ByteString token, u64 expected_work);
    void send_report(ProofOfWork);
    void finish();
    void fail(String reason, ShouldRetry, Optional<int> retry_after_ms = {});

    void post(StringView path, StringView activity, StringView content_type, ByteString body, Optional<ProofOfWork>,
        Function<void(ByteString body)> on_success);

    Optional<Payload> m_payload;
    u32 m_attempt { 1 };

    void arm_request_timeout();
    void cancel_post();
    void did_receive_response(StringView activity, bool had_network_error, Optional<u32> response_code, HTTP::HeaderList const& response_headers, ReadonlyBytes body, Function<void(ByteString)> const& on_success);

    RefPtr<Requests::Request> m_request;
    RefPtr<Core::Timer> m_request_timeout;
    // Identifies the post made through the post function, so that a response to one given up on goes unnoticed.
    u64 m_post_serial { 0 };
    Function<void()> m_cancel_post;
    RefPtr<Core::Timer> m_retry_timer;

    RefPtr<ReportSubmission> m_self_while_in_flight;

    RefPtr<Threading::Thread> m_proof_thread;
    Atomic<bool> m_proof_cancelled { false };
};

}
