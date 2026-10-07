/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/SystemFallbackFonts.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/FontFaceState.h>
#include <LibWeb/CSS/FontResolution.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/StyleEngineRustFFI.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

static unsigned font_width_bucket_from_percentage(double percentage)
{
    // Maps a font-width Percentage to the nearest standard Gfx::FontWidth bucket.

    struct Bucket {
        double percentage;
        unsigned width;
    };
    static constexpr Array<Bucket, 9> buckets = { {
        { 50.0, Gfx::FontWidth::UltraCondensed },
        { 62.5, Gfx::FontWidth::ExtraCondensed },
        { 75.0, Gfx::FontWidth::Condensed },
        { 87.5, Gfx::FontWidth::SemiCondensed },
        { 100.0, Gfx::FontWidth::Normal },
        { 112.5, Gfx::FontWidth::SemiExpanded },
        { 125.0, Gfx::FontWidth::Expanded },
        { 150.0, Gfx::FontWidth::ExtraExpanded },
        { 200.0, Gfx::FontWidth::UltraExpanded },
    } };
    auto best = buckets[0];
    auto best_distance = AK::fabs(percentage - best.percentage);
    for (size_t i = 1; i < buckets.size(); ++i) {
        auto distance = AK::fabs(percentage - buckets[i].percentage);
        if (distance < best_distance) {
            best_distance = distance;
            best = buckets[i];
        }
    }
    return best.width;
}

static FlyString font_family_name_for_platform(Utf16FlyString const& family_name)
{
    auto family_name_utf8 = MUST(family_name.view().to_utf8());
    return FlyString::from_utf8_without_validation(family_name_utf8.bytes());
}

#ifdef AK_OS_MACOS
static Optional<Gfx::SystemUIFontKind> macos_system_ui_font_kind_from_family_name(StringView family)
{
    if (family.is_one_of("-apple-system"sv, "-apple-system-font"sv, "-webkit-system-font"sv, "system-ui"sv, "ui-sans-serif"sv))
        return Gfx::SystemUIFontKind::System;
    if (family == "ui-serif"sv)
        return Gfx::SystemUIFontKind::Serif;
    if (family == "ui-monospace"sv)
        return Gfx::SystemUIFontKind::Monospace;
    if (family == "ui-rounded"sv)
        return Gfx::SystemUIFontKind::Rounded;
    return {};
}
#endif

// What one face of the snapshot renders with. A face still waiting on its load becomes a pending entry: a cascade is
// frozen and laid out on whichever thread resolves it, so the entry reads the snapshot and the typeface the face
// publishes, and only selecting it for a rendered code point, on the document thread, reaches the face, by its number
// through the document's registry.
static RefPtr<Gfx::FontCascadeList const> font_for_face(FontFaceSnapshot::Face const& face, float point_size, Gfx::FontVariationSettings const& variations, Gfx::ShapeFeatures const& shape_features)
{
    if (face.is_unusable)
        return {};
    auto font_list = Gfx::FontCascadeList::create();
    if (face.typeface) {
        font_list->add(face.typeface->font(point_size, variations, shape_features), face.unicode_ranges);
    } else if (face.has_urls) {
        font_list->add_pending_face(face.unicode_ranges, [face_id = face.id] {
            if (auto face = FontFaceState::with_id(face_id))
                return face->resolve_for_rendering();
            return Gfx::PendingFontState::Failed; }, [rendering_typeface = face.rendering_typeface, point_size, variations, shape_features]() -> RefPtr<Gfx::Font const> {
            if (auto typeface = rendering_typeface->get())
                return typeface->font(point_size, variations, shape_features);
            return {}; }, [state = face.rendering_state] { return state; });
    }
    if (font_list->is_empty())
        return {};
    return font_list;
}

struct MatchingFontCandidate {
    FontFaceKey key;
    unsigned width { Gfx::FontWidth::Normal };
    Gfx::Typeface const* system_typeface { nullptr };

