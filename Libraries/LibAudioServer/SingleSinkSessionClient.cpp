/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudioServer/SingleSinkSessionClient.h>

namespace AudioServer {

// SingleSinkSessionClient is for single-consumer output playback clients.
// It aggregates one sink registration with one output-session lifecycle.
// It keeps async create/ready races internal by tracking pending and active ids.

ErrorOr<NonnullRefPtr<SingleSinkSessionClient>> SingleSinkSessionClient::try_create(RefPtr<SessionClientOfAudioServer> session_client)
{
    if (!session_client)
        session_client = SessionClientOfAudioServer::default_client();
    if (!session_client)
        return Error::from_string_literal("SingleSinkSessionClient: no AudioServer session client available");

    auto session = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) SingleSinkSessionClient(session_client.release_nonnull())));

    session->m_output_sink_id = session->m_session_client->add_output_sink(
        [weak_session = session->make_weak_ptr()](AudioServer::OutputSink const& output_sink) {
            RefPtr<SingleSinkSessionClient> strong_session = weak_session.strong_ref();
            if (!strong_session)
                return false;

            strong_session->verify_thread_affinity();
            u64 session_id = output_sink.session_id;

            if (strong_session->m_active_session_id.has_value())
                return false;

            if (!strong_session->m_pending_session_id.has_value())
                strong_session->m_pending_session_id = session_id;

            if (strong_session->m_pending_session_id.value() != session_id)
                return false;

            strong_session->m_create_request_in_flight = false;
            strong_session->m_pending_session_id.clear();
            strong_session->m_active_session_id = session_id;

            if (strong_session->m_on_output_sink_ready)
                strong_session->m_on_output_sink_ready(output_sink);
            return true;
        },
        [weak_session = session->make_weak_ptr()](u64 session_id, ByteString const& error) {
            RefPtr<SingleSinkSessionClient> strong_session = weak_session.strong_ref();
            if (!strong_session)
                return;

            strong_session->verify_thread_affinity();
            strong_session->m_create_request_in_flight = false;

            if (strong_session->m_pending_session_id.has_value() && strong_session->m_pending_session_id.value() == session_id)
                strong_session->m_pending_session_id.clear();
            if (strong_session->m_active_session_id.has_value() && strong_session->m_active_session_id.value() == session_id) {
                strong_session->m_active_session_id.clear();
                strong_session->m_device_handle.clear();
            }
            if (!strong_session->m_active_session_id.has_value() && !strong_session->m_pending_session_id.has_value())
                strong_session->m_device_handle.clear();

            if (!strong_session->m_on_output_sink_failed)
                return;
            strong_session->m_on_output_sink_failed(session_id, error);
        });

    return session;
}

SingleSinkSessionClient::SingleSinkSessionClient(NonnullRefPtr<SessionClientOfAudioServer> session_client)
    : m_session_client(move(session_client))
{
}

SingleSinkSessionClient::~SingleSinkSessionClient()
{
    if (m_creation_thread != std::this_thread::get_id())
        // FIXME: Make teardown event loop driven so we can assert here.
        return;

    Optional<u64> active_session_id = m_active_session_id;
    Optional<u64> pending_session_id = m_pending_session_id;
    if (active_session_id.has_value())
        (void)m_session_client->destroy_session(active_session_id.value());
    if (pending_session_id.has_value() && (!active_session_id.has_value() || pending_session_id.value() != active_session_id.value()))
        (void)m_session_client->destroy_session(pending_session_id.value());

    m_active_session_id.clear();
    m_pending_session_id.clear();
    m_device_handle.clear();
    m_create_request_in_flight = false;

    if (m_output_sink_id != 0)
        m_session_client->remove_output_sink(m_output_sink_id);
}

