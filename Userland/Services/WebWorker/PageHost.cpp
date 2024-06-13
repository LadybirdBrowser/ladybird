/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <WebWorker/ConnectionFromClient.h>
#include <WebWorker/PageHost.h>

namespace WebWorker {

JS_DEFINE_ALLOCATOR(PageHost);

JS::NonnullGCPtr<PageHost> PageHost::create(JS::VM& vm, ConnectionFromClient& client)
{
    return vm.heap().allocate_without_realm<PageHost>(client);
}

PageHost::~PageHost() = default;

Web::Page& PageHost::page()
{
    return *m_page;
}

Web::Page const& PageHost::page() const
{
    return *m_page;
}

Gfx::Palette PageHost::palette() const
{
    return Gfx::Palette(*m_palette_impl);
}

void PageHost::setup_palette()
{
    // FIXME: We don't actually need a palette :thonk:
    auto buffer_or_error = Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme));
    VERIFY(!buffer_or_error.is_error());
    auto buffer = buffer_or_error.release_value();
    auto* theme = buffer.data<Gfx::SystemTheme>();
    theme->color[to_underlying(Gfx::ColorRole::Window)] = Color(Color::Magenta).value();
    theme->color[to_underlying(Gfx::ColorRole::WindowText)] = Color(Color::Cyan).value();
    m_palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(buffer);
}

bool PageHost::is_connection_open() const
{
    return m_client.is_open();
}

Web::DevicePixelRect PageHost::screen_rect() const
{
    return {};
}

double PageHost::device_pixels_per_css_pixel() const
{
    return 1.0;
}

Web::CSS::PreferredColorScheme PageHost::preferred_color_scheme() const
{
    return Web::CSS::PreferredColorScheme::Auto;
}

Web::CSS::PreferredContrast PageHost::preferred_contrast() const
{
    return Web::CSS::PreferredContrast::Auto;
}

Web::CSS::PreferredMotion PageHost::preferred_motion() const
{
    return Web::CSS::PreferredMotion::Auto;
}

void PageHost::paint(Web::DevicePixelRect const&, Gfx::Bitmap&, Web::PaintOptions)
{
}

void PageHost::request_file(Web::FileRequest request)
{
    m_client.request_file(move(request));
}

PageHost::PageHost(ConnectionFromClient& client)
    : m_client(client)
    , m_page(Web::Page::create(Web::Bindings::main_thread_vm(), *this))
{
    setup_palette();
}

void PageHost::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

}