    [[nodiscard]] RefPtr<Gfx::FontCascadeList const> font_with_point_size(FontFaceSnapshot const& snapshot, float point_size, Gfx::FontVariationSettings const& variations, Gfx::ShapeFeatures const& shape_features) const
    {
        if (system_typeface) {
            auto font_list = Gfx::FontCascadeList::create();
            font_list->add(system_typeface->font(point_size, variations, shape_features));
            return font_list;
        }

        auto const* faces = snapshot.faces(key);
        if (!faces)
            return {};

        auto font_list = Gfx::FontCascadeList::create();
        for (auto const& face : *faces) {
            // https://drafts.csswg.org/css-font-loading/#font-face-load
            // User agents can initiate font loads on their own, whenever they determine that a given font face is
            // necessary to render something on the page. When this happens, they must act as if they had called the
            // corresponding FontFace’s load() method described here.
            // NB: An unloaded face with no subsetting unicode-range starts loading once a style actually selects
            //     it. Loading happens via FontFace::load(), which mutates the face, its FontFaceSets and the
            //     document, so this only leaves the face's number behind; request_wanted_web_faces() loads it
            //     afterwards, and does nothing for a face past "unloaded" already.
            if (face.has_urls && !face.has_non_default_unicode_range)
                note_wanted_web_face(face.id);
            if (auto face_fonts = font_for_face(face, point_size, variations, shape_features)) {
                font_list->extend(*face_fonts);
                continue;
            }
            // Unloaded subset face: surface it as a pending entry so the fetch only
            // fires once font_for_code_point() sees a codepoint in its unicode-range.
            if (face.has_urls && face.has_non_default_unicode_range) {
                font_list->add_pending_face(
                    face.unicode_ranges, [face_id = face.id] {
                        if (auto face = FontFaceState::with_id(face_id))
                            return face->resolve_for_rendering();
                        return Gfx::PendingFontState::Failed; }, {}, [state = face.rendering_state] { return state; });
            }
        }
        if (font_list->is_empty())
            return {};
        return font_list;
    }
};

// Partial implementation of the font-matching algorithm: https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm
// FIXME: This should be replaced by the full CSS font selection algorithm.
static RefPtr<Gfx::FontCascadeList const> font_matching_algorithm(FontFaceSnapshot const& snapshot, Utf16FlyString const& family_name, int weight, Percentage const& font_width, int slope, float font_size_in_pt, Gfx::FontVariationSettings const& variations, Gfx::ShapeFeatures const& shape_features)
{
    // If a font family match occurs, the user agent assembles the set of font faces in that family and then
    // narrows the set to a single face using other font properties in the order given below.
    Vector<MatchingFontCandidate> matching_family_fonts;
    for (auto const& [key, faces] : snapshot.table()) {
        if (key.family_name.equals_ignoring_ascii_case(family_name)) {
            matching_family_fonts.empend(key);
            matching_family_fonts.last().width = font_width_bucket_from_percentage(key.width);
        }
    }
    if (matching_family_fonts.is_empty()) {
        Gfx::FontDatabase::the().for_each_typeface_with_family_name(font_family_name_for_platform(family_name), [&](Gfx::Typeface const& typeface) {
            matching_family_fonts.append({
                .key = {
                    .family_name = Utf16FlyString::from_fly_string(typeface.family()),
                    // FIXME: Support system fonts that have a range of weights, etc.
                    .weight = { static_cast<int>(typeface.weight()), static_cast<int>(typeface.weight()) },
                    .slope = typeface.slope(),
                },
                .width = typeface.width(),
                .system_typeface = &typeface,
            });
        });
    }

    if (matching_family_fonts.is_empty())
        return {};

    Vector<Parser::ValueParserFFI::FfiFontMatchingCandidate> weighed_candidates;
    weighed_candidates.ensure_capacity(matching_family_fonts.size());
    for (auto const& candidate : matching_family_fonts)
        weighed_candidates.unchecked_append({ candidate.key.weight.min, candidate.key.weight.max, candidate.key.slope, candidate.width });
    Vector<size_t> order;
    order.resize(matching_family_fonts.size());
    auto order_length = Parser::ValueParserFFI::rust_font_matching_order(weighed_candidates.data(), weighed_candidates.size(), weight, font_width_bucket_from_percentage(font_width.value()), slope, order.data());
    for (auto index : order.span().trim(order_length)) {
        if (auto found_font = matching_family_fonts[index].font_with_point_size(snapshot, font_size_in_pt, variations, shape_features))
            return found_font;
    }
    return {};
}

