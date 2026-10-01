/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <LibGfx/FontCascadeList.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFaceSnapshot.h>
#include <LibWeb/StyleEngineRustFFI.h>

template<>
struct AK::Traits<Web::CSS::ComputedFontCacheKey> : public AK::DefaultTraits<Web::CSS::ComputedFontCacheKey> {
    static unsigned hash(Web::CSS::ComputedFontCacheKey const& key)
    {
        unsigned hash = 0;
        for (auto const& family : key.font_families) {
            if (family.has<Web::CSS::GenericFontFamily>()) {
                hash = pair_int_hash(hash, to_underlying(family.get<Web::CSS::GenericFontFamily>()));
            } else {
                auto const& name = family.get<Web::CSS::ComputedFontFamilyName>();
                hash = pair_int_hash(hash, pair_int_hash(name.name.hash(), to_underlying(name.syntax)));
            }
        }

        hash = pair_int_hash(hash, to_underlying(key.font_optical_sizing));
        hash = pair_int_hash(hash, Traits<Web::CSSPixels>::hash(key.font_size));
        hash = pair_int_hash(hash, key.font_slope);
        hash = pair_int_hash(hash, Traits<double>::hash(key.font_weight));
        hash = pair_int_hash(hash, Traits<double>::hash(key.font_width.value()));
        for (auto const& [variation_name, variation_value] : key.font_variation_settings)
            hash = pair_int_hash(hash, pair_int_hash(variation_name.hash(), Traits<double>::hash(variation_value)));
        hash = pair_int_hash(hash, Traits<Web::CSS::FontFeatureData>::hash(key.font_feature_data));
        hash = pair_int_hash(hash, key.font_feature_values_scope.value());

        return hash;
    }
};

namespace Web::CSS {

// The @font-feature-values a resolution reads for a family, in the tree scope the request comes from.
using FontFeatureValuesProvider = Function<FontFeatureValues const&(Utf16FlyString const&)>;

// Resolve a font cascade from the @font-face snapshot and the process-wide font services alone. It cannot start a
// load: a web face it selects is only noted, and request_wanted_web_faces() loads it.
[[nodiscard]] NonnullRefPtr<Gfx::FontCascadeList const> resolve_font_cascade(FontFaceSnapshot const&, ComputedFontCacheKey const&, FontFeatureValuesProvider const&);

// The font-family list as font matching wants it: generic families kept apart from names, and a name's syntax kept.
[[nodiscard]] Vector<ComputedFontFamily> computed_font_families_from_style_value(StyleValue const& font_family);

// The computed values a style engine resolution request names beside the family, by FontResolutionFeatureInput; a
// null one has its property's initial value.
using FontResolutionFeatureInput = StyleEngineFFI::FontResolutionFeatureInput;
using FontResolutionFeatureValues = Array<RefPtr<StyleValue const>, StyleEngineFFI::FONT_RESOLUTION_FEATURE_INPUT_COUNT>;
[[nodiscard]] FontFeatureData font_feature_data_from_style_values(FontResolutionFeatureValues const&);
[[nodiscard]] HashMap<Utf16FlyString, double> font_variation_settings_from_style_values(FontResolutionFeatureValues const&);

// Resolve a request against a document's @font-face table, through the cascades it has resolved before. Outside a
// style update, the web faces the resolution selects start loading here.
[[nodiscard]] NonnullRefPtr<Gfx::FontCascadeList const> resolve_font_for_style_values(FontComputer const&, ComputedFontCacheKey);

// The cascades already resolved for a document, by request: a memo of a pure function of the snapshot and the request,
// which the document forgets entries of when a change to its @font-face table makes them stale.
class FontCascadeMemo final : public RefCounted<FontCascadeMemo> {
public:
    static NonnullRefPtr<FontCascadeMemo> create() { return adopt_ref(*new FontCascadeMemo); }

    [[nodiscard]] NonnullRefPtr<Gfx::FontCascadeList const> resolve(FontFaceSnapshot const&, ComputedFontCacheKey const&, FontFeatureValuesProvider const&);
    void forget_matching(Function<bool(ComputedFontCacheKey const&, NonnullRefPtr<Gfx::FontCascadeList const> const&)> const&);

private:
    FontCascadeMemo() = default;

    HashMap<ComputedFontCacheKey, NonnullRefPtr<Gfx::FontCascadeList const>> m_cascades;
    // The newest snapshot generation resolved against. An older one would write a stale cascade where every later
    // request reads it.
    u64 m_generation { 0 };
};

}

// The style engine resolves a font with nothing but the @font-face table and memo it was given, and the request, and
// gives up its references to them through these.
extern "C" WEB_API Web::CSS::StyleEngineFFI::FfiResolvedFont web_css_resolve_font(void* memo, void const* snapshot, Web::CSS::StyleEngineFFI::FfiFontResolutionRequest);
extern "C" WEB_API void web_css_font_face_snapshot_unreference(void const*);
extern "C" WEB_API void web_css_font_cascade_memo_unreference(void const*);
