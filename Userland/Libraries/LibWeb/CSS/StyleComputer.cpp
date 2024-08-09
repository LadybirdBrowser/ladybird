/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/Find.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <AK/TemporaryChange.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/FontStyleMapping.h>
#include <LibGfx/Font/OpenType/Typeface.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/WOFF/Loader.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheet.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IdentifierStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransitionStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnsetStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/ReferrerPolicy/AbstractOperations.h>
#include <math.h>
#include <stdio.h>

namespace AK {

// traits for FontFaceKey
template<>
struct Traits<Web::CSS::FontFaceKey> : public DefaultTraits<Web::CSS::FontFaceKey> {
    static unsigned hash(Web::CSS::FontFaceKey const& key) { return pair_int_hash(key.family_name.hash(), pair_int_hash(key.weight, key.slope)); }
};

}

namespace Web::CSS {

static DOM::Element const* element_to_inherit_style_from(DOM::Element const*, Optional<CSS::Selector::PseudoElement::Type>);

StyleComputer::StyleComputer(DOM::Document& document)
    : m_document(document)
    , m_default_font_metrics(16, Platform::FontPlugin::the().default_font().pixel_metrics())
    , m_root_element_font_metrics(m_default_font_metrics)
{
}

StyleComputer::~StyleComputer() = default;

FontLoader::FontLoader(StyleComputer& style_computer, FlyString family_name, Vector<Gfx::UnicodeRange> unicode_ranges, Vector<URL::URL> urls, Function<void(FontLoader const&)> on_load, Function<void()> on_fail)
    : m_style_computer(style_computer)
    , m_family_name(move(family_name))
    , m_unicode_ranges(move(unicode_ranges))
    , m_urls(move(urls))
    , m_on_load(move(on_load))
    , m_on_fail(move(on_fail))
{
}

FontLoader::~FontLoader() = default;

void FontLoader::resource_did_load()
{
    auto result = try_load_font();
    if (result.is_error()) {
        dbgln("Failed to parse font: {}", result.error());
        start_loading_next_url();
        return;
    }
    m_vector_font = result.release_value();
    m_style_computer.did_load_font(m_family_name);
    if (m_on_load)
        m_on_load(*this);
}

void FontLoader::resource_did_fail()
{
    if (m_on_fail) {
        m_on_fail();
    }
}

RefPtr<Gfx::Font> FontLoader::font_with_point_size(float point_size)
{
    if (!m_vector_font) {
        start_loading_next_url();
        return nullptr;
    }
    return m_vector_font->scaled_font(point_size);
}

void FontLoader::start_loading_next_url()
{
    if (resource() && resource()->is_pending())
        return;
    if (m_urls.is_empty())
        return;
    LoadRequest request;
    request.set_url(m_urls.take_first());

    // HACK: We're crudely computing the referer value and shoving it into the
    //       request until fetch infrastructure is used here.
    auto referrer_url = ReferrerPolicy::strip_url_for_use_as_referrer(m_style_computer.document().url());
    if (referrer_url.has_value() && !request.headers().contains("Referer"))
        request.set_header("Referer", referrer_url->serialize());

    set_resource(ResourceLoader::the().load_resource(Resource::Type::Generic, request));
}

ErrorOr<NonnullRefPtr<Gfx::Typeface>> FontLoader::try_load_font()
{
    // FIXME: This could maybe use the format() provided in @font-face as well, since often the mime type is just application/octet-stream and we have to try every format
    auto const& mime_type = resource()->mime_type();
    if (mime_type == "font/ttf"sv || mime_type == "application/x-font-ttf"sv) {
        if (auto result = OpenType::Typeface::try_load_from_externally_owned_memory(resource()->encoded_data()); !result.is_error()) {
            return result;
        }
    }
    if (mime_type == "font/woff"sv || mime_type == "application/font-woff"sv) {
        if (auto result = WOFF::try_load_from_externally_owned_memory(resource()->encoded_data()); !result.is_error()) {
            return result;
        }
    }
    if (mime_type == "font/woff2"sv || mime_type == "application/font-woff2"sv) {
        if (auto result = WOFF2::try_load_from_externally_owned_memory(resource()->encoded_data()); !result.is_error()) {
            return result;
        }
    }

    // We don't have the luxury of knowing the MIME type, so we have to try all formats.
    auto ttf = OpenType::Typeface::try_load_from_externally_owned_memory(resource()->encoded_data());
    if (!ttf.is_error())
        return ttf.release_value();
    auto woff = WOFF::try_load_from_externally_owned_memory(resource()->encoded_data());
    if (!woff.is_error())
        return woff.release_value();
    auto woff2 = WOFF2::try_load_from_externally_owned_memory(resource()->encoded_data());
    if (!woff2.is_error())
        return woff2.release_value();
    return Error::from_string_literal("Automatic format detection failed");
}

struct StyleComputer::MatchingFontCandidate {
    FontFaceKey key;
    Variant<FontLoaderList*, Gfx::Typeface const*> loader_or_typeface;

