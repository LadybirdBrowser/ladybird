/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibRequests/Forward.h>
#include <LibThreading/Forward.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API CrashReportSubmission
    : public RefCounted<CrashReportSubmission>
    , public Weakable<CrashReportSubmission> {
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

    struct Options {
        ByteString report_name;
        ByteString description;
        ByteString url;
        ByteString reviewed_text_digest;

        // What an earlier submission of this report sent. Sending it again keeps its submission ID, so a server that
        // already received it answers with the same receipt instead of storing a duplicate.
        ByteString manifest;
    };

    static NonnullRefPtr<CrashReportSubmission> create(CrashReportStore const&, Options);
    ~CrashReportSubmission();

    void start();

    Function<void(Stage)> on_progress;
    Function<void(ByteString const& manifest)> on_prepared;
    Function<void(String reason, u32 retry_number, u32 delay_seconds)> on_retry;
    Function<void()> on_sent;
    Function<void(Failure, String reason)> on_failed;

private:
    struct ProofOfWork {
        ByteString token;
        u64 nonce { 0 };
    };

    enum class ShouldRetry {
        No,
        Yes,
    };

    CrashReportSubmission(CrashReportStore const&, Options);

    void begin_attempt();
    void fail_preparation(String reason);
    void request_challenge();
    void solve_proof(ByteString token, u64 expected_work);
    void send_report(ProofOfWork);
    void finish();
    void fail(String reason, ShouldRetry, Optional<int> retry_after_ms = {});

    void post(StringView path, StringView activity, StringView content_type, ByteString body, Optional<ProofOfWork>,
        Function<void(ByteString body)> on_success);

    CrashReportStore m_store;
    Options m_options;
    ByteString m_report_text;
    ByteString m_manifest;
    u32 m_attempt { 1 };

    RefPtr<Requests::Request> m_request;
    RefPtr<Core::Timer> m_request_timeout;
    RefPtr<Core::Timer> m_retry_timer;

    RefPtr<CrashReportSubmission> m_self_while_in_flight;

    RefPtr<Threading::Thread> m_proof_thread;
    Atomic<bool> m_proof_cancelled { false };
};

}
