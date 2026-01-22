/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FontComputer.h"
#include <AK/NonnullRawPtr.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/WOFF/Loader.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/MimeSniff/Resource.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(FontComputer);
GC_DEFINE_ALLOCATOR(FontLoader);

struct FontFaceKey {
    NonnullRawPtr<FlyString const> family_name;
    int weight { 0 };
    int slope { 0 };
};

}

namespace AK {

namespace Detail {

template<>
inline constexpr bool IsHashCompatible<Web::CSS::FontFaceKey, Web::CSS::OwnFontFaceKey> = true;
template<>
inline constexpr bool IsHashCompatible<Web::CSS::OwnFontFaceKey, Web::CSS::FontFaceKey> = true;

}

template<>
struct Traits<Web::CSS::FontFaceKey> : public DefaultTraits<Web::CSS::FontFaceKey> {
    static unsigned hash(Web::CSS::FontFaceKey const& key) { return pair_int_hash(key.family_name->hash(), pair_int_hash(key.weight, key.slope)); }
};

template<>
struct Traits<Web::CSS::OwnFontFaceKey> : public DefaultTraits<Web::CSS::OwnFontFaceKey> {
    static unsigned hash(Web::CSS::OwnFontFaceKey const& key) { return pair_int_hash(key.family_name.hash(), pair_int_hash(key.weight, key.slope)); }
};

template<>
struct Traits<Web::CSS::ComputedFontCacheKey> : public DefaultTraits<Web::CSS::ComputedFontCacheKey> {
    static unsigned hash(Web::CSS::ComputedFontCacheKey const& key)
    {
        unsigned hash = 0;
        for (auto const& family_value : key.font_family->as_value_list().values()) {
            if (family_value->is_keyword())
                hash = pair_int_hash(hash, to_underlying(family_value->as_keyword().keyword()));
            else if (family_value->is_string())
                hash = pair_int_hash(hash, family_value->as_string().string_value().hash());
            else if (family_value->is_custom_ident())
                hash = pair_int_hash(hash, family_value->as_custom_ident().custom_ident().hash());
            else
                VERIFY_NOT_REACHED();
        }

        hash = pair_int_hash(hash, Traits<Web::CSSPixels>::hash(key.font_size));
        hash = pair_int_hash(hash, key.font_slope);
        hash = pair_int_hash(hash, Traits<double>::hash(key.font_weight));
        hash = pair_int_hash(hash, Traits<double>::hash(key.font_width.value()));
        for (auto const& [variation_name, variation_value] : key.font_variation_settings)
            hash = pair_int_hash(hash, pair_int_hash(variation_name.hash(), Traits<double>::hash(variation_value)));

        return hash;
    }
};

}

namespace Web::CSS {

OwnFontFaceKey::OwnFontFaceKey(FontFaceKey const& other)
    : family_name(other.family_name)
    , weight(other.weight)
    , slope(other.slope)
{
}

OwnFontFaceKey::operator FontFaceKey() const
{
    return FontFaceKey {
        family_name,
        weight,
        slope
    };
}

[[nodiscard]] bool OwnFontFaceKey::operator==(FontFaceKey const& other) const
{
    return family_name == other.family_name
        && weight == other.weight
        && slope == other.slope;
}

FontLoader::FontLoader(FontComputer& font_computer, RuleOrDeclaration rule_or_declaration, FlyString family_name, Vector<Gfx::UnicodeRange> unicode_ranges, Vector<URL> urls, GC::Ptr<GC::Function<void(RefPtr<Gfx::Typeface const>)>> on_load)
    : m_font_computer(font_computer)
    , m_rule_or_declaration(rule_or_declaration)
    , m_family_name(move(family_name))
    , m_unicode_ranges(move(unicode_ranges))
    , m_urls(move(urls))
    , m_on_load(move(on_load))
{
}

FontLoader::~FontLoader() = default;

void FontLoader::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_font_computer);
    if (auto* rule = m_rule_or_declaration.value.get_pointer<RuleOrDeclaration::Rule>())
        visitor.visit(rule->parent_style_sheet);
    else if (auto* block = m_rule_or_declaration.value.get_pointer<RuleOrDeclaration::StyleDeclaration>())
        visitor.visit(block->parent_rule);
    visitor.visit(m_fetch_controller);
    visitor.visit(m_on_load);
}