    [[nodiscard]] RefPtr<Gfx::FontCascadeList const> font_with_point_size(float point_size) const
    {
        RefPtr<Gfx::FontCascadeList> font_list = Gfx::FontCascadeList::create();
        if (auto* loader_list = loader_or_typeface.get_pointer<FontLoaderList*>(); loader_list) {
            for (auto const& loader : **loader_list) {
                if (auto font = loader->font_with_point_size(point_size); font)
                    font_list->add(*font, loader->unicode_ranges());
            }
            return font_list;
        }

        font_list->add(loader_or_typeface.get<Gfx::Typeface const*>()->scaled_font(point_size));
        return font_list;
    }
};

static CSSStyleSheet& default_stylesheet(DOM::Document const& document)
{
    static JS::Handle<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern StringView default_stylesheet_source;
        sheet = JS::make_handle(parse_css_stylesheet(CSS::Parser::ParsingContext(document), default_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& quirks_mode_stylesheet(DOM::Document const& document)
{
    static JS::Handle<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern StringView quirks_mode_stylesheet_source;
        sheet = JS::make_handle(parse_css_stylesheet(CSS::Parser::ParsingContext(document), quirks_mode_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& mathml_stylesheet(DOM::Document const& document)
{
    static JS::Handle<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern StringView mathml_stylesheet_source;
        sheet = JS::make_handle(parse_css_stylesheet(CSS::Parser::ParsingContext(document), mathml_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& svg_stylesheet(DOM::Document const& document)
{
    static JS::Handle<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern StringView svg_stylesheet_source;
        sheet = JS::make_handle(parse_css_stylesheet(CSS::Parser::ParsingContext(document), svg_stylesheet_source));
    }
    return *sheet;
}

template<typename Callback>
void StyleComputer::for_each_stylesheet(CascadeOrigin cascade_origin, Callback callback) const
{
    if (cascade_origin == CascadeOrigin::UserAgent) {
        callback(default_stylesheet(document()), {});
        if (document().in_quirks_mode())
            callback(quirks_mode_stylesheet(document()), {});
        callback(mathml_stylesheet(document()), {});
        callback(svg_stylesheet(document()), {});
    }
    if (cascade_origin == CascadeOrigin::User) {
        if (m_user_style_sheet)
            callback(*m_user_style_sheet, {});
    }
    if (cascade_origin == CascadeOrigin::Author) {
        document().for_each_css_style_sheet([&](CSSStyleSheet& sheet) {
            callback(sheet, {});
        });

        const_cast<DOM::Document&>(document()).for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) {
            shadow_root.for_each_css_style_sheet([&](CSSStyleSheet& sheet) {
                callback(sheet, &shadow_root);
            });
        });
    }
}

StyleComputer::RuleCache const& StyleComputer::rule_cache_for_cascade_origin(CascadeOrigin cascade_origin) const
{
    switch (cascade_origin) {
    case CascadeOrigin::Author:
        return *m_author_rule_cache;
    case CascadeOrigin::User:
        return *m_user_rule_cache;
    case CascadeOrigin::UserAgent:
        return *m_user_agent_rule_cache;
    default:
        TODO();
    }
}

[[nodiscard]] static bool filter_namespace_rule(DOM::Element const& element, MatchingRule const& rule)
{
    // FIXME: Filter out non-default namespace using prefixes
    if (auto namespace_rule = rule.sheet->default_namespace_rule()) {
        if (namespace_rule->namespace_uri() != element.namespace_uri())
            return false;
    }
    return true;
}

bool StyleComputer::should_reject_with_ancestor_filter(Selector const& selector) const
{
    for (u32 hash : selector.ancestor_hashes()) {
        if (hash == 0)
            break;
        if (!m_ancestor_filter.may_contain(hash))
            return true;
    }
    return false;
}

Vector<MatchingRule> StyleComputer::collect_matching_rules(DOM::Element const& element, CascadeOrigin cascade_origin, Optional<CSS::Selector::PseudoElement::Type> pseudo_element) const
{
    auto const& root_node = element.root();
    auto shadow_root = is<DOM::ShadowRoot>(root_node) ? static_cast<DOM::ShadowRoot const*>(&root_node) : nullptr;

    JS::GCPtr<DOM::Element const> shadow_host;
    if (element.is_shadow_host())
        shadow_host = element;
    else if (shadow_root)
        shadow_host = shadow_root->host();

    auto const& rule_cache = rule_cache_for_cascade_origin(cascade_origin);

    Vector<MatchingRule, 512> rules_to_run;
    auto add_rules_to_run = [&](Vector<MatchingRule> const& rules) {
        rules_to_run.grow_capacity(rules_to_run.size() + rules.size());
        if (pseudo_element.has_value()) {
            for (auto const& rule : rules) {
                if (rule.contains_pseudo_element && filter_namespace_rule(element, rule))
                    rules_to_run.unchecked_append(rule);
            }
        } else {
            for (auto const& rule : rules) {
                if (!rule.contains_pseudo_element && filter_namespace_rule(element, rule))
                    rules_to_run.unchecked_append(rule);
            }
        }
    };

    for (auto const& class_name : element.class_names()) {
        if (auto it = rule_cache.rules_by_class.find(class_name); it != rule_cache.rules_by_class.end())
            add_rules_to_run(it->value);
    }
    if (auto id = element.id(); id.has_value()) {
        if (auto it = rule_cache.rules_by_id.find(id.value()); it != rule_cache.rules_by_id.end())
            add_rules_to_run(it->value);
    }
    if (auto it = rule_cache.rules_by_tag_name.find(element.local_name()); it != rule_cache.rules_by_tag_name.end())
        add_rules_to_run(it->value);
    if (pseudo_element.has_value())
        add_rules_to_run(rule_cache.pseudo_element_rules);
    if (element.is_document_element())
        add_rules_to_run(rule_cache.root_rules);

    element.for_each_attribute([&](auto& name, auto&) {
        if (auto it = rule_cache.rules_by_attribute_name.find(name); it != rule_cache.rules_by_attribute_name.end()) {
            add_rules_to_run(it->value);
        }
    });

    add_rules_to_run(rule_cache.other_rules);

    Vector<MatchingRule> matching_rules;
    matching_rules.ensure_capacity(rules_to_run.size());
    for (auto const& rule_to_run : rules_to_run) {
        // FIXME: This needs to be revised when adding support for the ::shadow selector, as it needs to cross shadow boundaries.
        auto rule_root = rule_to_run.shadow_root;
        auto from_user_agent_or_user_stylesheet = rule_to_run.cascade_origin == CascadeOrigin::UserAgent || rule_to_run.cascade_origin == CascadeOrigin::User;

        // NOTE: Inside shadow trees, we only match rules that are defined in the shadow tree's style sheets.
        //       The key exception is the shadow tree's *shadow host*, which needs to match :host rules from inside the shadow root.
        //       Also note that UA or User style sheets don't have a scope, so they are always relevant.
        // FIXME: We should reorganize the data so that the document-level StyleComputer doesn't cache *all* rules,
        //        but instead we'd have some kind of "style scope" at the document level, and also one for each shadow root.
        //        Then we could only evaluate rules from the current style scope.
        bool rule_is_relevant_for_current_scope = rule_root == shadow_root
            || (element.is_shadow_host() && rule_root == element.shadow_root())
            || from_user_agent_or_user_stylesheet;

        if (!rule_is_relevant_for_current_scope)
            continue;

        // NOTE: When matching an element against a rule from outside the shadow root's style scope,
        //       we have to pass in null for the shadow host, otherwise combinator traversal will
        //       be confined to the element itself (since it refuses to cross the shadow boundary).
        auto shadow_host_to_use = shadow_host;
        if (element.is_shadow_host() && rule_root != element.shadow_root())
            shadow_host_to_use = nullptr;

        auto const& selector = rule_to_run.rule->selectors()[rule_to_run.selector_index];

        if (should_reject_with_ancestor_filter(*selector))
            continue;

        if (rule_to_run.can_use_fast_matches) {
            if (!SelectorEngine::fast_matches(selector, *rule_to_run.sheet, element, shadow_host_to_use))
                continue;
        } else {
            if (!SelectorEngine::matches(selector, *rule_to_run.sheet, element, shadow_host_to_use, pseudo_element))
                continue;
        }
        matching_rules.append(rule_to_run);
    }
    return matching_rules;
}

static void sort_matching_rules(Vector<MatchingRule>& matching_rules)
{
    quick_sort(matching_rules, [&](MatchingRule& a, MatchingRule& b) {
        auto const& a_selector = a.rule->selectors()[a.selector_index];
        auto const& b_selector = b.rule->selectors()[b.selector_index];
        auto a_specificity = a_selector->specificity();
        auto b_specificity = b_selector->specificity();
        if (a_specificity == b_specificity) {
            if (a.style_sheet_index == b.style_sheet_index)
                return a.rule_index < b.rule_index;
            return a.style_sheet_index < b.style_sheet_index;
        }
        return a_specificity < b_specificity;
    });
}

void StyleComputer::for_each_property_expanding_shorthands(PropertyID property_id, StyleValue const& value, AllowUnresolved allow_unresolved, Function<void(PropertyID, StyleValue const&)> const& set_longhand_property)
{
    auto map_logical_property_to_real_property = [](PropertyID property_id) -> Optional<PropertyID> {
        // FIXME: Honor writing-mode, direction and text-orientation.
        switch (property_id) {
        case PropertyID::MarginBlockStart:
            return PropertyID::MarginTop;
        case PropertyID::MarginBlockEnd:
            return PropertyID::MarginBottom;
        case PropertyID::MarginInlineStart:
            return PropertyID::MarginLeft;
        case PropertyID::MarginInlineEnd:
            return PropertyID::MarginRight;
        case PropertyID::PaddingBlockStart:
            return PropertyID::PaddingTop;
        case PropertyID::PaddingBlockEnd:
            return PropertyID::PaddingBottom;
        case PropertyID::PaddingInlineStart:
            return PropertyID::PaddingLeft;
        case PropertyID::PaddingInlineEnd:
            return PropertyID::PaddingRight;
        case PropertyID::InlineSize:
            return PropertyID::Width;
        case PropertyID::InsetBlockStart:
            return PropertyID::Top;
        case PropertyID::InsetBlockEnd:
            return PropertyID::Bottom;
        case PropertyID::InsetInlineStart:
            return PropertyID::Left;
        case PropertyID::InsetInlineEnd:
            return PropertyID::Right;
        case PropertyID::WebkitAlignContent:
            return PropertyID::AlignContent;
        case PropertyID::WebkitAlignItems:
            return PropertyID::AlignItems;
        case PropertyID::WebkitAlignSelf:
            return PropertyID::AlignSelf;
        case PropertyID::WebkitAnimation:
            return PropertyID::Animation;
        case PropertyID::WebkitAnimationDelay:
            return PropertyID::AnimationDelay;
        case PropertyID::WebkitAnimationDirection:
            return PropertyID::AnimationDirection;
        case PropertyID::WebkitAnimationDuration:
            return PropertyID::AnimationDuration;
        case PropertyID::WebkitAnimationFillMode:
            return PropertyID::AnimationFillMode;
        case PropertyID::WebkitAnimationIterationCount:
            return PropertyID::AnimationIterationCount;
        case PropertyID::WebkitAnimationName:
            return PropertyID::AnimationName;
        case PropertyID::WebkitAnimationPlayState:
            return PropertyID::AnimationPlayState;
        case PropertyID::WebkitAnimationTimingFunction:
            return PropertyID::AnimationTimingFunction;
        case PropertyID::WebkitAppearance:
            return PropertyID::Appearance;
        case PropertyID::WebkitBackgroundClip:
            return PropertyID::BackgroundClip;
        case PropertyID::WebkitBackgroundOrigin:
            return PropertyID::BackgroundOrigin;
        case PropertyID::WebkitBorderBottomLeftRadius:
            return PropertyID::BorderBottomLeftRadius;
        case PropertyID::WebkitBorderBottomRightRadius:
            return PropertyID::BorderBottomRightRadius;
        case PropertyID::WebkitBorderRadius:
            return PropertyID::BorderRadius;
        case PropertyID::WebkitBorderTopLeftRadius:
            return PropertyID::BorderTopLeftRadius;
        case PropertyID::WebkitBorderTopRightRadius:
            return PropertyID::BorderTopRightRadius;
        case PropertyID::WebkitBoxShadow:
            return PropertyID::BoxShadow;
        case PropertyID::WebkitBoxSizing:
            return PropertyID::BoxSizing;
        case PropertyID::WebkitFlex:
            return PropertyID::Flex;
        case PropertyID::WebkitFlexBasis:
            return PropertyID::FlexBasis;
        case PropertyID::WebkitFlexDirection:
            return PropertyID::FlexDirection;
        case PropertyID::WebkitFlexFlow:
            return PropertyID::FlexFlow;
        case PropertyID::WebkitFlexWrap:
            return PropertyID::FlexWrap;
        case PropertyID::WebkitJustifyContent:
            return PropertyID::JustifyContent;
        case PropertyID::WebkitMask:
            return PropertyID::Mask;
        case PropertyID::WebkitOrder:
            return PropertyID::Order;
        case PropertyID::WebkitTransform:
            return PropertyID::Transform;
        case PropertyID::WebkitTransformOrigin:
            return PropertyID::TransformOrigin;
        case PropertyID::WebkitTransition:
            return PropertyID::Transition;
        case PropertyID::WebkitTransitionDelay:
            return PropertyID::TransitionDelay;
        case PropertyID::WebkitTransitionDuration:
            return PropertyID::TransitionDuration;
        case PropertyID::WebkitTransitionProperty:
            return PropertyID::TransitionProperty;
        case PropertyID::WebkitTransitionTimingFunction:
            return PropertyID::TransitionTimingFunction;
        default:
            return {};
        }
    };

    struct StartAndEndPropertyIDs {
        PropertyID start;
        PropertyID end;
    };
    auto map_logical_property_to_real_properties = [](PropertyID property_id) -> Optional<StartAndEndPropertyIDs> {
        // FIXME: Honor writing-mode, direction and text-orientation.
        switch (property_id) {
        case PropertyID::MarginBlock:
            return StartAndEndPropertyIDs { PropertyID::MarginTop, PropertyID::MarginBottom };
        case PropertyID::MarginInline:
            return StartAndEndPropertyIDs { PropertyID::MarginLeft, PropertyID::MarginRight };
        case PropertyID::PaddingBlock:
            return StartAndEndPropertyIDs { PropertyID::PaddingTop, PropertyID::PaddingBottom };
        case PropertyID::PaddingInline:
            return StartAndEndPropertyIDs { PropertyID::PaddingLeft, PropertyID::PaddingRight };
        case PropertyID::InsetBlock:
            return StartAndEndPropertyIDs { PropertyID::Top, PropertyID::Bottom };
        case PropertyID::InsetInline:
            return StartAndEndPropertyIDs { PropertyID::Left, PropertyID::Right };
        default:
            return {};
        }
    };

    if (auto real_property_id = map_logical_property_to_real_property(property_id); real_property_id.has_value()) {
        for_each_property_expanding_shorthands(real_property_id.value(), value, allow_unresolved, set_longhand_property);
        return;
    }

    if (auto real_property_ids = map_logical_property_to_real_properties(property_id); real_property_ids.has_value()) {
        if (value.is_value_list() && value.as_value_list().size() == 2) {
            auto const& start = value.as_value_list().values()[0];
            auto const& end = value.as_value_list().values()[1];
            for_each_property_expanding_shorthands(real_property_ids->start, start, allow_unresolved, set_longhand_property);
            for_each_property_expanding_shorthands(real_property_ids->end, end, allow_unresolved, set_longhand_property);
            return;
        }
        for_each_property_expanding_shorthands(real_property_ids->start, value, allow_unresolved, set_longhand_property);
        for_each_property_expanding_shorthands(real_property_ids->end, value, allow_unresolved, set_longhand_property);
        return;
    }

    if (value.is_shorthand()) {
        auto& shorthand_value = value.as_shorthand();
        auto& properties = shorthand_value.sub_properties();
        auto& values = shorthand_value.values();
        for (size_t i = 0; i < properties.size(); ++i)
            for_each_property_expanding_shorthands(properties[i], values[i], allow_unresolved, set_longhand_property);
        return;
    }

    auto assign_edge_values = [&](PropertyID top_property, PropertyID right_property, PropertyID bottom_property, PropertyID left_property, auto const& values) {
        if (values.size() == 4) {
            set_longhand_property(top_property, values[0]);
            set_longhand_property(right_property, values[1]);
            set_longhand_property(bottom_property, values[2]);
            set_longhand_property(left_property, values[3]);
        } else if (values.size() == 3) {
            set_longhand_property(top_property, values[0]);
            set_longhand_property(right_property, values[1]);
            set_longhand_property(bottom_property, values[2]);
            set_longhand_property(left_property, values[1]);
        } else if (values.size() == 2) {
            set_longhand_property(top_property, values[0]);
            set_longhand_property(right_property, values[1]);
            set_longhand_property(bottom_property, values[0]);
            set_longhand_property(left_property, values[1]);
        } else if (values.size() == 1) {
            set_longhand_property(top_property, values[0]);
            set_longhand_property(right_property, values[0]);
            set_longhand_property(bottom_property, values[0]);
            set_longhand_property(left_property, values[0]);
        }
    };

    if (property_id == CSS::PropertyID::Border) {
        for_each_property_expanding_shorthands(CSS::PropertyID::BorderTop, value, allow_unresolved, set_longhand_property);
        for_each_property_expanding_shorthands(CSS::PropertyID::BorderRight, value, allow_unresolved, set_longhand_property);
        for_each_property_expanding_shorthands(CSS::PropertyID::BorderBottom, value, allow_unresolved, set_longhand_property);
        for_each_property_expanding_shorthands(CSS::PropertyID::BorderLeft, value, allow_unresolved, set_longhand_property);
        // FIXME: Also reset border-image, in line with the spec: https://www.w3.org/TR/css-backgrounds-3/#border-shorthands
        return;
    }

    if (property_id == CSS::PropertyID::BorderStyle) {
        if (value.is_value_list()) {
            auto const& values_list = value.as_value_list();
            assign_edge_values(PropertyID::BorderTopStyle, PropertyID::BorderRightStyle, PropertyID::BorderBottomStyle, PropertyID::BorderLeftStyle, values_list.values());
            return;
        }

        set_longhand_property(CSS::PropertyID::BorderTopStyle, value);
        set_longhand_property(CSS::PropertyID::BorderRightStyle, value);
        set_longhand_property(CSS::PropertyID::BorderBottomStyle, value);
        set_longhand_property(CSS::PropertyID::BorderLeftStyle, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderWidth) {
        if (value.is_value_list()) {
            auto const& values_list = value.as_value_list();
            assign_edge_values(PropertyID::BorderTopWidth, PropertyID::BorderRightWidth, PropertyID::BorderBottomWidth, PropertyID::BorderLeftWidth, values_list.values());
            return;
        }

        set_longhand_property(CSS::PropertyID::BorderTopWidth, value);
        set_longhand_property(CSS::PropertyID::BorderRightWidth, value);
        set_longhand_property(CSS::PropertyID::BorderBottomWidth, value);
        set_longhand_property(CSS::PropertyID::BorderLeftWidth, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderColor) {
        if (value.is_value_list()) {
            auto const& values_list = value.as_value_list();
            assign_edge_values(PropertyID::BorderTopColor, PropertyID::BorderRightColor, PropertyID::BorderBottomColor, PropertyID::BorderLeftColor, values_list.values());
            return;
        }

        set_longhand_property(CSS::PropertyID::BorderTopColor, value);
        set_longhand_property(CSS::PropertyID::BorderRightColor, value);
        set_longhand_property(CSS::PropertyID::BorderBottomColor, value);
        set_longhand_property(CSS::PropertyID::BorderLeftColor, value);
        return;
    }

    if (property_id == CSS::PropertyID::BackgroundPosition) {
        if (value.is_position()) {
            auto const& position = value.as_position();
            set_longhand_property(CSS::PropertyID::BackgroundPositionX, position.edge_x());
            set_longhand_property(CSS::PropertyID::BackgroundPositionY, position.edge_y());
        } else if (value.is_value_list()) {
            // Expand background-position layer list into separate lists for x and y positions:
            auto const& values_list = value.as_value_list();
            StyleValueVector x_positions {};
            StyleValueVector y_positions {};
            x_positions.ensure_capacity(values_list.size());
            y_positions.ensure_capacity(values_list.size());
            for (auto& layer : values_list.values()) {
                if (layer->is_position()) {
                    auto const& position = layer->as_position();
                    x_positions.unchecked_append(position.edge_x());
                    y_positions.unchecked_append(position.edge_y());
                } else {
                    x_positions.unchecked_append(layer);
                    y_positions.unchecked_append(layer);
                }
            }
            set_longhand_property(CSS::PropertyID::BackgroundPositionX, StyleValueList::create(move(x_positions), values_list.separator()));
            set_longhand_property(CSS::PropertyID::BackgroundPositionY, StyleValueList::create(move(y_positions), values_list.separator()));
        } else {
            set_longhand_property(CSS::PropertyID::BackgroundPositionX, value);
            set_longhand_property(CSS::PropertyID::BackgroundPositionY, value);
        }

        return;
    }

    if (property_id == CSS::PropertyID::Inset) {
        if (value.is_value_list()) {
            auto const& values_list = value.as_value_list();
            assign_edge_values(PropertyID::Top, PropertyID::Right, PropertyID::Bottom, PropertyID::Left, values_list.values());
            return;
        }

        set_longhand_property(CSS::PropertyID::Top, value);
        set_longhand_property(CSS::PropertyID::Right, value);
        set_longhand_property(CSS::PropertyID::Bottom, value);
        set_longhand_property(CSS::PropertyID::Left, value);
        return;
    }

    if (property_id == CSS::PropertyID::Margin) {
        if (value.is_value_list()) {
            auto const& values_list = value.as_value_list();
            assign_edge_values(PropertyID::MarginTop, PropertyID::MarginRight, PropertyID::MarginBottom, PropertyID::MarginLeft, values_list.values());
            return;
        }

        set_longhand_property(CSS::PropertyID::MarginTop, value);
        set_longhand_property(CSS::PropertyID::MarginRight, value);
        set_longhand_property(CSS::PropertyID::MarginBottom, value);
        set_longhand_property(CSS::PropertyID::MarginLeft, value);
        return;
    }

    if (property_id == CSS::PropertyID::Padding) {
        if (value.is_value_list()) {
            auto const& values_list = value.as_value_list();
            assign_edge_values(PropertyID::PaddingTop, PropertyID::PaddingRight, PropertyID::PaddingBottom, PropertyID::PaddingLeft, values_list.values());
            return;
        }

        set_longhand_property(CSS::PropertyID::PaddingTop, value);
        set_longhand_property(CSS::PropertyID::PaddingRight, value);
        set_longhand_property(CSS::PropertyID::PaddingBottom, value);
        set_longhand_property(CSS::PropertyID::PaddingLeft, value);
        return;
    }

    if (property_id == CSS::PropertyID::Gap || property_id == CSS::PropertyID::GridGap) {
        if (value.is_value_list()) {
            auto const& values_list = value.as_value_list();
            set_longhand_property(CSS::PropertyID::RowGap, values_list.values()[0]);
            set_longhand_property(CSS::PropertyID::ColumnGap, values_list.values()[1]);
            return;
        }
        set_longhand_property(CSS::PropertyID::RowGap, value);
        set_longhand_property(CSS::PropertyID::ColumnGap, value);
        return;
    }

    if (property_id == CSS::PropertyID::RowGap || property_id == CSS::PropertyID::GridRowGap) {
        set_longhand_property(CSS::PropertyID::RowGap, value);
        return;
    }

    if (property_id == CSS::PropertyID::ColumnGap || property_id == CSS::PropertyID::GridColumnGap) {
        set_longhand_property(CSS::PropertyID::ColumnGap, value);
        return;
    }

    if (property_id == CSS::PropertyID::MaxInlineSize || property_id == CSS::PropertyID::MinInlineSize) {
        // FIXME: Use writing-mode to determine if we should set width or height.
        bool is_horizontal = true;

        if (is_horizontal) {
            if (property_id == CSS::PropertyID::MaxInlineSize) {
                set_longhand_property(CSS::PropertyID::MaxWidth, value);
            } else {
                set_longhand_property(CSS::PropertyID::MinWidth, value);
            }
        } else {
            if (property_id == CSS::PropertyID::MaxInlineSize) {
                set_longhand_property(CSS::PropertyID::MaxHeight, value);
            } else {
                set_longhand_property(CSS::PropertyID::MinHeight, value);
            }
        }
        return;
    }

    if (property_id == CSS::PropertyID::Transition) {
        if (!value.is_transition()) {
            // Handle `none` as a shorthand for `all 0s ease 0s`.
            set_longhand_property(CSS::PropertyID::TransitionProperty, IdentifierStyleValue::create(CSS::ValueID::All));
            set_longhand_property(CSS::PropertyID::TransitionDuration, TimeStyleValue::create(CSS::Time::make_seconds(0)));
            set_longhand_property(CSS::PropertyID::TransitionDelay, TimeStyleValue::create(CSS::Time::make_seconds(0)));
            set_longhand_property(CSS::PropertyID::TransitionTimingFunction, IdentifierStyleValue::create(CSS::ValueID::Ease));
            return;
        }
        auto const& transitions = value.as_transition().transitions();
        Array<Vector<ValueComparingNonnullRefPtr<StyleValue const>>, 4> transition_values;
        for (auto const& transition : transitions) {
            transition_values[0].append(*transition.property_name);
            transition_values[1].append(transition.duration.as_style_value());
            transition_values[2].append(transition.delay.as_style_value());
            if (transition.easing)
                transition_values[3].append(*transition.easing);
        }

        set_longhand_property(CSS::PropertyID::TransitionProperty, StyleValueList::create(move(transition_values[0]), StyleValueList::Separator::Comma));
        set_longhand_property(CSS::PropertyID::TransitionDuration, StyleValueList::create(move(transition_values[1]), StyleValueList::Separator::Comma));
        set_longhand_property(CSS::PropertyID::TransitionDelay, StyleValueList::create(move(transition_values[2]), StyleValueList::Separator::Comma));
        set_longhand_property(CSS::PropertyID::TransitionTimingFunction, StyleValueList::create(move(transition_values[3]), StyleValueList::Separator::Comma));
        return;
    }

    if (property_id == CSS::PropertyID::Float) {
        auto ident = value.to_identifier();

        // FIXME: Honor writing-mode, direction and text-orientation.
        if (ident == CSS::ValueID::InlineStart) {
            set_longhand_property(CSS::PropertyID::Float, IdentifierStyleValue::create(CSS::ValueID::Left));
            return;
        } else if (ident == CSS::ValueID::InlineEnd) {
            set_longhand_property(CSS::PropertyID::Float, IdentifierStyleValue::create(CSS::ValueID::Right));
            return;
        }
    }

    if (property_is_shorthand(property_id)) {
        // ShorthandStyleValue was handled already.
        // That means if we got here, that `value` must be a CSS-wide keyword, which we should apply to our longhand properties.
        // We don't directly call `set_longhand_property()` because the longhands might have longhands of their own.
        // (eg `grid` -> `grid-template` -> `grid-template-areas` & `grid-template-rows` & `grid-template-columns`)
        // Forget this requirement if we're ignoring unresolved values and the value is unresolved.
        VERIFY(value.is_css_wide_keyword() || (allow_unresolved == AllowUnresolved::Yes && value.is_unresolved()));
        for (auto longhand : longhands_for_shorthand(property_id))
            for_each_property_expanding_shorthands(longhand, value, allow_unresolved, set_longhand_property);
        return;
    }

    set_longhand_property(property_id, value);
}

void StyleComputer::set_property_expanding_shorthands(StyleProperties& style, CSS::PropertyID property_id, StyleValue const& value, CSS::CSSStyleDeclaration const* declaration, StyleProperties const& style_for_revert, Important important)
{
    for_each_property_expanding_shorthands(property_id, value, AllowUnresolved::No, [&](PropertyID shorthand_id, StyleValue const& shorthand_value) {
        if (shorthand_value.is_revert()) {
            auto const& property_in_previous_cascade_origin = style_for_revert.m_property_values[to_underlying(shorthand_id)];
            if (property_in_previous_cascade_origin) {
                style.set_property(shorthand_id, *property_in_previous_cascade_origin, StyleProperties::Inherited::No, important);
                if (shorthand_id == CSS::PropertyID::AnimationName)
                    style.set_animation_name_source(style_for_revert.animation_name_source());
            }
        } else {
            style.set_property(shorthand_id, shorthand_value, StyleProperties::Inherited::No, important);
            if (shorthand_id == CSS::PropertyID::AnimationName)
                style.set_animation_name_source(declaration);
        }
    });
}

void StyleComputer::set_all_properties(DOM::Element& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, StyleProperties& style, StyleValue const& value, DOM::Document& document, CSS::CSSStyleDeclaration const* declaration, StyleProperties const& style_for_revert, Important important) const
{
    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        auto property_id = (CSS::PropertyID)i;

        if (value.is_revert()) {
            style.revert_property(property_id, style_for_revert);
            continue;
        }

        if (value.is_unset()) {
            if (is_inherited_property(property_id)) {
                style.set_property(
                    property_id,
                    get_inherit_value(document.realm(), property_id, &element, pseudo_element),
                    StyleProperties::Inherited::Yes,
                    important);
            } else {
                style.set_property(
                    property_id,
                    property_initial_value(document.realm(), property_id),
                    StyleProperties::Inherited::No,
                    important);
            }
            continue;
        }

        NonnullRefPtr<StyleValue> property_value = value;
        if (property_value->is_unresolved())
            property_value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingContext { document }, element, pseudo_element, property_id, property_value->as_unresolved());
        if (!property_value->is_unresolved())
            set_property_expanding_shorthands(style, property_id, property_value, declaration, style_for_revert);

        style.set_property_important(property_id, important);

        set_property_expanding_shorthands(style, property_id, value, declaration, style_for_revert, important);
    }
}

void StyleComputer::cascade_declarations(StyleProperties& style, DOM::Element& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, Vector<MatchingRule> const& matching_rules, CascadeOrigin cascade_origin, Important important) const
{
    auto style_for_revert = style.clone();

    for (auto const& match : matching_rules) {
        for (auto const& property : match.rule->declaration().properties()) {
            if (important != property.important)
                continue;

            if (property.property_id == CSS::PropertyID::All) {
                set_all_properties(element, pseudo_element, style, property.value, m_document, &match.rule->declaration(), style_for_revert, important);
                continue;
            }

            auto property_value = property.value;
            if (property.value->is_unresolved())
                property_value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingContext { document() }, element, pseudo_element, property.property_id, property.value->as_unresolved());
            if (!property_value->is_unresolved())
                set_property_expanding_shorthands(style, property.property_id, property_value, &match.rule->declaration(), style_for_revert, important);
        }
    }

    if (cascade_origin == CascadeOrigin::Author && !pseudo_element.has_value()) {
        if (auto const inline_style = element.inline_style()) {
            for (auto const& property : inline_style->properties()) {
                if (important != property.important)
                    continue;

                if (property.property_id == CSS::PropertyID::All) {
                    set_all_properties(element, pseudo_element, style, property.value, m_document, inline_style, style_for_revert, important);
                    continue;
                }

                auto property_value = property.value;
                if (property.value->is_unresolved())
                    property_value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingContext { document() }, element, pseudo_element, property.property_id, property.value->as_unresolved());
                if (!property_value->is_unresolved())
                    set_property_expanding_shorthands(style, property.property_id, property_value, inline_style, style_for_revert, important);
            }
        }
    }
}

static void cascade_custom_properties(DOM::Element& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, Vector<MatchingRule> const& matching_rules)
{
    size_t needed_capacity = 0;
    for (auto const& matching_rule : matching_rules)
        needed_capacity += matching_rule.rule->declaration().custom_properties().size();

    if (!pseudo_element.has_value()) {
        if (auto const inline_style = element.inline_style())
            needed_capacity += inline_style->custom_properties().size();
    }

    HashMap<FlyString, StyleProperty> custom_properties;
    custom_properties.ensure_capacity(needed_capacity);

    for (auto const& matching_rule : matching_rules) {
        for (auto const& it : matching_rule.rule->declaration().custom_properties())
            custom_properties.set(it.key, it.value);
    }

    if (!pseudo_element.has_value()) {
        if (auto const inline_style = element.inline_style()) {
            for (auto const& it : inline_style->custom_properties())
                custom_properties.set(it.key, it.value);
        }
    }

    element.set_custom_properties(pseudo_element, move(custom_properties));
}

static NonnullRefPtr<StyleValue const> interpolate_value(DOM::Element& element, StyleValue const& from, StyleValue const& to, float delta);

template<typename T>
static T interpolate_raw(T from, T to, float delta)
{
    if constexpr (IsSame<T, double>) {
        return from + (to - from) * static_cast<double>(delta);
    } else {
        return static_cast<RemoveCVReference<T>>(from + (to - from) * delta);
    }
}

// A null return value means the interpolated matrix was not invertible or otherwise invalid
static RefPtr<StyleValue const> interpolate_transform(DOM::Element& element, StyleValue const& from, StyleValue const& to, float delta)
{
    // Note that the spec uses column-major notation, so all the matrix indexing is reversed.

    static constexpr auto make_transformation = [](TransformationStyleValue const& transformation) -> Optional<Transformation> {
        Vector<TransformValue> values;

        for (auto const& value : transformation.values()) {
            switch (value->type()) {
            case StyleValue::Type::Angle:
                values.append(AngleOrCalculated { value->as_angle().angle() });
                break;
            case StyleValue::Type::Calculated:
                values.append(LengthPercentage { value->as_calculated() });
                break;
            case StyleValue::Type::Length:
                values.append(LengthPercentage { value->as_length().length() });
                break;
            case StyleValue::Type::Percentage:
                values.append(LengthPercentage { value->as_percentage().percentage() });
                break;
            case StyleValue::Type::Number:
                values.append(NumberPercentage { Number(Number::Type::Number, value->as_number().number()) });
                break;
            default:
                return {};
            }
        }

        return Transformation { transformation.transform_function(), move(values) };
    };

    static constexpr auto transformation_style_value_to_matrix = [](DOM::Element& element, TransformationStyleValue const& value) -> Optional<FloatMatrix4x4> {
        auto transformation = make_transformation(value.as_transformation());
        if (!transformation.has_value())
            return {};
        Optional<Painting::PaintableBox const&> paintable_box;
        if (auto layout_node = element.layout_node()) {
            if (auto paintable = layout_node->paintable(); paintable && is<Painting::PaintableBox>(paintable))
                paintable_box = *static_cast<Painting::PaintableBox*>(paintable);
        }
        if (auto matrix = transformation->to_matrix(paintable_box); !matrix.is_error())
            return matrix.value();
        return {};
    };

    static constexpr auto style_value_to_matrix = [](DOM::Element& element, StyleValue const& value) -> FloatMatrix4x4 {
        if (value.is_transformation())
            return transformation_style_value_to_matrix(element, value.as_transformation()).value_or(FloatMatrix4x4::identity());

        // This encompasses both the allowed value "none" and any invalid values
        if (!value.is_value_list())
            return FloatMatrix4x4::identity();

        auto matrix = FloatMatrix4x4::identity();
        for (auto const& value_element : value.as_value_list().values()) {
            if (value_element->is_transformation()) {
                if (auto value_matrix = transformation_style_value_to_matrix(element, value_element->as_transformation()); value_matrix.has_value())
                    matrix = matrix * value_matrix.value();
            }
        }

        return matrix;
    };

    struct DecomposedValues {
        FloatVector3 translation;
        FloatVector3 scale;
        FloatVector3 skew;
        FloatVector4 rotation;
        FloatVector4 perspective;
    };
    // https://drafts.csswg.org/css-transforms-2/#decomposing-a-3d-matrix
    static constexpr auto decompose = [](FloatMatrix4x4 matrix) -> Optional<DecomposedValues> {
        // https://drafts.csswg.org/css-transforms-1/#supporting-functions
        static constexpr auto combine = [](auto a, auto b, float ascl, float bscl) {
            return FloatVector3 {
                ascl * a[0] + bscl * b[0],
                ascl * a[1] + bscl * b[1],
                ascl * a[2] + bscl * b[2],
            };
        };

        // Normalize the matrix.
        if (matrix(3, 3) == 0.f)
            return {};

        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                matrix(i, j) /= matrix(3, 3);

        // perspectiveMatrix is used to solve for perspective, but it also provides
        // an easy way to test for singularity of the upper 3x3 component.
        auto perspective_matrix = matrix;
        for (int i = 0; i < 3; i++)
            perspective_matrix(3, i) = 0.f;
        perspective_matrix(3, 3) = 1.f;

        if (!perspective_matrix.is_invertible())
            return {};

        DecomposedValues values;

        // First, isolate perspective.
        if (matrix(3, 0) != 0.f || matrix(3, 1) != 0.f || matrix(3, 2) != 0.f) {
            // rightHandSide is the right hand side of the equation.
            // Note: It is the bottom side in a row-major matrix
            FloatVector4 bottom_side = {
                matrix(3, 0),
                matrix(3, 1),
                matrix(3, 2),
                matrix(3, 3),
            };

            // Solve the equation by inverting perspectiveMatrix and multiplying
            // rightHandSide by the inverse.
            auto inverse_perspective_matrix = perspective_matrix.inverse();
            auto transposed_inverse_perspective_matrix = inverse_perspective_matrix.transpose();
            values.perspective = transposed_inverse_perspective_matrix * bottom_side;
        } else {
            // No perspective.
            values.perspective = { 0.0, 0.0, 0.0, 1.0 };
        }

        // Next take care of translation
        for (int i = 0; i < 3; i++)
            values.translation[i] = matrix(i, 3);

        // Now get scale and shear. 'row' is a 3 element array of 3 component vectors
        FloatVector3 row[3];
        for (int i = 0; i < 3; i++)
            row[i] = { matrix(0, i), matrix(1, i), matrix(2, i) };

        // Compute X scale factor and normalize first row.
        values.scale[0] = row[0].length();
        row[0].normalize();

        // Compute XY shear factor and make 2nd row orthogonal to 1st.
        values.skew[0] = row[0].dot(row[1]);
        row[1] = combine(row[1], row[0], 1.f, -values.skew[0]);

        // Now, compute Y scale and normalize 2nd row.
        values.scale[1] = row[1].length();
        row[1].normalize();
        values.skew[0] /= values.scale[1];

        // Compute XZ and YZ shears, orthogonalize 3rd row
        values.skew[1] = row[0].dot(row[2]);
        row[2] = combine(row[2], row[0], 1.f, -values.skew[1]);
        values.skew[2] = row[1].dot(row[2]);
        row[2] = combine(row[2], row[1], 1.f, -values.skew[2]);

        // Next, get Z scale and normalize 3rd row.
        values.scale[2] = row[2].length();
        row[2].normalize();
        values.skew[1] /= values.scale[2];
        values.skew[2] /= values.scale[2];

        // At this point, the matrix (in rows) is orthonormal.
        // Check for a coordinate system flip.  If the determinant
        // is -1, then negate the matrix and the scaling factors.
        auto pdum3 = row[1].cross(row[2]);
        if (row[0].dot(pdum3) < 0.f) {
            for (int i = 0; i < 3; i++) {
                values.scale[i] *= -1.f;
                row[i][0] *= -1.f;
                row[i][1] *= -1.f;
                row[i][2] *= -1.f;
            }
        }

        // Now, get the rotations out
        values.rotation[0] = 0.5f * sqrt(max(1.f + row[0][0] - row[1][1] - row[2][2], 0.f));
        values.rotation[1] = 0.5f * sqrt(max(1.f - row[0][0] + row[1][1] - row[2][2], 0.f));
        values.rotation[2] = 0.5f * sqrt(max(1.f - row[0][0] - row[1][1] + row[2][2], 0.f));
        values.rotation[3] = 0.5f * sqrt(max(1.f + row[0][0] + row[1][1] + row[2][2], 0.f));

        if (row[2][1] > row[1][2])
            values.rotation[0] = -values.rotation[0];
        if (row[0][2] > row[2][0])
            values.rotation[1] = -values.rotation[1];
        if (row[1][0] > row[0][1])
            values.rotation[2] = -values.rotation[2];

        // FIXME: This accounts for the fact that the browser coordinate system is left-handed instead of right-handed.
        //        The reason for this is that the positive Y-axis direction points down instead of up. To fix this, we
        //        invert the Y axis. However, it feels like the spec pseudo-code above should have taken something like
        //        this into account, so we're probably doing something else wrong.
        values.rotation[2] *= -1;

        return values;
    };

    // https://drafts.csswg.org/css-transforms-2/#recomposing-to-a-3d-matrix
    static constexpr auto recompose = [](DecomposedValues const& values) -> FloatMatrix4x4 {
        auto matrix = FloatMatrix4x4::identity();

        // apply perspective
        for (int i = 0; i < 4; i++)
            matrix(3, i) = values.perspective[i];

        // apply translation
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 3; j++)
                matrix(i, 3) += values.translation[j] * matrix(i, j);
        }

        // apply rotation
        auto x = values.rotation[0];
        auto y = values.rotation[1];
        auto z = values.rotation[2];
        auto w = values.rotation[3];

        // Construct a composite rotation matrix from the quaternion values
        // rotationMatrix is a identity 4x4 matrix initially
        auto rotation_matrix = FloatMatrix4x4::identity();
        rotation_matrix(0, 0) = 1.f - 2.f * (y * y + z * z);
        rotation_matrix(1, 0) = 2.f * (x * y - z * w);
        rotation_matrix(2, 0) = 2.f * (x * z + y * w);
        rotation_matrix(0, 1) = 2.f * (x * y + z * w);
        rotation_matrix(1, 1) = 1.f - 2.f * (x * x + z * z);
        rotation_matrix(2, 1) = 2.f * (y * z - x * w);
        rotation_matrix(0, 2) = 2.f * (x * z - y * w);
        rotation_matrix(1, 2) = 2.f * (y * z + x * w);
        rotation_matrix(2, 2) = 1.f - 2.f * (x * x + y * y);

        matrix = matrix * rotation_matrix;

        // apply skew
        // temp is a identity 4x4 matrix initially
        auto temp = FloatMatrix4x4::identity();
        if (values.skew[2] != 0.f) {
            temp(1, 2) = values.skew[2];
            matrix = matrix * temp;
        }

        if (values.skew[1] != 0.f) {
            temp(1, 2) = 0.f;
            temp(0, 2) = values.skew[1];
            matrix = matrix * temp;
        }

        if (values.skew[0] != 0.f) {
            temp(0, 2) = 0.f;
            temp(0, 1) = values.skew[0];
            matrix = matrix * temp;
        }

        // apply scale
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 4; j++)
                matrix(j, i) *= values.scale[i];
        }

        return matrix;
    };

    // https://drafts.csswg.org/css-transforms-2/#interpolation-of-decomposed-3d-matrix-values
    static constexpr auto interpolate = [](DecomposedValues& from, DecomposedValues& to, float delta) -> DecomposedValues {
        auto product = clamp(from.rotation.dot(to.rotation), -1.0f, 1.0f);
        FloatVector4 interpolated_rotation;
        if (fabsf(product) == 1.0f) {
            interpolated_rotation = from.rotation;
        } else {
            auto theta = acos(product);
            auto w = sin(delta * theta) / sqrtf(1.0f - product * product);

            for (int i = 0; i < 4; i++) {
                from.rotation[i] *= cos(delta * theta) - product * w;
                to.rotation[i] *= w;
                interpolated_rotation[i] = from.rotation[i] + to.rotation[i];
            }
        }

        return {
            interpolate_raw(from.translation, to.translation, delta),
            interpolate_raw(from.scale, to.scale, delta),
            interpolate_raw(from.skew, to.skew, delta),
            interpolated_rotation,
            interpolate_raw(from.perspective, to.perspective, delta),
        };
    };

    auto from_matrix = style_value_to_matrix(element, from);
    auto to_matrix = style_value_to_matrix(element, to);
    auto from_decomposed = decompose(from_matrix);
    auto to_decomposed = decompose(to_matrix);
    if (!from_decomposed.has_value() || !to_decomposed.has_value())
        return {};
    auto interpolated_decomposed = interpolate(from_decomposed.value(), to_decomposed.value(), delta);
    auto interpolated = recompose(interpolated_decomposed);

    StyleValueVector values;
    values.ensure_capacity(16);
    for (int i = 0; i < 16; i++)
        values.append(NumberStyleValue::create(static_cast<double>(interpolated(i % 4, i / 4))));
    return StyleValueList::create({ TransformationStyleValue::create(TransformFunction::Matrix3d, move(values)) }, StyleValueList::Separator::Comma);
}

static Color interpolate_color(Color from, Color to, float delta)
{
    // https://drafts.csswg.org/css-color/#interpolation-space
    // If the host syntax does not define what color space interpolation should take place in, it defaults to Oklab.
    auto from_oklab = from.to_oklab();
    auto to_oklab = to.to_oklab();

    auto color = Color::from_oklab(
        interpolate_raw(from_oklab.L, to_oklab.L, delta),
        interpolate_raw(from_oklab.a, to_oklab.a, delta),
        interpolate_raw(from_oklab.b, to_oklab.b, delta));
    color.set_alpha(interpolate_raw(from.alpha(), to.alpha(), delta));
    return color;
}

static NonnullRefPtr<StyleValue const> interpolate_box_shadow(DOM::Element& element, StyleValue const& from, StyleValue const& to, float delta)
{
    // https://drafts.csswg.org/css-backgrounds/#box-shadow
    // Animation type: by computed value, treating none as a zero-item list and appending blank shadows
    //                 (transparent 0 0 0 0) with a corresponding inset keyword as needed to match the longer list if
    //                 the shorter list is otherwise compatible with the longer one

    static constexpr auto process_list = [](StyleValue const& value) {
        StyleValueVector shadows;
        if (value.is_value_list()) {
            for (auto const& element : value.as_value_list().values()) {
                if (element->is_shadow())
                    shadows.append(element);
            }
        } else if (value.is_shadow()) {
            shadows.append(value);
        } else if (!value.is_identifier() || value.as_identifier().id() != ValueID::None) {
            VERIFY_NOT_REACHED();
        }
        return shadows;
    };

    static constexpr auto extend_list_if_necessary = [](StyleValueVector& values, StyleValueVector const& other) {
        values.ensure_capacity(other.size());
        for (size_t i = values.size(); i < other.size(); i++) {
            values.unchecked_append(ShadowStyleValue::create(
                Color::Transparent,
                LengthStyleValue::create(Length::make_px(0)),
                LengthStyleValue::create(Length::make_px(0)),
                LengthStyleValue::create(Length::make_px(0)),
                LengthStyleValue::create(Length::make_px(0)),
                other[i]->as_shadow().placement()));
        }
    };

    StyleValueVector from_shadows = process_list(from);
    StyleValueVector to_shadows = process_list(to);

    extend_list_if_necessary(from_shadows, to_shadows);
    extend_list_if_necessary(to_shadows, from_shadows);

    VERIFY(from_shadows.size() == to_shadows.size());
    StyleValueVector result_shadows;
    result_shadows.ensure_capacity(from_shadows.size());

    for (size_t i = 0; i < from_shadows.size(); i++) {
        auto const& from_shadow = from_shadows[i]->as_shadow();
        auto const& to_shadow = to_shadows[i]->as_shadow();
        auto result_shadow = ShadowStyleValue::create(
            interpolate_color(from_shadow.color(), to_shadow.color(), delta),
            interpolate_value(element, from_shadow.offset_x(), to_shadow.offset_x(), delta),
            interpolate_value(element, from_shadow.offset_y(), to_shadow.offset_y(), delta),
            interpolate_value(element, from_shadow.blur_radius(), to_shadow.blur_radius(), delta),
            interpolate_value(element, from_shadow.spread_distance(), to_shadow.spread_distance(), delta),
            delta >= 0.5f ? to_shadow.placement() : from_shadow.placement());
        result_shadows.unchecked_append(result_shadow);
    }

    return StyleValueList::create(move(result_shadows), StyleValueList::Separator::Comma);
}

static NonnullRefPtr<StyleValue const> interpolate_value(DOM::Element& element, StyleValue const& from, StyleValue const& to, float delta)
{
    if (from.type() != to.type()) {
        // Handle mixed percentage and dimension types
        // https://www.w3.org/TR/css-values-4/#mixed-percentages

        struct NumericBaseTypeAndDefault {
            CSSNumericType::BaseType base_type;
            ValueComparingNonnullRefPtr<CSS::StyleValue> default_value;
        };
        static constexpr auto numeric_base_type_and_default = [](StyleValue const& value) -> Optional<NumericBaseTypeAndDefault> {
            switch (value.type()) {
            case StyleValue::Type::Angle: {
                static auto default_angle_value = AngleStyleValue::create(Angle::make_degrees(0));
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Angle, default_angle_value };
            }
            case StyleValue::Type::Frequency: {
                static auto default_frequency_value = FrequencyStyleValue::create(Frequency::make_hertz(0));
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Frequency, default_frequency_value };
            }
            case StyleValue::Type::Length: {
                static auto default_length_value = LengthStyleValue::create(Length::make_px(0));
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Length, default_length_value };
            }
            case StyleValue::Type::Percentage: {
                static auto default_percentage_value = PercentageStyleValue::create(Percentage { 0.0 });
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Percent, default_percentage_value };
            }
            case StyleValue::Type::Time: {
                static auto default_time_value = TimeStyleValue::create(Time::make_seconds(0));
                return NumericBaseTypeAndDefault { CSSNumericType::BaseType::Time, default_time_value };
            }
            default:
                return {};
            }
        };

        static constexpr auto to_calculation_node = [](StyleValue const& value) -> NonnullOwnPtr<CalculationNode> {
            switch (value.type()) {
            case StyleValue::Type::Angle:
                return NumericCalculationNode::create(value.as_angle().angle());
            case StyleValue::Type::Frequency:
                return NumericCalculationNode::create(value.as_frequency().frequency());
            case StyleValue::Type::Length:
                return NumericCalculationNode::create(value.as_length().length());
            case StyleValue::Type::Percentage:
                return NumericCalculationNode::create(value.as_percentage().percentage());
            case StyleValue::Type::Time:
                return NumericCalculationNode::create(value.as_time().time());
            default:
                VERIFY_NOT_REACHED();
            }
        };

        auto from_base_type_and_default = numeric_base_type_and_default(from);
        auto to_base_type_and_default = numeric_base_type_and_default(to);

        if (from_base_type_and_default.has_value() && to_base_type_and_default.has_value() && (from_base_type_and_default->base_type == CSSNumericType::BaseType::Percent || to_base_type_and_default->base_type == CSSNumericType::BaseType::Percent)) {
            // This is an interpolation from a numeric unit to a percentage, or vice versa. The trick here is to
            // interpolate two separate values. For example, consider an interpolation from 30px to 80%. It's quite
            // hard to understand how this interpolation works, but if instead we rewrite the values as "30px + 0%" and
            // "0px + 80%", then it is very simple to understand; we just interpolate each component separately.

            auto interpolated_from = interpolate_value(element, from, from_base_type_and_default->default_value, delta);
            auto interpolated_to = interpolate_value(element, to_base_type_and_default->default_value, to, delta);

            Vector<NonnullOwnPtr<CalculationNode>> values;
            values.ensure_capacity(2);
            values.unchecked_append(to_calculation_node(interpolated_from));
            values.unchecked_append(to_calculation_node(interpolated_to));
            auto calc_node = SumCalculationNode::create(move(values));
            return CalculatedStyleValue::create(move(calc_node), CSSNumericType { to_base_type_and_default->base_type, 1 });
        }

        return delta >= 0.5f ? to : from;
    }

    switch (from.type()) {
    case StyleValue::Type::Angle:
        return AngleStyleValue::create(Angle::make_degrees(interpolate_raw(from.as_angle().angle().to_degrees(), to.as_angle().angle().to_degrees(), delta)));
    case StyleValue::Type::Color:
        return ColorStyleValue::create(interpolate_color(from.as_color().color(), to.as_color().color(), delta));
    case StyleValue::Type::Integer:
        return IntegerStyleValue::create(interpolate_raw(from.as_integer().integer(), to.as_integer().integer(), delta));
    case StyleValue::Type::Length: {
        auto& from_length = from.as_length().length();
        auto& to_length = to.as_length().length();
        return LengthStyleValue::create(Length(interpolate_raw(from_length.raw_value(), to_length.raw_value(), delta), from_length.type()));
    }
    case StyleValue::Type::Number:
        return NumberStyleValue::create(interpolate_raw(from.as_number().number(), to.as_number().number(), delta));
    case StyleValue::Type::Percentage:
        return PercentageStyleValue::create(Percentage(interpolate_raw(from.as_percentage().percentage().value(), to.as_percentage().percentage().value(), delta)));
    case StyleValue::Type::Position: {
        // https://www.w3.org/TR/css-values-4/#combine-positions
        // FIXME: Interpolation of <position> is defined as the independent interpolation of each component (x, y) normalized as an offset from the top left corner as a <length-percentage>.
        auto& from_position = from.as_position();
        auto& to_position = to.as_position();
        return PositionStyleValue::create(
            interpolate_value(element, from_position.edge_x(), to_position.edge_x(), delta)->as_edge(),
            interpolate_value(element, from_position.edge_y(), to_position.edge_y(), delta)->as_edge());
    }
    case StyleValue::Type::Ratio: {
        auto from_ratio = from.as_ratio().ratio();
        auto to_ratio = to.as_ratio().ratio();

        // The interpolation of a <ratio> is defined by converting each <ratio> to a number by dividing the first value
        // by the second (so a ratio of 3 / 2 would become 1.5), taking the logarithm of that result (so the 1.5 would
        // become approximately 0.176), then interpolating those values. The result during the interpolation is
        // converted back to a <ratio> by inverting the logarithm, then interpreting the result as a <ratio> with the
        // result as the first value and 1 as the second value.
        auto from_number = log(from_ratio.value());
        auto to_number = log(to_ratio.value());
        auto interp_number = interpolate_raw(from_number, to_number, delta);
        return RatioStyleValue::create(Ratio(pow(M_E, interp_number)));
    }
    case StyleValue::Type::Rect: {
        auto from_rect = from.as_rect().rect();
        auto to_rect = to.as_rect().rect();
        return RectStyleValue::create({
            Length(interpolate_raw(from_rect.top_edge.raw_value(), to_rect.top_edge.raw_value(), delta), from_rect.top_edge.type()),
            Length(interpolate_raw(from_rect.right_edge.raw_value(), to_rect.right_edge.raw_value(), delta), from_rect.right_edge.type()),
            Length(interpolate_raw(from_rect.bottom_edge.raw_value(), to_rect.bottom_edge.raw_value(), delta), from_rect.bottom_edge.type()),
            Length(interpolate_raw(from_rect.left_edge.raw_value(), to_rect.left_edge.raw_value(), delta), from_rect.left_edge.type()),
        });
    }
    case StyleValue::Type::Transformation:
        VERIFY_NOT_REACHED();
    case StyleValue::Type::ValueList: {
        auto& from_list = from.as_value_list();
        auto& to_list = to.as_value_list();
        if (from_list.size() != to_list.size())
            return from;

        StyleValueVector interpolated_values;
        interpolated_values.ensure_capacity(from_list.size());
        for (size_t i = 0; i < from_list.size(); ++i)
            interpolated_values.append(interpolate_value(element, from_list.values()[i], to_list.values()[i], delta));

        return StyleValueList::create(move(interpolated_values), from_list.separator());
    }
    default:
        return from;
    }
}

