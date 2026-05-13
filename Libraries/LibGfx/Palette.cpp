/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Palette.h>

namespace Gfx {

NonnullRefPtr<PaletteImpl> PaletteImpl::create_with_anonymous_buffer(Core::AnonymousBuffer buffer)
{
    return adopt_ref(*new PaletteImpl(move(buffer)));
}

PaletteImpl::PaletteImpl(Core::AnonymousBuffer buffer)
    : m_theme_buffer(move(buffer))
{
}

Palette::Palette(NonnullRefPtr<PaletteImpl> impl)
    : m_impl(move(impl))
{
}

ByteString PaletteImpl::path(PathRole role) const
{
    VERIFY((int)role < (int)PathRole::__Count);
    return theme().path[(int)role];
}

NonnullRefPtr<PaletteImpl> PaletteImpl::clone() const
{
    auto new_theme_buffer = Core::AnonymousBuffer::create_with_size(m_theme_buffer.size()).release_value();
    memcpy(new_theme_buffer.data<SystemTheme>(), &theme(), m_theme_buffer.size());
    return adopt_ref(*new PaletteImpl(move(new_theme_buffer)));
}

void Palette::set_color(ColorRole role, Color color)
{
    if (m_impl->ref_count() != 1)
        m_impl = m_impl->clone();
    auto& theme = const_cast<SystemTheme&>(impl().theme());
    theme.color[(int)role] = color.value();
}

void Palette::set_flag(FlagRole role, bool value)
{
    if (m_impl->ref_count() != 1)
        m_impl = m_impl->clone();
    auto& theme = const_cast<SystemTheme&>(impl().theme());
    theme.flag[(int)role] = value;
}

}