bool FontLoader::is_loading() const
{
    return m_fetch_controller && !m_typeface;
}

RefPtr<Gfx::Font const> FontLoader::font_with_point_size(float point_size, Gfx::FontVariationSettings const& variations)
{
    if (!m_typeface) {
        if (!m_fetch_controller)
            start_loading_next_url();
        return nullptr;
    }
    return m_typeface->font(point_size, variations);
}

void FontLoader::start_loading_next_url()
{
    // FIXME: Load local() fonts somehow.
    if (m_fetch_controller && m_fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Ongoing)
        return;
    if (m_urls.is_empty())
        return;

    // https://drafts.csswg.org/css-fonts-4/#fetch-a-font
    // To fetch a font given a selected <url> url for @font-face rule, fetch url, with ruleOrDeclaration being rule,
    // destination "font", CORS mode "cors", and processResponse being the following steps given response res and null,
    // failure or a byte stream stream:
    m_fetch_controller = fetch_a_style_resource(m_urls.take_first(), m_rule_or_declaration, Fetch::Infrastructure::Request::Destination::Font, CorsMode::Cors,
        [loader = this](auto response, auto stream) {
            // 1. If stream is null, return.
            // 2. Load a font from stream according to its type.

            // NB: We need to fetch the next source if this one fails to fetch OR decode. So, first try to decode it.
            RefPtr<Gfx::Typeface const> typeface;
            if (auto* bytes = stream.template get_pointer<ByteBuffer>()) {
                if (auto maybe_typeface = loader->try_load_font(response, *bytes); !maybe_typeface.is_error())
                    typeface = maybe_typeface.release_value();
            }

            if (!typeface) {
                // NB: If we have other sources available, try the next one.
                if (loader->m_urls.is_empty()) {
                    loader->font_did_load_or_fail(nullptr);
                } else {
                    loader->m_fetch_controller = nullptr;
                    loader->start_loading_next_url();
                }
            } else {
                loader->font_did_load_or_fail(move(typeface));
            }
        });

    if (!m_fetch_controller)
        font_did_load_or_fail(nullptr);
}

void FontLoader::font_did_load_or_fail(RefPtr<Gfx::Typeface const> typeface)
{
    if (typeface) {
        m_typeface = typeface.release_nonnull();
        m_font_computer->did_load_font(m_family_name);
        if (m_on_load)
            m_on_load->function()(m_typeface);
    } else {
        if (m_on_load)
            m_on_load->function()(nullptr);
    }
    m_fetch_controller = nullptr;
}

ErrorOr<NonnullRefPtr<Gfx::Typeface const>> FontLoader::try_load_font(Fetch::Infrastructure::Response const& response, ByteBuffer const& bytes)
{
    // FIXME: This could maybe use the format() provided in @font-face as well, since often the mime type is just application/octet-stream and we have to try every format
    auto mime_type = Fetch::Infrastructure::extract_mime_type(response.header_list());
    if (!mime_type.has_value() || !mime_type->is_font()) {
        mime_type = MimeSniff::Resource::sniff(bytes, MimeSniff::SniffingConfiguration { .sniffing_context = MimeSniff::SniffingContext::Font });
    }
    if (mime_type.has_value()) {
        if (mime_type->essence() == "font/ttf"sv || mime_type->essence() == "application/x-font-ttf"sv || mime_type->essence() == "font/otf"sv) {
            if (auto result = Gfx::Typeface::try_load_from_temporary_memory(bytes); !result.is_error()) {
                return result;
            }
        }
        if (mime_type->essence() == "font/woff"sv || mime_type->essence() == "application/font-woff"sv) {
            if (auto result = WOFF::try_load_from_bytes(bytes); !result.is_error()) {
                return result;
            }
        }
        if (mime_type->essence() == "font/woff2"sv || mime_type->essence() == "application/font-woff2"sv) {
            if (auto result = WOFF2::try_load_from_bytes(bytes); !result.is_error()) {
                return result;
            }
        }
    }

    return Error::from_string_literal("Automatic format detection failed");
}

