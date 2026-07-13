/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/FontCascadeList.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/FontFace.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/ParsedFontFace.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::CSS {

class FontLoader;

class FontFace final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(FontFace, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(FontFace);

public:
    using FontFaceSource = FlattenVariant<Variant<Utf16String>, WebIDL::BufferSourceVariant>;

    [[nodiscard]] static GC::Ref<FontFace> construct_impl(JS::Realm&, Utf16String family, FontFaceSource source, Bindings::FontFaceDescriptors const& descriptors);
    [[nodiscard]] static GC::Ref<FontFace> create_css_connected(JS::Realm&, CSSFontFaceRule&);
    virtual ~FontFace() override;

    Utf16String family() const { return m_family.to_utf16_string(); }
    Utf16FlyString const& family_name() const { return m_family; }
    WebIDL::ExceptionOr<void> set_family(Utf16View);
    void set_family_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& style() const { return m_style; }
    WebIDL::ExceptionOr<void> set_style(Utf16View);
    void set_style_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& weight() const { return m_weight; }
    WebIDL::ExceptionOr<void> set_weight(Utf16View);
    void set_weight_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& stretch() const { return m_stretch; }
    WebIDL::ExceptionOr<void> set_stretch(Utf16View);
    void set_stretch_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& unicode_range() const { return m_unicode_range; }
    WebIDL::ExceptionOr<void> set_unicode_range(Utf16View);
    void set_unicode_range_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& feature_settings() const { return m_feature_settings; }
    WebIDL::ExceptionOr<void> set_feature_settings(Utf16View);
    void set_feature_settings_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& variation_settings() const { return m_variation_settings; }
    WebIDL::ExceptionOr<void> set_variation_settings(Utf16View);
    void set_variation_settings_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& display() const { return m_display; }
    WebIDL::ExceptionOr<void> set_display(Utf16View);
    void set_display_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& ascent_override() const { return m_ascent_override; }
    WebIDL::ExceptionOr<void> set_ascent_override(Utf16View);
    void set_ascent_override_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& descent_override() const { return m_descent_override; }
    WebIDL::ExceptionOr<void> set_descent_override(Utf16View);
    void set_descent_override_impl(NonnullRefPtr<StyleValue const> const& value);

    Utf16String const& line_gap_override() const { return m_line_gap_override; }
    WebIDL::ExceptionOr<void> set_line_gap_override(Utf16View);
    void set_line_gap_override_impl(NonnullRefPtr<StyleValue const> const& value);

    bool is_css_connected() const { return m_css_font_face_rule != nullptr; }
    void disconnect_from_css_rule();
    void reparse_connected_css_font_face_rule_descriptors();

    ParsedFontFace parsed_font_face() const;

    RefPtr<Gfx::Typeface const> typeface() const { return m_parsed_font; }

    FontWeightRange declared_weight_range() const { return m_cached_weight_range; }
    int declared_slope() const { return m_cached_slope; }
    int declared_width() const { return m_cached_width; }
    bool should_be_registered_with_font_computer() const { return is_css_connected() || status() == Bindings::FontFaceLoadStatus::Loaded; }

    RefPtr<Gfx::FontCascadeList const> font_with_point_size(float point_size, Gfx::FontVariationSettings const&, Gfx::ShapeFeatures const&) const;

    Vector<Gfx::UnicodeRange> const& unicode_ranges() const { return m_unicode_ranges; }
    bool has_urls() const { return !m_urls.is_empty(); }

    bool has_non_default_unicode_range() const
    {
        if (m_unicode_ranges.size() != 1)
            return true;
        auto const& range = m_unicode_ranges.first();
        return range.min_code_point() != 0 || range.max_code_point() != 0x10FFFF;
    }

    Bindings::FontFaceLoadStatus status() const { return m_status; }

    GC::Ref<WebIDL::Promise> load();
    GC::Ref<WebIDL::Promise> loaded() const;

    GC::Ref<WebIDL::Promise> font_status_promise() { return m_font_status_promise; }

    void add_to_set(FontFaceSet&);
    void remove_from_set(FontFaceSet&);

private:
    FontFace(JS::Realm&, GC::Ref<WebIDL::Promise> font_status_promise);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;
    void reject_status_promise(JS::Value reason);

    Optional<FontComputer&> font_computer() const;

    [[nodiscard]] Optional<ComputationContext> computation_context() const;

    // FIXME: Should we be storing StyleValues instead?
    Utf16FlyString m_family;
    Utf16String m_style;
    Utf16String m_weight;
    Utf16String m_stretch;
    Utf16String m_unicode_range;
    Vector<Gfx::UnicodeRange> m_unicode_ranges;
    Utf16String m_feature_settings;
    Utf16String m_variation_settings;
    Utf16String m_display;
    Utf16String m_ascent_override;
    Utf16String m_descent_override;
    Utf16String m_line_gap_override;

    FontWeightRange m_cached_weight_range { 400, 400 };
    int m_cached_slope { 0 };
    int m_cached_width { 100 };
    GC::Ptr<FontLoader> m_font_loader;

    // https://drafts.csswg.org/css-font-loading/#dom-fontface-status
    Bindings::FontFaceLoadStatus m_status { Bindings::FontFaceLoadStatus::Unloaded };

    GC::Ref<WebIDL::Promise> m_font_status_promise; // [[FontStatusPromise]]
    Vector<ParsedFontFace::Source> m_urls;          // [[Urls]]
    ByteBuffer m_binary_data {};                    // [[Data]]

    RefPtr<Gfx::Typeface const> m_parsed_font;
    RefPtr<Core::Promise<NonnullRefPtr<Gfx::Typeface const>>> m_font_load_promise;

    GC::Ptr<CSSFontFaceRule> m_css_font_face_rule;
    HashTable<GC::Ref<FontFaceSet>> m_containing_sets;
};

bool font_format_is_supported(Utf16View name);

bool font_tech_is_supported(FontTech);
bool font_tech_is_supported(Utf16View name);

}