static ValueComparingRefPtr<StyleValue const> interpolate_property(DOM::Element& element, PropertyID property_id, StyleValue const& from, StyleValue const& to, float delta)
{
    auto animation_type = animation_type_from_longhand_property(property_id);
    switch (animation_type) {
    case AnimationType::ByComputedValue:
        return interpolate_value(element, from, to, delta);
    case AnimationType::None:
        return to;
    case AnimationType::Custom: {
        if (property_id == PropertyID::Transform) {
            if (auto interpolated_transform = interpolate_transform(element, from, to, delta))
                return *interpolated_transform;

            // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
            // In some cases, an animation might cause a transformation matrix to be singular or non-invertible.
            // For example, an animation in which scale moves from 1 to -1. At the time when the matrix is in
            // such a state, the transformed element is not rendered.
            return {};
        }
        if (property_id == PropertyID::BoxShadow)
            return interpolate_box_shadow(element, from, to, delta);

        // FIXME: Handle all custom animatable properties
        [[fallthrough]];
    }
    // FIXME: Handle repeatable-list animatable properties
    case AnimationType::RepeatableList:
    case AnimationType::Discrete:
    default:
        return delta >= 0.5f ? to : from;
    }
}

void StyleComputer::collect_animation_into(DOM::Element& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, JS::NonnullGCPtr<Animations::KeyframeEffect> effect, StyleProperties& style_properties, AnimationRefresh refresh) const
{
    auto animation = effect->associated_animation();
    if (!animation)
        return;

    auto output_progress = effect->transformed_progress();
    if (!output_progress.has_value())
        return;

    if (!effect->key_frame_set())
        return;

    auto& keyframes = effect->key_frame_set()->keyframes_by_key;

    auto key = static_cast<u64>(output_progress.value() * 100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor);
    auto matching_keyframe_it = keyframes.find_largest_not_above_iterator(key);
    if (matching_keyframe_it.is_end()) {
        if constexpr (LIBWEB_CSS_ANIMATION_DEBUG) {
            dbgln("    Did not find any start keyframe for the current state ({}) :(", key);
            dbgln("    (have {} keyframes)", keyframes.size());
            for (auto it = keyframes.begin(); it != keyframes.end(); ++it)
                dbgln("        - {}", it.key());
        }
        return;
    }

    auto keyframe_start = matching_keyframe_it.key();
    auto keyframe_values = *matching_keyframe_it;

    auto initial_keyframe_it = matching_keyframe_it;
    auto keyframe_end_it = ++matching_keyframe_it;
    if (keyframe_end_it.is_end())
        keyframe_end_it = initial_keyframe_it;

    auto keyframe_end = keyframe_end_it.key();
    auto keyframe_end_values = *keyframe_end_it;

    auto progress_in_keyframe = [&] {
        if (keyframe_start == keyframe_end)
            return 0.f;
        return static_cast<float>(key - keyframe_start) / static_cast<float>(keyframe_end - keyframe_start);
    }();

    if constexpr (LIBWEB_CSS_ANIMATION_DEBUG) {
        auto valid_properties = keyframe_values.properties.size();
        dbgln("Animation {} contains {} properties to interpolate, progress = {}%", animation->id(), valid_properties, progress_in_keyframe * 100);
    }

    for (auto const& it : keyframe_values.properties) {
        auto resolve_property = [&](auto& property) {
            return property.visit(
                [&](Animations::KeyframeEffect::KeyFrameSet::UseInitial) -> RefPtr<StyleValue const> {
                    if (refresh == AnimationRefresh::Yes)
                        return {};
                    return style_properties.maybe_null_property(it.key);
                },
                [&](RefPtr<StyleValue const> value) -> RefPtr<StyleValue const> {
                    if (value->is_unresolved())
                        return Parser::Parser::resolve_unresolved_style_value(Parser::ParsingContext { element.document() }, element, pseudo_element, it.key, value->as_unresolved());
                    return value;
                });
        };

        auto resolved_start_property = resolve_property(it.value);

        auto const& end_property = keyframe_end_values.properties.get(it.key);
        if (!end_property.has_value()) {
            if (resolved_start_property) {
                style_properties.set_animated_property(it.key, *resolved_start_property);
                dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "No end property for property {}, using {}", string_from_property_id(it.key), resolved_start_property->to_string());
            }
            continue;
        }

        auto resolved_end_property = resolve_property(end_property.value());

        if (resolved_end_property && !resolved_start_property)
            resolved_start_property = CSS::property_initial_value(document().realm(), it.key);

        if (!resolved_start_property || !resolved_end_property)
            continue;

        auto start = resolved_start_property.release_nonnull();
        auto end = resolved_end_property.release_nonnull();

        if (style_properties.is_property_important(it.key)) {
            continue;
        }

        if (auto next_value = interpolate_property(*effect->target(), it.key, *start, *end, progress_in_keyframe)) {
            dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "Interpolated value for property {} at {}: {} -> {} = {}", string_from_property_id(it.key), progress_in_keyframe, start->to_string(), end->to_string(), next_value->to_string());
            style_properties.set_animated_property(it.key, *next_value);
        } else {
            // If interpolate_property() fails, the element should not be rendered
            dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "Interpolated value for property {} at {}: {} -> {} is invalid", string_from_property_id(it.key), progress_in_keyframe, start->to_string(), end->to_string());
            style_properties.set_animated_property(PropertyID::Visibility, IdentifierStyleValue::create(ValueID::Hidden));
        }
    }
}

