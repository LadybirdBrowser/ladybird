/*
 * Copyright (c) 2024, Oleg Luganskiy <oleg.lgnsk@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebView/Plugins/FontPlugin.h>

// Mock font provider that simulates missing fonts
class MockFontProvider : public Gfx::PathFontProvider {
public:
    MockFontProvider() = default;
    virtual ~MockFontProvider() override = default;

    void set_available_fonts(Vector<FlyString> fonts)
    {
        m_available_fonts = move(fonts);
    }

    virtual RefPtr<Gfx::Font> get_by_name(FlyString const& name, float point_size, unsigned weight, Gfx::FontWidth width, unsigned flags) override
    {
        // Only return a font if it's in our available fonts list.
        if (m_available_fonts.contains_slow(name))
            return Gfx::PathFontProvider::get_by_name(name, point_size, weight, width, flags);
        return {};
    }

private:
    Vector<FlyString> m_available_fonts;
};

TEST_CASE(font_plugin_handles_missing_monospace_font)
{
    // Create a mock font provider with no available fonts.
    auto mock_provider = make<MockFontProvider>();
    mock_provider->set_available_fonts({});

    // Create the font plugin with mock provider.
    // This should not crash despite no fonts being available.
    auto font_plugin = WebView::FontPlugin(false, mock_provider.ptr());
    
    // Verify a valid default fixed-width font.
    auto& default_fixed_width_font = font_plugin.default_fixed_width_font();
    EXPECT(default_fixed_width_font.is_fixed_width());
}

TEST_CASE(font_plugin_uses_fallback_when_primary_font_missing)
{
    // Create a mock font provider with some fonts but not the primary monospace font.
    auto mock_provider = make<MockFontProvider>();
    
    // Add some fallback fonts but not the primary one that would be returned by fontconfig.
    Vector<FlyString> available_fonts = {
        "Courier New"_fly_string,
        "Liberation Mono"_fly_string
    };
    mock_provider->set_available_fonts(move(available_fonts));
    
    // Load the fonts into the provider.
    for (auto const& path : Core::StandardPaths::font_directories().release_value_but_fixme_should_propagate_errors())
        mock_provider->load_all_fonts_from_uri(MUST(String::formatted("file://{}", path)));

    // Create the font plugin with mock provider.
    auto font_plugin = WebView::FontPlugin(false, mock_provider.ptr());
    
    // Verify a valid default fixed-width font.
    auto& default_fixed_width_font = font_plugin.default_fixed_width_font();
    EXPECT(default_fixed_width_font.is_fixed_width());
    
    // The font name should be one of fallback fonts.
    auto font_name = default_fixed_width_font.family();
    EXPECT(font_name == "Courier New"_fly_string || font_name == "Liberation Mono"_fly_string);
}

TEST_CASE(font_plugin_uses_primary_font_when_available)
{
    // Mock font provider with the primary monospace font available.
    auto mock_provider = make<MockFontProvider>();
    
    // JetBrainsMono is available.
    Vector<FlyString> available_fonts = {
        "JetBrainsMono Nerd Font"_fly_string
    };
    mock_provider->set_available_fonts(move(available_fonts));
    
    // Load the fonts into the provider.
    for (auto const& path : Core::StandardPaths::font_directories().release_value_but_fixme_should_propagate_errors())
        mock_provider->load_all_fonts_from_uri(MUST(String::formatted("file://{}", path)));

    // Create the font plugin with mock provider.
    auto font_plugin = WebView::FontPlugin(false, mock_provider.ptr());
    
    // Verify a valid default fixed-width font.
    auto& default_fixed_width_font = font_plugin.default_fixed_width_font();
    EXPECT(default_fixed_width_font.is_fixed_width());
    
    // The font name should be primary font.
    EXPECT_EQ(default_fixed_width_font.family(), "JetBrainsMono Nerd Font"_fly_string);
}
