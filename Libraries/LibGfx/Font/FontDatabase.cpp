/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <LibCore/StandardPaths.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/TypefaceSkia.h>

#if defined(AK_OS_HAIKU)
#    include <FindDirectory.h>
#endif

namespace Gfx {

// Key function for SystemFontProvider to emit the vtable here
SystemFontProvider::~SystemFontProvider() = default;

FontDatabase& FontDatabase::the()
{
    static FontDatabase& database = *new FontDatabase;
    return database;
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

FontVariationSettings default_font_variation_settings(float point_size, unsigned weight, unsigned width)
{
    FontVariationSettings variation_settings;
    variation_settings.set_weight(static_cast<float>(weight));
    // NB: We use the pixel size for 'opsz'
    variation_settings.set_optical_sizing(point_size / 0.75f);

    switch (width) {
    case FontWidth::UltraCondensed:
        variation_settings.set_width(50);
        break;
    case FontWidth::ExtraCondensed:
        variation_settings.set_width(62.5);
        break;
    case FontWidth::Condensed:
        variation_settings.set_width(75);
        break;
    case FontWidth::SemiCondensed:
        variation_settings.set_width(87.5);
        break;
    case FontWidth::Normal:
        variation_settings.set_width(100);
        break;
    case FontWidth::SemiExpanded:
        variation_settings.set_width(112.5);
        break;
    case FontWidth::Expanded:
        variation_settings.set_width(125);
        break;
    case FontWidth::ExtraExpanded:
        variation_settings.set_width(150);
        break;
    case FontWidth::UltraExpanded:
        variation_settings.set_width(200);
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    return variation_settings;
}

ShapeFeatures default_shape_features()
{
    // NB: These shape features match those applied when all CSS properties are initial values
    ShapeFeatures shape_features;
    shape_features.append({ { 'c', 'l', 'i', 'g' }, 1 });
    shape_features.append({ { 'k', 'e', 'r', 'n' }, 1 });
    shape_features.append({ { 'l', 'i', 'g', 'a' }, 1 });
    return shape_features;
}

RefPtr<Gfx::Font> FontDatabase::get(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& font_variation_settings, Optional<Gfx::ShapeFeatures> const& shape_features)
{
    return m_system_font_provider->get_font(family, point_size, weight, width, slope, font_variation_settings, shape_features);
}

RefPtr<Gfx::Font> FontDatabase::get_font_for_code_point(u32 code_point, float point_size, u16 weight, u16 width, u8 slope, bool prefer_color_emoji)
{
    CodePointFallbackKey key { code_point, weight, width, slope, prefer_color_emoji };
    auto& entry = m_code_point_fallback_cache.ensure(key, [&]() -> CodePointFallbackEntry {
        auto typeface_or_error = TypefaceSkia::find_typeface_for_code_point(code_point, weight, width, slope, prefer_color_emoji);
        if (typeface_or_error.is_error() || !typeface_or_error.value())
            return { {}, nullptr };

        auto typeface = typeface_or_error.release_value();
        return { typeface->family(), typeface };
    });

    // FIXME: Does it matter that we don't pass a FontVariationSettings or ShapeFeatures here?
    if (entry.typeface)
        return entry.typeface->font(point_size, {});

    return nullptr;
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

#elif defined(AK_OS_SERENITY)
    return Vector<String> { {
        "/res/fonts"_string,
    } };

#elif defined(AK_OS_MACOS)
    return Vector<String> { {
        "/System/Library/Fonts"_string,
        "/Library/Fonts"_string,
        TRY(String::formatted("{}/Library/Fonts"sv, Core::StandardPaths::home_directory())),
    } };

#elif defined(AK_OS_ANDROID)
    return Vector<String> { {
        // FIXME: We should be using the ASystemFontIterator NDK API here.
        // There is no guarantee that this will continue to exist on future versions of Android.
        "/system/fonts"_string,
    } };

#elif defined(AK_OS_WINDOWS)
    return Vector<String> { {
        TRY(String::formatted(R"({}\Fonts)"sv, getenv("WINDIR"))),
        TRY(String::formatted(R"({}\Microsoft\Windows\Fonts)"sv, getenv("LOCALAPPDATA"))),
    } };

#else
    Vector<String> paths;

    auto home_directory = Core::StandardPaths::home_directory();
    paths.append(TRY(String::formatted("{}/.fonts", home_directory)));

    auto user_data_directory = Core::StandardPaths::user_data_directory();
    paths.append(TRY(String::formatted("{}/fonts", user_data_directory)));
    paths.append(TRY(String::formatted("{}/X11/fonts", user_data_directory)));

    auto data_directories = Core::StandardPaths::system_data_directories();
    for (auto& data_directory : data_directories) {
        paths.append(TRY(String::formatted("{}/fonts", data_directory)));
        paths.append(TRY(String::formatted("{}/X11/fonts", data_directory)));
    }

    return paths;
#endif
}

}
