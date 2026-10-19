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
#include <LibRequests/Forward.h>
#include <LibThreading/Forward.h>
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

    virtual ~ReportSubmission();

    void start();
    void cancel();

    Function<void(Stage)> on_progress;
    Function<void(ByteString const& manifest)> on_prepared;
    Function<void(String reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds)> on_retry;
    Function<void()> on_sent;
    Function<void(Failure, String reason)> on_failed;

protected:
    ReportSubmission();

    virtual ErrorOr<Payload, String> prepare();
    virtual void did_send() { }

private:
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

    RefPtr<Requests::Request> m_request;
    RefPtr<Core::Timer> m_request_timeout;
    RefPtr<Core::Timer> m_retry_timer;

    RefPtr<ReportSubmission> m_self_while_in_flight;

    RefPtr<Threading::Thread> m_proof_thread;
    Atomic<bool> m_proof_cancelled { false };
};

}