static void apply_animation_properties(DOM::Document& document, StyleProperties& style, Animations::Animation& animation)
{
    auto& effect = verify_cast<Animations::KeyframeEffect>(*animation.effect());

    Optional<CSS::Time> duration;
    if (auto duration_value = style.maybe_null_property(PropertyID::AnimationDuration); duration_value) {
        if (duration_value->is_time()) {
            duration = duration_value->as_time().time();
        } else if (duration_value->is_identifier() && duration_value->as_identifier().id() == ValueID::Auto) {
            // We use empty optional to represent "auto".
            duration = {};
        }
    }

    CSS::Time delay { 0, CSS::Time::Type::S };
    if (auto delay_value = style.maybe_null_property(PropertyID::AnimationDelay); delay_value && delay_value->is_time())
        delay = delay_value->as_time().time();

    double iteration_count = 1.0;
    if (auto iteration_count_value = style.maybe_null_property(PropertyID::AnimationIterationCount); iteration_count_value) {
        if (iteration_count_value->is_identifier() && iteration_count_value->to_identifier() == ValueID::Infinite)
            iteration_count = HUGE_VAL;
        else if (iteration_count_value->is_number())
            iteration_count = iteration_count_value->as_number().number();
    }

    CSS::AnimationFillMode fill_mode { CSS::AnimationFillMode::None };
    if (auto fill_mode_property = style.maybe_null_property(PropertyID::AnimationFillMode); fill_mode_property && fill_mode_property->is_identifier()) {
        if (auto fill_mode_value = value_id_to_animation_fill_mode(fill_mode_property->to_identifier()); fill_mode_value.has_value())
            fill_mode = *fill_mode_value;
    }

    CSS::AnimationDirection direction { CSS::AnimationDirection::Normal };
    if (auto direction_property = style.maybe_null_property(PropertyID::AnimationDirection); direction_property && direction_property->is_identifier()) {
        if (auto direction_value = value_id_to_animation_direction(direction_property->to_identifier()); direction_value.has_value())
            direction = *direction_value;
    }

    CSS::AnimationPlayState play_state { CSS::AnimationPlayState::Running };
    if (auto play_state_property = style.maybe_null_property(PropertyID::AnimationPlayState); play_state_property && play_state_property->is_identifier()) {
        if (auto play_state_value = value_id_to_animation_play_state(play_state_property->to_identifier()); play_state_value.has_value())
            play_state = *play_state_value;
    }

    CSS::EasingStyleValue::Function timing_function { CSS::EasingStyleValue::CubicBezier::ease() };
    if (auto timing_property = style.maybe_null_property(PropertyID::AnimationTimingFunction); timing_property && timing_property->is_easing())
        timing_function = timing_property->as_easing().function();

    auto iteration_duration = duration.has_value()
        ? Variant<double, String> { duration.release_value().to_milliseconds() }
        : "auto"_string;
    effect.set_iteration_duration(iteration_duration);
    effect.set_start_delay(delay.to_milliseconds());
    effect.set_iteration_count(iteration_count);
    effect.set_timing_function(move(timing_function));
    effect.set_fill_mode(Animations::css_fill_mode_to_bindings_fill_mode(fill_mode));
    effect.set_playback_direction(Animations::css_animation_direction_to_bindings_playback_direction(direction));

    if (play_state != effect.last_css_animation_play_state()) {
        if (play_state == CSS::AnimationPlayState::Running && animation.play_state() == Bindings::AnimationPlayState::Paused) {
            HTML::TemporaryExecutionContext context(document.relevant_settings_object());
            animation.play().release_value_but_fixme_should_propagate_errors();
        } else if (play_state == CSS::AnimationPlayState::Paused && animation.play_state() != Bindings::AnimationPlayState::Paused) {
            HTML::TemporaryExecutionContext context(document.relevant_settings_object());
            animation.pause().release_value_but_fixme_should_propagate_errors();
        }

        effect.set_last_css_animation_play_state(play_state);
    }
}

