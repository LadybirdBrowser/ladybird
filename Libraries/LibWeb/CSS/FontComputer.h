/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteString.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/CellAllocator.h>
#include <LibGfx/FontCascadeList.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::CSS::Parser::ValueParserFFI {

enum class FontFeatureValuesRuleKind : uint8_t;

}

namespace Web::CSS {

class FontCascadeMemo;
class FontFaceSnapshot;
class RustRuleView;

struct FontWeightRange {
    int min { 0 };
    int max { 0 };
    [[nodiscard]] u32 hash() const { return pair_int_hash(min, max); }
    [[nodiscard]] bool operator==(FontWeightRange const&) const = default;
};

struct FontFaceKey {
    Utf16FlyString family_name;
    FontWeightRange weight;
    int slope { 0 };
    int width { 100 };
    [[nodiscard]] u32 hash() const { return pair_int_hash(family_name.ascii_case_insensitive_hash(), pair_int_hash(weight.hash(), pair_int_hash(slope, width))); }
    [[nodiscard]] bool operator==(FontFaceKey const& other) const
    {
        return family_name.equals_ignoring_ascii_case(other.family_name)
            && weight == other.weight
            && slope == other.slope
            && width == other.width;
    }
};

}

template<>
struct AK::Traits<Web::CSS::FontFaceKey> : public AK::DefaultTraits<Web::CSS::FontFaceKey> {
    static unsigned hash(Web::CSS::FontFaceKey const& key) { return key.hash(); }
};

namespace Web::CSS {

enum class ComputedFontFamilySyntax {
    CustomIdent,
    String,
};

struct ComputedFontFamilyName {
    Utf16FlyString name;
    ComputedFontFamilySyntax syntax { ComputedFontFamilySyntax::CustomIdent };

    bool operator==(ComputedFontFamilyName const&) const = default;
};

using ComputedFontFamily = Variant<GenericFontFamily, ComputedFontFamilyName>;

// The computed values a font resolution reads beside the family, by StyleEngineFFI::FontResolutionFeatureInput; a null
// one has its property's initial value. They select the OpenType features and the variations of the fonts.
static constexpr size_t font_resolution_feature_input_count = 11;
using FontResolutionFeatureValues = Array<ValueComparingRefPtr<StyleValue const>, font_resolution_feature_input_count>;

struct ComputedFontCacheKey {
    Vector<ComputedFontFamily> font_families;
    FontOpticalSizing font_optical_sizing;
    CSSPixels font_size;
    int font_slope;
    double font_weight;
    Percentage font_width;
    FontResolutionFeatureValues feature_values;
    // The tree scope whose @font-feature-values the request reads, or the document's when its
    // font-variant-alternates name no feature values, so that alike requests share one answer.
    TreeScopeID font_feature_values_scope;

    [[nodiscard]] bool operator==(ComputedFontCacheKey const& other) const = default;
};

struct FontFeatureValueKey {
    Parser::ValueParserFFI::FontFeatureValuesRuleKind kind;
    Utf16FlyString name;

    bool operator==(FontFeatureValueKey const&) const = default;
};

}

template<>
struct AK::Traits<Web::CSS::FontFeatureValueKey> : public AK::DefaultTraits<Web::CSS::FontFeatureValueKey> {
    static unsigned hash(Web::CSS::FontFeatureValueKey const& key) { return pair_int_hash(to_underlying(key.kind), key.name.hash()); }
};

namespace Web::CSS {

using FontFeatureValues = HashMap<FontFeatureValueKey, Vector<u32>>;

// The @font-feature-values an element of each tree scope that declares some sees, for every family named: the
// document's, and each such shadow tree's over those of the trees its host is in. An element of a scope that declares
// none sees those of the nearest one around it. Immutable once built, so the font computer and every snapshot built
// while it stands share one, and a snapshot may let go of it on another thread.
struct FontFeatureValuesByScope final : public AtomicRefCounted<FontFeatureValuesByScope> {
    HashMap<TreeScopeID, HashMap<Utf16FlyString, FontFeatureValues>> scopes;
    // The shadow tree scopes among them.
    Vector<TreeScopeID> shadow_scopes;
};

struct FontFeatureValuesCacheKey {
    TreeScopeID tree_scope;
    Utf16FlyString family_name;
    [[nodiscard]] u32 hash() const { return pair_int_hash(tree_scope.value(), family_name.hash()); }
    [[nodiscard]] bool operator==(FontFeatureValuesCacheKey const&) const = default;
};

class FontLoader final : public GC::Cell {
    GC_CELL(FontLoader, GC::Cell);
    GC_DECLARE_ALLOCATOR(FontLoader);

public:
    using Source = Variant<Utf16FlyString, URL>;
    FontLoader(FontComputer&, RuleOrDeclaration, Vector<Source> sources, GC::Ptr<GC::Function<void(RefPtr<Gfx::Typeface const>)>> on_load = {});

    virtual ~FontLoader();

    void start_loading_next_source();

    bool is_loading() const;
    void did_request_for_rendering();
    bool may_finish_from_cache() const;
    bool has_started_request() const;
    bool has_received_font_data() const { return m_has_received_font_data; }

    void subscribe(GC::Ref<GC::Function<void(RefPtr<Gfx::Typeface const>)>>);

private:
    virtual void visit_edges(Visitor&) override;

    Optional<ByteString> try_load_font_mime_type_essence(Fetch::Infrastructure::Response const&, ByteBuffer const&);

