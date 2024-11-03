/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibGfx/SystemTheme.h>
#include <string.h>

namespace Gfx {

static SystemTheme dummy_theme;
static SystemTheme const* theme_page = &dummy_theme;
static Core::AnonymousBuffer theme_buffer;

Core::AnonymousBuffer& current_system_theme_buffer()
{
    VERIFY(theme_buffer.is_valid());
    return theme_buffer;
}

void set_system_theme(Core::AnonymousBuffer buffer)
{
    theme_buffer = move(buffer);
    theme_page = theme_buffer.data<SystemTheme>();
}

ErrorOr<Core::AnonymousBuffer> load_system_theme(Core::ConfigFile const& file, Optional<ByteString> const& color_scheme)
{
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(sizeof(SystemTheme)));

    auto* data = buffer.data<SystemTheme>();

    if (color_scheme.has_value()) {
        if (color_scheme.value().length() > 255)
            return Error::from_string_literal("Passed an excessively long color scheme pathname");
        if (color_scheme.value() != "Custom"sv)
            memcpy(data->path[(int)PathRole::ColorScheme], color_scheme.value().characters(), color_scheme.value().length());
        else
            memcpy(buffer.data<SystemTheme>(), theme_buffer.data<SystemTheme>(), sizeof(SystemTheme));
    }

    auto get_color = [&](auto& name) -> Optional<Color> {
        auto color_string = file.read_entry("Colors", name);
        auto color = Color::from_string(color_string);
        if (color_scheme.has_value() && color_scheme.value() == "Custom"sv)
            return color;
        if (!color.has_value()) {
            auto maybe_color_config = Core::ConfigFile::open(data->path[(int)PathRole::ColorScheme]);
            if (maybe_color_config.is_error())
                maybe_color_config = Core::ConfigFile::open("/res/color-schemes/Default.ini");
            auto color_config = maybe_color_config.release_value();
            if (name == "ColorSchemeBackground"sv)
                color = Gfx::Color::from_string(color_config->read_entry("Primary", "Background"));
            else if (name == "ColorSchemeForeground"sv)
                color = Gfx::Color::from_string(color_config->read_entry("Primary", "Foreground"));
            else if (strncmp(name, "Bright", 6) == 0)
                color = Gfx::Color::from_string(color_config->read_entry("Bright", name + 6));
            else
                color = Gfx::Color::from_string(color_config->read_entry("Normal", name));

            if (!color.has_value())
                return Color(Color::Black);
        }
        return color.value();
    };

    auto get_flag = [&](auto& name) {
        return file.read_bool_entry("Flags", name, false);
    };

    auto get_alignment = [&](auto& name, auto role) {
        auto alignment = file.read_entry("Alignments", name).to_lowercase();
        if (alignment.is_empty()) {
            switch (role) {
            case (int)AlignmentRole::TitleAlignment:
                return Gfx::TextAlignment::CenterLeft;
            default:
                dbgln("Alignment {} has no fallback value!", name);
                return Gfx::TextAlignment::CenterLeft;
            }
        }

        if (alignment == "left" || alignment == "centerleft")
            return Gfx::TextAlignment::CenterLeft;
        else if (alignment == "right" || alignment == "centerright")
            return Gfx::TextAlignment::CenterRight;
        else if (alignment == "center")
            return Gfx::TextAlignment::Center;

        dbgln("Alignment {} has an invalid value!", name);
        return Gfx::TextAlignment::CenterLeft;
    };

    auto get_metric = [&](auto& name, auto role) {
        int metric = file.read_num_entry("Metrics", name, -1);
        if (metric == -1) {
            switch (role) {
            case (int)MetricRole::BorderThickness:
                return 4;
            case (int)MetricRole::BorderRadius:
                return 0;
            case (int)MetricRole::TitleHeight:
                return 19;
            case (int)MetricRole::TitleButtonHeight:
                return 15;
            case (int)MetricRole::TitleButtonWidth:
                return 15;
            default:
                dbgln("Metric {} has no fallback value!", name);
                return 16;
            }
        }
        return metric;
    };

