/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/FontClientEndpoint.h>
#include <LibCompositing/FontServerEndpoint.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibGfx/Font/Font.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Transport.h>
#include <LibThreading/Thread.h>
#include <LibWebView/FontService.h>
#include <LibWebView/FontServiceHost.h>

namespace WebView {

class FontServerConnection final
    : public IPC::ConnectionFromClient<FontClientEndpoint, FontServerEndpoint> {
public:
    FontServerConnection(FontServiceHost& host, NonnullOwnPtr<IPC::Transport> transport)
        : IPC::ConnectionFromClient<FontClientEndpoint, FontServerEndpoint>(*this, move(transport), 1)
        , m_host(host)
        , m_font_service(host.m_font_service)
    {
    }

private:
    virtual void die() override
    {
        m_host.m_connections.remove_first_matching([this](auto const& connection) { return connection.ptr() == this; });
    }

    virtual Messages::FontServer::InitTransportResponse init_transport([[maybe_unused]] int peer_pid) override
    {
#ifdef AK_OS_WINDOWS
        m_transport->set_peer_pid(peer_pid);
        return Core::System::getpid();
#endif
        VERIFY_NOT_REACHED();
    }

    virtual Messages::FontServer::OpenFontResponse open_font(u64 generation, u64 face_id) override
    {
        auto font = m_font_service.open_font(generation, face_id);
        return { move(font) };
    }

    virtual Messages::FontServer::MatchFontResponse match_font(String family, u16 weight, u16 width, u8 slope) override
    {
        auto font = m_font_service.match_font(family, weight, width, slope);
        return { move(font) };
    }

    virtual Messages::FontServer::MatchLocalFontResponse match_local_font(String name) override
    {
        auto font = m_font_service.match_local_font(name);
        return { move(font) };
    }

    virtual Messages::FontServer::MatchFontForCodePointResponse match_font_for_code_point(u32 code_point, u16 weight, u16 width, u8 slope, bool prefer_color_emoji) override
    {
        auto font = m_font_service.match_font_for_code_point(code_point, weight, width, slope, prefer_color_emoji);
        return { move(font) };
    }

    virtual Messages::FontServer::ResolveGenericFamilyResponse resolve_generic_family(String family, u16 weight, u8 slope) override
    {
        auto resolved = m_font_service.resolve_generic_family(family, weight, slope);
        if (!resolved.has_value())
            return Optional<String> {};
        return Optional<String> { resolved->to_string() };
    }

    FontServiceHost& m_host;
    FontService& m_font_service;
};

NonnullOwnPtr<FontServiceHost> FontServiceHost::create(FontService& font_service)
{
    auto host = adopt_own(*new FontServiceHost(font_service));

    MutexLocker locker(host->m_mutex);
    host->m_condition.wait_while([&] { return !host->m_event_loop; });
    return host;
}

FontServiceHost::FontServiceHost(FontService& font_service)
    : m_font_service(font_service)
    , m_thread(Threading::Thread::construct("Font service host"sv, [this] {
        return thread_main();
    }))
{
    m_thread->start();
}

FontServiceHost::~FontServiceHost()
{
    if (auto event_loop = m_event_loop->take()) {
        event_loop->quit(0);
        event_loop->wake();
    }
    (void)m_thread->join();
}

ErrorOr<IPC::TransportHandle> FontServiceHost::connect()
{
    // NB: A connection belongs to the thread that constructs it, so the host thread opens it.
    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<ErrorOr<IPC::TransportHandle>> result;
    MutexLocker locker(m_mutex);
    {
        auto event_loop = m_event_loop->take();
        VERIFY(event_loop);
        event_loop->deferred_invoke([&] {
            auto handle = open_connection();
            MutexLocker locker(m_mutex);
            result = move(handle);
            m_condition.broadcast();
        });
    }
    m_condition.wait_while([&] { return !result.has_value(); });
    return result.release_value();
}

ErrorOr<IPC::TransportHandle> FontServiceHost::open_connection()
{
    auto paired = TRY(IPC::Transport::create_paired());
    m_connections.append(adopt_ref(*new FontServerConnection(*this, move(paired.local))));
    return move(paired.remote_handle);
}

intptr_t FontServiceHost::thread_main()
{
    Core::EventLoop event_loop;
    {
        MutexLocker locker(m_mutex);
        m_event_loop = Core::EventLoop::current_weak();
        m_condition.broadcast();
    }

    auto result = event_loop.exec();
    for (auto& connection : exchange(m_connections, {})) {
        if (connection->is_open())
            connection->shutdown();
    }
    return result;
}

}
