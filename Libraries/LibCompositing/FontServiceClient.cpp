/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/FontClientEndpoint.h>
#include <LibCompositing/FontServerEndpoint.h>
#include <LibCompositing/FontServiceClient.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibGfx/Font/FontDatabase.h>
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
        set_peer_owns_this_process(true);
    }

    // The UI process serves the fonts, so a process that loses it can answer no font question and exits, as it does
    // when its main connection to the UI process goes.
    virtual void die() override { Core::Process::terminate_immediately(0); }
};

ErrorOr<NonnullRefPtr<FontServiceClient>> FontServiceClient::create(IPC::TransportHandle handle)
{
    auto client = adopt_ref(*new FontServiceClient(move(handle)));

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

    // A client that is done closes its side, which is not losing the service: shutdown() would end the process.
    if (connection->is_open())
        connection->transport().close();
    return 0;
}

ErrorOr<NonnullOwnPtr<Gfx::SharedFontProvider>> create_font_provider(IPC::TransportHandle handle, IPC::File catalog, u64 catalog_size, u64 generation)
{
    auto client = TRY(FontServiceClient::create(move(handle)));

    Gfx::SharedFontProviderCallbacks callbacks;
    callbacks.open_font = [client](u64 generation, u64 face_id) {
        return client->open_font(generation, face_id);
    };
    callbacks.match_local_font = [client](String const& name) {
        return client->match_local_font(name);
    };
    callbacks.match_font = [client](String const& family, u16 weight, u16 width, u8 slope) {
        return client->match_font(family, weight, width, slope);
    };
    callbacks.match_font_for_code_point = [client](u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji) {
        return client->match_font_for_code_point(code_point, weight, width, slope, prefer_color_emoji);
    };
    callbacks.resolve_generic_family = [client](String const& family, u16 weight, u8 slope) {
        return client->resolve_generic_family(family, weight, slope);
    };
    return Gfx::SharedFontProvider::create_from_catalog_file_or_empty(move(catalog), catalog_size, generation, move(callbacks));
}

ErrorOr<Gfx::SystemFontProvider*> install_font_service(IPC::TransportHandle handle, IPC::File catalog, u64 catalog_size, u64 generation)
{
    auto provider = TRY(create_font_provider(move(handle), move(catalog), catalog_size, generation));
    return &Gfx::FontDatabase::the().install_system_font_provider(move(provider));
}

}