struct FontComputer::MatchingFontCandidate {
    FontFaceKey key;
    Variant<FontLoaderList*, Gfx::Typeface const*> loader_or_typeface;

    [[nodiscard]] RefPtr<Gfx::FontCascadeList const> font_with_point_size(float point_size, Gfx::FontVariationSettings const& variations) const
    {
        auto font_list = Gfx::FontCascadeList::create();
        if (auto const* loader_list = loader_or_typeface.get_pointer<FontLoaderList*>(); loader_list) {
            for (auto const& loader : **loader_list) {
                if (auto font = loader->font_with_point_size(point_size, variations); font)
                    font_list->add(*font, loader->unicode_ranges());
            }
            return font_list;
        }

        font_list->add(loader_or_typeface.get<Gfx::Typeface const*>()->font(point_size, variations));
        return font_list;
    }
};

void FontComputer::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_loaded_fonts);
}

RefPtr<Gfx::FontCascadeList const> FontComputer::find_matching_font_weight_ascending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, Gfx::FontVariationSettings const& variations, bool inclusive)
{
    using Fn = AK::Function<bool(MatchingFontCandidate const&)>;
    auto pred = inclusive ? Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight >= target_weight; })
                          : Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight > target_weight; });
    auto it = find_if(candidates.begin(), candidates.end(), pred);
    for (; it != candidates.end(); ++it) {
        if (auto found_font = it->font_with_point_size(font_size_in_pt, variations))
            return found_font;
    }
    return {};
}

RefPtr<Gfx::FontCascadeList const> FontComputer::find_matching_font_weight_descending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, Gfx::FontVariationSettings const& variations, bool inclusive)
{
    using Fn = AK::Function<bool(MatchingFontCandidate const&)>;
    auto pred = inclusive ? Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight <= target_weight; })
                          : Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight < target_weight; });
    auto it = find_if(candidates.rbegin(), candidates.rend(), pred);
    for (; it != candidates.rend(); ++it) {
        if (auto found_font = it->font_with_point_size(font_size_in_pt, variations))
            return found_font;
    }
    return {};
}