static void const* feature_value_data(ComputedFontCacheKey const& key, FontResolutionFeatureInput input)
{
    auto const& value = key.feature_values[to_underlying(input)];
    return value ? value->rust_style_value_data() : nullptr;
}

// The OpenType features a request asks of the fonts of a family, with the family's @font-feature-values, if any.
static Gfx::ShapeFeatures shape_features_for(ComputedFontCacheKey const& key, FontFeatureValues const* family_feature_values)
{
    Array<void const*, font_resolution_feature_input_count> values;
    for (size_t index = 0; index < values.size(); ++index)
        values[index] = feature_value_data(key, static_cast<FontResolutionFeatureInput>(index));
    Gfx::ShapeFeatures features;
    Parser::ValueParserFFI::rust_font_shape_features(
        values.data(), family_feature_values,
        [](void const* family_feature_values, Parser::ValueParserFFI::FontFeatureValuesRuleKind kind, Parser::ValueParserFFI::FfiUtf16View name, size_t* count) -> u32 const* {
            auto values = static_cast<FontFeatureValues const*>(family_feature_values)->get({ kind, Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(name.utf16), name.length }) });
            if (!values.has_value())
                return nullptr;
            *count = values->size();
            return values->data();
        },
        &features,
        [](void* features, u32 tag, u32 value) {
            auto four_cc = Gfx::FourCC::from_u32(tag);
            static_cast<Gfx::ShapeFeatures*>(features)->append({ { four_cc.cc[0], four_cc.cc[1], four_cc.cc[2], four_cc.cc[3] }, value });
        });
    return features;
}