static void apply_dimension_attribute(StyleProperties& style, DOM::Element const& element, FlyString const& attribute_name, CSS::PropertyID property_id)
{
    auto attribute = element.attribute(attribute_name);
    if (!attribute.has_value())
        return;

    auto parsed_value = HTML::parse_dimension_value(*attribute);
    if (!parsed_value)
        return;

    style.set_property(property_id, parsed_value.release_nonnull());
}

// https://www.w3.org/TR/css-cascade/#cascading
void StyleComputer::compute_cascaded_values(StyleProperties& style, DOM::Element& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, bool& did_match_any_pseudo_element_rules, ComputeStyleMode mode) const
{
    // First, we collect all the CSS rules whose selectors match `element`:
    MatchingRuleSet matching_rule_set;
    matching_rule_set.user_agent_rules = collect_matching_rules(element, CascadeOrigin::UserAgent, pseudo_element);
    sort_matching_rules(matching_rule_set.user_agent_rules);
    matching_rule_set.user_rules = collect_matching_rules(element, CascadeOrigin::User, pseudo_element);
    sort_matching_rules(matching_rule_set.user_rules);
    matching_rule_set.author_rules = collect_matching_rules(element, CascadeOrigin::Author, pseudo_element);
    sort_matching_rules(matching_rule_set.author_rules);

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        VERIFY(pseudo_element.has_value());
        if (matching_rule_set.author_rules.is_empty() && matching_rule_set.user_rules.is_empty() && matching_rule_set.user_agent_rules.is_empty()) {
            did_match_any_pseudo_element_rules = false;
            return;
        }
        did_match_any_pseudo_element_rules = true;
    }

    // Then we resolve all the CSS custom properties ("variables") for this element:
    cascade_custom_properties(element, pseudo_element, matching_rule_set.author_rules);

    // Then we apply the declarations from the matched rules in cascade order:

    // Normal user agent declarations
    cascade_declarations(style, element, pseudo_element, matching_rule_set.user_agent_rules, CascadeOrigin::UserAgent, Important::No);

    // Normal user declarations
    cascade_declarations(style, element, pseudo_element, matching_rule_set.user_rules, CascadeOrigin::User, Important::No);

    // Author presentational hints (NOTE: The spec doesn't say exactly how to prioritize these.)
    if (!pseudo_element.has_value()) {
        element.apply_presentational_hints(style);

        if (element.supports_dimension_attributes()) {
            apply_dimension_attribute(style, element, HTML::AttributeNames::width, CSS::PropertyID::Width);
            apply_dimension_attribute(style, element, HTML::AttributeNames::height, CSS::PropertyID::Height);
        }

        // SVG presentation attributes are parsed as CSS values, so we need to handle potential custom properties here.
        if (element.is_svg_element()) {
            // FIXME: This is not very efficient, we should only resolve the custom properties that are actually used.
            for (auto i = to_underlying(CSS::first_property_id); i <= to_underlying(CSS::last_property_id); ++i) {
                auto property_id = (CSS::PropertyID)i;
                auto& property = style.m_property_values[i];
                if (property && property->is_unresolved())
                    property = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingContext { document() }, element, pseudo_element, property_id, property->as_unresolved());
            }
        }
    }

    // Normal author declarations
    cascade_declarations(style, element, pseudo_element, matching_rule_set.author_rules, CascadeOrigin::Author, Important::No);

    // Animation declarations [css-animations-2]
    auto animation_name = [&]() -> Optional<String> {
        auto animation_name = style.maybe_null_property(PropertyID::AnimationName);
        if (animation_name.is_null())
            return OptionalNone {};
        if (animation_name->is_string())
            return animation_name->as_string().string_value().to_string();
        return animation_name->to_string();
    }();

    if (animation_name.has_value()) {
        if (auto source_declaration = style.animation_name_source()) {
            auto& realm = element.realm();

            if (source_declaration != element.cached_animation_name_source(pseudo_element)) {
                // This animation name is new, so we need to create a new animation for it.
                if (auto existing_animation = element.cached_animation_name_animation(pseudo_element))
                    existing_animation->cancel(Animations::Animation::ShouldInvalidate::No);
                element.set_cached_animation_name_source(source_declaration, pseudo_element);

                auto effect = Animations::KeyframeEffect::create(realm);
                auto animation = CSSAnimation::create(realm);
                animation->set_id(animation_name.release_value());
                animation->set_timeline(m_document->timeline());
                animation->set_owning_element(element);
                animation->set_effect(effect);
                apply_animation_properties(m_document, style, animation);
                if (pseudo_element.has_value())
                    effect->set_pseudo_element(Selector::PseudoElement { pseudo_element.value() });

                auto const& rule_cache = rule_cache_for_cascade_origin(CascadeOrigin::Author);
                if (auto keyframe_set = rule_cache.rules_by_animation_keyframes.get(animation->id()); keyframe_set.has_value())
                    effect->set_key_frame_set(keyframe_set.value());

                effect->set_target(&element);
                element.set_cached_animation_name_animation(animation, pseudo_element);

                HTML::TemporaryExecutionContext context(m_document->relevant_settings_object());
                animation->play().release_value_but_fixme_should_propagate_errors();
            } else {
                // The animation hasn't changed, but some properties of the animation may have
                apply_animation_properties(m_document, style, *element.cached_animation_name_animation(pseudo_element));
            }
        }
    } else {
        // If the element had an existing animation, cancel it
        if (auto existing_animation = element.cached_animation_name_animation(pseudo_element)) {
            existing_animation->cancel(Animations::Animation::ShouldInvalidate::No);
            element.set_cached_animation_name_animation({}, pseudo_element);
            element.set_cached_animation_name_source({}, pseudo_element);
        }
    }

    auto animations = element.get_animations({ .subtree = false });
    for (auto& animation : animations) {
        if (auto effect = animation->effect(); effect && effect->is_keyframe_effect()) {
            auto& keyframe_effect = *static_cast<Animations::KeyframeEffect*>(effect.ptr());
            if (keyframe_effect.pseudo_element_type() == pseudo_element)
                collect_animation_into(element, pseudo_element, keyframe_effect, style);
        }
    }

    // Important author declarations
    cascade_declarations(style, element, pseudo_element, matching_rule_set.author_rules, CascadeOrigin::Author, Important::Yes);

    // Important user declarations
    cascade_declarations(style, element, pseudo_element, matching_rule_set.user_rules, CascadeOrigin::User, Important::Yes);

    // Important user agent declarations
    cascade_declarations(style, element, pseudo_element, matching_rule_set.user_agent_rules, CascadeOrigin::UserAgent, Important::Yes);

    // FIXME: Transition declarations [css-transitions-1]
}