ErrorOr<void> SingleSinkSessionClient::request_output_sink(OutputSinkReadyHandler on_ready, OutputSinkFailedHandler on_failed, AudioServer::DeviceHandle device_handle, u32 target_latency_ms)
{
    verify_thread_affinity();

    m_on_output_sink_ready = move(on_ready);
    m_on_output_sink_failed = move(on_failed);

    if (m_create_request_in_flight || m_active_session_id.has_value() || m_pending_session_id.has_value()) {
        if (m_device_handle.has_value() && m_device_handle.value() != device_handle)
            return Error::from_string_literal("SingleSinkSessionClient: output session already active or pending for different device");
        return {};
    }

    m_device_handle = device_handle;

    m_create_request_in_flight = true;

    auto wrapped_on_error = [weak_session = make_weak_ptr()](ByteString const& error) {
        RefPtr<SingleSinkSessionClient> strong_session = weak_session.strong_ref();
        if (!strong_session)
            return;

        strong_session->verify_thread_affinity();
        strong_session->m_create_request_in_flight = false;
        strong_session->m_pending_session_id.clear();
        if (!strong_session->m_active_session_id.has_value())
            strong_session->m_device_handle.clear();

        if (strong_session->m_on_output_sink_failed)
            strong_session->m_on_output_sink_failed(0, error);
    };

    auto request_or_error = m_session_client->create_session(
        target_latency_ms,
        [weak_session = make_weak_ptr()](u64 created_session_id) {
            RefPtr<SingleSinkSessionClient> strong_session = weak_session.strong_ref();
            if (!strong_session)
                return;

            strong_session->verify_thread_affinity();
            strong_session->m_create_request_in_flight = false;

            bool destroy_created_session = false;
            Optional<u64> expected_session_id;
            if (strong_session->m_active_session_id.has_value())
                expected_session_id = strong_session->m_active_session_id.value();
            else if (strong_session->m_pending_session_id.has_value())
                expected_session_id = strong_session->m_pending_session_id.value();

            if (expected_session_id.has_value()) {
                // ready may have arrived before response; keep if ids match, drop otherwise.
                destroy_created_session = expected_session_id.value() != created_session_id;
            } else {
                strong_session->m_pending_session_id = created_session_id;
            }

            if (destroy_created_session)
                (void)strong_session->m_session_client->destroy_session(created_session_id);
        },
        move(wrapped_on_error),
        device_handle);

    if (request_or_error.is_error()) {
        m_create_request_in_flight = false;
        m_device_handle.clear();
        return request_or_error.release_error();
    }

    (void)request_or_error.release_value();
    return {};
}

ErrorOr<u64> SingleSinkSessionClient::destroy_output_sink(Function<void()> on_success, SessionClientOfAudioServer::RequestErrorHandler on_error)
{
    verify_thread_affinity();

    Optional<u64> session_id;
    if (m_active_session_id.has_value())
        session_id = m_active_session_id.value();
    else if (m_pending_session_id.has_value())
        session_id = m_pending_session_id.value();

    if (!session_id.has_value())
        return Error::from_string_literal("SingleSinkSessionClient: no output session to destroy");

    u64 target_session_id = session_id.value();

    return m_session_client->destroy_session(
        target_session_id,
        [weak_session = make_weak_ptr(), on_success = move(on_success), target_session_id]() mutable {
            RefPtr<SingleSinkSessionClient> strong_session = weak_session.strong_ref();
            if (!strong_session)
                return;

            strong_session->verify_thread_affinity();
            if (strong_session->m_active_session_id.has_value() && strong_session->m_active_session_id.value() == target_session_id)
                strong_session->m_active_session_id.clear();
            if (strong_session->m_pending_session_id.has_value() && strong_session->m_pending_session_id.value() == target_session_id)
                strong_session->m_pending_session_id.clear();
            if (!strong_session->m_active_session_id.has_value() && !strong_session->m_pending_session_id.has_value())
                strong_session->m_device_handle.clear();

            if (on_success)
                on_success();
        },
        move(on_error));
}

ErrorOr<void> SingleSinkSessionClient::release_output_sink_if_any()
{
    verify_thread_affinity();
    if (!m_active_session_id.has_value() && !m_pending_session_id.has_value())
        return {};

    TRY(destroy_output_sink());
    return {};
}

ErrorOr<u64> SingleSinkSessionClient::set_output_sink_volume(double volume, Function<void()> on_success, SessionClientOfAudioServer::RequestErrorHandler on_error)
{
    verify_thread_affinity();
    if (!m_active_session_id.has_value())
        return Error::from_string_literal("SingleSinkSessionClient: no active output session");

    return m_session_client->set_output_sink_volume(m_active_session_id.value(), volume, move(on_success), move(on_error));
}

Optional<u64> SingleSinkSessionClient::active_session_id() const
{
    verify_thread_affinity();
    return m_active_session_id;
}

}