    void font_did_load_or_fail(RefPtr<Gfx::Typeface const>);

    GC::Ref<FontComputer> m_font_computer;
    RuleOrDeclaration m_rule_or_declaration;
    RefPtr<Gfx::Typeface const> m_typeface;
    Vector<Source> m_sources;
    GC::Ptr<Fetch::Infrastructure::FetchController> m_fetch_controller;
    Vector<GC::Ref<GC::Function<void(RefPtr<Gfx::Typeface const>)>>> m_subscribers;
    Optional<DOM::DocumentLoadEventDelayer> m_document_load_event_delayer;
    bool m_has_completed { false };
    bool m_has_received_font_data { false };
};

class WEB_API FontComputer final : public GC::Cell {
    GC_CELL(FontComputer, GC::Cell);
    GC_DECLARE_ALLOCATOR(FontComputer);

public:
    FontComputer();
    explicit FontComputer(DOM::Document&);
    virtual ~FontComputer() override;

    DOM::Document& document() { return *m_document; }
    DOM::Document const& document() const { return *m_document; }

    Gfx::Font const& initial_font() const;
    bool should_defer_initial_paint();
    bool has_completed_initial_paint() const { return m_has_completed_initial_paint; }
    bool initial_paint_had_pending_fonts() const { return m_initial_paint_had_pending_fonts; }

    void clear_computed_font_cache(Utf16FlyString const& family_name);
    void clear_font_feature_values_cache(Utf16FlyString const& family_name);
    void did_load_font(Utf16FlyString const& family_name);
    void did_load_font(FontFaceKey const&);
    // The style transaction that flew has been drained, so the styles it computed are the elements' own.
    void did_end_flown_style_drain();

    void register_font_face(NonnullRefPtr<FontFaceState>);
    void unregister_font_face(NonnullRefPtr<FontFaceState>);
    void synchronize_font_face_order(Vector<NonnullRefPtr<FontFaceState>> const&);

    GC::Ptr<FontLoader> load_font_face(ReadonlySpan<FontLoader::Source>, RefPtr<StyleSheetState>, GC::Ptr<GC::Function<void(RefPtr<Gfx::Typeface const>)>> on_load = {});

    void load_fonts_from_sheet(StyleSheetState&);
    void unload_fonts_from_sheet(StyleSheetState&);
    // An @font-feature-values rule reaches the elements of every tree scope its sheet is in, where @font-face only
    // reaches the document's font source, so its sheet forgets what was built from it whoever owns the sheet.
    void forget_font_feature_values_declared_by(RustRuleView const&);
    void forget_font_feature_values_declared_in(StyleSheetState const&);

    u64 environment_generation() const { return m_environment_generation; }

    // The @font-face table at the current font environment generation. Inside a font face change batch the live
    // table moves ahead of it, and the generation catches up when the batch ends; nothing resolves a font in between.
    [[nodiscard]] NonnullRefPtr<FontFaceSnapshot const> font_face_snapshot() const;
    // The cascades resolved for this document.
    [[nodiscard]] FontCascadeMemo const& font_cascade_memo() const { return *m_font_cascade_memo; }
    // A resolution's view of the @font-feature-values of one tree scope.
    [[nodiscard]] Function<FontFeatureValues const&(Utf16FlyString const&)> font_feature_values_provider(TreeScopeID) const;

private:
    virtual void visit_edges(Visitor&) override;

    // The one funnel: every change to what a font resolution would answer passes through here.
    void bump_environment_generation();

    void begin_font_face_change_batch();
    void end_font_face_change_batch();
    void clear_computed_font_cache_for_families(Vector<Utf16FlyString> const& family_names);
    using ElementUsesChangedFonts = Function<bool(DOM::Element const&)>;
    void record_font_input_changes(ElementUsesChangedFonts);

    FontFeatureValues const& font_feature_values_for_family(Utf16FlyString const& family_name, TreeScopeID) const;
    FontFeatureValues font_feature_values_in_scope(Utf16FlyString const& family_name, TreeScopeID) const;
    NonnullRefPtr<FontFeatureValuesByScope const> font_feature_values_by_scope() const;

    GC::Ptr<DOM::Document> m_document;

    HashMap<FontFaceKey, Vector<NonnullRefPtr<FontFaceState>>> m_font_faces;
    HashMap<String, GC::Ref<FontLoader>> m_loaders_by_source;

    NonnullRefPtr<FontCascadeMemo> m_font_cascade_memo;
    // NB: Tree scopes are never numbered again, so the entries of a shadow root that is gone answer nothing. They stay
    //     until their family is next forgotten.
    mutable HashMap<FontFeatureValuesCacheKey, FontFeatureValues> m_font_feature_values_cache;
    mutable RefPtr<FontFeatureValuesByScope const> m_font_feature_values_by_scope;

    bool m_has_completed_initial_paint { false };
    bool m_initial_paint_had_pending_fonts { false };
    u32 m_font_face_change_batch_depth { 0 };
    u64 m_environment_generation { 1 };
    mutable RefPtr<FontFaceSnapshot const> m_font_face_snapshot;
    Vector<Utf16FlyString> m_batched_font_face_change_families;
    // What font resolution answers changed beside a style transaction that flew, which computed styles from the old
    // answers: the elements that use the changed fonts are found once the transaction's drain installed them.
    Vector<ElementUsesChangedFonts> m_font_changes_beside_flown_transaction;
};

}