DOM::Element const* element_to_inherit_style_from(DOM::Element const* element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element)
{
    // Pseudo-elements treat their originating element as their parent.
    DOM::Element const* parent_element = nullptr;
    if (pseudo_element.has_value()) {
        parent_element = element;
    } else if (element) {
        parent_element = element->parent_or_shadow_host_element();
    }
    return parent_element;
}

NonnullRefPtr<StyleValue const> StyleComputer::get_inherit_value(JS::Realm& initial_value_context_realm, CSS::PropertyID property_id, DOM::Element const* element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element)
{
    auto* parent_element = element_to_inherit_style_from(element, pseudo_element);

    if (!parent_element || !parent_element->computed_css_values())
        return property_initial_value(initial_value_context_realm, property_id);
    return parent_element->computed_css_values()->property(property_id);
}

void StyleComputer::compute_defaulted_property_value(StyleProperties& style, DOM::Element const* element, CSS::PropertyID property_id, Optional<CSS::Selector::PseudoElement::Type> pseudo_element) const
{
    // FIXME: If we don't know the correct initial value for a property, we fall back to InitialStyleValue.

    auto& value_slot = style.m_property_values[to_underlying(property_id)];
    if (!value_slot) {
        if (is_inherited_property(property_id)) {
            style.set_property(
                property_id,
                get_inherit_value(document().realm(), property_id, element, pseudo_element),
                StyleProperties::Inherited::Yes,
                Important::No);
        } else {
            style.set_property(property_id, property_initial_value(document().realm(), property_id));
        }
        return;
    }

    if (value_slot->is_initial()) {
        value_slot = property_initial_value(document().realm(), property_id);
        return;
    }

    if (value_slot->is_inherit()) {
        value_slot = get_inherit_value(document().realm(), property_id, element, pseudo_element);
        style.set_property_inherited(property_id, StyleProperties::Inherited::Yes);
        return;
    }

    // https://www.w3.org/TR/css-cascade-4/#inherit-initial
    // If the cascaded value of a property is the unset keyword,
    if (value_slot->is_unset()) {
        if (is_inherited_property(property_id)) {
            // then if it is an inherited property, this is treated as inherit,
            value_slot = get_inherit_value(document().realm(), property_id, element, pseudo_element);
            style.set_property_inherited(property_id, StyleProperties::Inherited::Yes);
        } else {
            // and if it is not, this is treated as initial.
            value_slot = property_initial_value(document().realm(), property_id);
        }
    }
}

// https://www.w3.org/TR/css-cascade/#defaulting
void StyleComputer::compute_defaulted_values(StyleProperties& style, DOM::Element const* element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element) const
{
    // Walk the list of all known CSS properties and:
    // - Add them to `style` if they are missing.
    // - Resolve `inherit` and `initial` as needed.
    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        auto property_id = (CSS::PropertyID)i;
        compute_defaulted_property_value(style, element, property_id, pseudo_element);
    }

    // https://www.w3.org/TR/css-color-4/#resolving-other-colors
    // In the color property, the used value of currentcolor is the inherited value.
    auto color = style.property(CSS::PropertyID::Color);
    if (color->to_identifier() == CSS::ValueID::Currentcolor) {
        color = get_inherit_value(document().realm(), CSS::PropertyID::Color, element, pseudo_element);
        style.set_property(CSS::PropertyID::Color, color);
    }
}

Length::FontMetrics StyleComputer::calculate_root_element_font_metrics(StyleProperties const& style) const
{
    auto root_value = style.property(CSS::PropertyID::FontSize);

    auto font_pixel_metrics = style.first_available_computed_font().pixel_metrics();
    Length::FontMetrics font_metrics { m_default_font_metrics.font_size, font_pixel_metrics };
    font_metrics.font_size = root_value->as_length().length().to_px(viewport_rect(), font_metrics, font_metrics);
    font_metrics.line_height = style.compute_line_height(viewport_rect(), font_metrics, font_metrics);

    return font_metrics;
}

RefPtr<Gfx::FontCascadeList const> StyleComputer::find_matching_font_weight_ascending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, bool inclusive)
{
    using Fn = AK::Function<bool(MatchingFontCandidate const&)>;
    auto pred = inclusive ? Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight >= target_weight; })
                          : Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight > target_weight; });
    auto it = find_if(candidates.begin(), candidates.end(), pred);
    for (; it != candidates.end(); ++it) {
        if (auto found_font = it->font_with_point_size(font_size_in_pt))
            return found_font;
    }
    return {};
}

RefPtr<Gfx::FontCascadeList const> StyleComputer::find_matching_font_weight_descending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, bool inclusive)
{
    using Fn = AK::Function<bool(MatchingFontCandidate const&)>;
    auto pred = inclusive ? Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight <= target_weight; })
                          : Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight < target_weight; });
    auto it = find_if(candidates.rbegin(), candidates.rend(), pred);
    for (; it != candidates.rend(); ++it) {
        if (auto found_font = it->font_with_point_size(font_size_in_pt))
            return found_font;
    }
    return {};
}

// Partial implementation of the font-matching algorithm: https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm
// FIXME: This should be replaced by the full CSS font selection algorithm.
RefPtr<Gfx::FontCascadeList const> StyleComputer::font_matching_algorithm(FontFaceKey const& key, float font_size_in_pt) const
{
    // If a font family match occurs, the user agent assembles the set of font faces in that family and then
    // narrows the set to a single face using other font properties in the order given below.
    Vector<MatchingFontCandidate> matching_family_fonts;
    for (auto const& font_key_and_loader : m_loaded_fonts) {
        if (font_key_and_loader.key.family_name.equals_ignoring_ascii_case(key.family_name))
            matching_family_fonts.empend(font_key_and_loader.key, const_cast<FontLoaderList*>(&font_key_and_loader.value));
    }
    Gfx::FontDatabase::the().for_each_typeface_with_family_name(key.family_name, [&](Gfx::Typeface const& typeface) {
        matching_family_fonts.empend(
            FontFaceKey {
                .family_name = typeface.family(),
                .weight = static_cast<int>(typeface.weight()),
                .slope = typeface.slope(),
            },
            &typeface);
    });
    quick_sort(matching_family_fonts, [](auto const& a, auto const& b) {
        return a.key.weight < b.key.weight;
    });
    // FIXME: 1. font-stretch is tried first.
    // FIXME: 2. font-style is tried next.
    // We don't have complete support of italic and oblique fonts, so matching on font-style can be simplified to:
    // If a matching slope is found, all faces which don't have that matching slope are excluded from the matching set.
    auto style_it = find_if(matching_family_fonts.begin(), matching_family_fonts.end(),
        [&](auto const& matching_font_candidate) { return matching_font_candidate.key.slope == key.slope; });
    if (style_it != matching_family_fonts.end()) {
        matching_family_fonts.remove_all_matching([&](auto const& matching_font_candidate) {
            return matching_font_candidate.key.slope != key.slope;
        });
    }
    // 3. font-weight is matched next.
    // If the desired weight is inclusively between 400 and 500, weights greater than or equal to the target weight
    // are checked in ascending order until 500 is hit and checked, followed by weights less than the target weight
    // in descending order, followed by weights greater than 500, until a match is found.
    if (key.weight >= 400 && key.weight <= 500) {
        auto it = find_if(matching_family_fonts.begin(), matching_family_fonts.end(),
            [&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight >= key.weight; });
        for (; it != matching_family_fonts.end() && it->key.weight <= 500; ++it) {
            if (auto found_font = it->font_with_point_size(font_size_in_pt))
                return found_font;
        }
        if (auto found_font = find_matching_font_weight_descending(matching_family_fonts, key.weight, font_size_in_pt, false))
            return found_font;
        for (; it != matching_family_fonts.end(); ++it) {
            if (auto found_font = it->font_with_point_size(font_size_in_pt))
                return found_font;
        }
    }
    // If the desired weight is less than 400, weights less than or equal to the desired weight are checked in descending order
    // followed by weights above the desired weight in ascending order until a match is found.
    if (key.weight < 400) {
        if (auto found_font = find_matching_font_weight_descending(matching_family_fonts, key.weight, font_size_in_pt, true))
            return found_font;
        if (auto found_font = find_matching_font_weight_ascending(matching_family_fonts, key.weight, font_size_in_pt, false))
            return found_font;
    }
    // If the desired weight is greater than 500, weights greater than or equal to the desired weight are checked in ascending order
    // followed by weights below the desired weight in descending order until a match is found.
    if (key.weight > 500) {
        if (auto found_font = find_matching_font_weight_ascending(matching_family_fonts, key.weight, font_size_in_pt, true))
            return found_font;
        if (auto found_font = find_matching_font_weight_descending(matching_family_fonts, key.weight, font_size_in_pt, false))
            return found_font;
    }
    return {};
}

RefPtr<Gfx::FontCascadeList const> StyleComputer::compute_font_for_style_values(DOM::Element const* element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, StyleValue const& font_family, StyleValue const& font_size, StyleValue const& font_style, StyleValue const& font_weight, StyleValue const& font_stretch, int math_depth) const
{
    auto* parent_element = element_to_inherit_style_from(element, pseudo_element);

    auto width = font_stretch.to_font_stretch_width();

    auto weight = font_weight.to_font_weight();
    bool bold = weight > Gfx::FontWeight::Regular;

    // FIXME: Should be based on "user's default font size"
    CSSPixels font_size_in_px = 16;

    Gfx::FontPixelMetrics font_pixel_metrics;
    if (parent_element && parent_element->computed_css_values())
        font_pixel_metrics = parent_element->computed_css_values()->first_available_computed_font().pixel_metrics();
    else
        font_pixel_metrics = Platform::FontPlugin::the().default_font().pixel_metrics();
    auto parent_font_size = [&]() -> CSSPixels {
        if (!parent_element || !parent_element->computed_css_values())
            return font_size_in_px;
        auto value = parent_element->computed_css_values()->property(CSS::PropertyID::FontSize);
        if (value->is_length()) {
            auto length = value->as_length().length();
            if (length.is_absolute() || length.is_relative()) {
                Length::FontMetrics font_metrics { font_size_in_px, font_pixel_metrics };
                return length.to_px(viewport_rect(), font_metrics, m_root_element_font_metrics);
            }
        }
        return font_size_in_px;
    };
    Length::FontMetrics font_metrics { parent_font_size(), font_pixel_metrics };

    if (font_size.is_identifier()) {
        // https://w3c.github.io/csswg-drafts/css-fonts/#absolute-size-mapping
        auto get_absolute_size_mapping = [](Web::CSS::ValueID identifier) -> CSSPixelFraction {
            switch (identifier) {
            case CSS::ValueID::XxSmall:
                return CSSPixels(3) / 5;
            case CSS::ValueID::XSmall:
                return CSSPixels(3) / 4;
            case CSS::ValueID::Small:
                return CSSPixels(8) / 9;
            case CSS::ValueID::Medium:
                return 1;
            case CSS::ValueID::Large:
                return CSSPixels(6) / 5;
            case CSS::ValueID::XLarge:
                return CSSPixels(3) / 2;
            case CSS::ValueID::XxLarge:
                return 2;
            case CSS::ValueID::XxxLarge:
                return 3;
            case CSS::ValueID::Smaller:
                return CSSPixels(4) / 5;
            case CSS::ValueID::Larger:
                return CSSPixels(5) / 4;
            default:
                return 1;
            }
        };

        auto const identifier = static_cast<IdentifierStyleValue const&>(font_size).id();

        if (identifier == ValueID::Math) {
            auto math_scaling_factor = [&]() {
                // https://w3c.github.io/mathml-core/#the-math-script-level-property
                // If the specified value font-size is math then the computed value of font-size is obtained by multiplying
                // the inherited value of font-size by a nonzero scale factor calculated by the following procedure:
                // 1. Let A be the inherited math-depth value, B the computed math-depth value, C be 0.71 and S be 1.0
                int inherited_math_depth = parent_element && parent_element->computed_css_values()
                    ? parent_element->computed_css_values()->math_depth()
                    : InitialValues::math_depth();
                int computed_math_depth = math_depth;
                auto size_ratio = 0.71;
                auto scale = 1.0;
                // 2. If A = B then return S.
                bool invert_scale_factor = false;
                if (inherited_math_depth == computed_math_depth) {
                    return scale;
                }
                //    If B < A, swap A and B and set InvertScaleFactor to true.
                else if (computed_math_depth < inherited_math_depth) {
                    AK::swap(inherited_math_depth, computed_math_depth);
                    invert_scale_factor = true;
                }
                //    Otherwise B > A and set InvertScaleFactor to false.
                else {
                    invert_scale_factor = false;
                }
                // 3. Let E be B - A > 0.
                double e = (computed_math_depth - inherited_math_depth) > 0;
                // FIXME: 4. If the inherited first available font has an OpenType MATH table:
                //    - If A ≤ 0 and B ≥ 2 then multiply S by scriptScriptPercentScaleDown and decrement E by 2.
                //    - Otherwise if A = 1 then multiply S by scriptScriptPercentScaleDown / scriptPercentScaleDown and decrement E by 1.
                //    - Otherwise if B = 1 then multiply S by scriptPercentScaleDown and decrement E by 1.
                // 5. Multiply S by C^E.
                scale *= AK::pow(size_ratio, e);
                // 6. Return S if InvertScaleFactor is false and 1/S otherwise.
                if (!invert_scale_factor)
                    return scale;
                return 1.0 / scale;
            };
            font_size_in_px = parent_font_size().scale_by(math_scaling_factor());
        } else {
            // https://w3c.github.io/csswg-drafts/css-fonts/#valdef-font-size-relative-size
            // TODO: If the parent element has a keyword font size in the absolute size keyword mapping table,
            //       larger may compute the font size to the next entry in the table,
            //       and smaller may compute the font size to the previous entry in the table.
            if (identifier == CSS::ValueID::Smaller || identifier == CSS::ValueID::Larger) {
                if (parent_element && parent_element->computed_css_values()) {
                    font_size_in_px = CSSPixels::nearest_value_for(parent_element->computed_css_values()->first_available_computed_font().pixel_metrics().size);
                }
            }
            font_size_in_px *= get_absolute_size_mapping(identifier);
        }
    } else {
        Length::ResolutionContext const length_resolution_context {
            .viewport_rect = viewport_rect(),
            .font_metrics = font_metrics,
            .root_font_metrics = m_root_element_font_metrics,
        };

        Optional<Length> maybe_length;
        if (font_size.is_percentage()) {
            // Percentages refer to parent element's font size
            maybe_length = Length::make_px(CSSPixels::nearest_value_for(font_size.as_percentage().percentage().as_fraction() * parent_font_size().to_double()));

        } else if (font_size.is_length()) {
            maybe_length = font_size.as_length().length();
        } else if (font_size.is_calculated()) {
            if (font_size.as_calculated().contains_percentage()) {
                maybe_length = font_size.as_calculated().resolve_length_percentage(length_resolution_context, Length::make_px(parent_font_size()));
            } else {
                maybe_length = font_size.as_calculated().resolve_length(length_resolution_context);
            }
        }
        if (maybe_length.has_value()) {
            font_size_in_px = maybe_length.value().to_px(length_resolution_context);
        }
    }

    auto slope = font_style.to_font_slope();

    // FIXME: Implement the full font-matching algorithm: https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm

    // Note: This is modified by the find_font() lambda
    bool monospace = false;

    float const font_size_in_pt = font_size_in_px * 0.75f;

    auto find_font = [&](FlyString const& family) -> RefPtr<Gfx::FontCascadeList const> {
        FontFaceKey const key {
            .family_name = family,
            .weight = weight,
            .slope = slope,
        };

        auto result = Gfx::FontCascadeList::create();
        if (auto it = m_loaded_fonts.find(key); it != m_loaded_fonts.end()) {
            auto const& loaders = it->value;
            for (auto const& loader : loaders) {
                if (auto found_font = loader->font_with_point_size(font_size_in_pt))
                    result->add(*found_font, loader->unicode_ranges());
            }
            return result;
        }

        if (auto found_font = font_matching_algorithm(key, font_size_in_pt); found_font && !found_font->is_empty()) {
            return found_font;
        }

        if (auto found_font = Gfx::FontDatabase::the().get(family, font_size_in_pt, weight, width, slope)) {
            result->add(*found_font);
            return result;
        }

        return {};
    };

    auto find_fallback_font_names = [](String const& family) -> Vector<String> {
        auto names = Platform::FontPlugin::the().fallback_font_names(family);
        if (!names.has_value()) {
            return {};
        }
        return names.value();
    };

    auto identifier_to_family = [&monospace](ValueID font_id) -> FlyString {
        Platform::GenericFont generic_font {};
        switch (font_id) {
        case ValueID::Monospace:
        case ValueID::UiMonospace:
            generic_font = Platform::GenericFont::Monospace;
            monospace = true;
            break;
        case ValueID::Serif:
            generic_font = Platform::GenericFont::Serif;
            break;
        case ValueID::Fantasy:
            generic_font = Platform::GenericFont::Fantasy;
            break;
        case ValueID::SansSerif:
            generic_font = Platform::GenericFont::SansSerif;
            break;
        case ValueID::Cursive:
            generic_font = Platform::GenericFont::Cursive;
            break;
        case ValueID::UiSerif:
            generic_font = Platform::GenericFont::UiSerif;
            break;
        case ValueID::UiSansSerif:
            generic_font = Platform::GenericFont::UiSansSerif;
            break;
        case ValueID::UiRounded:
            generic_font = Platform::GenericFont::UiRounded;
            break;
        default:
            return {};
        }
        return Platform::FontPlugin::the().generic_font_name(generic_font);
    };

    Vector<String> font_names;
    if (font_family.is_value_list()) {
        auto const& family_list = static_cast<StyleValueList const&>(font_family).values();
        for (auto const& family : family_list) {
            if (family->is_identifier()) {
                font_names.append(identifier_to_family(family->to_identifier()).to_string());
            } else if (family->is_string()) {
                font_names.append(family->as_string().string_value());
            } else if (family->is_custom_ident()) {
                font_names.append(family->as_custom_ident().custom_ident().to_string());
            }
        }
    } else if (font_family.is_identifier()) {
        font_names.append(identifier_to_family(font_family.to_identifier()).to_string());
    } else if (font_family.is_string()) {
        font_names.append(font_family.as_string().string_value());
    } else if (font_family.is_custom_ident()) {
        font_names.append(font_family.as_custom_ident().custom_ident().to_string());
    }

    for (auto const& family : font_names) {
        auto const& fallback_font_names = find_fallback_font_names(family);
        for (auto const& name : fallback_font_names) {
            if (!font_names.contains_slow(name)) {
                font_names.append(name);
            }
        }
    }

    auto font_list = Gfx::FontCascadeList::create();
    for (auto const& family : font_names) {
        if (auto ft = find_font(family)) {
            font_list->extend(*ft);
        }
    }

    auto found_font = StyleProperties::font_fallback(monospace, bold);
    font_list->add(found_font->with_size(font_size_in_pt));

    return font_list;
}

