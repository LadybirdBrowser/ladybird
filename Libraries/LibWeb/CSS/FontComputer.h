/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibGfx/FontCascadeList.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/CSS/FontFeatureData.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

#pragma once

namespace Web::CSS {

struct FontFaceKey;

struct OwnFontFaceKey {
    explicit OwnFontFaceKey(FontFaceKey const& other);

    operator FontFaceKey() const;

    [[nodiscard]] u32 hash() const { return pair_int_hash(family_name.hash(), pair_int_hash(weight, slope)); }
    [[nodiscard]] bool operator==(OwnFontFaceKey const& other) const = default;
    [[nodiscard]] bool operator==(FontFaceKey const& other) const;

    FlyString family_name;
    int weight { 0 };
    int slope { 0 };
};

struct ComputedFontCacheKey {
    ValueComparingNonnullRefPtr<StyleValue const> font_family;
    FontOpticalSizing font_optical_sizing;
    CSSPixels font_size;
    int font_slope;
    double font_weight;
    Percentage font_width;
    HashMap<FlyString, double> font_variation_settings;
    FontFeatureData font_feature_data;

    [[nodiscard]] bool operator==(ComputedFontCacheKey const& other) const = default;
};

class FontLoader final : public GC::Cell {
    GC_CELL(FontLoader, GC::Cell);
    GC_DECLARE_ALLOCATOR(FontLoader);

public:
    FontLoader(FontComputer&, RuleOrDeclaration, FlyString family_name, Vector<Gfx::UnicodeRange> unicode_ranges, Vector<URL> urls, GC::Ptr<GC::Function<void(RefPtr<Gfx::Typeface const>)>> on_load = {});

    virtual ~FontLoader();

    Vector<Gfx::UnicodeRange> const& unicode_ranges() const { return m_unicode_ranges; }

    RefPtr<Gfx::Font const> font_with_point_size(float point_size, Gfx::FontVariationSettings const& variations, Gfx::ShapeFeatures const& shape_features);
    void start_loading_next_url();

    bool is_loading() const;

private:
    virtual void visit_edges(Visitor&) override;

    ErrorOr<NonnullRefPtr<Gfx::Typeface const>> try_load_font(Fetch::Infrastructure::Response const&, ByteBuffer const&);

    void font_did_load_or_fail(RefPtr<Gfx::Typeface const>);

    GC::Ref<FontComputer> m_font_computer;
    RuleOrDeclaration m_rule_or_declaration;
    FlyString m_family_name;
    Vector<Gfx::UnicodeRange> m_unicode_ranges;
    RefPtr<Gfx::Typeface const> m_typeface;
    Vector<URL> m_urls;
    GC::Ptr<Fetch::Infrastructure::FetchController> m_fetch_controller;
    GC::Ptr<GC::Function<void(RefPtr<Gfx::Typeface const>)>> m_on_load;
};

class WEB_API FontComputer final : public GC::Cell {
    GC_CELL(FontComputer, GC::Cell);
    GC_DECLARE_ALLOCATOR(FontComputer);

public:
    explicit FontComputer(DOM::Document& document)
        : m_document(document)
    {
    }

    ~FontComputer() = default;

    DOM::Document& document() { return m_document; }
    DOM::Document const& document() const { return m_document; }

    Gfx::Font const& initial_font() const;

    void did_load_font(FlyString const& family_name);

    GC::Ptr<FontLoader> load_font_face(ParsedFontFace const&, GC::Ptr<GC::Function<void(RefPtr<Gfx::Typeface const>)>> on_load = {});

    void load_fonts_from_sheet(CSSStyleSheet&);
    void unload_fonts_from_sheet(CSSStyleSheet&);

    NonnullRefPtr<Gfx::FontCascadeList const> compute_font_for_style_values(StyleValue const& font_family, CSSPixels const& font_size, int font_slope, double font_weight, Percentage const& font_width, FontOpticalSizing font_optical_sizing, HashMap<FlyString, double> const& font_variation_settings, FontFeatureData const& font_feature_data) const;

    size_t number_of_css_font_faces_with_loading_in_progress() const;

private:
    virtual void visit_edges(Visitor&) override;

    struct MatchingFontCandidate;
    static RefPtr<Gfx::FontCascadeList const> find_matching_font_weight_ascending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, Gfx::FontVariationSettings const& variations, FontFeatureData const& font_feature_data, bool inclusive);
    static RefPtr<Gfx::FontCascadeList const> find_matching_font_weight_descending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, Gfx::FontVariationSettings const& variations, FontFeatureData const& font_feature_data, bool inclusive);
    NonnullRefPtr<Gfx::FontCascadeList const> compute_font_for_style_values_impl(StyleValue const& font_family, CSSPixels const& font_size, int font_slope, double font_weight, Percentage const& font_width, FontOpticalSizing font_optical_sizing, HashMap<FlyString, double> const& font_variation_settings, FontFeatureData const& font_feature_data) const;
    RefPtr<Gfx::FontCascadeList const> font_matching_algorithm(FlyString const& family_name, int weight, int slope, float font_size_in_pt, Gfx::FontVariationSettings const& variations, FontFeatureData const& font_feature_data) const;

    GC::Ref<DOM::Document> m_document;

    using FontLoaderList = Vector<GC::Ref<FontLoader>>;
    HashMap<OwnFontFaceKey, FontLoaderList> m_loaded_fonts;

    mutable HashMap<ComputedFontCacheKey, NonnullRefPtr<Gfx::FontCascadeList const>> m_computed_font_cache;
};

}
