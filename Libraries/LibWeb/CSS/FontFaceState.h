/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Weakable.h>
#include <LibGC/HeapVector.h>
#include <LibGC/Weak.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/FontCascadeList.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/FontFace.h>
#include <LibWeb/CSS/ParsedFontFace.h>
#include <LibWeb/CSS/RustDescriptorBlock.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::CSS {

class FontLoader;
using FontFaceLoadStatus = Bindings::FontFaceLoadStatus;
using FontFaceDescriptors = Bindings::FontFaceDescriptors;

class FontFaceState final : public RefCounted<FontFaceState>
    , public Weakable<FontFaceState> {

public:
    using FontFaceSource = FlattenVariant<Variant<Utf16String>, WebIDL::BufferSourceVariant>;

    [[nodiscard]] static NonnullRefPtr<FontFaceState> create_for_constructor(JS::Object&, Utf16String family, FontFaceSource source, Bindings::FontFaceDescriptors const& descriptors);
    [[nodiscard]] static NonnullRefPtr<FontFaceState> create_css_connected(JS::Realm&, u64 rule_identity, StyleSheetState&);
    ~FontFaceState();
    FontFace& cssom_font_face() const;
    void visit_edges(GC::Cell::Visitor&);
    GC::Ref<GC::HeapVector<NonnullRefPtr<FontFaceState>>> keep_alive_during_load();
    void load_for_style();

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

    bool is_css_connected() const { return m_css_font_face_rule_identity.has_value(); }
    Optional<u64> css_rule_identity() const
    {
        return m_css_font_face_rule_identity;
    }
    void disconnect_from_css_rule();
    void reparse_connected_css_font_face_rule_descriptors();

    ParsedFontFace parsed_font_face() const;

    RefPtr<Gfx::Typeface const> typeface() const { return m_parsed_font; }

    FontWeightRange declared_weight_range() const { return m_cached_weight_range; }
    int declared_slope() const { return m_cached_slope; }
    int declared_width() const { return m_cached_width; }
    bool should_be_registered_with_font_computer() const;

    RefPtr<Gfx::FontCascadeList const> font_with_point_size(float point_size, Gfx::FontVariationSettings const&, Gfx::ShapeFeatures const&) const;

    Vector<Gfx::UnicodeRange> const& unicode_ranges() const { return m_unicode_ranges; }
    bool has_urls() const { return !m_urls.is_empty(); }
    bool is_pending_rendering_from_cache() const;
    bool has_pending_rendering() const;
    void set_font_display_time_for_testing(u32 milliseconds);
    Gfx::PendingFontState resolve_for_rendering();

    bool has_non_default_unicode_range() const
    {
        if (m_unicode_ranges.size() != 1)
            return true;
        auto const& range = m_unicode_ranges.first();
        return range.min_code_point() != 0 || range.max_code_point() != 0x10FFFF;
    }

    FontFaceLoadStatus status() const { return m_status; }

    GC::Ref<WebIDL::Promise> load();
    GC::Ref<WebIDL::Promise> loaded() const;

    GC::Ref<WebIDL::Promise> font_status_promise() { return loaded(); }

    void add_to_set(FontFaceSet&);
    void remove_from_set(FontFaceSet&);

private:
    FontFaceState(GC::Ref<HTML::EnvironmentSettingsObject>, GC::Ptr<WebIDL::Promise> font_status_promise = nullptr);

    JS::Object& task_global_object() const;
    void reject_status_promise(WebIDL::Exception);
    void did_load(RefPtr<Gfx::Typeface const>);

    Optional<FontComputer&> font_computer() const;
    void update_font_display_period();
    i64 font_download_elapsed_time() const;
    void invalidate_font_display();
    CSSFontFaceRule& cssom_rule() const;
    RustDescriptorBlock connected_descriptors() const;

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
    GC::Ref<HTML::EnvironmentSettingsObject> m_environment;

    // https://drafts.csswg.org/css-fonts-4/#font-display-timeline
    enum class FontDisplayPeriod : u8 {
        Block,
        Swap,
        Failure,
    };
    FontDisplay m_font_display { FontDisplay::Auto };
    FontDisplayPeriod m_font_display_period { FontDisplayPeriod::Block };
    Optional<MonotonicTime> m_font_download_timer_start;
    GC::Ptr<Platform::Timer> m_font_download_timer;
    Optional<u32> m_font_display_time_for_testing;
    bool m_font_display_failed { false };
    bool m_font_download_completed { false };

    // https://drafts.csswg.org/css-font-loading/#dom-fontface-status
    FontFaceLoadStatus m_status;

    mutable GC::Ptr<WebIDL::Promise> m_font_status_promise; // [[FontStatusPromise]]
    Vector<ParsedFontFace::Source> m_urls;                  // [[Urls]]
    ByteBuffer m_binary_data {};                            // [[Data]]

    RefPtr<Gfx::Typeface const> m_parsed_font;
    RefPtr<Core::Promise<NonnullRefPtr<Gfx::Typeface const>>> m_font_load_promise;

    Optional<u64> m_css_font_face_rule_identity;
    RefPtr<StyleSheetState> m_source_style_sheet;
    HashTable<GC::Ref<FontFaceSet>> m_containing_sets;
    mutable GC::Weak<FontFace> m_cssom_font_face;
    GC::Ptr<WebIDL::DOMException> m_load_error;
    bool m_visiting_edges { false };
};

bool font_format_is_supported(Utf16View name);

}