    auto get_path = [&](auto& name, auto role, bool allow_empty) {
        auto path = file.read_entry("Paths", name);
        if (path.is_empty()) {
            switch (role) {
            case (int)PathRole::TitleButtonIcons:
                return "/res/icons/16x16/";
            default:
                return allow_empty ? "" : "/res/";
            }
        }
        return &path[0];
    };

#define ENCODE_PATH(x, allow_empty)                                                                              \
    do {                                                                                                         \
        auto path = get_path(#x, (int)PathRole::x, allow_empty);                                                 \
        memcpy(data->path[(int)PathRole::x], path, min(strlen(path) + 1, sizeof(data->path[(int)PathRole::x]))); \
        data->path[(int)PathRole::x][sizeof(data->path[(int)PathRole::x]) - 1] = '\0';                           \
    } while (0)

    ENCODE_PATH(TitleButtonIcons, false);
    ENCODE_PATH(ActiveWindowShadow, true);
    ENCODE_PATH(InactiveWindowShadow, true);
    ENCODE_PATH(TaskbarShadow, true);
    ENCODE_PATH(MenuShadow, true);
    ENCODE_PATH(TooltipShadow, true);
    if (!color_scheme.has_value())
        ENCODE_PATH(ColorScheme, true);

#undef __ENUMERATE_COLOR_ROLE
#define __ENUMERATE_COLOR_ROLE(role)                                    \
    {                                                                   \
        Optional<Color> result = get_color(#role);                      \
        if (result.has_value())                                         \
            data->color[(int)ColorRole::role] = result.value().value(); \
    }
    ENUMERATE_COLOR_ROLES(__ENUMERATE_COLOR_ROLE)
#undef __ENUMERATE_COLOR_ROLE

#undef __ENUMERATE_ALIGNMENT_ROLE
#define __ENUMERATE_ALIGNMENT_ROLE(role) \
    data->alignment[(int)AlignmentRole::role] = get_alignment(#role, (int)AlignmentRole::role);
    ENUMERATE_ALIGNMENT_ROLES(__ENUMERATE_ALIGNMENT_ROLE)
#undef __ENUMERATE_ALIGNMENT_ROLE

#undef __ENUMERATE_FLAG_ROLE
#define __ENUMERATE_FLAG_ROLE(role)                            \
    {                                                          \
        if (#role != "BoldTextAsBright"sv)                     \
            data->flag[(int)FlagRole::role] = get_flag(#role); \
    }
    ENUMERATE_FLAG_ROLES(__ENUMERATE_FLAG_ROLE)
#undef __ENUMERATE_FLAG_ROLE

#undef __ENUMERATE_METRIC_ROLE
#define __ENUMERATE_METRIC_ROLE(role) \
    data->metric[(int)MetricRole::role] = get_metric(#role, (int)MetricRole::role);
    ENUMERATE_METRIC_ROLES(__ENUMERATE_METRIC_ROLE)
#undef __ENUMERATE_METRIC_ROLE

    if (!color_scheme.has_value() || color_scheme.value() != "Custom"sv) {
        auto maybe_color_config = Core::ConfigFile::open(data->path[(int)PathRole::ColorScheme]);
        if (!maybe_color_config.is_error()) {
            auto color_config = maybe_color_config.release_value();
            data->flag[(int)FlagRole::BoldTextAsBright] = color_config->read_bool_entry("Options", "ShowBoldTextAsBright", true);
        }
    }

    return buffer;
}

ErrorOr<Core::AnonymousBuffer> load_system_theme(ByteString const& path, Optional<ByteString> const& color_scheme)
{
    auto config_file = TRY(Core::ConfigFile::open(path));
    return TRY(load_system_theme(config_file, color_scheme));
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