NonnullRefPtr<Gfx::FontCascadeList const> resolve_font_cascade(FontFaceSnapshot const& snapshot, ComputedFontCacheKey const& key, FontFeatureValuesProvider const& font_feature_values_for_family)
{
    auto const& font_families = key.font_families;
    auto const& font_size = key.font_size;
    auto slope = key.font_slope;
    auto font_weight = key.font_weight;
    auto const& font_width = key.font_width;
    auto font_optical_sizing = key.font_optical_sizing;

    // FIXME: We round to int here as that is what is expected by our font infrastructure below
    auto weight = round_to<int>(font_weight);

    // FIXME: We need to respect `font-size-adjust` once that is implemented.
    auto font_size_used_value = font_size.to_float();

    Gfx::FontVariationSettings variation;
    variation.set_weight(font_weight);
    variation.set_width(font_width.value());

    // NB: The spec recommends that we use the 'used value' of font-size for 'opsz' when font-optical-sizing is 'auto'.
    // FIXME: User agents must not select a value for the "opsz" axis which is not supported by the font used for
    //        rendering the text. This can be accomplished by clamping a chosen value to the range supported by the
    //        font. https://drafts.csswg.org/css-fonts/#font-optical-sizing-def
    if (font_optical_sizing == FontOpticalSizing::Auto)
        variation.set_optical_sizing(font_size_used_value);

    Parser::ValueParserFFI::rust_font_variation_axes(feature_value_data(key, FontResolutionFeatureInput::FontVariationSettings), &variation, [](void* variation, u32 tag, double value) {
        static_cast<Gfx::FontVariationSettings*>(variation)->axes.set(Gfx::FourCC::from_u32(tag), value);
    });

    // FIXME: Implement the full font-matching algorithm: https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm
    float const font_size_in_pt = font_size_used_value * 0.75f;

#ifdef AK_OS_MACOS
    auto find_macos_system_ui_font = [&](Gfx::SystemUIFontKind kind, Utf16FlyString const& family) -> RefPtr<Gfx::FontCascadeList const> {
        auto shape_features = shape_features_for(key, &font_feature_values_for_family(family));
        auto typeface = Gfx::TypefaceSkia::match_system_ui(kind, font_size_used_value, weight, font_width_bucket_from_percentage(font_width.value()), slope);
        if (typeface.is_error() || !typeface.value())
            return {};

        auto font_list = Gfx::FontCascadeList::create();
        font_list->add(typeface.value()->font(font_size_in_pt, variation, shape_features));
        return font_list;
    };
#endif

    auto find_font = [&](Utf16FlyString const& family) -> RefPtr<Gfx::FontCascadeList const> {
        auto const shape_features = shape_features_for(key, &font_feature_values_for_family(family));

        // OPTIMIZATION: Look for an exact match in loaded fonts first.
        // FIXME: Respect the other font-* descriptors
        FontFaceKey lookup_key {
            .family_name = family,
            .weight = { weight, weight },
            .slope = slope,
            .width = static_cast<int>(font_width.value()),
        };
        if (auto const* faces = snapshot.faces(lookup_key)) {
            auto result = Gfx::FontCascadeList::create();
            for (auto const& face : *faces) {
                if (auto face_fonts = font_for_face(face, font_size_in_pt, variation, shape_features))
                    result->extend(*face_fonts);
            }
            if (!result->is_empty())
                return result;
        }

#ifdef AK_OS_MACOS
        auto platform_family = font_family_name_for_platform(family);
        if (auto system_ui_font_kind = macos_system_ui_font_kind_from_family_name(platform_family.bytes_as_string_view()); system_ui_font_kind.has_value()) {
            if (auto system_font = find_macos_system_ui_font(system_ui_font_kind.value(), family))
                return system_font;
        }
#endif

        if (auto found_font = font_matching_algorithm(snapshot, family, weight, font_width, slope, font_size_in_pt, variation, shape_features); found_font && !found_font->is_empty())
            return found_font;

        return {};
    };

    auto find_generic_font = [&](GenericFontFamily family) -> RefPtr<Gfx::FontCascadeList const> {
        auto font_id = to_keyword(family);
#ifdef AK_OS_MACOS
        if (auto system_ui_font_kind = macos_system_ui_font_kind_from_family_name(string_from_keyword(font_id)); system_ui_font_kind.has_value()) {
            auto family = utf16_fly_string_from_keyword(font_id);
            if (auto system_font = find_macos_system_ui_font(system_ui_font_kind.value(), family))
                return system_font;
        }
#endif

        Platform::GenericFont generic_font {};
        switch (font_id) {
        case Keyword::Monospace:
            generic_font = Platform::GenericFont::Monospace;
            break;
        case Keyword::UiMonospace:
            generic_font = Platform::GenericFont::UiMonospace;
            break;
        case Keyword::Serif:
            generic_font = Platform::GenericFont::Serif;
            break;
        case Keyword::Fantasy:
            generic_font = Platform::GenericFont::Fantasy;
            break;
        case Keyword::SansSerif:
            generic_font = Platform::GenericFont::SansSerif;
            break;
        case Keyword::UiSerif:
            generic_font = Platform::GenericFont::UiSerif;
            break;
        case Keyword::UiRounded:
            generic_font = Platform::GenericFont::UiRounded;
            break;
        case Keyword::SystemUi:
        case Keyword::UiSansSerif:
            generic_font = Platform::GenericFont::UiSansSerif;
            break;
        case Keyword::Cursive:
            generic_font = Platform::GenericFont::Cursive;
            break;
        default:
            return {};
        }
        return find_font(Utf16FlyString::from_fly_string(Platform::FontPlugin::the().generic_font_name(generic_font)));
    };

    auto font_list = Gfx::FontCascadeList::create();

    for (auto const& family : font_families) {
        RefPtr<Gfx::FontCascadeList const> other_font_list;
        if (family.has<GenericFontFamily>()) {
            other_font_list = find_generic_font(family.get<GenericFontFamily>());
        } else {
            other_font_list = find_font(family.get<ComputedFontFamilyName>().name);
        }

        if (other_font_list)
            font_list->extend(*other_font_list);
    }

    // NB: @font-feature-values can't apply to the default font since it's not loaded from CSS
    auto default_font = Platform::FontPlugin::the().default_font(font_size_in_pt, variation, shape_features_for(key, nullptr));
    if (font_list->is_empty()) {
        if (auto fallback_font_list = find_font(Utf16FlyString::from_fly_string(Platform::FontPlugin::the().generic_font_name(Platform::GenericFont::UiSansSerif))))
            font_list->extend(*fallback_font_list);
    }
    if (font_list->is_empty()) {
        // This is needed to make sure we check default font before reaching to emojis.
        font_list->add(*default_font);
    }

    // The default font is already included in the font list, but we explicitly set it
    // as the last-resort font. This ensures that if none of the specified fonts contain
    // the requested code point, there is still a font available to provide a fallback glyph.
    font_list->set_last_resort_font(*default_font);

    if (Platform::FontPlugin::the().is_layout_test_mode()) {
        for (auto font_name : Platform::FontPlugin::the().symbol_font_names()) {
            if (auto other_font_list = find_font(Utf16FlyString::from_fly_string(font_name)))
                font_list->extend_fallback(*other_font_list);
        }
    } else {
        font_list->set_system_font_fallback_callback([](u32 code_point, Gfx::EmojiPresentation presentation, Gfx::Font const& reference_font) -> RefPtr<Gfx::Font const> {
            Gfx::SystemFallbackFontKey key {
                .code_point = code_point,
                .weight = static_cast<u16>(reference_font.weight()),
                .width = reference_font.typeface().width(),
                .slope = static_cast<u8>(reference_font.slope()),
                .prefer_color_emoji = presentation == Gfx::EmojiPresentation::Emoji,
            };
            return Gfx::system_fallback_font(key, reference_font.point_size());
        });
    }

    // The cascade is complete. Freeze it here, on the document thread, so that every layout pass that receives it
    // reads a snapshot instead of the live list.
    font_list->freeze();

    return font_list;
}

