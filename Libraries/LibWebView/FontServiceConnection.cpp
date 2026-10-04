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
#include <LibWebView/FontServiceConnection.h>

namespace WebView {

class FontServerConnection final
    : public IPC::ConnectionFromClient<FontClientEndpoint, FontServerEndpoint> {
public:
    FontServerConnection(NonnullOwnPtr<IPC::Transport> transport, FontService& font_service)
        : IPC::ConnectionFromClient<FontClientEndpoint, FontServerEndpoint>(*this, move(transport), 1)
        , m_font_service(font_service)
    {
    }

private:
    virtual void die() override
    {
        Core::EventLoop::current().quit(0);
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

    FontService& m_font_service;
};

ErrorOr<NonnullRefPtr<FontServiceConnection>> FontServiceConnection::create(FontService& font_service)
{
    auto connection = adopt_ref(*new FontServiceConnection(font_service));

    Optional<Error> initialization_error;
    {
        MutexLocker locker(connection->m_mutex);
        connection->m_initialization_condition.wait_while([&] { return !connection->m_initialized; });
        initialization_error = move(connection->m_initialization_error);
    }

    if (initialization_error.has_value())
        return initialization_error.release_value();
    return connection;
}

FontServiceConnection::FontServiceConnection(FontService& font_service)
    : m_font_service(font_service)
    , m_thread(Threading::Thread::construct("Font service IPC"sv, [this] {
        return thread_main();
    }))
{
    m_thread->start();
}

FontServiceConnection::~FontServiceConnection()
{
    RefPtr<Core::WeakEventLoopReference> event_loop;
    {
        MutexLocker locker(m_mutex);
        event_loop = m_event_loop;
    }

    if (event_loop) {
        if (auto strong_event_loop = event_loop->take()) {
            strong_event_loop->quit(0);
            strong_event_loop->wake();
        }
    }

    if (m_thread->needs_to_be_joined())
        (void)m_thread->join();
}

IPC::TransportHandle FontServiceConnection::take_transport_handle()
{
    MutexLocker locker(m_mutex);
    VERIFY(m_transport_handle.has_value());
    return m_transport_handle.release_value();
}

intptr_t FontServiceConnection::thread_main()
{
    Core::EventLoop event_loop;
    auto paired_or_error = IPC::Transport::create_paired();
    if (paired_or_error.is_error()) {
        MutexLocker locker(m_mutex);
        m_initialization_error = paired_or_error.release_error();
        m_initialized = true;
        m_initialization_condition.broadcast();
        return 1;
    }

    auto paired = paired_or_error.release_value();
    auto connection = adopt_ref(*new FontServerConnection(move(paired.local), *m_font_service));

    {
        MutexLocker locker(m_mutex);
        m_event_loop = Core::EventLoop::current_weak();
        m_transport_handle = move(paired.remote_handle);
        m_initialized = true;
        m_initialization_condition.broadcast();
    }

    auto result = event_loop.exec();
    if (connection->is_open())
        connection->shutdown();

    {
        MutexLocker locker(m_mutex);
        m_event_loop.clear();
    }
    return result;
}

}