// Partial implementation of the font-matching algorithm: https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm
// FIXME: This should be replaced by the full CSS font selection algorithm.
RefPtr<Gfx::FontCascadeList const> FontComputer::font_matching_algorithm(FlyString const& family_name, int weight, int slope, float font_size_in_pt, Gfx::FontVariationSettings const& variations) const
{
    // If a font family match occurs, the user agent assembles the set of font faces in that family and then
    // narrows the set to a single face using other font properties in the order given below.
    Vector<MatchingFontCandidate> matching_family_fonts;
    for (auto const& font_key_and_loader : m_loaded_fonts) {
        if (font_key_and_loader.key.family_name.equals_ignoring_ascii_case(family_name))
            matching_family_fonts.empend(font_key_and_loader.key, const_cast<FontLoaderList*>(&font_key_and_loader.value));
    }
    Gfx::FontDatabase::the().for_each_typeface_with_family_name(family_name, [&](Gfx::Typeface const& typeface) {
        matching_family_fonts.empend(
            FontFaceKey {
                .family_name = typeface.family(),
                .weight = static_cast<int>(typeface.weight()),
                .slope = typeface.slope(),
            },
            &typeface);
    });

    if (matching_family_fonts.is_empty())
        return {};

    quick_sort(matching_family_fonts, [](auto const& a, auto const& b) {
        return a.key.weight < b.key.weight;
    });
    // FIXME: 1. font-stretch is tried first.
    // FIXME: 2. font-style is tried next.
    // We don't have complete support of italic and oblique fonts, so matching on font-style can be simplified to:
    // If a matching slope is found, all faces which don't have that matching slope are excluded from the matching set.
    auto style_it = find_if(matching_family_fonts.begin(), matching_family_fonts.end(),
        [&](auto const& matching_font_candidate) { return matching_font_candidate.key.slope == slope; });
    if (style_it != matching_family_fonts.end()) {
        matching_family_fonts.remove_all_matching([&](auto const& matching_font_candidate) {
            return matching_font_candidate.key.slope != slope;
        });
    }
    // 3. font-weight is matched next.
    // If the desired weight is inclusively between 400 and 500, weights greater than or equal to the target weight
    // are checked in ascending order until 500 is hit and checked, followed by weights less than the target weight
    // in descending order, followed by weights greater than 500, until a match is found.

    if (weight >= 400 && weight <= 500) {
        auto it = find_if(matching_family_fonts.begin(), matching_family_fonts.end(),
            [&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight >= weight; });
        for (; it != matching_family_fonts.end() && it->key.weight <= 500; ++it) {
            if (auto found_font = it->font_with_point_size(font_size_in_pt, variations))
                return found_font;
        }
        if (auto found_font = find_matching_font_weight_descending(matching_family_fonts, weight, font_size_in_pt, variations, false))
            return found_font;
        for (; it != matching_family_fonts.end(); ++it) {
            if (auto found_font = it->font_with_point_size(font_size_in_pt, variations))
                return found_font;
        }
    }
    // If the desired weight is less than 400, weights less than or equal to the desired weight are checked in descending order
    // followed by weights above the desired weight in ascending order until a match is found.
    if (weight < 400) {
        if (auto found_font = find_matching_font_weight_descending(matching_family_fonts, weight, font_size_in_pt, variations, true))
            return found_font;
        if (auto found_font = find_matching_font_weight_ascending(matching_family_fonts, weight, font_size_in_pt, variations, false))
            return found_font;
    }
    // If the desired weight is greater than 500, weights greater than or equal to the desired weight are checked in ascending order
    // followed by weights below the desired weight in descending order until a match is found.
    if (weight > 500) {
        if (auto found_font = find_matching_font_weight_ascending(matching_family_fonts, weight, font_size_in_pt, variations, true))
            return found_font;
        if (auto found_font = find_matching_font_weight_descending(matching_family_fonts, weight, font_size_in_pt, variations, false))
            return found_font;
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<Gfx::FontCascadeList const> FontComputer::compute_font_for_style_values(StyleValue const& font_family, CSSPixels const& font_size, int font_slope, double font_weight, Percentage const& font_width, HashMap<FlyString, double> const& font_variation_settings) const
{
    ComputedFontCacheKey cache_key {
        .font_family = font_family,
        .font_size = font_size,
        .font_slope = font_slope,
        .font_weight = font_weight,
        .font_width = font_width,
        .font_variation_settings = font_variation_settings,
    };

    return m_computed_font_cache.ensure(cache_key, [&]() {
        return compute_font_for_style_values_impl(font_family, font_size, font_slope, font_weight, font_width, font_variation_settings);
    });
}

NonnullRefPtr<Gfx::FontCascadeList const> FontComputer::compute_font_for_style_values_impl(StyleValue const& font_family, CSSPixels const& font_size, int slope, double font_weight, Percentage const& font_width, HashMap<FlyString, double> const& font_variation_settings) const
{
    // FIXME: We round to int here as that is what is expected by our font infrastructure below
    auto weight = round_to<int>(font_weight);

    Gfx::FontVariationSettings variation;
    variation.set_weight(font_weight);
    variation.set_width(font_width.value());

    for (auto const& [tag_string, value] : font_variation_settings) {
        auto string_view = tag_string.bytes_as_string_view();
        if (string_view.length() != 4)
            continue;

        auto tag = Gfx::FourCC(string_view.characters_without_null_termination());

        variation.axes.set(tag, value);
    }

    // FIXME: Implement the full font-matching algorithm: https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm

    float const font_size_in_pt = font_size * 0.75f;

    auto find_font = [&](FlyString const& family) -> RefPtr<Gfx::FontCascadeList const> {
        // OPTIMIZATION: Look for an exact match in loaded fonts first.
        // FIXME: Respect the other font-* descriptors
        FontFaceKey key {
            .family_name = family,
            .weight = weight,
            .slope = slope,
        };
        if (auto it = m_loaded_fonts.find(key); it != m_loaded_fonts.end()) {
            auto result = Gfx::FontCascadeList::create();
            auto const& loaders = it->value;
            for (auto const& loader : loaders) {
                if (auto found_font = loader->font_with_point_size(font_size_in_pt, variation))
                    result->add(*found_font, loader->unicode_ranges());
            }

            return result;
        }

        if (auto found_font = font_matching_algorithm(family, weight, slope, font_size_in_pt, variation); found_font && !found_font->is_empty())
            return found_font;

        return {};
    };

    auto find_generic_font = [&](Keyword font_id) -> RefPtr<Gfx::FontCascadeList const> {
        Platform::GenericFont generic_font {};
        switch (font_id) {
        case Keyword::Monospace:
        case Keyword::UiMonospace:
            generic_font = Platform::GenericFont::Monospace;
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
        case Keyword::Cursive:
            generic_font = Platform::GenericFont::Cursive;
            break;
        case Keyword::UiSerif:
            generic_font = Platform::GenericFont::UiSerif;
            break;
        case Keyword::UiSansSerif:
            generic_font = Platform::GenericFont::UiSansSerif;
            break;
        case Keyword::UiRounded:
            generic_font = Platform::GenericFont::UiRounded;
            break;
        default:
            return {};
        }
        return find_font(Platform::FontPlugin::the().generic_font_name(generic_font, weight, slope));
    };

    auto font_list = Gfx::FontCascadeList::create();

    for (auto const& family : font_family.as_value_list().values()) {
        RefPtr<Gfx::FontCascadeList const> other_font_list;
        if (family->is_keyword()) {
            other_font_list = find_generic_font(family->to_keyword());
        } else if (family->is_string()) {
            other_font_list = find_font(family->as_string().string_value());
        } else if (family->is_custom_ident()) {
            other_font_list = find_font(family->as_custom_ident().custom_ident());
        }

        if (other_font_list)
            font_list->extend(*other_font_list);
    }

    auto default_font = Platform::FontPlugin::the().default_font(font_size_in_pt);
    if (font_list->is_empty()) {
        // This is needed to make sure we check default font before reaching to emojis.
        font_list->add(*default_font);
    }

    // Add emoji and symbol fonts
    for (auto font_name : Platform::FontPlugin::the().symbol_font_names()) {
        if (auto other_font_list = find_font(font_name)) {
            font_list->extend(*other_font_list);
        }
    }

    // The default font is already included in the font list, but we explicitly set it
    // as the last-resort font. This ensures that if none of the specified fonts contain
    // the requested code point, there is still a font available to provide a fallback glyph.
    font_list->set_last_resort_font(*default_font);

    if (!Platform::FontPlugin::the().is_layout_test_mode()) {
        font_list->set_system_font_fallback_callback([](u32 code_point, Gfx::Font const& reference_font) -> RefPtr<Gfx::Font const> {
            return Gfx::FontDatabase::the().get_font_for_code_point(
                code_point,
                reference_font.point_size(),
                reference_font.weight(),
                reference_font.typeface().width(),
                reference_font.slope());
        });
    }

    return font_list;
}

Gfx::Font const& FontComputer::initial_font() const
{
    // FIXME: This is not correct.
    static auto font = ComputedProperties::font_fallback(false, false, 12);
    return font;
}

void FontComputer::did_load_font(FlyString const&)
{
    m_computed_font_cache = {};
    document().invalidate_style(DOM::StyleInvalidationReason::CSSFontLoaded);
}

GC::Ptr<FontLoader> FontComputer::load_font_face(ParsedFontFace const& font_face, GC::Ptr<GC::Function<void(RefPtr<Gfx::Typeface const>)>> on_load)
{
    if (font_face.sources().is_empty()) {
        if (on_load)
            on_load->function()({});
        return {};
    }

    FontFaceKey key {
        .family_name = font_face.font_family(),
        .weight = font_face.weight().value_or(0),
        .slope = font_face.slope().value_or(0),
    };

    // FIXME: Pass the sources directly, so the font loader can make use of the format information, or load local fonts.
    Vector<URL> urls;
    for (auto const& source : font_face.sources()) {
        if (source.local_or_url.has<URL>())
            urls.append(source.local_or_url.get<URL>());
        // FIXME: Handle local()
    }

    if (urls.is_empty()) {
        if (on_load)
            on_load->function()({});
        return {};
    }

    RuleOrDeclaration rule_or_declaration {
        .environment_settings_object = document().relevant_settings_object(),
        .value = RuleOrDeclaration::Rule {
            .parent_style_sheet = font_face.parent_rule()->parent_style_sheet(),
        }
    };

    auto loader = heap().allocate<FontLoader>(*this, rule_or_declaration, font_face.font_family(), font_face.unicode_ranges(), move(urls), move(on_load));
    auto& loader_ref = *loader;
    auto maybe_font_loaders_list = m_loaded_fonts.get(key);
    if (maybe_font_loaders_list.has_value()) {
        maybe_font_loaders_list->append(move(loader));
    } else {
        FontLoaderList loaders;
        loaders.append(loader);
        m_loaded_fonts.set(OwnFontFaceKey(key), move(loaders));
    }
    // Actual object owned by font loader list inside m_loaded_fonts, this isn't use-after-move/free
    return loader_ref;
}

void FontComputer::load_fonts_from_sheet(CSSStyleSheet& sheet)
{
    for (auto const& rule : sheet.rules()) {
        auto* font_face_rule = as_if<CSSFontFaceRule>(*rule);
        if (!font_face_rule)
            continue;
        if (!font_face_rule->is_valid())
            continue;
        if (auto font_loader = load_font_face(font_face_rule->font_face())) {
            sheet.add_associated_font_loader(*font_loader);
        }

        auto font_face = FontFace::create_css_connected(document().realm(), *font_face_rule);
        document().fonts()->add_css_connected_font(font_face);
    }
}

void FontComputer::unload_fonts_from_sheet(CSSStyleSheet& sheet)
{
    for (auto& [_, font_loader_list] : m_loaded_fonts) {
        font_loader_list.remove_all_matching([&](auto& font_loader) {
            return sheet.has_associated_font_loader(*font_loader);
        });
    }

    // https://drafts.csswg.org/css-font-loading/#font-face-css-connection
    // If a @font-face rule is removed from the document, its connected FontFace object is no longer CSS-connected.
    for (auto const& rule : sheet.rules()) {
        if (auto* font_face_rule = as_if<CSSFontFaceRule>(*rule))
            font_face_rule->disconnect_font_face();
    }
}

size_t FontComputer::number_of_css_font_faces_with_loading_in_progress() const
{
    size_t count = 0;
    for (auto const& [_, loaders] : m_loaded_fonts) {
        for (auto const& loader : loaders) {
            if (loader->is_loading())
                ++count;
        }
    }
    return count;
}

}
