/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <LibCore/StandardPaths.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>

#if defined(AK_OS_HAIKU)
#    include <FindDirectory.h>
#endif

namespace Gfx {

// Key function for SystemFontProvider to emit the vtable here
SystemFontProvider::~SystemFontProvider() = default;

FontDatabase& FontDatabase::the()
{
    static FontDatabase s_the;
    return s_the;
}

SystemFontProvider& FontDatabase::install_system_font_provider(NonnullOwnPtr<SystemFontProvider> provider)
{
    VERIFY(!m_system_font_provider);
    m_system_font_provider = move(provider);
    return *m_system_font_provider;
}

StringView FontDatabase::system_font_provider_name() const
{
    VERIFY(m_system_font_provider);
    return m_system_font_provider->name();
}

FontDatabase::FontDatabase() = default;

RefPtr<Gfx::Font> FontDatabase::get(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope)
{
    return m_system_font_provider->get_font(family, point_size, weight, width, slope);
}

void FontDatabase::for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)> callback)
{
    m_system_font_provider->for_each_typeface_with_family_name(family_name, move(callback));
}

ErrorOr<Vector<String>> FontDatabase::font_directories()
{
#if defined(AK_OS_HAIKU)
    Vector<String> paths_vector;
    char** paths;
    size_t paths_count;
    if (find_paths(B_FIND_PATH_FONTS_DIRECTORY, NULL, &paths, &paths_count) == B_OK) {
        for (size_t i = 0; i < paths_count; ++i) {
            StringBuilder builder;
            builder.append(paths[i], strlen(paths[i]));
            paths_vector.append(TRY(builder.to_string()));
        }
    }
    return paths_vector;
#else
    auto paths = Vector { {
#    if defined(AK_OS_SERENITY)
        "/res/fonts"_string,
#    elif defined(AK_OS_MACOS)
        "/System/Library/Fonts"_string,
        "/Library/Fonts"_string,
        TRY(String::formatted("{}/Library/Fonts"sv, Core::StandardPaths::home_directory())),
#    elif defined(AK_OS_ANDROID)
        // FIXME: We should be using the ASystemFontIterator NDK API here.
        // There is no guarantee that this will continue to exist on future versions of Android.
        "/system/fonts"_string,
#    elif defined(AK_OS_WINDOWS)
        TRY(String::formatted(R"({}\Fonts)"sv, getenv("WINDIR"))),
        TRY(String::formatted(R"({}\Microsoft\Windows\Fonts)"sv, getenv("LOCALAPPDATA"))),
#    else
        TRY(String::formatted("{}/fonts", Core::StandardPaths::user_data_directory())),
        TRY(String::formatted("{}/X11/fonts", Core::StandardPaths::user_data_directory())),
#    endif
    } };
#    if !(defined(AK_OS_SERENITY) || defined(AK_OS_MACOS) || defined(AK_OS_WINDOWS))
    auto data_directories = Core::StandardPaths::system_data_directories();
    for (auto& data_directory : data_directories) {
        paths.append(TRY(String::formatted("{}/fonts", data_directory)));
        paths.append(TRY(String::formatted("{}/X11/fonts", data_directory)));
    }
#    endif
    return paths;
#endif
}

}
