/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibThreading/ThreadPool.h>
#include <LibWasm/Types.h>
#include <WasmCompiler/Compiler.h>
#include <WasmCompiler/ConnectionFromClient.h>

namespace WasmCompiler {

static constexpr size_t MAXIMUM_INPUT_BYTES = 512 * MiB;
static constexpr size_t MAXIMUM_PENDING_INPUT_BYTES = 1 * GiB;
static constexpr size_t MAXIMUM_PENDING_JOBS = 32;
static constexpr size_t MAXIMUM_PENDING_REQUESTS = 256;
static constexpr size_t MAXIMUM_PENDING_REQUESTS_PER_CLIENT = 8;
static constexpr size_t MAXIMUM_CONCURRENT_COMPILATIONS = 2;
static constexpr auto MAXIMUM_JOB_DURATION = AK::Duration::from_seconds(60);

void Compiler::compile(ConnectionFromClient& client, u64 request_id, Core::AnonymousBuffer const& input)
{
    auto client_id = client.client_id();

    if (m_pending_requests >= MAXIMUM_PENDING_REQUESTS || !client_has_capacity(client_id)) {
        warnln("WasmCompiler: Pending request limit reached");
        client.async_did_compile(request_id, {});
        return;
    }

    if (!input.is_valid() || input.size() > MAXIMUM_INPUT_BYTES) {
        warnln("WasmCompiler: Rejecting {}-byte input buffer", input.size());
        client.async_did_compile(request_id, {});
        return;
    }

    auto private_input = input.snapshot(Core::AnonymousBuffer::Sealability::Sealable);
    if (private_input.is_error()) {
        warnln("WasmCompiler: Failed to snapshot input buffer: {}", private_input.error());
        client.async_did_compile(request_id, {});
        return;
    }

    Optional<ByteString> digest;

    for (auto& job : m_jobs) {
        if (job.value->input.size() != private_input.value().size())
            continue;

        if (!digest.has_value())
            digest = ByteString { Crypto::Hash::SHA256::hash(private_input.value().bytes()).bytes() };
        if (!job.value->digest.has_value())
            job.value->digest = ByteString { Crypto::Hash::SHA256::hash(job.value->input.bytes()).bytes() };

        if (*job.value->digest == *digest && job.value->input.bytes() == private_input.value().bytes()) {
            dbgln_if(WASM_CRANELIFT_DEBUG, "WasmCompiler: Coalesced an in-flight compilation request");
            job.value->requests.empend(client.make_weak_ptr<ConnectionFromClient>(), client_id, request_id);
            add_pending_request(client_id);
            return;
        }
    }

    if (m_jobs.size() >= MAXIMUM_PENDING_JOBS
        || m_next_job_id == NumericLimits<u64>::max()
        || private_input.value().size() > MAXIMUM_PENDING_INPUT_BYTES - m_pending_input_bytes) {
        warnln("WasmCompiler: Pending compilation limit reached");
        client.async_did_compile(request_id, {});
        return;
    }

    auto job = make<Job>(MonotonicTime::now());
    job->digest = move(digest);
    job->input = private_input.release_value();
    job->requests.empend(client.make_weak_ptr<ConnectionFromClient>(), client_id, request_id);

    auto job_id = m_next_job_id++;
    m_pending_jobs.enqueue(job_id);
    m_pending_input_bytes += job->input.size();
    m_jobs.set(job_id, move(job));

    add_pending_request(client_id);
    start_pending_jobs();
}

bool Compiler::client_has_capacity(int client_id) const
{
    return m_pending_requests_per_client.get(client_id).value_or(0) < MAXIMUM_PENDING_REQUESTS_PER_CLIENT;
}

void Compiler::add_pending_request(int client_id)
{
    VERIFY(m_pending_requests < MAXIMUM_PENDING_REQUESTS);
    ++m_pending_requests;

    m_pending_requests_per_client.set(client_id, m_pending_requests_per_client.get(client_id).value_or(0) + 1);
}

void Compiler::remove_pending_request(int client_id)
{
    VERIFY(m_pending_requests > 0);
    --m_pending_requests;

    auto count = m_pending_requests_per_client.get(client_id);
    VERIFY(count.has_value() && *count > 0);

    if (*count == 1)
        m_pending_requests_per_client.remove(client_id);
    else
        m_pending_requests_per_client.set(client_id, *count - 1);
}

void Compiler::start_pending_jobs()
{
    while (m_active_jobs < MAXIMUM_CONCURRENT_COMPILATIONS && !m_pending_jobs.is_empty()) {
        auto job_id = m_pending_jobs.dequeue();
        auto job = m_jobs.get(job_id);
        VERIFY(job.has_value());

        auto time_in_queue = MonotonicTime::now() - job.value()->enqueued_at;
        if (time_in_queue >= MAXIMUM_JOB_DURATION) {
            expire_job(job_id);
            continue;
        }

        dbgln_if(WASM_CRANELIFT_DEBUG, "WasmCompiler: Compiling a {}-byte batch", job.value()->input.size());
        ++m_active_jobs;

        auto watchdog_interval = static_cast<int>(MAXIMUM_JOB_DURATION.to_milliseconds());
        job.value()->watchdog = Core::Timer::create_single_shot(watchdog_interval, [this, job_id] {
            Core::EventLoop::current().deferred_invoke([this, job_id] {
                if (!m_jobs.contains(job_id))
                    return;
                warnln("WasmCompiler: Compilation request exceeded {} seconds", MAXIMUM_JOB_DURATION.to_seconds());
                Core::Process::terminate_immediately(1);
            });
        });
        job.value()->watchdog->start();

        Threading::ThreadPool::the().submit([this, main_thread_event_loop = Core::EventLoop::current_weak(), job_id, input = job.value()->input]() mutable {
            Core::AnonymousBuffer output;
            auto start = MonotonicTime::now();

            if (auto result = ::Wasm::compile_cranelift_buffer(input); result.is_error())
                warnln("WasmCompiler: Compilation failed: {}", result.error());
            else
                output = result.release_value();

            auto compilation_duration = MonotonicTime::now() - start;

            if (auto event_loop = main_thread_event_loop->take()) {
                event_loop->deferred_invoke([this, job_id, output = move(output), compilation_duration]() mutable {
                    complete_job(job_id, move(output), compilation_duration);
                });
            }
        });
    }
}

void Compiler::expire_job(u64 job_id)
{
    warnln("WasmCompiler: Compilation request expired in the queue");
    finish_job(job_id, {}, MonotonicTime::now() - m_jobs.get(job_id).value()->enqueued_at);
}

void Compiler::complete_job(u64 job_id, Core::AnonymousBuffer output, AK::Duration compilation_duration)
{
    VERIFY(m_active_jobs > 0);
    --m_active_jobs;

    finish_job(job_id, move(output), compilation_duration);
    start_pending_jobs();
}

void Compiler::finish_job(u64 job_id, Core::AnonymousBuffer output, AK::Duration compilation_duration)
{
    auto job = m_jobs.take(job_id);
    VERIFY(job.has_value());

    if (job.value()->watchdog)
        job.value()->watchdog->stop();

    VERIFY(m_pending_input_bytes >= job.value()->input.size());
    m_pending_input_bytes -= job.value()->input.size();

    dbgln_if(WASM_CRANELIFT_DEBUG, "WasmCompiler: Completed request in {} ms ({})",
        compilation_duration.to_milliseconds(), output.is_valid() ? "success"sv : "failure"sv);
    auto single_recipient = job.value()->requests.size() == 1;

    for (auto const& request : job.value()->requests) {
        remove_pending_request(request.client_id);

        if (auto client = request.client.strong_ref()) {
            Core::AnonymousBuffer shared_output;

            if (output.is_valid()) {
                if (single_recipient)
                    shared_output = move(output);
                else if (auto result = output.snapshot(Core::AnonymousBuffer::Sealability::Sealable); !result.is_error())
                    shared_output = result.release_value();
            }

            client->async_did_compile(request.request_id, shared_output);
        }
    }
}

}
