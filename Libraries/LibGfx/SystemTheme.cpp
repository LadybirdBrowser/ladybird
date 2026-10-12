/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibGfx/SystemTheme.h>

namespace Gfx {

static auto& theme_buffer()
{
    static NeverDestroyed<Core::AnonymousBuffer> buffer;
    return *buffer;
}

Core::AnonymousBuffer& current_system_theme_buffer()
{
    VERIFY(theme_buffer().is_valid());
    return theme_buffer();
}

void set_system_theme(Core::AnonymousBuffer buffer)
{
    theme_buffer() = move(buffer);
}

ErrorOr<Core::AnonymousBuffer> load_system_theme(Core::ConfigFile const& file)
{
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(sizeof(SystemTheme)));

    auto* data = buffer.data<SystemTheme>();

    auto get_color = [&](auto& name) {
        return Color::from_string(file.read_entry("Colors", name)).value_or(Color::Black);
    };

    auto get_flag = [&](auto& name) {
        return file.read_bool_entry("Flags", name, false);
    };

#undef __ENUMERATE_COLOR_ROLE
#define __ENUMERATE_COLOR_ROLE(role) \
    data->color[(int)ColorRole::role] = get_color(#role).value();
    ENUMERATE_COLOR_ROLES(__ENUMERATE_COLOR_ROLE)
#undef __ENUMERATE_COLOR_ROLE

#undef __ENUMERATE_FLAG_ROLE
#define __ENUMERATE_FLAG_ROLE(role) \
    data->flag[(int)FlagRole::role] = get_flag(#role);
    ENUMERATE_FLAG_ROLES(__ENUMERATE_FLAG_ROLE)
#undef __ENUMERATE_FLAG_ROLE

    return buffer;
}

ErrorOr<Core::AnonymousBuffer> load_system_theme(ByteString const& path)
{
    auto config_file = TRY(Core::ConfigFile::open(path));
    return TRY(load_system_theme(config_file));
}

ErrorOr<Vector<SystemThemeMetaData>> list_installed_system_themes()
{
    Vector<SystemThemeMetaData> system_themes;
    Core::DirIterator dt("/res/themes", Core::DirIterator::SkipDots);
    while (dt.has_next()) {
        auto theme_name = dt.next_path();
        auto theme_path = ByteString::formatted("/res/themes/{}", theme_name);
        auto config_file = TRY(Core::ConfigFile::open(theme_path));
        auto menu_name = config_file->read_entry("Menu", "Name", theme_name);
        TRY(system_themes.try_append({ LexicalPath::title(theme_name), menu_name, theme_path }));
    }
    quick_sort(system_themes, [](auto& a, auto& b) { return a.name < b.name; });
    return system_themes;
}

}