Vector<ComputedFontFamily> computed_font_families_from_style_value(StyleValue const& font_family)
{
    Vector<ComputedFontFamily> font_families;
    auto const& values = font_family.as_value_list().values();
    font_families.ensure_capacity(values.size());
    for (auto const& value : values) {
        if (value->is_keyword()) {
            auto generic_family = keyword_to_generic_font_family(value->to_keyword());
            VERIFY(generic_family.has_value());
            font_families.unchecked_append(generic_family.release_value());
        } else {
            font_families.unchecked_append(ComputedFontFamilyName {
                .name = string_from_style_value(value),
                .syntax = value->is_string() ? ComputedFontFamilySyntax::String : ComputedFontFamilySyntax::CustomIdent,
            });
        }
    }
    return font_families;
}

NonnullRefPtr<Gfx::FontCascadeList const> resolve_font_for_style_values(FontComputer const& font_computer, ComputedFontCacheKey key)
{
    // Only font-variant-alternates read the tree scope's @font-feature-values, so every other request resolves once for
    // all scopes.
    if (!key.feature_values[to_underlying(FontResolutionFeatureInput::FontVariantAlternates)])
        key.font_feature_values_scope = {};
    auto font_list = font_computer.font_cascade_memo().resolve(font_computer.font_face_snapshot(), key, font_computer.font_feature_values_provider(key.font_feature_values_scope));
    // Inside a style update the loads wait for its end; everywhere else, such as canvas, they happen right here.
    request_wanted_web_faces();
    return font_list;
}

