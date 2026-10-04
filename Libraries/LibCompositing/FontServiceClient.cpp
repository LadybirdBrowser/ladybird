/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/FontClientEndpoint.h>
#include <LibCompositing/FontServerEndpoint.h>
#include <LibCompositing/FontServiceClient.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibThreading/Thread.h>

namespace Compositing {

class FontServiceConnectionToServer final : public IPC::ConnectionToServer<FontClientEndpoint, FontServerEndpoint> {
    C_OBJECT(FontServiceConnectionToServer);

private:
    explicit FontServiceConnectionToServer(NonnullOwnPtr<IPC::Transport> transport)
        : IPC::ConnectionToServer<FontClientEndpoint, FontServerEndpoint>(*this, move(transport))
    {
    }

    // Not fatal: every later question fails to send and answers as if nothing matched.
    virtual void die() override { }
};

ErrorOr<NonnullOwnPtr<FontServiceClient>> FontServiceClient::create(IPC::TransportHandle handle)
{
    auto client = adopt_own(*new FontServiceClient(move(handle)));

    MutexLocker locker(client->m_mutex);
    client->m_condition.wait_while([&] { return !client->m_initialized; });
    if (!client->m_connected)
        return Error::from_string_literal("Unable to connect to the font service");
    return client;
}

FontServiceClient::FontServiceClient(IPC::TransportHandle handle)
    : m_transport_handle(move(handle))
    , m_thread(Threading::Thread::construct("Font service client"sv, [this] {
        return thread_main();
    }))
{
    m_thread->start();
}

FontServiceClient::~FontServiceClient()
{
    {
        MutexLocker locker(m_mutex);
        m_should_quit = true;
        m_condition.broadcast();
    }
    if (m_thread->needs_to_be_joined())
        (void)m_thread->join();
}

void FontServiceClient::ask(Question const& question)
{
    MutexLocker caller_locker(m_caller_mutex);
    MutexLocker locker(m_mutex);
    m_question = &question;
    m_condition.broadcast();
    m_condition.wait_while([&] { return m_question; });
}

Gfx::BrokeredFont FontServiceClient::open_font(u64 generation, u64 face_id)
{
    Gfx::BrokeredFont font;
    ask([&](FontServiceConnectionToServer& connection) {
        if (auto response = connection.send_sync_but_allow_failure<Messages::FontServer::OpenFont>(generation, face_id))
            font = response->take_font();
    });
    return font;
}

Gfx::BrokeredFont FontServiceClient::match_font(String const& family, u16 weight, u16 width, u8 slope)
{
    Gfx::BrokeredFont font;
    ask([&](FontServiceConnectionToServer& connection) {
        if (auto response = connection.send_sync_but_allow_failure<Messages::FontServer::MatchFont>(family, weight, width, slope))
            font = response->take_font();
    });
    return font;
}

Gfx::BrokeredFont FontServiceClient::match_local_font(String const& name)
{
    Gfx::BrokeredFont font;
    ask([&](FontServiceConnectionToServer& connection) {
        if (auto response = connection.send_sync_but_allow_failure<Messages::FontServer::MatchLocalFont>(name))
            font = response->take_font();
    });
    return font;
}

Gfx::BrokeredFont FontServiceClient::match_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    Gfx::BrokeredFont font;
    ask([&](FontServiceConnectionToServer& connection) {
        if (auto response = connection.send_sync_but_allow_failure<Messages::FontServer::MatchFontForCodePoint>(code_point, weight, width, slope, prefer_color_emoji))
            font = response->take_font();
    });
    return font;
}

Optional<FlyString> FontServiceClient::resolve_generic_family(String const& family, u16 weight, u8 slope)
{
    Optional<FlyString> resolved_family;
    ask([&](FontServiceConnectionToServer& connection) {
        if (auto response = connection.send_sync_but_allow_failure<Messages::FontServer::ResolveGenericFamily>(family, weight, slope)) {
            if (auto name = response->take_resolved_family(); name.has_value())
                resolved_family = FlyString { name.release_value() };
        }
    });
    return resolved_family;
}

intptr_t FontServiceClient::thread_main()
{
    // Never run: LibIPC needs a loop on this thread, because a connection registers a notifier when it is
    // constructed and defers a call after every drain.
    Core::EventLoop event_loop;

    RefPtr<FontServiceConnectionToServer> connection;
    {
        MutexLocker locker(m_mutex);
        if (auto transport = m_transport_handle->create_transport(); !transport.is_error())
            connection = FontServiceConnectionToServer::construct(transport.release_value());
        m_transport_handle.clear();
        m_connected = connection != nullptr;
        m_initialized = true;
        m_condition.broadcast();
    }
    if (!connection)
        return 1;

#ifdef AK_OS_WINDOWS
    if (auto response = connection->send_sync_but_allow_failure<Messages::FontServer::InitTransport>(Core::System::getpid()))
        connection->transport().set_peer_pid(response->peer_pid());
#endif

    for (;;) {
        Question const* question = nullptr;
        {
            MutexLocker locker(m_mutex);
            m_condition.wait_while([&] { return !m_question && !m_should_quit; });
            if (m_should_quit)
                break;
            question = m_question;
        }

        (*question)(*connection);

        // The drain that delivered the answer deferred a call to this thread's loop. Nothing else ever runs
        // it, so run it here rather than let one accumulate per answer.
        event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);

        MutexLocker locker(m_mutex);
        m_question = nullptr;
        m_condition.broadcast();
    }

    if (connection->is_open())
        connection->shutdown();
    return 0;
}

}