void StyleComputer::compute_font(StyleProperties& style, DOM::Element const* element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element) const
{
    // To compute the font, first ensure that we've defaulted the relevant CSS font properties.
    // FIXME: This should be more sophisticated.
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontFamily, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontSize, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontStretch, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontStyle, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontWeight, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::LineHeight, pseudo_element);

    auto font_family = style.property(CSS::PropertyID::FontFamily);
    auto font_size = style.property(CSS::PropertyID::FontSize);
    auto font_style = style.property(CSS::PropertyID::FontStyle);
    auto font_weight = style.property(CSS::PropertyID::FontWeight);
    auto font_stretch = style.property(CSS::PropertyID::FontStretch);

    auto font_list = compute_font_for_style_values(element, pseudo_element, font_family, font_size, font_style, font_weight, font_stretch, style.math_depth());
    VERIFY(font_list);
    VERIFY(!font_list->is_empty());

    RefPtr<Gfx::Font const> const found_font = font_list->first();

    style.set_property(CSS::PropertyID::FontSize, LengthStyleValue::create(CSS::Length::make_px(CSSPixels::nearest_value_for(found_font->pixel_size()))));
    style.set_property(CSS::PropertyID::FontWeight, NumberStyleValue::create(font_weight->to_font_weight()));

    style.set_computed_font_list(*font_list);

    if (element && is<HTML::HTMLHtmlElement>(*element)) {
        const_cast<StyleComputer&>(*this).m_root_element_font_metrics = calculate_root_element_font_metrics(style);
    }
}

Gfx::Font const& StyleComputer::initial_font() const
{
    // FIXME: This is not correct.
    return StyleProperties::font_fallback(false, false);
}

void StyleComputer::absolutize_values(StyleProperties& style) const
{
    Length::FontMetrics font_metrics {
        m_root_element_font_metrics.font_size,
        style.first_available_computed_font().pixel_metrics()
    };

    auto font_size = style.property(CSS::PropertyID::FontSize)->as_length().length().to_px(viewport_rect(), font_metrics, m_root_element_font_metrics);
    font_metrics.font_size = font_size;

    // NOTE: Percentage line-height values are relative to the font-size of the element.
    //       We have to resolve them right away, so that the *computed* line-height is ready for inheritance.
    //       We can't simply absolutize *all* percentage values against the font size,
    //       because most percentages are relative to containing block metrics.
    auto& line_height_value_slot = style.m_property_values[to_underlying(CSS::PropertyID::LineHeight)];
    if (line_height_value_slot && line_height_value_slot->is_percentage()) {
        line_height_value_slot = LengthStyleValue::create(
            Length::make_px(CSSPixels::nearest_value_for(font_size * static_cast<double>(line_height_value_slot->as_percentage().percentage().as_fraction()))));
    }

    auto line_height = style.compute_line_height(viewport_rect(), font_metrics, m_root_element_font_metrics);
    font_metrics.line_height = line_height;

    // NOTE: line-height might be using lh which should be resolved against the parent line height (like we did here already)
    if (line_height_value_slot && line_height_value_slot->is_length())
        line_height_value_slot = LengthStyleValue::create(Length::make_px(line_height));

    for (size_t i = 0; i < style.m_property_values.size(); ++i) {
        auto& value_slot = style.m_property_values[i];
        if (!value_slot)
            continue;
        value_slot = value_slot->absolutized(viewport_rect(), font_metrics, m_root_element_font_metrics);
    }

    style.set_line_height({}, line_height);
}

void StyleComputer::resolve_effective_overflow_values(StyleProperties& style) const
{
    // https://www.w3.org/TR/css-overflow-3/#overflow-control
    // The visible/clip values of overflow compute to auto/hidden (respectively) if one of overflow-x or
    // overflow-y is neither visible nor clip.
    auto overflow_x = value_id_to_overflow(style.property(PropertyID::OverflowX)->to_identifier());
    auto overflow_y = value_id_to_overflow(style.property(PropertyID::OverflowY)->to_identifier());
    auto overflow_x_is_visible_or_clip = overflow_x == Overflow::Visible || overflow_x == Overflow::Clip;
    auto overflow_y_is_visible_or_clip = overflow_y == Overflow::Visible || overflow_y == Overflow::Clip;
    if (!overflow_x_is_visible_or_clip || !overflow_y_is_visible_or_clip) {
        if (overflow_x == CSS::Overflow::Visible)
            style.set_property(CSS::PropertyID::OverflowX, IdentifierStyleValue::create(CSS::ValueID::Auto));
        if (overflow_x == CSS::Overflow::Clip)
            style.set_property(CSS::PropertyID::OverflowX, IdentifierStyleValue::create(CSS::ValueID::Hidden));
        if (overflow_y == CSS::Overflow::Visible)
            style.set_property(CSS::PropertyID::OverflowY, IdentifierStyleValue::create(CSS::ValueID::Auto));
        if (overflow_y == CSS::Overflow::Clip)
            style.set_property(CSS::PropertyID::OverflowY, IdentifierStyleValue::create(CSS::ValueID::Hidden));
    }
}

enum class BoxTypeTransformation {
    None,
    Blockify,
    Inlinify,
};

static BoxTypeTransformation required_box_type_transformation(StyleProperties const& style, DOM::Element const& element, Optional<CSS::Selector::PseudoElement::Type> const& pseudo_element)
{
    // NOTE: We never blockify <br> elements. They are always inline.
    //       There is currently no way to express in CSS how a <br> element really behaves.
    //       Spec issue: https://github.com/whatwg/html/issues/2291
    if (is<HTML::HTMLBRElement>(element))
        return BoxTypeTransformation::None;

    // Absolute positioning or floating an element blockifies the box’s display type. [CSS2]
    if (style.position() == CSS::Positioning::Absolute || style.position() == CSS::Positioning::Fixed || style.float_() != CSS::Float::None)
        return BoxTypeTransformation::Blockify;

    // FIXME: Containment in a ruby container inlinifies the box’s display type, as described in [CSS-RUBY-1].

    // NOTE: If we're computing style for a pseudo-element, the effective parent will be the originating element itself, not its parent.
    auto const* parent = pseudo_element.has_value() ? &element : element.parent_element();

    // A parent with a grid or flex display value blockifies the box’s display type. [CSS-GRID-1] [CSS-FLEXBOX-1]
    if (parent && parent->computed_css_values()) {
        auto const& parent_display = parent->computed_css_values()->display();
        if (parent_display.is_grid_inside() || parent_display.is_flex_inside())
            return BoxTypeTransformation::Blockify;
    }

    return BoxTypeTransformation::None;
}

// https://drafts.csswg.org/css-display/#transformations
void StyleComputer::transform_box_type_if_needed(StyleProperties& style, DOM::Element const& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element) const
{
    // 2.7. Automatic Box Type Transformations

    // Some layout effects require blockification or inlinification of the box type,
    // which sets the box’s computed outer display type to block or inline (respectively).
    // (This has no effect on display types that generate no box at all, such as none or contents.)

    auto display = style.display();
    if (display.is_none() || display.is_contents())
        return;

    auto new_display = display;

    if (display.is_math_inside()) {
        // https://w3c.github.io/mathml-core/#new-display-math-value
        // For elements that are not MathML elements, if the specified value of display is inline math or block math
        // then the computed value is block flow and inline flow respectively.
        if (element.namespace_uri() != Namespace::MathML)
            new_display = CSS::Display { display.outside(), CSS::DisplayInside::Flow };
        // For the mtable element the computed value is block table and inline table respectively.
        else if (element.tag_name().equals_ignoring_ascii_case("mtable"sv))
            new_display = CSS::Display { display.outside(), CSS::DisplayInside::Table };
        // For the mtr element, the computed value is table-row.
        else if (element.tag_name().equals_ignoring_ascii_case("mtr"sv))
            new_display = CSS::Display { CSS::DisplayInternal::TableRow };
        // For the mtd element, the computed value is table-cell.
        else if (element.tag_name().equals_ignoring_ascii_case("mtd"sv))
            new_display = CSS::Display { CSS::DisplayInternal::TableCell };
    }

    switch (required_box_type_transformation(style, element, pseudo_element)) {
    case BoxTypeTransformation::None:
        break;
    case BoxTypeTransformation::Blockify:
        if (display.is_block_outside())
            return;
        // If a layout-internal box is blockified, its inner display type converts to flow so that it becomes a block container.
        if (display.is_internal()) {
            new_display = CSS::Display::from_short(CSS::Display::Short::Block);
        } else {
            VERIFY(display.is_outside_and_inside());

            // For legacy reasons, if an inline block box (inline flow-root) is blockified, it becomes a block box (losing its flow-root nature).
            // For consistency, a run-in flow-root box also blockifies to a block box.
            if (display.is_inline_block()) {
                new_display = CSS::Display { CSS::DisplayOutside::Block, CSS::DisplayInside::Flow, display.list_item() };
            } else {
                new_display = CSS::Display { CSS::DisplayOutside::Block, display.inside(), display.list_item() };
            }
        }
        break;
    case BoxTypeTransformation::Inlinify:
        if (display.is_inline_outside()) {
            // FIXME: If an inline box (inline flow) is inlinified, it recursively inlinifies all of its in-flow children,
            //        so that no block-level descendants break up the inline formatting context in which it participates.
            if (display.is_flow_inside()) {
                dbgln("FIXME: Inlinify inline box children recursively");
            }
            break;
        }
        if (display.is_internal()) {
            // Inlinification has no effect on layout-internal boxes. (However, placement in such an inline context will typically cause them
            // to be wrapped in an appropriately-typed anonymous inline-level box.)
        } else {
            VERIFY(display.is_outside_and_inside());

            // If a block box (block flow) is inlinified, its inner display type is set to flow-root so that it remains a block container.
            if (display.is_block_outside() && display.is_flow_inside()) {
                new_display = CSS::Display { CSS::DisplayOutside::Inline, CSS::DisplayInside::FlowRoot, display.list_item() };
            }

            new_display = CSS::Display { CSS::DisplayOutside::Inline, display.inside(), display.list_item() };
        }
        break;
    }

    if (new_display != display)
        style.set_property(CSS::PropertyID::Display, DisplayStyleValue::create(new_display));
}

NonnullRefPtr<StyleProperties> StyleComputer::create_document_style() const
{
    auto style = StyleProperties::create();
    compute_math_depth(style, nullptr, {});
    compute_font(style, nullptr, {});
    compute_defaulted_values(style, nullptr, {});
    absolutize_values(style);
    style->set_property(CSS::PropertyID::Width, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().width())));
    style->set_property(CSS::PropertyID::Height, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().height())));
    style->set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::Block)));
    return style;
}

