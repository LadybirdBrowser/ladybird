/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ConfigFile.h>
#include <LibGfx/SystemTheme.h>

namespace Gfx {

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

}
