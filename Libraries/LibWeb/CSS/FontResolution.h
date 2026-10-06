/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
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
        // NB: Which feature values a request names is enough to tell most requests apart, and equality compares them.
        for (auto const& value : key.feature_values)
            hash = pair_int_hash(hash, value ? 1 : 0);
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

using FontResolutionFeatureInput = StyleEngineFFI::FontResolutionFeatureInput;
static_assert(font_resolution_feature_input_count == StyleEngineFFI::FONT_RESOLUTION_FEATURE_INPUT_COUNT);

// Resolve a request against a document's @font-face table, through the cascades it has resolved before. Outside a
// style update, the web faces the resolution selects start loading here.
[[nodiscard]] NonnullRefPtr<Gfx::FontCascadeList const> resolve_font_for_style_values(FontComputer const&, ComputedFontCacheKey);

// The cascades already resolved for a document, by request: a memo of a pure function of the snapshot and the request,
// which the document forgets entries of when a change to its @font-face table makes them stale.
// The style engine holds a reference beside the document's and resolves through it from whichever thread it runs on,
// so the memo counts references atomically and takes a lock around its table.
class FontCascadeMemo final : public AtomicRefCounted<FontCascadeMemo> {
public:
    static NonnullRefPtr<FontCascadeMemo> create() { return adopt_ref(*new FontCascadeMemo); }

    // NB: A memo of a pure function, so filling it does not change what it answers.
    [[nodiscard]] NonnullRefPtr<Gfx::FontCascadeList const> resolve(FontFaceSnapshot const&, ComputedFontCacheKey const&, FontFeatureValuesProvider const&) const;
    // NB: Only under a font environment generation newer than any resolved against: the style engine names the memo's
    //     cascades without holding a reference to them for as long as a generation stands, so the forgotten ones are
    //     retired, not released, as a style transaction that flew may still name them.
    void forget_matching(u64 environment_generation, Function<bool(ComputedFontCacheKey const&, NonnullRefPtr<Gfx::FontCascadeList const> const&)> const&);
    // Releases the retired cascades, on the document thread, where the engine is about to resolve against the newest
    // table: what it named from older ones is never read again.
    void release_retired() const;

    struct ResolutionAgainstOlderTable {
        ComputedFontCacheKey key;
        NonnullRefPtr<Gfx::FontCascadeList const> font_list;
    };
    // The cascades resolved against an older table since last taken, which may answer their requests differently now.
    [[nodiscard]] Vector<ResolutionAgainstOlderTable> take_resolutions_against_older_tables() const;

private:
    FontCascadeMemo() = default;

    // Guards the four members below. A resolution runs under it, so a cascade is resolved once however many threads
    // want it.
    mutable Mutex m_mutex;
    mutable HashMap<ComputedFontCacheKey, NonnullRefPtr<Gfx::FontCascadeList const>> m_cascades;
    // The newest snapshot generation resolved against or forgotten under. An older one would write a stale cascade
    // where every later request reads it.
    mutable u64 m_generation { 0 };
    // The cascades forgotten, or resolved against an older table, that the engine may still name.
    mutable Vector<NonnullRefPtr<Gfx::FontCascadeList const>> m_retired;
    mutable Vector<ResolutionAgainstOlderTable> m_resolutions_against_older_tables;
};

}

// The style engine resolves a font with nothing but the @font-face table and memo it was given, and the request, and
// gives up its references to them through these.
extern "C" WEB_API Web::CSS::StyleEngineFFI::FfiResolvedFont web_css_resolve_font(void const* memo, void const* snapshot, Web::CSS::StyleEngineFFI::FfiFontResolutionRequest);
extern "C" WEB_API void web_css_font_face_snapshot_unreference(void const*);
extern "C" WEB_API void web_css_font_cascade_memo_unreference(void const*);