NonnullRefPtr<StyleProperties> StyleComputer::compute_style(DOM::Element& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element) const
{
    return compute_style_impl(element, move(pseudo_element), ComputeStyleMode::Normal).release_nonnull();
}

RefPtr<StyleProperties> StyleComputer::compute_pseudo_element_style_if_needed(DOM::Element& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element) const
{
    return compute_style_impl(element, move(pseudo_element), ComputeStyleMode::CreatePseudoElementStyleIfNeeded);
}

RefPtr<StyleProperties> StyleComputer::compute_style_impl(DOM::Element& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, ComputeStyleMode mode) const
{
    build_rule_cache_if_needed();

    // Special path for elements that use pseudo element as style selector
    if (element.use_pseudo_element().has_value()) {
        auto& parent_element = verify_cast<HTML::HTMLElement>(*element.root().parent_or_shadow_host());
        auto style = compute_style(parent_element, *element.use_pseudo_element());

        // Merge back inline styles
        if (element.has_attribute(HTML::AttributeNames::style)) {
            auto* inline_style = parse_css_style_attribute(CSS::Parser::ParsingContext(document()), *element.get_attribute(HTML::AttributeNames::style), element);
            for (auto const& property : inline_style->properties())
                style->set_property(property.property_id, property.value);
        }
        return style;
    }

    auto style = StyleProperties::create();
    // 1. Perform the cascade. This produces the "specified style"
    bool did_match_any_pseudo_element_rules = false;
    compute_cascaded_values(style, element, pseudo_element, did_match_any_pseudo_element_rules, mode);

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded && !did_match_any_pseudo_element_rules)
        return nullptr;

    // 2. Compute the math-depth property, since that might affect the font-size
    compute_math_depth(style, &element, pseudo_element);

    // 3. Compute the font, since that may be needed for font-relative CSS units
    compute_font(style, &element, pseudo_element);

    // 4. Absolutize values, turning font/viewport relative lengths into absolute lengths
    absolutize_values(style);

    // 5. Default the values, applying inheritance and 'initial' as needed
    compute_defaulted_values(style, &element, pseudo_element);

    // 6. Run automatic box type transformations
    transform_box_type_if_needed(style, element, pseudo_element);

    // 7. Resolve effective overflow values
    resolve_effective_overflow_values(style);

    // 8. Let the element adjust computed style
    element.adjust_computed_style(style);

    return style;
}

void StyleComputer::build_rule_cache_if_needed() const
{
    if (m_author_rule_cache && m_user_rule_cache && m_user_agent_rule_cache)
        return;
    const_cast<StyleComputer&>(*this).build_rule_cache();
}

NonnullOwnPtr<StyleComputer::RuleCache> StyleComputer::make_rule_cache_for_cascade_origin(CascadeOrigin cascade_origin)
{
    auto rule_cache = make<RuleCache>();

    size_t num_class_rules = 0;
    size_t num_id_rules = 0;
    size_t num_tag_name_rules = 0;
    size_t num_pseudo_element_rules = 0;
    size_t num_root_rules = 0;
    size_t num_attribute_rules = 0;

    Vector<MatchingRule> matching_rules;
    size_t style_sheet_index = 0;
    for_each_stylesheet(cascade_origin, [&](auto& sheet, JS::GCPtr<DOM::ShadowRoot> shadow_root) {
        size_t rule_index = 0;
        sheet.for_each_effective_style_rule([&](auto const& rule) {
            size_t selector_index = 0;
            for (CSS::Selector const& selector : rule.selectors()) {
                MatchingRule matching_rule {
                    shadow_root,
                    &rule,
                    sheet,
                    style_sheet_index,
                    rule_index,
                    selector_index,
                    selector.specificity(),
                    cascade_origin,
                    false,
                    false,
                    SelectorEngine::can_use_fast_matches(selector),
                };

                for (auto const& simple_selector : selector.compound_selectors().last().simple_selectors) {
                    if (!matching_rule.contains_pseudo_element) {
                        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoElement) {
                            matching_rule.contains_pseudo_element = true;
                            ++num_pseudo_element_rules;
                        }
                    }
                    if (!matching_rule.contains_root_pseudo_class) {
                        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoClass
                            && simple_selector.pseudo_class().type == CSS::PseudoClass::Root) {
                            matching_rule.contains_root_pseudo_class = true;
                            ++num_root_rules;
                        }
                    }
                }

                bool added_to_bucket = false;
                for (auto const& simple_selector : selector.compound_selectors().last().simple_selectors) {
                    if (simple_selector.type == CSS::Selector::SimpleSelector::Type::Id) {
                        rule_cache->rules_by_id.ensure(simple_selector.name()).append(move(matching_rule));
                        ++num_id_rules;
                        added_to_bucket = true;
                        break;
                    }
                    if (simple_selector.type == CSS::Selector::SimpleSelector::Type::Class) {
                        rule_cache->rules_by_class.ensure(simple_selector.name()).append(move(matching_rule));
                        ++num_class_rules;
                        added_to_bucket = true;
                        break;
                    }
                    if (simple_selector.type == CSS::Selector::SimpleSelector::Type::TagName) {
                        rule_cache->rules_by_tag_name.ensure(simple_selector.qualified_name().name.lowercase_name).append(move(matching_rule));
                        ++num_tag_name_rules;
                        added_to_bucket = true;
                        break;
                    }
                }
                if (!added_to_bucket) {
                    if (matching_rule.contains_pseudo_element) {
                        rule_cache->pseudo_element_rules.append(move(matching_rule));
                    } else if (matching_rule.contains_root_pseudo_class) {
                        rule_cache->root_rules.append(move(matching_rule));
                    } else {
                        for (auto const& simple_selector : selector.compound_selectors().last().simple_selectors) {
                            if (simple_selector.type == CSS::Selector::SimpleSelector::Type::Attribute) {
                                rule_cache->rules_by_attribute_name.ensure(simple_selector.attribute().qualified_name.name.lowercase_name).append(move(matching_rule));
                                ++num_attribute_rules;
                                added_to_bucket = true;
                                break;
                            }
                        }
                        if (!added_to_bucket) {
                            rule_cache->other_rules.append(move(matching_rule));
                        }
                    }
                }

                ++selector_index;
            }
            ++rule_index;
        });

        // Loosely based on https://drafts.csswg.org/css-animations-2/#keyframe-processing
        sheet.for_each_effective_keyframes_at_rule([&](CSSKeyframesRule const& rule) {
            auto keyframe_set = adopt_ref(*new Animations::KeyframeEffect::KeyFrameSet);
            HashTable<PropertyID> animated_properties;

            // Forwards pass, resolve all the user-specified keyframe properties.
            for (auto const& keyframe_rule : *rule.css_rules()) {
                auto const& keyframe = verify_cast<CSSKeyframeRule>(*keyframe_rule);
                Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame resolved_keyframe;

                auto key = static_cast<u64>(keyframe.key().value() * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor);
                auto const& keyframe_style = *keyframe.style_as_property_owning_style_declaration();
                for (auto const& it : keyframe_style.properties()) {
                    // Unresolved properties will be resolved in collect_animation_into()
                    for_each_property_expanding_shorthands(it.property_id, it.value, AllowUnresolved::Yes, [&](PropertyID shorthand_id, StyleValue const& shorthand_value) {
                        animated_properties.set(shorthand_id);
                        resolved_keyframe.properties.set(shorthand_id, NonnullRefPtr<StyleValue const> { shorthand_value });
                    });
                }

                keyframe_set->keyframes_by_key.insert(key, resolved_keyframe);
            }

            Animations::KeyframeEffect::generate_initial_and_final_frames(keyframe_set, animated_properties);

            if constexpr (LIBWEB_CSS_DEBUG) {
                dbgln("Resolved keyframe set '{}' into {} keyframes:", rule.name(), keyframe_set->keyframes_by_key.size());
                for (auto it = keyframe_set->keyframes_by_key.begin(); it != keyframe_set->keyframes_by_key.end(); ++it)
                    dbgln("    - keyframe {}: {} properties", it.key(), it->properties.size());
            }

            rule_cache->rules_by_animation_keyframes.set(rule.name(), move(keyframe_set));
        });
        ++style_sheet_index;
    });

    size_t total_rules = num_class_rules + num_id_rules + num_tag_name_rules + num_pseudo_element_rules + num_root_rules + num_attribute_rules + rule_cache->other_rules.size();
    if constexpr (LIBWEB_CSS_DEBUG) {
        dbgln("Built rule cache!");
        dbgln("           ID: {}", num_id_rules);
        dbgln("        Class: {}", num_class_rules);
        dbgln("      TagName: {}", num_tag_name_rules);
        dbgln("PseudoElement: {}", num_pseudo_element_rules);
        dbgln("         Root: {}", num_root_rules);
        dbgln("    Attribute: {}", num_attribute_rules);
        dbgln("        Other: {}", rule_cache->other_rules.size());
        dbgln("        Total: {}", total_rules);
    }
    return rule_cache;
}

void StyleComputer::build_rule_cache()
{
    if (auto user_style_source = document().page().user_style(); user_style_source.has_value()) {
        m_user_style_sheet = JS::make_handle(parse_css_stylesheet(CSS::Parser::ParsingContext(document()), user_style_source.value()));
    }

    m_author_rule_cache = make_rule_cache_for_cascade_origin(CascadeOrigin::Author);
    m_user_rule_cache = make_rule_cache_for_cascade_origin(CascadeOrigin::User);
    m_user_agent_rule_cache = make_rule_cache_for_cascade_origin(CascadeOrigin::UserAgent);
}

void StyleComputer::invalidate_rule_cache()
{
    m_author_rule_cache = nullptr;

    // NOTE: We could be smarter about keeping the user rule cache, and style sheet.
    //       Currently we are re-parsing the user style sheet every time we build the caches,
    //       as it may have changed.
    m_user_rule_cache = nullptr;
    m_user_style_sheet = nullptr;

    // NOTE: It might not be necessary to throw away the UA rule cache.
    //       If we are sure that it's safe, we could keep it as an optimization.
    m_user_agent_rule_cache = nullptr;
}

void StyleComputer::did_load_font(FlyString const&)
{
    document().invalidate_style();
}

Optional<FontLoader&> StyleComputer::load_font_face(ParsedFontFace const& font_face, Function<void(FontLoader const&)> on_load, Function<void()> on_fail)
{
    if (font_face.sources().is_empty()) {
        if (on_fail)
            on_fail();
        return {};
    }

    FontFaceKey key {
        .family_name = font_face.font_family(),
        .weight = font_face.weight().value_or(0),
        .slope = font_face.slope().value_or(0),
    };

    Vector<URL::URL> urls;
    for (auto const& source : font_face.sources()) {
        // FIXME: These should be loaded relative to the stylesheet URL instead of the document URL.
        if (source.local_or_url.has<URL::URL>())
            urls.append(m_document->parse_url(MUST(source.local_or_url.get<URL::URL>().to_string())));
        // FIXME: Handle local()
    }

    if (urls.is_empty()) {
        if (on_fail)
            on_fail();
        return {};
    }

    auto loader = make<FontLoader>(const_cast<StyleComputer&>(*this), font_face.font_family(), font_face.unicode_ranges(), move(urls), move(on_load), move(on_fail));
    auto& loader_ref = *loader;
    auto maybe_font_loaders_list = const_cast<StyleComputer&>(*this).m_loaded_fonts.get(key);
    if (maybe_font_loaders_list.has_value()) {
        maybe_font_loaders_list->append(move(loader));
    } else {
        FontLoaderList loaders;
        loaders.append(move(loader));
        const_cast<StyleComputer&>(*this).m_loaded_fonts.set(key, move(loaders));
    }
    // Actual object owned by font loader list inside m_loaded_fonts, this isn't use-after-move/free
    return loader_ref;
}

void StyleComputer::load_fonts_from_sheet(CSSStyleSheet const& sheet)
{
    for (auto const& rule : sheet.rules()) {
        if (!is<CSSFontFaceRule>(*rule))
            continue;
        (void)load_font_face(static_cast<CSSFontFaceRule const&>(*rule).font_face());
    }
}

void StyleComputer::compute_math_depth(StyleProperties& style, DOM::Element const* element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element) const
{
    // https://w3c.github.io/mathml-core/#propdef-math-depth

    // First, ensure that the relevant CSS properties have been defaulted.
    // FIXME: This should be more sophisticated.
    compute_defaulted_property_value(style, element, CSS::PropertyID::MathDepth, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::MathStyle, pseudo_element);

    auto inherited_math_depth = [&]() {
        if (!element || !element->parent_element())
            return InitialValues::math_depth();
        return element->parent_element()->computed_css_values()->math_depth();
    };

    auto value = style.property(CSS::PropertyID::MathDepth);
    if (!value->is_math_depth()) {
        style.set_math_depth(inherited_math_depth());
        return;
    }
    auto& math_depth = value->as_math_depth();

    auto resolve_integer = [&](StyleValue const& integer_value) {
        if (integer_value.is_integer())
            return integer_value.as_integer().integer();
        if (integer_value.is_calculated())
            return integer_value.as_calculated().resolve_integer().value();
        VERIFY_NOT_REACHED();
    };

    // The computed value of the math-depth value is determined as follows:
    // - If the specified value of math-depth is auto-add and the inherited value of math-style is compact
    //   then the computed value of math-depth of the element is its inherited value plus one.
    if (math_depth.is_auto_add() && style.property(CSS::PropertyID::MathStyle)->to_identifier() == CSS::ValueID::Compact) {
        style.set_math_depth(inherited_math_depth() + 1);
        return;
    }
    // - If the specified value of math-depth is of the form add(<integer>) then the computed value of
    //   math-depth of the element is its inherited value plus the specified integer.
    if (math_depth.is_add()) {
        style.set_math_depth(inherited_math_depth() + resolve_integer(*math_depth.integer_value()));
        return;
    }
    // - If the specified value of math-depth is of the form <integer> then the computed value of math-depth
    //   of the element is the specified integer.
    if (math_depth.is_integer()) {
        style.set_math_depth(resolve_integer(*math_depth.integer_value()));
        return;
    }
    // - Otherwise, the computed value of math-depth of the element is the inherited one.
    style.set_math_depth(inherited_math_depth());
}

static void for_each_element_hash(DOM::Element const& element, auto callback)
{
    callback(element.local_name().hash());
    if (element.id().has_value())
        callback(element.id().value().hash());
    for (auto const& class_ : element.class_names())
        callback(class_.hash());
    element.for_each_attribute([&](auto& attribute) {
        callback(attribute.local_name().hash());
    });
}

void StyleComputer::reset_ancestor_filter()
{
    m_ancestor_filter.clear();
}

void StyleComputer::push_ancestor(DOM::Element const& element)
{
    for_each_element_hash(element, [&](u32 hash) {
        m_ancestor_filter.increment(hash);
    });
}

void StyleComputer::pop_ancestor(DOM::Element const& element)
{
    for_each_element_hash(element, [&](u32 hash) {
        m_ancestor_filter.decrement(hash);
    });
}

}
