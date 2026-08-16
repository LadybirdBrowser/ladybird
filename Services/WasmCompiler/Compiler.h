/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <AK/Time.h>
#include <AK/WeakPtr.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Timer.h>
#include <WasmCompiler/Forward.h>

namespace WasmCompiler {

class Compiler {
public:
    void compile(ConnectionFromClient&, u64 request_id, Core::AnonymousBuffer const&);

private:
    struct Request {
        WeakPtr<ConnectionFromClient> client;
        int client_id { 0 };
        u64 request_id { 0 };
    };

    struct Job {
        explicit Job(MonotonicTime enqueued_at)
            : enqueued_at(enqueued_at)
        {
        }

        Optional<ByteString> digest;
        Core::AnonymousBuffer input;
        Vector<Request> requests;
        MonotonicTime enqueued_at;
        RefPtr<Core::Timer> watchdog;
    };

    bool client_has_capacity(int client_id) const;
    void add_pending_request(int client_id);
    void remove_pending_request(int client_id);
    void start_pending_jobs();
    void expire_job(u64 job_id);
    void complete_job(u64 job_id, Core::AnonymousBuffer, AK::Duration);
    void finish_job(u64 job_id, Core::AnonymousBuffer, AK::Duration);

    HashMap<u64, NonnullOwnPtr<Job>> m_jobs;
    u64 m_next_job_id { 1 };
    size_t m_active_jobs { 0 };

    Queue<u64> m_pending_jobs;
    HashMap<int, size_t> m_pending_requests_per_client;
    size_t m_pending_input_bytes { 0 };
    size_t m_pending_requests { 0 };
};

}
