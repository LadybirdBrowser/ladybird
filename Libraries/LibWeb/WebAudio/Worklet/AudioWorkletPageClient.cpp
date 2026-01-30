/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Worklet/AudioWorkletPageClient.h>

namespace Web::WebAudio::Render {

GC_DEFINE_ALLOCATOR(AudioWorkletPageClient);

GC::Ref<AudioWorkletPageClient> AudioWorkletPageClient::create(JS::VM& vm)
{
    auto client = vm.heap().allocate<AudioWorkletPageClient>();
    client->setup_palette();
    client->m_page = Web::Page::create(vm, client);
    return client;
}

void AudioWorkletPageClient::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

void AudioWorkletPageClient::setup_palette()
{
    auto buffer_or_error = Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme));
    VERIFY(!buffer_or_error.is_error());
    auto buffer = buffer_or_error.release_value();
    auto* theme = buffer.data<Gfx::SystemTheme>();
    theme->color[to_underlying(Gfx::ColorRole::Window)] = Color(Color::Black).value();
    theme->color[to_underlying(Gfx::ColorRole::WindowText)] = Color(Color::White).value();
    m_palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(buffer);
}

}