NonnullRefPtr<Gfx::FontCascadeList const> FontCascadeMemo::resolve(FontFaceSnapshot const& snapshot, ComputedFontCacheKey const& key, FontFeatureValuesProvider const& font_feature_values_for_family) const
{
    MutexLocker locker(m_mutex);
    // A style transaction that flew resolves against the snapshot it was sealed with, which a change to the
    // @font-face table beside it leaves older than the memo: its answers are its own, and stay out of the memo, retired
    // with the cascades the change forgot.
    if (snapshot.generation() < m_generation) {
        auto font_list = resolve_font_cascade(snapshot, key, font_feature_values_for_family);
        m_retired.append(font_list);
        m_resolutions_against_older_tables.append({ key, font_list });
        return font_list;
    }
    m_generation = snapshot.generation();
    return m_cascades.ensure(key, [&] {
        return resolve_font_cascade(snapshot, key, font_feature_values_for_family);
    });
}

NonnullRefPtr<Gfx::FontCascadeList const> FontCascadeMemo::resolve_for_fork(FontFaceSnapshot const& snapshot, ComputedFontCacheKey const& key, FontFeatureValuesProvider const& font_feature_values_for_family) const
{
    MutexLocker locker(m_mutex);
    if (snapshot.generation() < m_generation)
        return resolve_font_cascade(snapshot, key, font_feature_values_for_family);
    m_generation = snapshot.generation();
    return m_cascades.ensure(key, [&] {
        return resolve_font_cascade(snapshot, key, font_feature_values_for_family);
    });
}

void FontCascadeMemo::forget_matching(u64 environment_generation, Function<bool(ComputedFontCacheKey const&, NonnullRefPtr<Gfx::FontCascadeList const> const&)> const& predicate)
{
    MutexLocker locker(m_mutex);
    VERIFY(environment_generation > m_generation);
    m_generation = environment_generation;
    m_cascades.remove_all_matching([&](auto const& key, auto const& font_list) {
        if (!predicate(key, font_list))
            return false;
        m_retired.append(font_list);
        return true;
    });
}

void FontCascadeMemo::release_retired() const
{
    // NB: A cascade's destructor releases fonts into the document thread's caches, so it runs outside the lock.
    Vector<NonnullRefPtr<Gfx::FontCascadeList const>> retired;
    Vector<ResolutionAgainstOlderTable> resolutions_against_older_tables;
    {
        MutexLocker locker(m_mutex);
        retired = move(m_retired);
        resolutions_against_older_tables = move(m_resolutions_against_older_tables);
    }
}

Vector<FontCascadeMemo::ResolutionAgainstOlderTable> FontCascadeMemo::take_resolutions_against_older_tables() const
{
    MutexLocker locker(m_mutex);
    return exchange(m_resolutions_against_older_tables, {});
}

}

namespace Web::CSS {

enum class ResolvedFor : u8 {
    Engine,
    Fork,
};

static StyleEngineFFI::FfiResolvedFont resolve_font_for_style_engine(void const* memo, void const* snapshot, StyleEngineFFI::FfiFontResolutionRequest request, ResolvedFor resolved_for)
{
    auto value_of = [](StyleEngineFFI::FfiHostHandle handle) {
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(reinterpret_cast<StyleValueFFI::StyleValueData const*>(handle)));
    };
    auto font_family = value_of(request.font_family);
    ComputedFontCacheKey key {
        .font_families = computed_font_families_from_style_value(*font_family),
        .font_optical_sizing = static_cast<FontOpticalSizing>(request.font_optical_sizing),
        .font_size = CSSPixels::from_raw(request.font_size_raw),
        .font_slope = request.font_slope,
        .font_weight = request.font_weight,
        .font_width = Percentage(request.font_width),
        .feature_values = {},
        .font_feature_values_scope = TreeScopeID { request.font_feature_values_scope },
    };
    // The engine names a value for each feature input whose property does not have its initial value.
    for (size_t index = 0; index < key.feature_values.size(); ++index) {
        if (auto handle = request.font_feature_values[index])
            key.feature_values[index] = value_of(handle);
    }
    auto const& font_faces = *static_cast<FontFaceSnapshot const*>(snapshot);
    auto const& cascade_memo = *static_cast<FontCascadeMemo const*>(memo);
    FontFeatureValuesProvider font_feature_values_for_family = [&](Utf16FlyString const& family) -> FontFeatureValues const& { return font_faces.font_feature_values(key.font_feature_values_scope, family); };
    auto font_list = resolved_for == ResolvedFor::Fork
        ? cascade_memo.resolve_for_fork(font_faces, key, font_feature_values_for_family)
        : cascade_memo.resolve(font_faces, key, font_feature_values_for_family);
    // The metric probe must not load a face: the first available font answers without one.
    auto const& first_available_font = font_list->first_available_font();
    auto const metrics = first_available_font.pixel_metrics();
    // NB: The engine takes no reference. The memo keeps the cascade alive until the font environment generation
    //     changes, and the engine's resolver cache answers only for the generation it was filled at. A fork of the
    //     render state, which sees no generation change, takes the reference it is handed instead.
    if (resolved_for == ResolvedFor::Fork)
        font_list->ref();
    return {
        // Handles, not pointers: the engine names these host objects and hands them back here.
        .first_available_font = reinterpret_cast<StyleEngineFFI::FfiHostHandle>(&first_available_font),
        .font_cascade_list = reinterpret_cast<StyleEngineFFI::FfiHostHandle>(font_list.ptr()),
        .ascent = metrics.ascent,
        .descent = metrics.descent,
        .x_height = metrics.x_height,
        .zero_advance = metrics.advance_of_ascii_zero,
    };
}

}

// The style engine resolves a font through this, with nothing but the table and memo it was given and the request: the
// request's family is an opaque handle the engine holds, which becomes a value again here.
extern "C" Web::CSS::StyleEngineFFI::FfiResolvedFont web_css_resolve_font(void const* memo, void const* snapshot, Web::CSS::StyleEngineFFI::FfiFontResolutionRequest request)
{
    return Web::CSS::resolve_font_for_style_engine(memo, snapshot, request, Web::CSS::ResolvedFor::Engine);
}

// A fork of the render state resolves a font through this, as the engine does, taking over a reference to the cascade.
extern "C" WEB_API Web::CSS::StyleEngineFFI::FfiResolvedFont web_css_resolve_font_for_fork(void const* memo, void const* snapshot, Web::CSS::StyleEngineFFI::FfiFontResolutionRequest request);

extern "C" Web::CSS::StyleEngineFFI::FfiResolvedFont web_css_resolve_font_for_fork(void const* memo, void const* snapshot, Web::CSS::StyleEngineFFI::FfiFontResolutionRequest request)
{
    return Web::CSS::resolve_font_for_style_engine(memo, snapshot, request, Web::CSS::ResolvedFor::Fork);
}

// A fork of the render state takes its own reference to the font objects the style engine holds.
extern "C" WEB_API void web_css_font_face_snapshot_reference(void const*);
extern "C" WEB_API void web_css_font_cascade_memo_reference(void const*);

extern "C" void web_css_font_face_snapshot_reference(void const* snapshot)
{
    static_cast<Web::CSS::FontFaceSnapshot const*>(snapshot)->ref();
}

extern "C" void web_css_font_cascade_memo_reference(void const* memo)
{
    static_cast<Web::CSS::FontCascadeMemo const*>(memo)->ref();
}

extern "C" void web_css_font_face_snapshot_unreference(void const* snapshot)
{
    static_cast<Web::CSS::FontFaceSnapshot const*>(snapshot)->unref();
}

extern "C" void web_css_font_cascade_memo_unreference(void const* memo)
{
    static_cast<Web::CSS::FontCascadeMemo const*>(memo)->unref();
}
