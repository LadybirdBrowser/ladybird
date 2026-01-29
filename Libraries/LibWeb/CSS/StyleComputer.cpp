/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Bitmap.h>
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/Find.h>
#include <AK/FixedBitmap.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/Interpolation.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleSheet.h>
#include <LibWeb/CSS/StyleValues/AddFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <math.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StyleComputer);

CSSStyleProperties const& MatchingRule::declaration() const
{
    if (rule->type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(*rule).declaration();
    if (rule->type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(*rule).declaration();
    VERIFY_NOT_REACHED();
}

SelectorList const& MatchingRule::absolutized_selectors() const
{
    if (rule->type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(*rule).absolutized_selectors();
    if (rule->type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(*rule).parent_style_rule().absolutized_selectors();
    VERIFY_NOT_REACHED();
}

FlyString const& MatchingRule::qualified_layer_name() const
{
    if (rule->type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(*rule).qualified_layer_name();
    if (rule->type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(*rule).parent_style_rule().qualified_layer_name();
    VERIFY_NOT_REACHED();
}

StyleComputer::StyleComputer(DOM::Document& document)
    : m_document(document)
    , m_default_font_metrics(16, Platform::FontPlugin::the().default_font(16)->pixel_metrics(), InitialValues::line_height())
    , m_root_element_font_metrics(m_default_font_metrics)
{
    m_ancestor_filter = make<CountingBloomFilter<u8, 14>>();
}

StyleComputer::~StyleComputer() = default;

void StyleComputer::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    if (m_has_result_cache)
        visitor.visit(*m_has_result_cache);
}

Optional<String> StyleComputer::user_agent_style_sheet_source(StringView name)
{
    extern String default_stylesheet_source;
    extern String quirks_mode_stylesheet_source;
    extern String mathml_stylesheet_source;
    extern String svg_stylesheet_source;

    if (name == "CSS/Default.css"sv)
        return default_stylesheet_source;
    if (name == "CSS/QuirksMode.css"sv)
        return quirks_mode_stylesheet_source;
    if (name == "MathML/Default.css"sv)
        return mathml_stylesheet_source;
    if (name == "SVG/Default.css"sv)
        return svg_stylesheet_source;
    return {};
}

RuleCache const* StyleComputer::rule_cache_for_cascade_origin(CascadeOrigin cascade_origin, Optional<FlyString const> qualified_layer_name, GC::Ptr<DOM::ShadowRoot const> shadow_root) const
{
    auto& style_scope = shadow_root ? shadow_root->style_scope() : document().style_scope();
    style_scope.build_rule_cache_if_needed();

    auto const* rule_caches_by_layer = [&]() -> RuleCaches const* {
        switch (cascade_origin) {
        case CascadeOrigin::Author:
            return style_scope.m_author_rule_cache;
        case CascadeOrigin::User:
            return style_scope.m_user_rule_cache;
        case CascadeOrigin::UserAgent:
            return style_scope.m_user_agent_rule_cache;
        default:
            VERIFY_NOT_REACHED();
        }
    }();
    if (!rule_caches_by_layer)
        return nullptr;
    if (!qualified_layer_name.has_value())
        return &rule_caches_by_layer->main;
    return rule_caches_by_layer->by_layer.get(*qualified_layer_name).value_or(nullptr);
}

[[nodiscard]] static bool filter_namespace_rule(Optional<FlyString> const& element_namespace_uri, MatchingRule const& rule)
{
    // FIXME: Filter out non-default namespace using prefixes
    if (rule.default_namespace.has_value() && element_namespace_uri != rule.default_namespace)
        return false;
    return true;
}

InvalidationSet StyleComputer::invalidation_set_for_properties(Vector<InvalidationSet::Property> const& properties, StyleScope const& style_scope) const
{
    if (!style_scope.m_style_invalidation_data)
        return {};
    auto const& descendant_invalidation_sets = style_scope.m_style_invalidation_data->descendant_invalidation_sets;
    InvalidationSet result;
    for (auto const& property : properties) {
        if (auto it = descendant_invalidation_sets.find(property); it != descendant_invalidation_sets.end())
            result.include_all_from(it->value);
    }
    return result;
}

bool StyleComputer::invalidation_property_used_in_has_selector(InvalidationSet::Property const& property, StyleScope const& style_scope) const
{
    if (!style_scope.m_style_invalidation_data)
        return true;
    switch (property.type) {
    case InvalidationSet::Property::Type::Id:
        if (style_scope.m_style_invalidation_data->ids_used_in_has_selectors.contains(property.name()))
            return true;
        break;
    case InvalidationSet::Property::Type::Class:
        if (style_scope.m_style_invalidation_data->class_names_used_in_has_selectors.contains(property.name()))
            return true;
        break;
    case InvalidationSet::Property::Type::Attribute:
        if (style_scope.m_style_invalidation_data->attribute_names_used_in_has_selectors.contains(property.name()))
            return true;
        break;
    case InvalidationSet::Property::Type::TagName:
        if (style_scope.m_style_invalidation_data->tag_names_used_in_has_selectors.contains(property.name()))
            return true;
        break;
    case InvalidationSet::Property::Type::PseudoClass:
        if (style_scope.m_style_invalidation_data->pseudo_classes_used_in_has_selectors.contains(property.value.get<PseudoClass>()))
            return true;
        break;
    default:
        break;
    }
    return false;
}

Vector<MatchingRule const*> StyleComputer::collect_matching_rules(DOM::AbstractElement abstract_element, CascadeOrigin cascade_origin, PseudoClassBitmap& attempted_pseudo_class_matches, Optional<FlyString const> qualified_layer_name) const
{
    auto const& root_node = abstract_element.element().root();
    auto shadow_root = as_if<DOM::ShadowRoot>(root_node);
    auto element_shadow_root = abstract_element.element().shadow_root();
    auto const& element_namespace_uri = abstract_element.element().namespace_uri();

    GC::Ptr<DOM::Element const> shadow_host;
    if (element_shadow_root)
        shadow_host = abstract_element.element();
    else if (shadow_root)
        shadow_host = shadow_root->host();

    Vector<MatchingRule const&, 512> rules_to_run;

    auto add_rule_to_run = [&](MatchingRule const& rule_to_run) {
        // FIXME: This needs to be revised when adding support for the ::shadow selector, as it needs to cross shadow boundaries.
        auto rule_root = rule_to_run.shadow_root;
        auto from_user_agent_or_user_stylesheet = rule_to_run.cascade_origin == CascadeOrigin::UserAgent || rule_to_run.cascade_origin == CascadeOrigin::User;

        // NOTE: Inside shadow trees, we only match rules that are defined in the shadow tree's style sheets.
        //       Exceptions are:
        //       - the shadow tree's *shadow host*, which needs to match :host rules from inside the shadow root.
        //       - ::slotted() rules, which need to match elements assigned to slots from inside the shadow root.
        //       - UA or User style sheets don't have a scope, so they are always relevant.
        // FIXME: We should reorganize the data so that the document-level StyleComputer doesn't cache *all* rules,
        //        but instead we'd have some kind of "style scope" at the document level, and also one for each shadow root.
        //        Then we could only evaluate rules from the current style scope.
        bool rule_is_relevant_for_current_scope = rule_root == shadow_root
            || (element_shadow_root && rule_root == element_shadow_root)
            || from_user_agent_or_user_stylesheet
            || rule_to_run.slotted
            || rule_to_run.contains_part_pseudo_element;

        if (!rule_is_relevant_for_current_scope)
            return;

        auto const& selector = rule_to_run.selector;
        if (selector.can_use_ancestor_filter() && should_reject_with_ancestor_filter(selector))
            return;

        rules_to_run.unchecked_append(rule_to_run);
    };

    auto add_rules_to_run = [&](Vector<MatchingRule> const& rules) {
        rules_to_run.grow_capacity(rules_to_run.size() + rules.size());
        if (abstract_element.pseudo_element().has_value()) {
            for (auto const& rule : rules) {
                if (rule.contains_pseudo_element && filter_namespace_rule(element_namespace_uri, rule))
                    add_rule_to_run(rule);
            }
        } else {
            for (auto const& rule : rules) {
                if ((rule.slotted || rule.contains_part_pseudo_element || !rule.contains_pseudo_element) && filter_namespace_rule(element_namespace_uri, rule))
                    add_rule_to_run(rule);
            }
        }
    };

    auto add_rules_from_cache = [&](RuleCache const& rule_cache) {
        rule_cache.for_each_matching_rules(abstract_element, [&](auto const& matching_rules) {
            add_rules_to_run(matching_rules);
            return IterationDecision::Continue;
        });
    };

    if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, nullptr))
        add_rules_from_cache(*rule_cache);

    if (shadow_root) {
        if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, shadow_root))
            add_rules_from_cache(*rule_cache);
    }

    if (element_shadow_root) {
        if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, element_shadow_root))
            add_rules_from_cache(*rule_cache);
    }

    if (auto assigned_slot = abstract_element.element().assigned_slot_internal()) {
        if (auto const* slot_shadow_root = as_if<DOM::ShadowRoot>(assigned_slot->root())) {
            if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, slot_shadow_root)) {
                add_rules_to_run(rule_cache->slotted_rules);
            }
        }
    }

    // ::part() can apply to anything in a shadow tree, that is either an element with a `part` attribute or a pseudo-element.
    // Rules from any ancestor style scope can apply.
    if (shadow_root && (abstract_element.pseudo_element().has_value() || !abstract_element.element().part_names().is_empty())) {
        for (auto* part_shadow_root = abstract_element.element().shadow_including_first_ancestor_of_type<DOM::ShadowRoot>();
            part_shadow_root;
            part_shadow_root = part_shadow_root->shadow_including_first_ancestor_of_type<DOM::ShadowRoot>()) {

            if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, part_shadow_root)) {
                add_rules_to_run(rule_cache->part_rules);
            }
        }
        if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, nullptr)) {
            add_rules_to_run(rule_cache->part_rules);
        }
    }

    Vector<MatchingRule const*> matching_rules;
    matching_rules.ensure_capacity(rules_to_run.size());

    for (auto const& rule_to_run : rules_to_run) {
        // NOTE: When matching an element against a rule from outside the shadow root's style scope,
        //       we have to pass in null for the shadow host, otherwise combinator traversal will
        //       be confined to the element itself (since it refuses to cross the shadow boundary).
        auto rule_root = rule_to_run.shadow_root;
        auto shadow_host_to_use = shadow_host;
        if (abstract_element.element().is_shadow_host() && rule_root != abstract_element.element().shadow_root())
            shadow_host_to_use = nullptr;

        auto const& selector = rule_to_run.selector;

        SelectorEngine::MatchContext context {
            .style_sheet_for_rule = *rule_to_run.sheet,
            .subject = abstract_element.element(),
            .collect_per_element_selector_involvement_metadata = true,
            .has_result_cache = m_has_result_cache.ptr(),
        };
        ScopeGuard guard = [&] {
            attempted_pseudo_class_matches |= context.attempted_pseudo_class_matches;
        };
        if (selector.is_slotted()) {
            if (!abstract_element.element().assigned_slot_internal())
                continue;
            // We're collecting rules for element, which is assigned to a slot.
            // For ::slotted() matching, slot should be used as a subject instead of element,
            // while element itself is saved in matching context, so selector engine could
            // switch back to it when matching inside ::slotted() argument.
            auto const& slot = *abstract_element.element().assigned_slot_internal();
            context.slotted_element = &abstract_element.element();
            context.subject = &slot;
            if (!SelectorEngine::matches(selector, slot, shadow_host_to_use, context, PseudoElement::Slotted))
                continue;
        } else if (!SelectorEngine::matches(selector, abstract_element.element(), shadow_host_to_use, context, abstract_element.pseudo_element()))
            continue;
        matching_rules.append(&rule_to_run);
    }

    return matching_rules;
}

static void sort_matching_rules(Vector<MatchingRule const*>& matching_rules)
{
    quick_sort(matching_rules, [&](MatchingRule const* a, MatchingRule const* b) {
        auto const& a_selector = a->selector;
        auto const& b_selector = b->selector;
        auto a_specificity = a_selector.specificity();
        auto b_specificity = b_selector.specificity();
        if (a_specificity == b_specificity) {
            if (a->style_sheet_index == b->style_sheet_index)
                return a->rule_index < b->rule_index;
            return a->style_sheet_index < b->style_sheet_index;
        }
        return a_specificity < b_specificity;
    });
}

void StyleComputer::for_each_property_expanding_shorthands(PropertyID property_id, StyleValue const& value, Function<void(PropertyID, StyleValue const&)> const& set_longhand_property)
{
    if (property_is_shorthand(property_id) && (value.is_unresolved() || value.is_pending_substitution())) {
        // If a shorthand property contains an arbitrary substitution function in its value, the longhand properties
        // it’s associated with must instead be filled in with a special, unobservable-to-authors pending-substitution
        // value that indicates the shorthand contains an arbitrary substitution function, and thus the longhand’s
        // value can’t be determined until after substituted.
        // https://drafts.csswg.org/css-values-5/#pending-substitution-value
        // Ensure we keep the longhand around until it can be resolved.
        set_longhand_property(property_id, value);
        auto pending_substitution_value = PendingSubstitutionStyleValue::create(value);
        for (auto longhand_id : longhands_for_shorthand(property_id)) {
            for_each_property_expanding_shorthands(longhand_id, pending_substitution_value, set_longhand_property);
        }
        return;
    }

    if (value.is_shorthand()) {
        auto& shorthand_value = value.as_shorthand();
        auto& properties = shorthand_value.sub_properties();
        auto& values = shorthand_value.values();
        for (size_t i = 0; i < properties.size(); ++i)
            for_each_property_expanding_shorthands(properties[i], values[i], set_longhand_property);
        return;
    }

    if (property_is_shorthand(property_id)) {
        // ShorthandStyleValue was handled already, as were unresolved shorthands.
        // That means the only values we should see are the CSS-wide keywords, or the guaranteed-invalid value.
        // Both should be applied to our longhand properties.
        // We don't directly call `set_longhand_property()` because the longhands might have longhands of their own.
        // (eg `grid` -> `grid-template` -> `grid-template-areas` & `grid-template-rows` & `grid-template-columns`)
        VERIFY(value.is_css_wide_keyword() || value.is_guaranteed_invalid());
        for (auto longhand : longhands_for_shorthand(property_id))
            for_each_property_expanding_shorthands(longhand, value, set_longhand_property);
        return;
    }

    set_longhand_property(property_id, value);
}

void StyleComputer::cascade_declarations(
    CascadedProperties& cascaded_properties,
    DOM::AbstractElement abstract_element,
    Vector<MatchingRule const*> const& matching_rules,
    CascadeOrigin cascade_origin,
    Important important,
    Optional<FlyString> layer_name,
    Optional<LogicalAliasMappingContext> logical_alias_mapping_context,
    ReadonlySpan<PropertyID> properties_to_cascade) const
{
    AK::FixedBitmap<to_underlying(last_property_id) + 1> seen_properties(false);
    auto cascade_style_declaration = [&](CSSStyleProperties const& declaration) {
        seen_properties.fill(false);
        for (auto const& property : declaration.properties()) {

            // OPTIMIZATION: If we've been asked to only cascade a specific set of properties, skip the rest.
            if (!properties_to_cascade.is_empty()) {
                if (!properties_to_cascade.contains_slow(property.property_id))
                    continue;
            }

            if (important != property.important)
                continue;

            if (abstract_element.pseudo_element().has_value() && !pseudo_element_supports_property(*abstract_element.pseudo_element(), property.property_id))
                continue;

            auto property_value = property.value;

            if (property_value->is_unresolved())
                property_value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams { abstract_element.document() }, abstract_element, PropertyNameAndID::from_id(property.property_id), property_value->as_unresolved());

            if (property_value->is_guaranteed_invalid()) {
                // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
                // When substitution results in a property’s value containing the guaranteed-invalid value, this makes the
                // declaration invalid at computed-value time. When this happens, the computed value is one of the
                // following depending on the property’s type:

                // -> The property is a non-registered custom property
                // -> The property is a registered custom property with universal syntax
                // FIXME: Process custom properties here?
                if (false) {
                    // The computed value is the guaranteed-invalid value.
                }
                // -> Otherwise
                else {
                    // Either the property’s inherited value or its initial value depending on whether the property is
                    // inherited or not, respectively, as if the property’s value had been specified as the unset keyword.
                    property_value = KeywordStyleValue::create(Keyword::Unset);
                }
            }

            for_each_property_expanding_shorthands(property.property_id, property_value, [&](PropertyID longhand_id, StyleValue const& longhand_value) {
                // If we're a PSV that's already been seen, that should mean that our shorthand already got
                // resolved and gave us a value, so we don't want to overwrite it with a PSV.
                if (seen_properties.get(to_underlying(longhand_id)) && property_value->is_pending_substitution())
                    return;
                seen_properties.set(to_underlying(longhand_id), true);

                PropertyID physical_property_id;

                if (property_is_logical_alias(longhand_id)) {
                    if (!logical_alias_mapping_context.has_value())
                        return;
                    physical_property_id = map_logical_alias_to_physical_property(longhand_id, logical_alias_mapping_context.value());
                } else {
                    physical_property_id = longhand_id;
                }

                if (longhand_value.is_revert()) {
                    cascaded_properties.revert_property(physical_property_id, important, cascade_origin);
                } else if (longhand_value.is_revert_layer()) {
                    cascaded_properties.revert_layer_property(physical_property_id, important, layer_name);
                } else {
                    cascaded_properties.set_property(physical_property_id, longhand_value, important, cascade_origin, layer_name, declaration);
                }
            });
        }
    };

    for (auto const& match : matching_rules) {
        cascade_style_declaration(match->declaration());
    }

    if (cascade_origin == CascadeOrigin::Author && !abstract_element.pseudo_element().has_value()) {
        if (auto const inline_style = abstract_element.element().inline_style()) {
            cascade_style_declaration(*inline_style);
        }
    }
}

static void cascade_custom_properties(DOM::AbstractElement abstract_element, Vector<MatchingRule const*> const& matching_rules, OrderedHashMap<FlyString, StyleProperty>& custom_properties)
{
    size_t needed_capacity = 0;
    for (auto const& matching_rule : matching_rules)
        needed_capacity += matching_rule->declaration().custom_properties().size();

    if (!abstract_element.pseudo_element().has_value()) {
        if (auto const inline_style = abstract_element.element().inline_style())
            needed_capacity += inline_style->custom_properties().size();
    }

    custom_properties.ensure_capacity(custom_properties.size() + needed_capacity);

    OrderedHashMap<FlyString, StyleProperty> important_custom_properties;
    for (auto const& matching_rule : matching_rules) {
        for (auto const& it : matching_rule->declaration().custom_properties()) {
            auto style_value = it.value.value;
            if (style_value->is_revert_layer())
                continue;

            if (it.value.important == Important::Yes) {
                important_custom_properties.set(it.key, it.value);
            }
            custom_properties.set(it.key, it.value);
        }
    }

    if (!abstract_element.pseudo_element().has_value()) {
        if (auto const inline_style = abstract_element.element().inline_style()) {
            for (auto const& it : inline_style->custom_properties()) {
                if (it.value.important == Important::Yes) {
                    important_custom_properties.set(it.key, it.value);
                }
                custom_properties.set(it.key, it.value);
            }
        }
    }

    custom_properties.update(important_custom_properties);
}

void StyleComputer::collect_animation_into(DOM::AbstractElement abstract_element, GC::Ref<Animations::KeyframeEffect> effect, ComputedProperties& computed_properties) const
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
    if (keyframes.size() < 2) {
        if constexpr (LIBWEB_CSS_ANIMATION_DEBUG) {
            dbgln("    Did not find enough keyframes ({} keyframes)", keyframes.size());
            for (auto it = keyframes.begin(); it != keyframes.end(); ++it)
                dbgln("        - {}", it.key());
        }
        return;
    }

    double progress = round(output_progress.value() * 100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor);
    // FIXME: Support progress values outside the range of i64.
    i64 key = 0;
    if (progress > NumericLimits<i64>::max()) {
        key = NumericLimits<i64>::max();
    } else if (progress < NumericLimits<i64>::min()) {
        key = NumericLimits<i64>::min();
    } else {
        key = static_cast<i64>(progress);
    }
    auto keyframe_start_it = [&] {
        if (output_progress.value() <= 0) {
            return keyframes.begin();
        }
        auto potential_match = keyframes.find_largest_not_above_iterator(key);
        auto next = potential_match;
        ++next;
        if (next.is_end()) {
            --potential_match;
        }
        return potential_match;
    }();
    auto keyframe_start = static_cast<i64>(keyframe_start_it.key());
    auto keyframe_values = *keyframe_start_it;

    auto keyframe_end_it = ++keyframe_start_it;
    VERIFY(!keyframe_end_it.is_end());
    auto keyframe_end = static_cast<i64>(keyframe_end_it.key());
    auto keyframe_end_values = *keyframe_end_it;

    auto progress_in_keyframe = (progress - keyframe_start) / static_cast<double>(keyframe_end - keyframe_start);

    if constexpr (LIBWEB_CSS_ANIMATION_DEBUG) {
        auto valid_properties = keyframe_values.properties.size();
        dbgln("Animation {} contains {} properties to interpolate, progress = {}%", animation->id(), valid_properties, progress_in_keyframe * 100);
    }

    // FIXME: Follow https://drafts.csswg.org/web-animations-1/#ref-for-computed-keyframes in whatever the right place is.
    auto compute_keyframe_values = [&computed_properties, &abstract_element, this](auto const& keyframe_values) {
        HashMap<PropertyID, RefPtr<StyleValue const>> result;
        HashMap<PropertyID, PropertyID> longhands_set_by_property_id;
        AK::FixedBitmap<number_of_longhand_properties> property_is_set_by_use_initial(false);

        auto property_is_logical_alias_including_shorthands = [&](PropertyID property_id) {
            if (property_is_shorthand(property_id))
                // NOTE: All expanded longhands for a logical alias shorthand are logical aliases so we only need to check the first one.
                return property_is_logical_alias(expanded_longhands_for_shorthand(property_id)[0]);

            return property_is_logical_alias(property_id);
        };

        // https://drafts.csswg.org/web-animations-1/#ref-for-computed-keyframes
        auto is_property_preferred = [&](PropertyID a, PropertyID b) {
            // If conflicts arise when expanding shorthand properties or replacing logical properties with physical properties, apply the following rules in order until the conflict is resolved:
            // 1. Longhand properties override shorthand properties (e.g. border-top-color overrides border-top).
            if (property_is_shorthand(a) != property_is_shorthand(b))
                return !property_is_shorthand(a);

            // 2. Shorthand properties with fewer longhand components override those with more longhand components (e.g. border-top overrides border-color).
            if (property_is_shorthand(a)) {
                auto number_of_expanded_shorthands_a = expanded_longhands_for_shorthand(a).size();
                auto number_of_expanded_shorthands_b = expanded_longhands_for_shorthand(b).size();

                if (number_of_expanded_shorthands_a != number_of_expanded_shorthands_b)
                    return number_of_expanded_shorthands_a < number_of_expanded_shorthands_b;
            }

            auto property_a_is_logical_alias = property_is_logical_alias_including_shorthands(a);
            auto property_b_is_logical_alias = property_is_logical_alias_including_shorthands(b);

            // 3. Physical properties override logical properties.
            if (property_a_is_logical_alias != property_b_is_logical_alias)
                return !property_a_is_logical_alias;

            // 4. For shorthand properties with an equal number of longhand components, properties whose IDL name (see
            //    the CSS property to IDL attribute algorithm [CSSOM]) appears earlier when sorted in ascending order
            //    by the Unicode codepoints that make up each IDL name, override those who appear later.
            return camel_case_string_from_property_id(a) < camel_case_string_from_property_id(b);
        };

        Length::FontMetrics font_metrics {
            computed_properties.font_size(),
            computed_properties.first_available_computed_font(document().font_computer())->pixel_metrics(),
            computed_properties.line_height()
        };

        HashMap<PropertyID, RefPtr<StyleValue const>> specified_values;

        for (auto const& [property_id, value] : keyframe_values.properties) {
            bool is_use_initial = false;

            auto style_value = value.visit(
                [&](Animations::KeyframeEffect::KeyFrameSet::UseInitial) -> RefPtr<StyleValue const> {
                    if (property_is_shorthand(property_id))
                        return {};
                    is_use_initial = true;
                    return computed_properties.property(property_id, ComputedProperties::WithAnimationsApplied::No);
                },
                [&](RefPtr<StyleValue const> value) -> RefPtr<StyleValue const> {
                    return value;
                });

            if (!style_value) {
                specified_values.set(property_id, nullptr);
                continue;
            }

            // If the style value is a PendingSubstitutionStyleValue we should skip it to avoid overwriting any value
            // already set by resolving the relevant shorthand's value.
            if (style_value->is_pending_substitution())
                continue;

            if (style_value->is_unresolved())
                style_value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams { abstract_element.document() }, abstract_element, PropertyNameAndID::from_id(property_id), style_value->as_unresolved());

            for_each_property_expanding_shorthands(property_id, *style_value, [&](PropertyID longhand_id, StyleValue const& longhand_value) {
                auto physical_longhand_id = map_logical_alias_to_physical_property(longhand_id, LogicalAliasMappingContext { computed_properties.writing_mode(), computed_properties.direction() });
                auto physical_longhand_id_bitmap_index = to_underlying(physical_longhand_id) - to_underlying(first_longhand_property_id);

                // Don't overwrite values if this is the result of a UseInitial
                if (specified_values.contains(physical_longhand_id) && specified_values.get(physical_longhand_id) != nullptr && is_use_initial)
                    return;

                // Don't overwrite unless the value was originally set by a UseInitial or this property is preferred over the one that set it originally
                if (specified_values.contains(physical_longhand_id) && specified_values.get(physical_longhand_id) != nullptr && !property_is_set_by_use_initial.get(physical_longhand_id_bitmap_index) && !is_property_preferred(property_id, longhands_set_by_property_id.get(physical_longhand_id).value()))
                    return;

                auto const& specified_value_with_css_wide_keywords_applied = [&]() -> StyleValue const& {
                    if (longhand_value.is_inherit() || (longhand_value.is_unset() && is_inherited_property(longhand_id))) {
                        if (auto inherited_animated_value = get_animated_inherit_value(longhand_id, abstract_element); inherited_animated_value.has_value())
                            return inherited_animated_value->value;

                        return get_non_animated_inherit_value(longhand_id, abstract_element);
                    }

                    if (longhand_value.is_initial() || longhand_value.is_unset())
                        return property_initial_value(longhand_id);

                    if (longhand_value.is_revert() || longhand_value.is_revert_layer())
                        return computed_properties.property(longhand_id);

                    return longhand_value;
                }();

                longhands_set_by_property_id.set(physical_longhand_id, property_id);
                property_is_set_by_use_initial.set(physical_longhand_id_bitmap_index, is_use_initial);
                specified_values.set(physical_longhand_id, specified_value_with_css_wide_keywords_applied);
            });
        }

        auto const& inheritance_parent = abstract_element.element_to_inherit_style_from();
        auto inheritance_parent_has_computed_properties = inheritance_parent.has_value() && inheritance_parent->computed_properties();
        ComputationContext font_computation_context {
            .length_resolution_context = inheritance_parent_has_computed_properties ? Length::ResolutionContext::for_element(inheritance_parent.value()) : Length::ResolutionContext::for_window(*m_document->window()),
            .abstract_element = abstract_element
        };

        if (auto const& font_size_specified_value = specified_values.get(PropertyID::FontSize); font_size_specified_value.has_value()) {
            // FIXME: We need to respect the math-depth of this computed keyframe if it is present
            auto computed_math_depth = computed_properties.math_depth();
            auto inherited_font_size = inheritance_parent_has_computed_properties ? inheritance_parent->computed_properties()->font_size() : InitialValues::font_size();
            auto inherited_math_depth = inheritance_parent_has_computed_properties ? inheritance_parent->computed_properties()->math_depth() : InitialValues::math_depth();

            auto const& font_size_in_computed_form = compute_font_size(
                *font_size_specified_value.value(),
                computed_math_depth,
                inherited_font_size,
                inherited_math_depth,
                font_computation_context);

            result.set(PropertyID::FontSize, font_size_in_computed_form);
        }

        if (auto const& font_weight_specified_value = specified_values.get(PropertyID::FontWeight); font_weight_specified_value.has_value()) {
            auto inherited_font_weight = inheritance_parent_has_computed_properties ? inheritance_parent->computed_properties()->font_weight() : InitialValues::font_weight();

            auto const& font_weight_in_computed_form = compute_font_weight(
                *font_weight_specified_value.value(),
                inherited_font_weight,
                font_computation_context);

            result.set(PropertyID::FontWeight, font_weight_in_computed_form);
        }

        if (auto const& font_width_specified_value = specified_values.get(PropertyID::FontWidth); font_width_specified_value.has_value())
            result.set(PropertyID::FontWidth, compute_font_width(*font_width_specified_value.value(), font_computation_context));

        if (auto const& font_style_specified_value = specified_values.get(PropertyID::FontStyle); font_style_specified_value.has_value())
            result.set(PropertyID::FontStyle, compute_font_style(*font_style_specified_value.value(), font_computation_context));

        if (auto const& line_height_specified_value = specified_values.get(PropertyID::LineHeight); line_height_specified_value.has_value()) {
            ComputationContext line_height_computation_context {
                .length_resolution_context = {
                    .viewport_rect = viewport_rect(),
                    .font_metrics = {
                        computed_properties.font_size(),
                        computed_properties.first_available_computed_font(document().font_computer())->pixel_metrics(),
                        inheritance_parent_has_computed_properties ? inheritance_parent->computed_properties()->line_height() : InitialValues::line_height() },
                    .root_font_metrics = m_root_element_font_metrics },
                .abstract_element = abstract_element
            };

            result.set(PropertyID::LineHeight, compute_line_height(*line_height_specified_value.value(), line_height_computation_context));
        }

        ComputationContext computation_context {
            .length_resolution_context = {
                .viewport_rect = viewport_rect(),
                .font_metrics = font_metrics,
                .root_font_metrics = m_root_element_font_metrics },
            .abstract_element = abstract_element
        };

        // NOTE: This doesn't necessarily return the specified value if we reach into computed_properties but that
        //       doesn't matter as a computed value is always valid as a specified value.
        Function<NonnullRefPtr<StyleValue const>(PropertyID)> get_property_specified_value = [&](PropertyID property_id) -> NonnullRefPtr<StyleValue const> {
            if (auto keyframe_value = specified_values.get(property_id); keyframe_value.has_value() && keyframe_value.value())
                return *keyframe_value.value();

            return computed_properties.property(property_id);
        };

        for (auto const& [property_id, style_value] : specified_values) {
            if (!style_value)
                continue;

            if (first_is_one_of(property_id, PropertyID::FontSize, PropertyID::FontWeight, PropertyID::FontWidth, PropertyID::FontStyle, PropertyID::LineHeight))
                continue;

            result.set(property_id, compute_value_of_property(property_id, *style_value, get_property_specified_value, computation_context, m_document->page().client().device_pixels_per_css_pixel()));
        }

        return result;
    };
    HashMap<PropertyID, RefPtr<StyleValue const>> computed_start_values = compute_keyframe_values(keyframe_values);
    HashMap<PropertyID, RefPtr<StyleValue const>> computed_end_values = compute_keyframe_values(keyframe_end_values);
    auto to_composite_operation = [&](Bindings::CompositeOperationOrAuto composite_operation_or_auto) {
        switch (composite_operation_or_auto) {
        case Bindings::CompositeOperationOrAuto::Accumulate:
            return Bindings::CompositeOperation::Accumulate;
        case Bindings::CompositeOperationOrAuto::Add:
            return Bindings::CompositeOperation::Add;
        case Bindings::CompositeOperationOrAuto::Replace:
            return Bindings::CompositeOperation::Replace;
        case Bindings::CompositeOperationOrAuto::Auto:
            return effect->composite();
        }
        VERIFY_NOT_REACHED();
    };

    auto is_result_of_transition = animation->is_css_transition() ? AnimatedPropertyResultOfTransition::Yes : AnimatedPropertyResultOfTransition::No;

    auto start_composite_operation = to_composite_operation(keyframe_values.composite);
    auto end_composite_operation = to_composite_operation(keyframe_end_values.composite);

    for (auto const& it : computed_start_values) {
        auto resolved_start_property = it.value;
        RefPtr resolved_end_property = computed_end_values.get(it.key).value_or(nullptr);

        if (!resolved_end_property) {
            if (resolved_start_property) {
                computed_properties.set_animated_property(it.key, *resolved_start_property, is_result_of_transition);
                dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "No end property for property {}, using {}", string_from_property_id(it.key), resolved_start_property->to_string(SerializationMode::Normal));
            }
            continue;
        }

        if (resolved_end_property && !resolved_start_property)
            resolved_start_property = property_initial_value(it.key);

        if (!resolved_start_property || !resolved_end_property)
            continue;

        auto start = resolved_start_property.release_nonnull();
        auto end = resolved_end_property.release_nonnull();

        // OPTIMIZATION: Values resulting from animations other than CSS transitions are overriden by important
        //               properties so there's no need to calculate them
        if (!animation->is_css_transition() && computed_properties.is_property_important(it.key)) {
            continue;
        }

        auto const& underlying_value = computed_properties.property(it.key);
        if (auto composited_start_value = composite_value(it.key, underlying_value, start, start_composite_operation))
            start = *composited_start_value;

        if (auto composited_end_value = composite_value(it.key, underlying_value, end, end_composite_operation))
            end = *composited_end_value;

        if (auto next_value = interpolate_property(*effect->target(), it.key, *start, *end, progress_in_keyframe, AllowDiscrete::Yes)) {
            dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "Interpolated value for property {} at {}: {} -> {} = {}", string_from_property_id(it.key), progress_in_keyframe, start->to_string(SerializationMode::Normal), end->to_string(SerializationMode::Normal), next_value->to_string(SerializationMode::Normal));
            computed_properties.set_animated_property(it.key, *next_value, is_result_of_transition);
        } else {
            // If interpolate_property() fails, the element should not be rendered
            dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "Interpolated value for property {} at {}: {} -> {} is invalid", string_from_property_id(it.key), progress_in_keyframe, start->to_string(SerializationMode::Normal), end->to_string(SerializationMode::Normal));
            computed_properties.set_animated_property(PropertyID::Visibility, KeywordStyleValue::create(Keyword::Hidden), is_result_of_transition);
        }
    }
}

static void apply_animation_properties(DOM::Document const& document, ComputedProperties::AnimationProperties const& animation_properties, Animations::Animation& animation)
{
    VERIFY(animation.effect());

    auto& effect = as<Animations::KeyframeEffect>(*animation.effect());

    effect.set_specified_iteration_duration(animation_properties.duration);
    effect.set_specified_start_delay(animation_properties.delay);
    // https://drafts.csswg.org/web-animations-2/#updating-animationeffect-timing
    // Timing properties may also be updated due to a style change. Any change to a CSS animation property that affects
    // timing requires rerunning the procedure to normalize specified timing.
    effect.normalize_specified_timing();
    effect.set_iteration_count(animation_properties.iteration_count);
    effect.set_timing_function(animation_properties.timing_function);
    effect.set_fill_mode(Animations::css_fill_mode_to_bindings_fill_mode(animation_properties.fill_mode));
    effect.set_playback_direction(Animations::css_animation_direction_to_bindings_playback_direction(animation_properties.direction));
    effect.set_composite(Animations::css_animation_composition_to_bindings_composite_operation(animation_properties.composition));

    if (animation_properties.play_state != animation.last_css_animation_play_state()) {
        if (animation_properties.play_state == CSS::AnimationPlayState::Running && animation.play_state() != Bindings::AnimationPlayState::Running) {
            HTML::TemporaryExecutionContext context(document.realm());
            animation.play().release_value_but_fixme_should_propagate_errors();
        } else if (animation_properties.play_state == CSS::AnimationPlayState::Paused && animation.play_state() != Bindings::AnimationPlayState::Paused) {
            HTML::TemporaryExecutionContext context(document.realm());
            animation.pause().release_value_but_fixme_should_propagate_errors();
        }

        animation.set_last_css_animation_play_state(animation_properties.play_state);
    }
}

// https://drafts.csswg.org/css-animations-1/#animations
void StyleComputer::process_animation_definitions(ComputedProperties const& computed_properties, DOM::AbstractElement& abstract_element) const
{
    auto const& animation_definitions = computed_properties.animations();

    auto& document = abstract_element.document();

    auto* element_animations = abstract_element.css_defined_animations();

    // If we have a nullptr for element_animations it means that the pseudo element was invalid and thus we shouldn't apply animations
    if (!element_animations)
        return;

    HashTable<FlyString> defined_animation_names;

    for (auto const& animation_properties : animation_definitions) {
        defined_animation_names.set(animation_properties.name);

        // Changes to the values of animation properties while the animation is running apply as if the animation had
        // those values from when it began
        if (auto const& existing_animation = element_animations->get(animation_properties.name); existing_animation.has_value()) {
            apply_animation_properties(document, animation_properties, existing_animation.value());
            return;
        }

        // An animation applies to an element if its name appears as one of the identifiers in the computed value of the
        // animation-name property and the animation uses a valid @keyframes rule
        auto animation = CSSAnimation::create(document.realm());
        animation->set_animation_name(animation_properties.name);
        animation->set_timeline(document.timeline());
        animation->set_owning_element(abstract_element);

        auto effect = Animations::KeyframeEffect::create(document.realm());
        animation->set_effect(effect);

        apply_animation_properties(document, animation_properties, animation);

        if (auto const* rule_cache = rule_cache_for_cascade_origin(CascadeOrigin::Author, {}, {})) {
            if (auto keyframe_set = rule_cache->rules_by_animation_keyframes.get(animation_properties.name); keyframe_set.has_value())
                effect->set_key_frame_set(keyframe_set.value());
        }

        effect->set_target(abstract_element);
        abstract_element.set_has_css_defined_animations();
        element_animations->set(animation_properties.name, animation);
    }

    // Once an animation has started it continues until it ends or the animation-name is removed
    for (auto const& existing_animation_name : element_animations->keys()) {
        if (defined_animation_names.contains(existing_animation_name))
            continue;

        element_animations->get(existing_animation_name).value()->cancel(Animations::Animation::ShouldInvalidate::No);
        element_animations->remove(existing_animation_name);
    }
}

static void apply_dimension_attribute(CascadedProperties& cascaded_properties, DOM::Element const& element, FlyString const& attribute_name, CSS::PropertyID property_id)
{
    auto attribute = element.attribute(attribute_name);
    if (!attribute.has_value())
        return;

    auto parsed_value = HTML::parse_dimension_value(*attribute);
    if (!parsed_value)
        return;

    cascaded_properties.set_property_from_presentational_hint(property_id, parsed_value.release_nonnull());
}

static void compute_transitioned_properties(ComputedProperties const& style, DOM::AbstractElement abstract_element)
{
    // FIXME: For now we don't bother registering transitions on the first computation since they can't run (because
    //        there is nothing to transition from) but this will change once we implement @starting-style
    if (!abstract_element.computed_properties())
        return;
    // FIXME: Add transition helpers on AbstractElement.
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();

    element.clear_registered_transitions(pseudo_element);

    auto const& delay = style.property(PropertyID::TransitionDelay);
    auto const& duration = style.property(PropertyID::TransitionDuration);

    auto const value_is_list_containing_a_single_time_of_zero_seconds = [](StyleValue const& value) -> bool {
        if (!value.is_value_list())
            return false;

        auto const& value_list = value.as_value_list().values();

        if (value_list.size() != 1)
            return false;

        if (!value_list[0]->is_time())
            return false;

        return value_list[0]->as_time().time().to_seconds() == 0;
    };

    // OPTIMIZATION: Registered transitions with a "combined duration" of less than or equal to 0s are equivalent to not
    //               having a transition registered at all, except in the case that we already have an associated
    //               transition for that property, so we can skip registering them. This implementation intentionally
    //               ignores some of those cases (e.g. transitions being registered but for other properties, multiple
    //               transitions, negative delays, etc) since it covers the common (initial property values) case and
    //               the other cases are rare enough that the cost of identifying them would likely more than offset any
    //               gains.
    if (
        element.property_ids_with_existing_transitions(pseudo_element).is_empty()
        && value_is_list_containing_a_single_time_of_zero_seconds(delay)
        && value_is_list_containing_a_single_time_of_zero_seconds(duration)) {
        return;
    }

    element.add_transitioned_properties(pseudo_element, style.transitions());
}

// https://drafts.csswg.org/css-transitions/#starting
void StyleComputer::start_needed_transitions(ComputedProperties const& previous_style, ComputedProperties& new_style, DOM::AbstractElement abstract_element) const
{
    // https://drafts.csswg.org/css-transitions/#transition-combined-duration
    auto combined_duration = [](Animations::Animatable::TransitionAttributes const& transition_attributes) {
        // Define the combined duration of the transition as the sum of max(matching transition duration, 0s) and the matching transition delay.
        return max(transition_attributes.duration, 0) + transition_attributes.delay;
    };

    // For each element and property, the implementation must act as follows:
    // NB: We know that a DocumentTimeline's current time is always in milliseconds
    VERIFY(m_document->timeline()->current_time()->type == Animations::TimeValue::Type::Milliseconds);
    auto style_change_event_time = m_document->timeline()->current_time()->value;

    // FIXME: Add some transition helpers to AbstractElement.
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();

    // OPTIMIZATION: Instead of iterating over all properties we split the logic into two loops, one for the properties
    //               which appear in transition-property and one for those which have existing transitions
    for (auto property_id : element.property_ids_with_matching_transition_property_entry(pseudo_element)) {
        auto matching_transition_properties = element.property_transition_attributes(pseudo_element, property_id).value();
        auto const& before_change_value = previous_style.property(property_id, ComputedProperties::WithAnimationsApplied::Yes);
        auto const& after_change_value = new_style.property(property_id, ComputedProperties::WithAnimationsApplied::No);

        auto existing_transition = element.property_transition(pseudo_element, property_id);
        bool has_running_transition = existing_transition && !existing_transition->is_finished() && !existing_transition->is_idle();
        bool has_completed_transition = existing_transition && (existing_transition->is_finished() || existing_transition->is_idle());

        auto start_a_transition = [&](auto delay, auto start_time, auto end_time, auto const& start_value, auto const& end_value, auto const& reversing_adjusted_start_value, auto reversing_shortening_factor) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Starting a transition of {} from {} to {}", string_from_property_id(property_id), start_value.to_string(SerializationMode::Normal), end_value.to_string(SerializationMode::Normal));

            auto transition = CSSTransition::start_a_transition(abstract_element, property_id,
                document().transition_generation(), delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
            // Immediately set the property's value to the transition's current value, to prevent single-frame jumps.
            collect_animation_into(abstract_element, as<Animations::KeyframeEffect>(*transition->effect()), new_style);
        };

        // 1. If all of the following are true:
        if (
            // - the element does not have a running transition for the property,
            (!has_running_transition) &&
            // - there is a matching transition-property value, and
            // NOTE: We only iterate over properties for which this is true
            // - the before-change style is different from the after-change style for that property, and the values for the property are transitionable,
            (!before_change_value.equals(after_change_value) && property_values_are_transitionable(property_id, before_change_value, after_change_value, element, matching_transition_properties.transition_behavior)) &&
            // - the element does not have a completed transition for the property
            //   or the end value of the completed transition is different from the after-change style for the property,
            (!has_completed_transition || !existing_transition->transition_end_value()->equals(after_change_value)) &&
            // - the combined duration is greater than 0s,
            (combined_duration(matching_transition_properties) > 0)) {

            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 1.");

            // then implementations must remove the completed transition (if present) from the set of completed transitions
            if (has_completed_transition)
                element.remove_transition(pseudo_element, property_id);
            // and start a transition whose:

            // AD-HOC: We pass delay to the constructor separately so we can use it to construct the contained KeyframeEffect
            auto delay = matching_transition_properties.delay;

            // - start time is the time of the style change event plus the matching transition delay,
            auto start_time = style_change_event_time;

            // - end time is the start time plus the matching transition duration,
            auto end_time = start_time + matching_transition_properties.duration;

            // - start value is the value of the transitioning property in the before-change style,
            auto const& start_value = before_change_value;

            // - end value is the value of the transitioning property in the after-change style,
            auto const& end_value = after_change_value;

            // - reversing-adjusted start value is the same as the start value, and
            auto const& reversing_adjusted_start_value = start_value;

            // - reversing shortening factor is 1.
            double reversing_shortening_factor = 1;

            start_a_transition(delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
        }

        // 2. Otherwise, if the element has a completed transition for the property
        //    and the end value of the completed transition is different from the after-change style for the property,
        //    then implementations must remove the completed transition from the set of completed transitions.
        else if (has_completed_transition && !existing_transition->transition_end_value()->equals(after_change_value)) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 2.");
            element.remove_transition(pseudo_element, property_id);
        }

        // NOTE: Step 3 is handled in a separate loop below for performance reasons

        // 4. If the element has a running transition for the property,
        //    there is a matching transition-property value,
        //    and the end value of the running transition is not equal to the value of the property in the after-change style, then:
        if (has_running_transition && !existing_transition->transition_end_value()->equals(after_change_value)) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4. existing end value = {}, after change value = {}", existing_transition->transition_end_value()->to_string(SerializationMode::Normal), after_change_value.to_string(SerializationMode::Normal));
            // 1. If the current value of the property in the running transition is equal to the value of the property in the after-change style,
            //    or if these two values are not transitionable,
            //    then implementations must cancel the running transition.
            auto& current_value = new_style.property(property_id, ComputedProperties::WithAnimationsApplied::Yes);
            if (current_value.equals(after_change_value) || !property_values_are_transitionable(property_id, current_value, after_change_value, element, matching_transition_properties.transition_behavior)) {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.1");
                existing_transition->cancel();
            }

            // 2. Otherwise, if the combined duration is less than or equal to 0s,
            //    or if the current value of the property in the running transition is not transitionable with the value of the property in the after-change style,
            //    then implementations must cancel the running transition.
            else if ((combined_duration(matching_transition_properties) <= 0)
                || !property_values_are_transitionable(property_id, current_value, after_change_value, element, matching_transition_properties.transition_behavior)) {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.2");
                existing_transition->cancel();
            }

            // 3. Otherwise, if the reversing-adjusted start value of the running transition is the same as the value of the property in the after-change style
            //    (see the section on reversing of transitions for why these case exists),
            else if (existing_transition->reversing_adjusted_start_value()->equals(after_change_value)) {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.3");
                // implementations must cancel the running transition and start a new transition whose:
                existing_transition->cancel();
                // AD-HOC: Remove the cancelled transition, otherwise it breaks the invariant that there is only one
                // running or completed transition for a property at once.
                element.remove_transition(pseudo_element, property_id);

                // - reversing-adjusted start value is the end value of the running transition,
                auto reversing_adjusted_start_value = existing_transition->transition_end_value();

                // - reversing shortening factor is the absolute value, clamped to the range [0, 1], of the sum of:
                //   1. the output of the timing function of the old transition at the time of the style change event,
                //      times the reversing shortening factor of the old transition
                auto term_1 = existing_transition->timing_function_output_at_time(style_change_event_time) * existing_transition->reversing_shortening_factor();
                //   2. 1 minus the reversing shortening factor of the old transition.
                auto term_2 = 1 - existing_transition->reversing_shortening_factor();
                double reversing_shortening_factor = clamp(abs(term_1 + term_2), 0.0, 1.0);

                // AD-HOC: We pass delay to the constructor separately so we can use it to construct the contained KeyframeEffect
                auto delay = (matching_transition_properties.delay >= 0
                        ? (matching_transition_properties.delay)
                        : (reversing_shortening_factor * matching_transition_properties.delay));

                // - start time is the time of the style change event plus:
                //   1. if the matching transition delay is nonnegative, the matching transition delay, or
                //   2. if the matching transition delay is negative, the product of the new transition’s reversing shortening factor and the matching transition delay,
                auto start_time = style_change_event_time;

                // - end time is the start time plus the product of the matching transition duration and the new transition’s reversing shortening factor,
                auto end_time = start_time + (matching_transition_properties.duration * reversing_shortening_factor);

                // - start value is the current value of the property in the running transition,
                auto const& start_value = current_value;

                // - end value is the value of the property in the after-change style,
                auto const& end_value = after_change_value;

                start_a_transition(delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
            }

            // 4. Otherwise,
            else {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.4");
                // implementations must cancel the running transition and start a new transition whose:
                existing_transition->cancel();
                // AD-HOC: Remove the cancelled transition, otherwise it breaks the invariant that there is only one
                // running or completed transition for a property at once.
                element.remove_transition(pseudo_element, property_id);

                // AD-HOC: We pass delay to the constructor separately so we can use it to construct the contained KeyframeEffect
                auto delay = matching_transition_properties.delay;

                // - start time is the time of the style change event plus the matching transition delay,
                auto start_time = style_change_event_time;

                // - end time is the start time plus the matching transition duration,
                auto end_time = start_time + matching_transition_properties.duration;

                // - start value is the current value of the property in the running transition,
                auto const& start_value = current_value;

                // - end value is the value of the property in the after-change style,
                auto const& end_value = after_change_value;

                // - reversing-adjusted start value is the same as the start value, and
                auto const& reversing_adjusted_start_value = start_value;

                // - reversing shortening factor is 1.
                double reversing_shortening_factor = 1;

                start_a_transition(delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
            }
        }
    }

    for (auto property_id : element.property_ids_with_existing_transitions(pseudo_element)) {
        // 3. If the element has a running transition or completed transition for the property, and there is not a
        //    matching transition-property value, then implementations must cancel the running transition or remove the
        //    completed transition from the set of completed transitions.
        if (element.property_transition_attributes(pseudo_element, property_id).has_value())
            continue;

        auto const& existing_transition = element.property_transition(pseudo_element, property_id);

        dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 3.");
        if (!existing_transition->is_finished() && !existing_transition->is_idle())
            existing_transition->cancel();
        else
            element.remove_transition(pseudo_element, property_id);
    }
}

StyleComputer::MatchingRuleSet StyleComputer::build_matching_rule_set(DOM::AbstractElement abstract_element, PseudoClassBitmap& attempted_pseudo_class_matches, bool& did_match_any_pseudo_element_rules, ComputeStyleMode mode, StyleScope const& style_scope) const
{
    // First, we collect all the CSS rules whose selectors match `element`:
    MatchingRuleSet matching_rule_set;
    matching_rule_set.user_agent_rules = collect_matching_rules(abstract_element, CascadeOrigin::UserAgent, attempted_pseudo_class_matches);
    sort_matching_rules(matching_rule_set.user_agent_rules);
    matching_rule_set.user_rules = collect_matching_rules(abstract_element, CascadeOrigin::User, attempted_pseudo_class_matches);
    sort_matching_rules(matching_rule_set.user_rules);

    // @layer-ed author rules
    for (auto const& layer_name : style_scope.m_qualified_layer_names_in_order) {
        auto layer_rules = collect_matching_rules(abstract_element, CascadeOrigin::Author, attempted_pseudo_class_matches, layer_name);
        sort_matching_rules(layer_rules);
        matching_rule_set.author_rules.append({ layer_name, layer_rules });
    }
    // Un-@layer-ed author rules
    auto unlayered_author_rules = collect_matching_rules(abstract_element, CascadeOrigin::Author, attempted_pseudo_class_matches);
    sort_matching_rules(unlayered_author_rules);
    matching_rule_set.author_rules.append({ {}, unlayered_author_rules });

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        VERIFY(abstract_element.pseudo_element().has_value());
        did_match_any_pseudo_element_rules = !matching_rule_set.author_rules.is_empty()
            || !matching_rule_set.user_rules.is_empty()
            || !matching_rule_set.user_agent_rules.is_empty();
    }
    return matching_rule_set;
}

// https://www.w3.org/TR/css-cascade/#cascading
// https://drafts.csswg.org/css-cascade-5/#layering
GC::Ref<CascadedProperties> StyleComputer::compute_cascaded_values(DOM::AbstractElement abstract_element, bool did_match_any_pseudo_element_rules, ComputeStyleMode mode, MatchingRuleSet const& matching_rule_set, Optional<LogicalAliasMappingContext> logical_alias_mapping_context, ReadonlySpan<PropertyID> properties_to_cascade) const
{
    auto cascaded_properties = m_document->heap().allocate<CascadedProperties>();
    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        if (!did_match_any_pseudo_element_rules)
            return cascaded_properties;
    }

    // Normal user agent declarations
    cascade_declarations(*cascaded_properties, abstract_element, matching_rule_set.user_agent_rules, CascadeOrigin::UserAgent, Important::No, {}, logical_alias_mapping_context, properties_to_cascade);

    // Normal user declarations
    cascade_declarations(*cascaded_properties, abstract_element, matching_rule_set.user_rules, CascadeOrigin::User, Important::No, {}, logical_alias_mapping_context, properties_to_cascade);

    // Author presentational hints
    // The spec calls this a special "Author presentational hint origin":
    // "For the purpose of cascading this author presentational hint origin is treated as an independent origin;
    // however for the purpose of the revert keyword (but not for the revert-layer keyword) it is considered
    // part of the author origin."
    // https://drafts.csswg.org/css-cascade-5/#author-presentational-hint-origin
    if (!abstract_element.pseudo_element().has_value()) {
        auto& element = abstract_element.element();
        element.apply_presentational_hints(cascaded_properties);
        if (element.supports_dimension_attributes()) {
            apply_dimension_attribute(cascaded_properties, element, HTML::AttributeNames::width, CSS::PropertyID::Width);
            apply_dimension_attribute(cascaded_properties, element, HTML::AttributeNames::height, CSS::PropertyID::Height);
        }

        // SVG presentation attributes are parsed as CSS values, so we need to handle potential custom properties here.
        if (element.is_svg_element())
            cascaded_properties->resolve_unresolved_properties(abstract_element);
    }

    // Normal author declarations, ordered by @layer, with un-@layer-ed rules last
    for (auto const& layer : matching_rule_set.author_rules) {
        cascade_declarations(cascaded_properties, abstract_element, layer.rules, CascadeOrigin::Author, Important::No, layer.qualified_layer_name, logical_alias_mapping_context, properties_to_cascade);
    }

    // Important author declarations, with un-@layer-ed rules first, followed by each @layer in reverse order.
    for (auto const& layer : matching_rule_set.author_rules.in_reverse()) {
        cascade_declarations(cascaded_properties, abstract_element, layer.rules, CascadeOrigin::Author, Important::Yes, {}, logical_alias_mapping_context, properties_to_cascade);
    }

    // Important user declarations
    cascade_declarations(cascaded_properties, abstract_element, matching_rule_set.user_rules, CascadeOrigin::User, Important::Yes, {}, logical_alias_mapping_context, properties_to_cascade);

    // Important user agent declarations
    cascade_declarations(cascaded_properties, abstract_element, matching_rule_set.user_agent_rules, CascadeOrigin::UserAgent, Important::Yes, {}, logical_alias_mapping_context, properties_to_cascade);

    // Transition declarations [css-transitions-1]
    // Note that we have to do these after finishing computing the style,
    // so they're not done here, but as the final step in compute_properties()

    return cascaded_properties;
}

NonnullRefPtr<StyleValue const> StyleComputer::get_non_animated_inherit_value(PropertyID property_id, DOM::AbstractElement abstract_element)
{
    auto parent_element = abstract_element.element_to_inherit_style_from();

    if (!parent_element.has_value() || !parent_element->computed_properties())
        return property_initial_value(property_id);

    return parent_element->computed_properties()->property(property_id, ComputedProperties::WithAnimationsApplied::No);
}

Optional<StyleComputer::AnimatedInheritValue> StyleComputer::get_animated_inherit_value(PropertyID property_id, DOM::AbstractElement abstract_element)
{
    auto parent_element = abstract_element.element_to_inherit_style_from();

    if (!parent_element.has_value() || !parent_element->computed_properties())
        return {};

    if (auto animated_value = parent_element->computed_properties()->animated_property_values().get(property_id); animated_value.has_value())
        return AnimatedInheritValue {
            .value = *animated_value.value(),
            .is_result_of_transition = parent_element->computed_properties()->is_animated_property_result_of_transition(property_id)
                ? AnimatedPropertyResultOfTransition::Yes
                : AnimatedPropertyResultOfTransition::No
        };

    return {};
}

Length::FontMetrics StyleComputer::calculate_root_element_font_metrics(ComputedProperties const& style) const
{
    auto const& root_value = style.property(CSS::PropertyID::FontSize);

    auto font_pixel_metrics = style.first_available_computed_font(document().font_computer())->pixel_metrics();
    Length::FontMetrics font_metrics { m_default_font_metrics.font_size, font_pixel_metrics, InitialValues::line_height() };
    font_metrics.font_size = root_value.as_length().length().to_px(viewport_rect(), font_metrics, font_metrics);
    font_metrics.line_height = style.line_height();

    return font_metrics;
}

CSSPixels StyleComputer::default_user_font_size()
{
    // FIXME: This value should be configurable by the user.
    return 16;
}

// https://w3c.github.io/csswg-drafts/css-fonts/#absolute-size-mapping
CSSPixels StyleComputer::absolute_size_mapping(AbsoluteSize absolute_size, CSSPixels default_font_size)
{
    // An <absolute-size> keyword refers to an entry in a table of font sizes computed and kept by the user agent. See
    // § 2.5.1 Absolute Size Keyword Mapping Table.
    switch (absolute_size) {
    case AbsoluteSize::XxSmall:
        return default_font_size * CSSPixels(3) / 5;
    case AbsoluteSize::XSmall:
        return default_font_size * CSSPixels(3) / 4;
    case AbsoluteSize::Small:
        return default_font_size * CSSPixels(8) / 9;
    case AbsoluteSize::Medium:
        return default_font_size;
    case AbsoluteSize::Large:
        return default_font_size * CSSPixels(6) / 5;
    case AbsoluteSize::XLarge:
        return default_font_size * CSSPixels(3) / 2;
    case AbsoluteSize::XxLarge:
        return default_font_size * 2;
    case AbsoluteSize::XxxLarge:
        return default_font_size * 3;
    }

    VERIFY_NOT_REACHED();
}

// https://drafts.csswg.org/css-fonts/#font-size-prop
CSSPixels StyleComputer::relative_size_mapping(RelativeSize relative_size, CSSPixels inherited_font_size)
{
    // A <relative-size> keyword is interpreted relative to the computed font-size of the parent element and possibly
    // the table of font sizes.

    // If the parent element has a keyword font size in the absolute size keyword mapping table, larger may compute the
    // font size to the next entry in the table, and smaller may compute the font size to the previous entry in the
    // table. For example, if the parent element has a font size of font-size:medium, specifying a value of larger may
    // make the font size of the child element font-size:large.

    // Instead of using next and previous items in the previous keyword table, User agents may instead use a simple
    // ratio to increase or decrease the font size relative to the parent element. The specific ratio is unspecified,
    // but should be around 1.2–1.5. This ratio may vary across different elements.
    switch (relative_size) {
    case RelativeSize::Smaller:
        return inherited_font_size * CSSPixels(4) / 5;
    case RelativeSize::Larger:
        return inherited_font_size * CSSPixels(5) / 4;
    }
    VERIFY_NOT_REACHED();
}

void StyleComputer::compute_font(ComputedProperties& style, Optional<DOM::AbstractElement> abstract_element) const
{
    auto const& inheritance_parent = abstract_element.has_value() ? abstract_element->element_to_inherit_style_from() : OptionalNone {};

    auto inheritance_parent_has_computed_properties = inheritance_parent.has_value() && inheritance_parent->computed_properties();

    auto inherited_font_size = inheritance_parent_has_computed_properties ? inheritance_parent->computed_properties()->font_size() : InitialValues::font_size();
    auto inherited_math_depth = inheritance_parent_has_computed_properties ? inheritance_parent->computed_properties()->math_depth() : InitialValues::math_depth();
    auto inherited_math_style = inheritance_parent_has_computed_properties ? inheritance_parent->computed_properties()->math_style() : InitialValues::math_style();

    ComputationContext font_computation_context {
        .length_resolution_context = inheritance_parent_has_computed_properties ? Length::ResolutionContext::for_element(inheritance_parent.value()) : Length::ResolutionContext::for_window(*m_document->window()),
        .abstract_element = abstract_element
    };

    auto const& math_depth_specified_value = style.property(PropertyID::MathDepth, ComputedProperties::WithAnimationsApplied::No);
    style.set_property_without_modifying_flags(
        PropertyID::MathDepth,
        compute_math_depth(math_depth_specified_value, inherited_math_depth, inherited_math_style, font_computation_context));

    auto const& font_size_specified_value = style.property(PropertyID::FontSize, ComputedProperties::WithAnimationsApplied::No);

    style.set_property_without_modifying_flags(
        PropertyID::FontSize,
        compute_font_size(font_size_specified_value, style.math_depth(), inherited_font_size, inherited_math_depth, font_computation_context));

    auto inherited_font_weight = inheritance_parent_has_computed_properties ? inheritance_parent->computed_properties()->font_weight() : InitialValues::font_weight();

    auto const& font_weight_specified_value = style.property(PropertyID::FontWeight, ComputedProperties::WithAnimationsApplied::No);

    style.set_property_without_modifying_flags(
        PropertyID::FontWeight,
        compute_font_weight(font_weight_specified_value, inherited_font_weight, font_computation_context));

    auto const& font_width_specified_value = style.property(PropertyID::FontWidth, ComputedProperties::WithAnimationsApplied::No);

    style.set_property_without_modifying_flags(
        PropertyID::FontWidth,
        compute_font_width(font_width_specified_value, font_computation_context));

    auto const& font_style_specified_value = style.property(PropertyID::FontStyle, ComputedProperties::WithAnimationsApplied::No);

    style.set_property_without_modifying_flags(
        PropertyID::FontStyle,
        compute_font_style(font_style_specified_value, font_computation_context));

    auto const& font_variation_settings_value = style.property(PropertyID::FontVariationSettings, ComputedProperties::WithAnimationsApplied::No);

    style.set_property_without_modifying_flags(
        PropertyID::FontVariationSettings,
        compute_font_feature_tag_value_list(font_variation_settings_value, font_computation_context));

    RefPtr<Gfx::Font const> const found_font = style.first_available_computed_font(m_document->font_computer());

    Length::FontMetrics line_height_font_metrics {
        style.font_size(),
        found_font->pixel_metrics(),
        inheritance_parent_has_computed_properties ? inheritance_parent->computed_properties()->line_height() : InitialValues::line_height()
    };

    ComputationContext line_height_computation_context {
        .length_resolution_context = {
            .viewport_rect = viewport_rect(),
            .font_metrics = line_height_font_metrics,
            .root_font_metrics = abstract_element.has_value() && is<HTML::HTMLHtmlElement>(abstract_element->element()) ? line_height_font_metrics : m_root_element_font_metrics,
        },
        .abstract_element = abstract_element
    };

    auto const& line_height_specified_value = style.property(CSS::PropertyID::LineHeight, ComputedProperties::WithAnimationsApplied::No);

    style.set_property_without_modifying_flags(
        PropertyID::LineHeight,
        compute_line_height(line_height_specified_value, line_height_computation_context));

    if (abstract_element.has_value() && is<HTML::HTMLHtmlElement>(abstract_element->element())) {
        const_cast<StyleComputer&>(*this).m_root_element_font_metrics = calculate_root_element_font_metrics(style);
    }
}

LogicalAliasMappingContext StyleComputer::compute_logical_alias_mapping_context(DOM::AbstractElement abstract_element, ComputeStyleMode mode, MatchingRuleSet const& matching_rule_set) const
{
    auto normalize_value = [&](auto property_id, auto value) {
        if (!value || value->is_inherit() || value->is_unset()) {
            if (auto const inheritance_parent = abstract_element.element_to_inherit_style_from(); inheritance_parent.has_value()) {
                value = inheritance_parent->computed_properties()->property(property_id);
            } else {
                value = property_initial_value(property_id);
            }
        }

        if (value->is_initial())
            value = property_initial_value(property_id);

        return value;
    };

    bool did_match_any_pseudo_element_rules = false;

    static Array<PropertyID, 2> properties_to_cascade {
        PropertyID::WritingMode,
        PropertyID::Direction,
    };
    auto cascaded_properties = compute_cascaded_values(
        abstract_element,
        did_match_any_pseudo_element_rules,
        mode, matching_rule_set,
        {},
        properties_to_cascade);

    auto writing_mode = normalize_value(PropertyID::WritingMode, cascaded_properties->property(PropertyID::WritingMode));
    auto direction = normalize_value(PropertyID::Direction, cascaded_properties->property(PropertyID::Direction));

    return LogicalAliasMappingContext {
        .writing_mode = keyword_to_writing_mode(writing_mode->to_keyword()).release_value(),
        .direction = keyword_to_direction(direction->to_keyword()).release_value()
    };
}

void StyleComputer::compute_property_values(ComputedProperties& style, Optional<DOM::AbstractElement> abstract_element) const
{
    Length::FontMetrics font_metrics {
        style.font_size(),
        style.first_available_computed_font(document().font_computer())->pixel_metrics(),
        style.line_height()
    };

    ComputationContext computation_context {
        .length_resolution_context = {
            .viewport_rect = viewport_rect(),
            .font_metrics = font_metrics,
            .root_font_metrics = m_root_element_font_metrics,
        },
        .abstract_element = abstract_element
    };

    // NOTE: This doesn't necessarily return the specified value if we have already computed this property but that
    //       doesn't matter as a computed value is always valid as a specified value.
    Function<NonnullRefPtr<StyleValue const>(PropertyID)> const get_property_specified_value = [&](auto property_id) -> NonnullRefPtr<StyleValue const> {
        return style.property(property_id);
    };

    auto device_pixels_per_css_pixel = m_document->page().client().device_pixels_per_css_pixel();
    style.for_each_property([&](PropertyID property_id, auto& specified_value) {
        auto const& computed_value = compute_value_of_property(property_id, specified_value, get_property_specified_value, computation_context, device_pixels_per_css_pixel);

        style.set_property_without_modifying_flags(property_id, computed_value);
    });

    style.set_display_before_box_type_transformation(style.display());
}

void StyleComputer::resolve_effective_overflow_values(ComputedProperties& style) const
{
    // https://www.w3.org/TR/css-overflow-3/#overflow-control
    // The visible/clip values of overflow compute to auto/hidden (respectively) if one of overflow-x or
    // overflow-y is neither visible nor clip.
    auto overflow_x = keyword_to_overflow(style.property(PropertyID::OverflowX).to_keyword());
    auto overflow_y = keyword_to_overflow(style.property(PropertyID::OverflowY).to_keyword());
    auto overflow_x_is_visible_or_clip = overflow_x == Overflow::Visible || overflow_x == Overflow::Clip;
    auto overflow_y_is_visible_or_clip = overflow_y == Overflow::Visible || overflow_y == Overflow::Clip;
    if (!overflow_x_is_visible_or_clip || !overflow_y_is_visible_or_clip) {
        if (overflow_x == CSS::Overflow::Visible)
            style.set_property(CSS::PropertyID::OverflowX, KeywordStyleValue::create(Keyword::Auto));
        if (overflow_x == CSS::Overflow::Clip)
            style.set_property(CSS::PropertyID::OverflowX, KeywordStyleValue::create(Keyword::Hidden));
        if (overflow_y == CSS::Overflow::Visible)
            style.set_property(CSS::PropertyID::OverflowY, KeywordStyleValue::create(Keyword::Auto));
        if (overflow_y == CSS::Overflow::Clip)
            style.set_property(CSS::PropertyID::OverflowY, KeywordStyleValue::create(Keyword::Hidden));
    }
}

static void compute_text_align(ComputedProperties& style, DOM::AbstractElement abstract_element)
{
    auto text_align_keyword = style.property(PropertyID::TextAlign).to_keyword();

    // https://drafts.csswg.org/css-text-4/#valdef-text-align-match-parent
    // This value behaves the same as inherit (computes to its parent’s computed value) except that an inherited
    // value of start or end is interpreted against the parent’s direction value and results in a computed value of
    // either left or right. Computes to start when specified on the root element.
    if (text_align_keyword == Keyword::MatchParent) {
        if (auto const parent = abstract_element.element_to_inherit_style_from(); parent.has_value()) {
            auto const& parent_text_align = parent->computed_properties()->property(PropertyID::TextAlign);
            auto const& parent_direction = parent->computed_properties()->direction();
            switch (parent_text_align.to_keyword()) {
            case Keyword::Start:
                if (parent_direction == Direction::Ltr) {
                    style.set_property(PropertyID::TextAlign, KeywordStyleValue::create(Keyword::Left));
                } else {
                    style.set_property(PropertyID::TextAlign, KeywordStyleValue::create(Keyword::Right));
                }
                break;

            case Keyword::End:
                if (parent_direction == Direction::Ltr) {
                    style.set_property(PropertyID::TextAlign, KeywordStyleValue::create(Keyword::Right));
                } else {
                    style.set_property(PropertyID::TextAlign, KeywordStyleValue::create(Keyword::Left));
                }
                break;

            default:
                style.set_property(PropertyID::TextAlign, parent_text_align);
            }
        } else {
            style.set_property(PropertyID::TextAlign, KeywordStyleValue::create(Keyword::Start));
        }
    }

    // AD-HOC: The -libweb-inherit-or-center style defaults to centering, unless a style value usually would have been
    //         inherited. This is used to support the ad-hoc default <th> text-align behavior.
    if (text_align_keyword == Keyword::LibwebInheritOrCenter && abstract_element.element().local_name() == HTML::TagNames::th) {
        for (auto parent_element = abstract_element.element_to_inherit_style_from(); parent_element.has_value(); parent_element = parent_element->element_to_inherit_style_from()) {
            auto parent_computed = parent_element->computed_properties();
            auto parent_cascaded = parent_element->cascaded_properties();
            if (!parent_computed || !parent_cascaded)
                break;
            if (parent_cascaded->property(PropertyID::TextAlign)) {
                auto const& style_value = parent_computed->property(PropertyID::TextAlign);
                style.set_property(PropertyID::TextAlign, style_value, ComputedProperties::Inherited::Yes);
                break;
            }
        }
    }
}

enum class BoxTypeTransformation {
    None,
    Blockify,
    Inlinify,
};

static BoxTypeTransformation required_box_type_transformation(ComputedProperties const& style, DOM::AbstractElement abstract_element)
{
    // NOTE: We never blockify <br> elements. They are always inline.
    //       There is currently no way to express in CSS how a <br> element really behaves.
    //       Spec issue: https://github.com/whatwg/html/issues/2291
    if (!abstract_element.pseudo_element().has_value() && is<HTML::HTMLBRElement>(abstract_element.element()))
        return BoxTypeTransformation::None;

    // Absolute positioning or floating an element blockifies the box’s display type. [CSS2]
    if (style.position() == Positioning::Absolute || style.position() == Positioning::Fixed || style.float_() != Float::None)
        return BoxTypeTransformation::Blockify;

    // FIXME: Containment in a ruby container inlinifies the box’s display type, as described in [CSS-RUBY-1].

    // NOTE: If we're computing style for a pseudo-element, the effective parent will be the originating element itself, not its parent.
    auto parent = abstract_element.element_to_inherit_style_from();

    // Climb out of `display: contents` context.
    while (parent.has_value() && parent->computed_properties() && parent->computed_properties()->display().is_contents())
        parent = parent->element_to_inherit_style_from();

    // A parent with a grid or flex display value blockifies the box’s display type. [CSS-GRID-1] [CSS-FLEXBOX-1]
    if (parent.has_value() && parent->computed_properties()) {
        auto const& parent_display = parent->computed_properties()->display();
        if (parent_display.is_grid_inside() || parent_display.is_flex_inside())
            return BoxTypeTransformation::Blockify;
    }

    return BoxTypeTransformation::None;
}

// https://drafts.csswg.org/css-display/#transformations
void StyleComputer::transform_box_type_if_needed(ComputedProperties& style, DOM::AbstractElement abstract_element) const
{
    // 2.7. Automatic Box Type Transformations

    // Some layout effects require blockification or inlinification of the box type,
    // which sets the box’s computed outer display type to block or inline (respectively).
    // (This has no effect on display types that generate no box at all, such as none or contents.)

    auto display = style.display();

    if (display.is_none() || (display.is_contents() && !abstract_element.element().is_document_element()))
        return;

    // https://drafts.csswg.org/css-display/#root
    // The root element’s display type is always blockified, and its principal box always establishes an independent formatting context.
    if (abstract_element.element().is_document_element() && !display.is_block_outside()) {
        style.set_property(PropertyID::Display, DisplayStyleValue::create(Display::from_short(Display::Short::Block)));
        return;
    }

    auto new_display = display;

    if (display.is_math_inside()) {
        // https://w3c.github.io/mathml-core/#new-display-math-value
        // For elements that are not MathML elements, if the specified value of display is inline math or block math
        // then the computed value is block flow and inline flow respectively.
        if (abstract_element.element().namespace_uri() != Namespace::MathML)
            new_display = Display { display.outside(), DisplayInside::Flow };
        // For the mtable element the computed value is block table and inline table respectively.
        else if (abstract_element.element().tag_name().equals_ignoring_ascii_case("mtable"sv))
            new_display = Display { display.outside(), DisplayInside::Table };
        // For the mtr element, the computed value is table-row.
        else if (abstract_element.element().tag_name().equals_ignoring_ascii_case("mtr"sv))
            new_display = Display { DisplayInternal::TableRow };
        // For the mtd element, the computed value is table-cell.
        else if (abstract_element.element().tag_name().equals_ignoring_ascii_case("mtd"sv))
            new_display = Display { DisplayInternal::TableCell };
    }

    switch (required_box_type_transformation(style, abstract_element)) {
    case BoxTypeTransformation::None:
        break;
    case BoxTypeTransformation::Blockify:
        if (display.is_block_outside())
            return;
        // If a layout-internal box is blockified, its inner display type converts to flow so that it becomes a block container.
        if (display.is_internal()) {
            new_display = Display::from_short(Display::Short::Block);
        } else {
            VERIFY(display.is_outside_and_inside());

            // For legacy reasons, if an inline block box (inline flow-root) is blockified, it becomes a block box (losing its flow-root nature).
            // For consistency, a run-in flow-root box also blockifies to a block box.
            if (display.is_inline_block()) {
                new_display = Display { DisplayOutside::Block, DisplayInside::Flow, display.list_item() };
            } else {
                new_display = Display { DisplayOutside::Block, display.inside(), display.list_item() };
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
                new_display = Display { DisplayOutside::Inline, DisplayInside::FlowRoot, display.list_item() };
            }

            new_display = Display { DisplayOutside::Inline, display.inside(), display.list_item() };
        }
        break;
    }

    if (new_display != display)
        style.set_property(PropertyID::Display, DisplayStyleValue::create(new_display));
}

GC::Ref<ComputedProperties> StyleComputer::create_document_style() const
{
    auto style = document().heap().allocate<CSS::ComputedProperties>();
    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        style->set_property(property_id, property_initial_value(property_id));
    }

    compute_font(style, {});
    compute_property_values(style, {});
    style->set_property(CSS::PropertyID::Width, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().width())));
    style->set_property(CSS::PropertyID::Height, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().height())));
    style->set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::Block)));
    return style;
}

GC::Ref<ComputedProperties> StyleComputer::compute_style(DOM::AbstractElement abstract_element, Optional<bool&> did_change_custom_properties) const
{
    auto& style_scope = abstract_element.style_scope();
    return *compute_style_impl(abstract_element, ComputeStyleMode::Normal, did_change_custom_properties, style_scope);
}

GC::Ptr<ComputedProperties> StyleComputer::compute_pseudo_element_style_if_needed(DOM::AbstractElement abstract_element, Optional<bool&> did_change_custom_properties) const
{
    auto& style_scope = abstract_element.style_scope();
    return compute_style_impl(abstract_element, ComputeStyleMode::CreatePseudoElementStyleIfNeeded, did_change_custom_properties, style_scope);
}

GC::Ptr<ComputedProperties> StyleComputer::compute_style_impl(DOM::AbstractElement abstract_element, ComputeStyleMode mode, Optional<bool&> did_change_custom_properties, StyleScope const& style_scope) const
{
    style_scope.build_rule_cache_if_needed();

    // Special path for elements that represent a pseudo-element in some element's internal shadow tree.
    if (abstract_element.element().use_pseudo_element().has_value()) {
        auto& element = abstract_element.element();
        auto& host_element = *element.root().parent_or_shadow_host_element();

        // We have to decide where to inherit from. If the pseudo-element has a parent element,
        // we inherit from that. Otherwise, we inherit from the host element in the light DOM.
        DOM::AbstractElement abstract_element_for_pseudo_element { host_element, element.use_pseudo_element() };
        if (auto parent_element = element.parent_element())
            abstract_element_for_pseudo_element.set_inheritance_override(*parent_element);
        else
            abstract_element_for_pseudo_element.set_inheritance_override(host_element);

        auto style = compute_style(abstract_element_for_pseudo_element);

        // Merge back inline styles
        if (auto inline_style = element.inline_style()) {
            for (auto const& property : inline_style->properties())
                style->set_property(property.property_id, property.value);
        }
        abstract_element.element().adjust_computed_style(style);
        return style;
    }

    ScopeGuard guard { [&abstract_element]() { abstract_element.element().set_needs_style_update(false); } };

    // 1. Perform the cascade. This produces the "specified style"
    bool did_match_any_pseudo_element_rules = false;
    PseudoClassBitmap attempted_pseudo_class_matches;
    auto matching_rule_set = build_matching_rule_set(abstract_element, attempted_pseudo_class_matches, did_match_any_pseudo_element_rules, mode, style_scope);

    auto old_custom_properties = abstract_element.custom_properties();

    // Resolve all the CSS custom properties ("variables") for this element:
    if (!abstract_element.pseudo_element().has_value() || pseudo_element_supports_property(*abstract_element.pseudo_element(), PropertyID::Custom)) {
        OrderedHashMap<FlyString, StyleProperty> custom_properties;
        for (auto& layer : matching_rule_set.author_rules) {
            cascade_custom_properties(abstract_element, layer.rules, custom_properties);
        }
        abstract_element.set_custom_properties(move(custom_properties));
    }

    auto logical_alias_mapping_context = compute_logical_alias_mapping_context(abstract_element, mode, matching_rule_set);
    auto cascaded_properties = compute_cascaded_values(abstract_element, did_match_any_pseudo_element_rules, mode, matching_rule_set, logical_alias_mapping_context, {});
    abstract_element.set_cascaded_properties(cascaded_properties);

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        // NOTE: If we're computing style for a pseudo-element, we look for a number of reasons to bail early.

        // Bail if no pseudo-element rules matched.
        if (!did_match_any_pseudo_element_rules)
            return {};

        // Bail if no pseudo-element would be generated due to...
        // - content: none
        // - content: normal (for ::before and ::after)
        bool content_is_normal = false;
        if (auto content_value = cascaded_properties->property(CSS::PropertyID::Content)) {
            if (content_value->is_keyword()) {
                auto content = content_value->as_keyword().keyword();
                if (content == CSS::Keyword::None)
                    return {};
                content_is_normal = content == CSS::Keyword::Normal;
            } else {
                content_is_normal = false;
            }
        } else {
            // NOTE: `normal` is the initial value, so the absence of a value is treated as `normal`.
            content_is_normal = true;
        }
        if (content_is_normal && first_is_one_of(*abstract_element.pseudo_element(), CSS::PseudoElement::Before, CSS::PseudoElement::After)) {
            return {};
        }
    }

    auto computed_properties = compute_properties(abstract_element, cascaded_properties);
    computed_properties->set_attempted_pseudo_class_matches(attempted_pseudo_class_matches);

    if (did_change_custom_properties.has_value() && abstract_element.custom_properties() != old_custom_properties) {
        *did_change_custom_properties = true;
    }

    return computed_properties;
}

static bool is_monospace(StyleValue const& value)
{
    if (!value.is_value_list())
        return false;

    auto const& values = value.as_value_list().values();

    return values.size() == 1 && values[0]->to_keyword() == Keyword::Monospace;
}

// HACK: This function implements time-travelling inheritance for the font-size property
//       in situations where the cascade ended up with `font-family: monospace`.
//       In such cases, other browsers will magically change the meaning of keyword font sizes
//       *even in earlier stages of the cascade!!* to be relative to the default monospace font size (13px)
//       instead of the default font size (16px).
//       See this blog post for a lot more details about this weirdness:
//       https://manishearth.github.io/blog/2017/08/10/font-size-an-unexpectedly-complex-css-property/
RefPtr<StyleValue const> StyleComputer::recascade_font_size_if_needed(DOM::AbstractElement abstract_element, CascadedProperties& cascaded_properties) const
{
    // Check for `font-family: monospace`. Note that `font-family: monospace, AnythingElse` does not trigger this path.
    // Some CSS frameworks use `font-family: monospace, monospace` to work around this behavior.
    auto font_family_value = cascaded_properties.property(CSS::PropertyID::FontFamily);
    if (!font_family_value || !is_monospace(*font_family_value))
        return nullptr;

    // FIXME: This should be configurable.
    constexpr CSSPixels default_monospace_font_size_in_px = 13;
    static auto monospace_font_family_name = Platform::FontPlugin::the().generic_font_name(Platform::GenericFont::Monospace, 400, 0);
    static auto monospace_font = Gfx::FontDatabase::the().get(monospace_font_family_name, default_monospace_font_size_in_px * 0.75f, 400, Gfx::FontWidth::Normal, 0);

    // Reconstruct the line of ancestor elements we need to inherit style from, and then do the cascade again
    // but only for the font-size property.
    Vector<DOM::AbstractElement> ancestors;
    for (auto ancestor = abstract_element.element_to_inherit_style_from(); ancestor.has_value(); ancestor = ancestor->element_to_inherit_style_from())
        ancestors.append(*ancestor);

    NonnullRefPtr<StyleValue const> new_font_size = CSS::LengthStyleValue::create(CSS::Length::make_px(default_monospace_font_size_in_px));
    CSSPixels current_size_in_px = default_monospace_font_size_in_px;

    for (auto& ancestor : ancestors.in_reverse()) {
        auto& ancestor_cascaded_properties = *ancestor.cascaded_properties();
        auto font_size_value = ancestor_cascaded_properties.property(CSS::PropertyID::FontSize);

        if (!font_size_value)
            continue;
        if (font_size_value->is_initial() || font_size_value->is_unset()) {
            current_size_in_px = default_monospace_font_size_in_px;
            continue;
        }
        if (font_size_value->is_inherit()) {
            // Do nothing.
            continue;
        }

        if (auto absolute_size = keyword_to_absolute_size(font_size_value->to_keyword()); absolute_size.has_value()) {
            current_size_in_px = absolute_size_mapping(absolute_size.value(), default_monospace_font_size_in_px);
            continue;
        }

        if (auto relative_size = keyword_to_relative_size(font_size_value->to_keyword()); relative_size.has_value()) {
            current_size_in_px = relative_size_mapping(relative_size.value(), current_size_in_px);
            continue;
        }

        // FIXME: Resolve `font-size: math`
        if (font_size_value->to_keyword() == Keyword::Math) {
            continue;
        }

        if (font_size_value->is_percentage()) {
            current_size_in_px = CSSPixels::nearest_value_for(font_size_value->as_percentage().percentage().as_fraction() * current_size_in_px);
            continue;
        }

        if (font_size_value->is_calculated()) {
            dbgln("FIXME: Support calc() when time-traveling for monospace font-size");
            continue;
        }

        VERIFY(font_size_value->is_length());

        auto inherited_line_height = ancestor.element_to_inherit_style_from().map([](auto&& parent_element) { return parent_element.computed_properties()->line_height(); }).value_or(InitialValues::line_height());

        current_size_in_px = font_size_value->as_length().length().to_px(viewport_rect(), Length::FontMetrics { current_size_in_px, monospace_font->with_size(current_size_in_px * 0.75f)->pixel_metrics(), inherited_line_height }, m_root_element_font_metrics);
    };

    return CSS::LengthStyleValue::create(CSS::Length::make_px(current_size_in_px));
}

GC::Ref<ComputedProperties> StyleComputer::compute_properties(DOM::AbstractElement abstract_element, CascadedProperties& cascaded_properties) const
{
    auto computed_style = document().heap().allocate<CSS::ComputedProperties>();

    auto new_font_size = recascade_font_size_if_needed(abstract_element, cascaded_properties);
    if (new_font_size)
        computed_style->set_property(PropertyID::FontSize, *new_font_size, ComputedProperties::Inherited::No, Important::No);

    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<CSS::PropertyID>(i);
        auto inherited = ComputedProperties::Inherited::No;
        RefPtr<StyleValue const> value;
        auto important = Important::No;

        if (auto cascaded_style_property = cascaded_properties.style_property(property_id); cascaded_style_property.has_value()) {
            important = cascaded_style_property->important;
            value = cascaded_style_property->value;
        }

        // NOTE: We've already handled font-size above.
        if (property_id == PropertyID::FontSize && !value && new_font_size)
            continue;

        bool should_inherit = (!value && is_inherited_property(property_id));

        // https://www.w3.org/TR/css-cascade-4/#inherit
        // If the cascaded value of a property is the inherit keyword, the property’s specified and computed values are the inherited value.
        should_inherit |= value && value->is_inherit();

        // https://www.w3.org/TR/css-cascade-4/#inherit-initial
        // If the cascaded value of a property is the unset keyword, then if it is an inherited property, this is treated as inherit, and if it is not, this is treated as initial.
        should_inherit |= value && value->is_unset() && is_inherited_property(property_id);

        // https://www.w3.org/TR/css-color-4/#resolving-other-colors
        // In the color property, the used value of currentcolor is the resolved inherited value.
        should_inherit |= property_id == PropertyID::Color && value && value->to_keyword() == Keyword::Currentcolor;

        // FIXME: Logical properties should inherit from their parent's equivalent unmapped logical property.
        if (should_inherit) {
            inherited = ComputedProperties::Inherited::Yes;
            value = get_non_animated_inherit_value(property_id, abstract_element);

            if (auto animated_value = get_animated_inherit_value(property_id, abstract_element); animated_value.has_value())
                computed_style->set_animated_property(property_id, animated_value->value, animated_value->is_result_of_transition, ComputedProperties::Inherited::Yes);
        }

        if (!value || value->is_initial() || value->is_unset())
            value = property_initial_value(property_id);

        computed_style->set_property(property_id, value.release_nonnull(), inherited, important);
    }

    // Compute the value of custom properties
    compute_custom_properties(computed_style, abstract_element);

    // 2. Compute the font, since that may be needed for font-relative CSS units
    compute_font(computed_style, abstract_element);

    // 3. Convert properties into their computed forms
    compute_property_values(computed_style, abstract_element);

    // 4. Add or modify CSS-defined animations
    process_animation_definitions(computed_style, abstract_element);

    auto animations = abstract_element.element().get_animations_internal(
        Animations::Animatable::GetAnimationsSorted::Yes,
        Animations::GetAnimationsOptions { .subtree = false });
    if (animations.is_exception()) {
        dbgln("Error getting animations for element {}", abstract_element.debug_description());
    } else {
        for (auto& animation : animations.value()) {
            if (auto effect = animation->effect(); effect && effect->is_keyframe_effect()) {
                auto& keyframe_effect = *static_cast<Animations::KeyframeEffect*>(effect.ptr());
                if (keyframe_effect.pseudo_element_type() == abstract_element.pseudo_element())
                    collect_animation_into(abstract_element, keyframe_effect, computed_style);
            }
        }
    }

    // 5. Run automatic box type transformations
    transform_box_type_if_needed(computed_style, abstract_element);

    // 6. Apply any property-specific computed value logic
    resolve_effective_overflow_values(computed_style);
    compute_text_align(computed_style, abstract_element);

    // 7. Let the element adjust computed style
    if (!abstract_element.pseudo_element().has_value())
        abstract_element.element().adjust_computed_style(computed_style);

    // 8. Transition declarations [css-transitions-1]
    // Theoretically this should be part of the cascade, but it works with computed values, which we don't have until now.
    compute_transitioned_properties(computed_style, abstract_element);
    if (auto previous_style = abstract_element.computed_properties()) {
        start_needed_transitions(*previous_style, computed_style, abstract_element);
    }

    return computed_style;
}

struct SimplifiedSelectorForBucketing {
    CSS::Selector::SimpleSelector::Type type;
    FlyString name;
};

static Optional<SimplifiedSelectorForBucketing> is_roundabout_selector_bucketable_as_something_simpler(CSS::Selector::SimpleSelector const& simple_selector)
{
    if (simple_selector.type != CSS::Selector::SimpleSelector::Type::PseudoClass)
        return {};

    if (simple_selector.pseudo_class().type != CSS::PseudoClass::Is
        && simple_selector.pseudo_class().type != CSS::PseudoClass::Where)
        return {};

    if (simple_selector.pseudo_class().argument_selector_list.size() != 1)
        return {};

    auto const& argument_selector = *simple_selector.pseudo_class().argument_selector_list.first();

    auto const& compound_selector = argument_selector.compound_selectors().last();
    if (compound_selector.simple_selectors.size() != 1)
        return {};

    auto const& inner_simple_selector = compound_selector.simple_selectors.first();
    if (inner_simple_selector.type == CSS::Selector::SimpleSelector::Type::Class
        || inner_simple_selector.type == CSS::Selector::SimpleSelector::Type::Id) {
        return SimplifiedSelectorForBucketing { inner_simple_selector.type, inner_simple_selector.name() };
    }

    if (inner_simple_selector.type == CSS::Selector::SimpleSelector::Type::TagName) {
        return SimplifiedSelectorForBucketing { inner_simple_selector.type, inner_simple_selector.qualified_name().name.lowercase_name };
    }

    return {};
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_value_of_custom_property(DOM::AbstractElement abstract_element, FlyString const& name, Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts)
{
    // https://drafts.csswg.org/css-variables/#propdef-
    // The computed value of a custom property is its specified value with any arbitrary-substitution functions replaced.
    // FIXME: These should probably be part of ComputedProperties.
    auto& document = abstract_element.document();

    auto value = abstract_element.get_custom_property(name);
    if (!value || value->is_initial())
        return document.custom_property_initial_value(name);

    // Unset is the same as inherit for inherited properties, and by default all custom properties are inherited.
    // FIXME: Support non-inherited registered custom properties.
    if (value->is_inherit() || value->is_unset()) {
        auto element_to_inherit_style_from = abstract_element.element_to_inherit_style_from();
        if (!element_to_inherit_style_from.has_value())
            return document.custom_property_initial_value(name);
        auto inherited_value = element_to_inherit_style_from->get_custom_property(name);
        if (!inherited_value)
            return document.custom_property_initial_value(name);
        return inherited_value.release_nonnull();
    }

    if (value->is_revert()) {
        // FIXME: Implement reverting custom properties.
    }
    if (value->is_revert_layer()) {
        // FIXME: Implement reverting custom properties.
    }

    if (!value->is_unresolved() || !value->as_unresolved().contains_arbitrary_substitution_function())
        return value.release_nonnull();

    auto& unresolved = value->as_unresolved();
    return Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams {}, abstract_element, PropertyNameAndID::from_name(name).release_value(), unresolved, guarded_contexts);
}

void StyleComputer::compute_custom_properties(ComputedProperties&, DOM::AbstractElement abstract_element) const
{
    // https://drafts.csswg.org/css-variables/#propdef-
    // The computed value of a custom property is its specified value with any arbitrary-substitution functions replaced.
    // FIXME: These should probably be part of ComputedProperties.
    auto custom_properties = abstract_element.custom_properties();
    decltype(custom_properties) resolved_custom_properties;

    for (auto const& [name, style_property] : custom_properties) {
        resolved_custom_properties.set(name,
            StyleProperty {
                .important = style_property.important,
                .property_id = style_property.property_id,
                .value = compute_value_of_custom_property(abstract_element, name),
            });
    }
    abstract_element.set_custom_properties(move(resolved_custom_properties));
}

static CSSPixels line_width_keyword_to_css_pixels(Keyword keyword)
{
    // https://drafts.csswg.org/css-backgrounds/#typedef-line-width
    // The thin, medium, and thick keywords are equivalent to 1px, 3px, and 5px, respectively.
    switch (keyword) {
    case Keyword::Thin:
        return CSSPixels { 1 };
    case Keyword::Medium:
        return CSSPixels { 3 };
    case Keyword::Thick:
        return CSSPixels { 5 };
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://www.w3.org/TR/css-values-4/#snap-a-length-as-a-border-width
static CSSPixels snap_a_length_as_a_border_width(double device_pixels_per_css_pixel, CSSPixels length)
{
    // 1. Assert: len is non-negative.
    VERIFY(length >= 0);

    // 2. If len is an integer number of device pixels, do nothing.
    auto device_pixels = length.to_double() * device_pixels_per_css_pixel;
    if (device_pixels == trunc(device_pixels))
        return length;

    // 3. If len is greater than zero, but less than 1 device pixel, round len up to 1 device pixel.
    if (device_pixels > 0 && device_pixels < 1)
        return CSSPixels::nearest_value_for(1 / device_pixels_per_css_pixel);

    // 4. If len is greater than 1 device pixel, round it down to the nearest integer number of device pixels.
    if (device_pixels > 1)
        return CSSPixels::nearest_value_for(floor(device_pixels) / device_pixels_per_css_pixel);

    return length;
}

static NonnullRefPtr<StyleValue const> compute_style_value_list(NonnullRefPtr<StyleValue const> const& style_value, Function<NonnullRefPtr<StyleValue const>(NonnullRefPtr<StyleValue const> const&)> const& compute_entry)
{
    StyleValueVector computed_entries;

    for (auto const& entry : style_value->as_value_list().values())
        computed_entries.append(compute_entry(entry));

    return StyleValueList::create(move(computed_entries), StyleValueList::Separator::Comma);
}

static NonnullRefPtr<StyleValue const> repeat_style_value_list_to_n_elements(NonnullRefPtr<StyleValue const> const& style_value, size_t n)
{
    auto const& value_list = style_value->as_value_list();

    if (value_list.size() == n)
        return style_value;

    StyleValueVector repeated_values;
    repeated_values.ensure_capacity(n);

    for (size_t i = 0; i < n; ++i)
        repeated_values.unchecked_append(value_list.value_at(i, true));

    return StyleValueList::create(move(repeated_values), value_list.separator());
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_value_of_property(
    PropertyID property_id,
    NonnullRefPtr<StyleValue const> const& specified_value,
    Function<NonnullRefPtr<StyleValue const>(PropertyID)> const& get_property_specified_value,
    ComputationContext const& computation_context,
    double device_pixels_per_css_pixel)
{
    auto const& absolutized_value = specified_value->absolutized(computation_context);

    switch (property_id) {
    case PropertyID::AnimationName:
        return compute_animation_name(absolutized_value);
    // NB: The background properties are coordinated at compute time rather than use time, unlike other coordinating list property groups
    case PropertyID::BackgroundAttachment:
    case PropertyID::BackgroundClip:
    case PropertyID::BackgroundImage:
    case PropertyID::BackgroundOrigin:
    case PropertyID::BackgroundPositionX:
    case PropertyID::BackgroundPositionY:
    case PropertyID::BackgroundRepeat:
    case PropertyID::BackgroundSize:
        return repeat_style_value_list_to_n_elements(absolutized_value, get_property_specified_value(PropertyID::BackgroundImage)->as_value_list().size());
    case PropertyID::BorderBottomWidth:
    case PropertyID::BorderLeftWidth:
    case PropertyID::BorderRightWidth:
    case PropertyID::BorderTopWidth:
    case PropertyID::OutlineWidth:
        return compute_border_or_outline_width(absolutized_value, device_pixels_per_css_pixel);
    case PropertyID::CornerBottomLeftShape:
    case PropertyID::CornerBottomRightShape:
    case PropertyID::CornerTopLeftShape:
    case PropertyID::CornerTopRightShape:
        return compute_corner_shape(absolutized_value);
    case PropertyID::FontFeatureSettings:
    case PropertyID::FontVariationSettings:
        return compute_font_feature_tag_value_list(absolutized_value, computation_context);
    case PropertyID::LetterSpacing:
    case PropertyID::WordSpacing:
        if (absolutized_value->to_keyword() == Keyword::Normal)
            return LengthStyleValue::create(Length::make_px(0));
        return absolutized_value;
    case PropertyID::FillOpacity:
    case PropertyID::FloodOpacity:
    case PropertyID::Opacity:
    case PropertyID::StopOpacity:
    case PropertyID::StrokeOpacity:
    case PropertyID::ShapeImageThreshold:
        return compute_opacity(absolutized_value);
    case PropertyID::PositionArea:
        return compute_position_area(absolutized_value);
    default:
        return absolutized_value;
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_animation_name(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // https://drafts.csswg.org/css-animations-1/#animation-name
    // list, each item either a case-sensitive css identifier or the keyword none

    return compute_style_value_list(absolutized_value, [](NonnullRefPtr<StyleValue const> const& entry) -> NonnullRefPtr<StyleValue const> {
        // none | <custom-ident>
        if (entry->to_keyword() == Keyword::None || entry->is_custom_ident())
            return entry;

        // <string>
        if (entry->is_string()) {
            auto const& string_value = entry->as_string().string_value();

            // AD-HOC: We shouldn't convert strings that aren't valid <custom-ident>s
            if (is_css_wide_keyword(string_value) || string_value.is_one_of_ignoring_ascii_case("default"sv, "none"sv))
                return entry;

            return CustomIdentStyleValue::create(entry->as_string().string_value());
        }

        VERIFY_NOT_REACHED();
    });
}

// https://drafts.csswg.org/css-fonts-4/#font-variation-settings-def
// https://drafts.csswg.org/css-fonts/#font-feature-settings-prop
NonnullRefPtr<StyleValue const> StyleComputer::compute_font_feature_tag_value_list(NonnullRefPtr<StyleValue const> const& specified_value, ComputationContext const& computation_context)
{
    // NB: The computation logic is the same for both font-feature-settings and font-variation-settings, first we
    //     deduplicate feature tags (with latter taking precedence), then we sort them in ascending order by code unit
    auto const& absolutized_value = specified_value->absolutized(computation_context);

    if (absolutized_value->is_keyword())
        return absolutized_value;

    auto const& value_list = absolutized_value->as_value_list();
    OrderedHashMap<FlyString, NonnullRefPtr<OpenTypeTaggedStyleValue const>> axis_tags_map;
    for (size_t i = 0; i < value_list.values().size(); i++) {
        auto const& axis_tag = value_list.values().at(i)->as_open_type_tagged();
        axis_tags_map.set(axis_tag.tag(), axis_tag);
    }

    StyleValueVector axis_tags;

    for (auto const& [key, axis_tag] : axis_tags_map)
        axis_tags.append(axis_tag);

    quick_sort(axis_tags, [](auto& a, auto& b) {
        return a->as_open_type_tagged().tag() < b->as_open_type_tagged().tag();
    });

    return StyleValueList::create(move(axis_tags), StyleValueList::Separator::Comma);
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_border_or_outline_width(NonnullRefPtr<StyleValue const> const& absolutized_value, double device_pixels_per_css_pixel)
{
    // https://drafts.csswg.org/css-backgrounds/#border-width
    // absolute length, snapped as a border width
    auto const absolute_length = [&]() -> CSSPixels {
        if (absolutized_value->is_calculated())
            return absolutized_value->as_calculated().resolve_length({})->absolute_length_to_px();

        if (absolutized_value->is_length())
            return absolutized_value->as_length().length().absolute_length_to_px();

        if (absolutized_value->is_keyword())
            return line_width_keyword_to_css_pixels(absolutized_value->to_keyword());

        VERIFY_NOT_REACHED();
    }();

    return LengthStyleValue::create(Length::make_px(snap_a_length_as_a_border_width(device_pixels_per_css_pixel, absolute_length)));
}

// https://drafts.csswg.org/css-borders-4/#propdef-corner-top-left-shape
NonnullRefPtr<StyleValue const> StyleComputer::compute_corner_shape(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // the corresponding superellipse() value

    if (absolutized_value->is_superellipse())
        return absolutized_value;

    switch (absolutized_value->to_keyword()) {
    case Keyword::Round:
        // The corner shape is a quarter of a convex ellipse. Equivalent to superellipse(1).
        return SuperellipseStyleValue::create(NumberStyleValue::create(1));
    case Keyword::Squircle:
        // The corner shape is a quarter of a "squircle", a convex curve between round and square. Equivalent to superellipse(2).
        return SuperellipseStyleValue::create(NumberStyleValue::create(2));
    case Keyword::Square:
        // The corner shape is a convex 90deg angle. Equivalent to superellipse(infinity).
        return SuperellipseStyleValue::create(NumberStyleValue::create(AK::Infinity<double>));
    case Keyword::Bevel:
        // The corner shape is a straight diagonal line, neither convex nor concave. Equivalent to superellipse(0).
        return SuperellipseStyleValue::create(NumberStyleValue::create(0));
    case Keyword::Scoop:
        // The corner shape is a concave quarter-ellipse. Equivalent to superellipse(-1).
        return SuperellipseStyleValue::create(NumberStyleValue::create(-1));
    case Keyword::Notch:
        // The corner shape is a concave 90deg angle. Equivalent to superellipse(-infinity).
        return SuperellipseStyleValue::create(NumberStyleValue::create(-AK::Infinity<double>));
    default:
        VERIFY_NOT_REACHED();
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_size(NonnullRefPtr<StyleValue const> const& specified_value, int computed_math_depth, CSSPixels inherited_font_size, int inherited_math_depth, ComputationContext const& computation_context)
{
    // https://drafts.csswg.org/css-fonts/#font-size-prop
    // an absolute length

    auto const& absolutized_value = specified_value->absolutized(computation_context);

    // <absolute-size>
    if (auto absolute_size = keyword_to_absolute_size(absolutized_value->to_keyword()); absolute_size.has_value())
        return LengthStyleValue::create(Length::make_px(absolute_size_mapping(absolute_size.value(), default_user_font_size())));

    // <relative-size>
    if (auto relative_size = keyword_to_relative_size(absolutized_value->to_keyword()); relative_size.has_value())
        return LengthStyleValue::create(Length::make_px(relative_size_mapping(relative_size.value(), inherited_font_size)));

    // <length-percentage [0,∞]>
    // A length value specifies an absolute font size (independent of the user agent’s font table). Negative lengths are invalid.
    if (absolutized_value->is_length())
        return absolutized_value;

    // A percentage value specifies an absolute font size relative to the parent element’s computed font-size. Negative percentages are invalid.
    if (absolutized_value->is_percentage())
        return LengthStyleValue::create(Length::make_px(inherited_font_size * absolutized_value->as_percentage().percentage().as_fraction()));

    if (absolutized_value->is_calculated())
        return LengthStyleValue::create(absolutized_value->as_calculated().resolve_length({ .percentage_basis = Length::make_px(inherited_font_size) }).value());

    // math
    // Special mathematical scaling rules must be applied when determining the computed value of the font-size property.
    if (absolutized_value->to_keyword() == Keyword::Math) {
        auto math_scaling_factor = [&]() {
            // https://w3c.github.io/mathml-core/#the-math-script-level-property
            // If the specified value font-size is math then the computed value of font-size is obtained by multiplying
            // the inherited value of font-size by a nonzero scale factor calculated by the following procedure:
            // 1. Let A be the inherited math-depth value, B the computed math-depth value, C be 0.71 and S be 1.0
            auto size_ratio = 0.71;
            auto scale = 1.0;
            // 2. If A = B then return S.
            bool invert_scale_factor = false;
            if (inherited_math_depth == computed_math_depth) {
                return scale;
            }
            //    If B < A, swap A and B and set InvertScaleFactor to true.
            if (computed_math_depth < inherited_math_depth) {
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
        }();

        return LengthStyleValue::create(Length::make_px(inherited_font_size.scaled(math_scaling_factor)));
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_style(NonnullRefPtr<StyleValue const> const& specified_value, ComputationContext const& computation_context)
{
    // https://drafts.csswg.org/css-fonts-4/#font-style-prop
    // the keyword specified, plus angle in degrees if specified

    // NB: We always parse as a FontStyleStyleValue, but StylePropertyMap is able to set a KeywordStyleValue directly.
    if (specified_value->is_keyword())
        return FontStyleStyleValue::create(keyword_to_font_style_keyword(specified_value->to_keyword()).release_value());

    return specified_value->absolutized(computation_context);
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_weight(NonnullRefPtr<StyleValue const> const& specified_value, double inherited_font_weight, ComputationContext const& computation_context)
{
    // https://drafts.csswg.org/css-fonts-4/#font-weight-prop
    // a number, see below

    auto const& absolutized_value = specified_value->absolutized(computation_context);

    // <number [1,1000]>
    if (absolutized_value->is_number())
        return absolutized_value;

    // AD-HOC: Anywhere we support a numbers we should also support calcs
    if (absolutized_value->is_calculated())
        return NumberStyleValue::create(absolutized_value->as_calculated().resolve_number({}).value());

    // normal
    // Same as 400.
    if (absolutized_value->to_keyword() == Keyword::Normal)
        return NumberStyleValue::create(400);

    // bold
    // Same as 700.
    if (absolutized_value->to_keyword() == Keyword::Bold)
        return NumberStyleValue::create(700);

    // Specified values of bolder and lighter indicate weights relative to the weight of the parent element. The
    // computed weight is calculated based on the inherited font-weight value using the chart below.
    //
    // Inherited value (w)  bolder     lighter
    // w < 100              400        No change
    // 100 ≤ w < 350        400        100
    // 350 ≤ w < 550        700        100
    // 550 ≤ w < 750        900        400
    // 750 ≤ w < 900        900        700
    // 900 ≤ w              No change  700

    // bolder
    // Specifies a bolder weight than the inherited value. See § 2.2.1 Relative Weights.
    if (absolutized_value->to_keyword() == Keyword::Bolder) {
        if (inherited_font_weight < 350)
            return NumberStyleValue::create(400);

        if (inherited_font_weight < 550)
            return NumberStyleValue::create(700);

        if (inherited_font_weight < 900)
            return NumberStyleValue::create(900);

        return NumberStyleValue::create(inherited_font_weight);
    }

    // lighter
    // Specifies a lighter weight than the inherited value. See § 2.2.1 Relative Weights.
    if (absolutized_value->to_keyword() == Keyword::Lighter) {
        if (inherited_font_weight < 100)
            return NumberStyleValue::create(inherited_font_weight);

        if (inherited_font_weight < 550)
            return NumberStyleValue::create(100);

        if (inherited_font_weight < 750)
            return NumberStyleValue::create(400);

        return NumberStyleValue::create(700);
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_width(NonnullRefPtr<StyleValue const> const& specified_value, ComputationContext const& computation_context)
{
    // https://drafts.csswg.org/css-fonts-4/#font-width-prop
    // a percentage, see below

    auto absolutized_value = specified_value->absolutized(computation_context);

    // <percentage [0,∞]>
    if (absolutized_value->is_percentage())
        return absolutized_value;

    // AD-HOC: We support calculated percentages as well
    if (absolutized_value->is_calculated())
        return PercentageStyleValue::create(absolutized_value->as_calculated().resolve_percentage({}).value());

    switch (absolutized_value->to_keyword()) {
    // ultra-condensed 50%
    case Keyword::UltraCondensed:
        return PercentageStyleValue::create(Percentage(50));
    // extra-condensed 62.5%
    case Keyword::ExtraCondensed:
        return PercentageStyleValue::create(Percentage(62.5));
    // condensed 75%
    case Keyword::Condensed:
        return PercentageStyleValue::create(Percentage(75));
    // semi-condensed 87.5%
    case Keyword::SemiCondensed:
        return PercentageStyleValue::create(Percentage(87.5));
    // normal 100%
    case Keyword::Normal:
        return PercentageStyleValue::create(Percentage(100));
    // semi-expanded 112.5%
    case Keyword::SemiExpanded:
        return PercentageStyleValue::create(Percentage(112.5));
    // expanded 125%
    case Keyword::Expanded:
        return PercentageStyleValue::create(Percentage(125));
    // extra-expanded 150%
    case Keyword::ExtraExpanded:
        return PercentageStyleValue::create(Percentage(150));
    // ultra-expanded 200%
    case Keyword::UltraExpanded:
        return PercentageStyleValue::create(Percentage(200));
    default:
        VERIFY_NOT_REACHED();
    }
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_line_height(NonnullRefPtr<StyleValue const> const& specified_value, ComputationContext const& computation_context)
{
    // https://drafts.csswg.org/css-inline-3/#line-height-property

    auto absolutized_value = specified_value->absolutized(computation_context);

    // normal
    // <length [0,∞]>
    // <number [0,∞]>
    if (absolutized_value->to_keyword() == Keyword::Normal || absolutized_value->is_length() || absolutized_value->is_number())
        return absolutized_value;

    // NOTE: We also support calc()'d lengths (percentages resolve to lengths so we don't have to handle them separately)
    if (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_length_percentage())
        return LengthStyleValue::create(absolutized_value->as_calculated().resolve_length({ .percentage_basis = Length::make_px(computation_context.length_resolution_context.font_metrics.font_size) }).value());

    // NOTE: We also support calc()'d numbers
    if (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_number())
        return NumberStyleValue::create(absolutized_value->as_calculated().resolve_number({ .percentage_basis = Length::make_px(computation_context.length_resolution_context.font_metrics.font_size) }).value());

    // <percentage [0,∞]>
    if (absolutized_value->is_percentage())
        return LengthStyleValue::create(Length::make_px(computation_context.length_resolution_context.font_metrics.font_size * absolutized_value->as_percentage().percentage().as_fraction()));

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_opacity(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // https://drafts.csswg.org/css-color-4/#transparency
    // specified number, clamped to the range [0,1]

    // <number>
    if (absolutized_value->is_number())
        return NumberStyleValue::create(clamp(absolutized_value->as_number().number(), 0, 1));

    // NOTE: We also support calc()'d numbers
    if (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_number())
        return NumberStyleValue::create(absolutized_value->as_calculated().resolve_number({}).value());

    // <percentage>
    if (absolutized_value->is_percentage())
        return NumberStyleValue::create(clamp(absolutized_value->as_percentage().percentage().as_fraction(), 0, 1));

    // NOTE: We also support calc()'d percentages
    if (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_percentage())
        return NumberStyleValue::create(absolutized_value->as_calculated().resolve_percentage({})->as_fraction());

    VERIFY_NOT_REACHED();
}

// https://drafts.csswg.org/css-anchor-position/#position-area-computed
NonnullRefPtr<StyleValue const> StyleComputer::compute_position_area(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // The computed value of a <position-area> value is the two keywords indicating the selected tracks in each axis,
    // with the long (block-start) and short (start) logical keywords treated as equivalent. It serializes in the order
    // given in the grammar (above), with the logical keywords serialized in their short forms (e.g. start start
    // instead of block-start inline-start).
    if (absolutized_value->is_keyword())
        return absolutized_value;

    auto to_short_keyword = [](NonnullRefPtr<KeywordStyleValue const> const& keyword_value) -> NonnullRefPtr<KeywordStyleValue const> {
        switch (keyword_value->keyword()) {
        case Keyword::BlockStart:
        case Keyword::InlineStart:
            return KeywordStyleValue::create(Keyword::Start);
        case Keyword::BlockEnd:
        case Keyword::InlineEnd:
            return KeywordStyleValue::create(Keyword::End);
        case Keyword::SelfBlockStart:
        case Keyword::SelfInlineStart:
            return KeywordStyleValue::create(Keyword::SelfStart);
        case Keyword::SelfBlockEnd:
        case Keyword::SelfInlineEnd:
            return KeywordStyleValue::create(Keyword::SelfEnd);
        case Keyword::SpanBlockStart:
        case Keyword::SpanInlineStart:
            return KeywordStyleValue::create(Keyword::SpanStart);
        case Keyword::SpanBlockEnd:
        case Keyword::SpanInlineEnd:
            return KeywordStyleValue::create(Keyword::SpanEnd);
        case Keyword::SpanSelfBlockStart:
        case Keyword::SpanSelfInlineStart:
            return KeywordStyleValue::create(Keyword::SpanSelfStart);
        case Keyword::SpanSelfBlockEnd:
        case Keyword::SpanSelfInlineEnd:
            return KeywordStyleValue::create(Keyword::SpanSelfEnd);
        default:
            break;
        }
        return keyword_value;
    };

    auto const& value_list = absolutized_value->as_value_list();
    VERIFY(value_list.size() == 2);

    auto const& block_value = value_list.values().at(0);
    auto const& inline_value = value_list.values().at(1);
    if (block_value->as_keyword().keyword() == Keyword::SpanAll) {
        switch (inline_value->as_keyword().keyword()) {
        case Keyword::Start:
            return KeywordStyleValue::create(Keyword::InlineStart);
        case Keyword::End:
            return KeywordStyleValue::create(Keyword::InlineEnd);
        case Keyword::SelfStart:
            return KeywordStyleValue::create(Keyword::SelfInlineStart);
        case Keyword::SelfEnd:
            return KeywordStyleValue::create(Keyword::SelfInlineEnd);
        case Keyword::SpanStart:
            return KeywordStyleValue::create(Keyword::SpanInlineStart);
        case Keyword::SpanEnd:
            return KeywordStyleValue::create(Keyword::SpanInlineEnd);
        case Keyword::SpanSelfStart:
            return KeywordStyleValue::create(Keyword::SpanSelfInlineStart);
        case Keyword::SpanSelfEnd:
            return KeywordStyleValue::create(Keyword::SpanSelfInlineEnd);
        default:
            return absolutized_value;
        }
    }
    if (inline_value->as_keyword().keyword() == Keyword::SpanAll) {
        switch (block_value->as_keyword().keyword()) {
        case Keyword::Start:
            return KeywordStyleValue::create(Keyword::BlockStart);
        case Keyword::End:
            return KeywordStyleValue::create(Keyword::BlockEnd);
        case Keyword::SelfStart:
            return KeywordStyleValue::create(Keyword::SelfBlockStart);
        case Keyword::SelfEnd:
            return KeywordStyleValue::create(Keyword::SelfBlockEnd);
        case Keyword::SpanStart:
            return KeywordStyleValue::create(Keyword::SpanBlockStart);
        case Keyword::SpanEnd:
            return KeywordStyleValue::create(Keyword::SpanBlockEnd);
        case Keyword::SpanSelfStart:
            return KeywordStyleValue::create(Keyword::SpanSelfBlockStart);
        case Keyword::SpanSelfEnd:
            return KeywordStyleValue::create(Keyword::SpanSelfBlockEnd);
        default:
            return absolutized_value;
        }
    }
    auto short_block_value = to_short_keyword(block_value->as_keyword());
    auto short_inline_value = to_short_keyword(inline_value->as_keyword());
    if (*block_value != short_block_value || *inline_value != short_inline_value)
        return StyleValueList::create({ short_block_value, short_inline_value }, StyleValueList::Separator::Space);

    return absolutized_value;
}

// https://w3c.github.io/mathml-core/#propdef-math-depth
NonnullRefPtr<StyleValue const> StyleComputer::compute_math_depth(NonnullRefPtr<StyleValue const> const& specified_value, int inherited_math_depth, MathStyle inherited_math_style, ComputationContext const& computation_context)
{
    auto absolutized_value = specified_value->absolutized(computation_context);

    auto resolve_integer = [&](StyleValue const& integer_value) {
        if (integer_value.is_integer())
            return integer_value.as_integer().integer();

        if (integer_value.is_calculated())
            return integer_value.as_calculated().resolve_integer({}).value();

        VERIFY_NOT_REACHED();
    };

    // The computed value of the math-depth value is determined as follows:
    // - If the specified value of math-depth is auto-add and the inherited value of math-style is compact
    //   then the computed value of math-depth of the element is its inherited value plus one.
    if (absolutized_value->to_keyword() == Keyword::AutoAdd && inherited_math_style == MathStyle::Compact)
        return IntegerStyleValue::create(inherited_math_depth + 1);

    // - If the specified value of math-depth is of the form add(<integer>) then the computed value of
    //   math-depth of the element is its inherited value plus the specified integer.
    if (absolutized_value->is_add_function())
        return IntegerStyleValue::create(inherited_math_depth + resolve_integer(*absolutized_value->as_add_function().value()));

    // - If the specified value of math-depth is of the form <integer> then the computed value of math-depth
    //   of the element is the specified integer.
    if (absolutized_value->is_integer() || absolutized_value->is_calculated())
        return IntegerStyleValue::create(resolve_integer(*absolutized_value));

    // - Otherwise, the computed value of math-depth of the element is the inherited one.
    return IntegerStyleValue::create(inherited_math_depth);
}

static void for_each_element_hash(DOM::Element const& element, auto callback)
{
    callback(element.local_name().ascii_case_insensitive_hash());
    if (element.id().has_value())
        callback(element.id().value().hash());
    for (auto const& class_ : element.class_names())
        callback(class_.hash());
    element.for_each_attribute([&](auto& attribute) {
        callback(attribute.name().ascii_case_insensitive_hash());
    });
}

void StyleComputer::reset_ancestor_filter()
{
    m_ancestor_filter->clear();
}

void StyleComputer::reset_has_result_cache()
{
    if (!m_has_result_cache)
        m_has_result_cache = make<SelectorEngine::HasResultCache>();
    else
        m_has_result_cache->clear();
}

void StyleComputer::push_ancestor(DOM::Element const& element)
{
    for_each_element_hash(element, [&](u32 hash) {
        m_ancestor_filter->increment(hash);
    });
}

void StyleComputer::pop_ancestor(DOM::Element const& element)
{
    for_each_element_hash(element, [&](u32 hash) {
        m_ancestor_filter->decrement(hash);
    });
}

void RuleCache::add_rule(MatchingRule const& matching_rule, Optional<PseudoElement> pseudo_element, bool contains_root_pseudo_class)
{
    if (matching_rule.slotted) {
        slotted_rules.append(matching_rule);
        return;
    }
    if (matching_rule.contains_part_pseudo_element) {
        part_rules.append(matching_rule);
        return;
    }
    // NOTE: We traverse the simple selectors in reverse order to make sure that class/ID buckets are preferred over tag buckets
    //       in the common case of div.foo or div#foo selectors.
    auto add_to_id_bucket = [&](FlyString const& name) {
        rules_by_id.ensure(name).append(matching_rule);
    };

    auto add_to_class_bucket = [&](FlyString const& name) {
        rules_by_class.ensure(name).append(matching_rule);
    };

    auto add_to_tag_name_bucket = [&](FlyString const& name) {
        rules_by_tag_name.ensure(name).append(matching_rule);
    };

    for (auto const& simple_selector : matching_rule.selector.compound_selectors().last().simple_selectors.in_reverse()) {
        if (simple_selector.type == Selector::SimpleSelector::Type::Id) {
            add_to_id_bucket(simple_selector.name());
            return;
        }
        if (simple_selector.type == Selector::SimpleSelector::Type::Class) {
            add_to_class_bucket(simple_selector.name());
            return;
        }
        if (simple_selector.type == Selector::SimpleSelector::Type::TagName) {
            add_to_tag_name_bucket(simple_selector.qualified_name().name.lowercase_name);
            return;
        }
        // NOTE: Selectors like `:is/where(.foo)` and `:is/where(.foo .bar)` are bucketed as class selectors for `foo` and `bar` respectively.
        if (auto simplified = is_roundabout_selector_bucketable_as_something_simpler(simple_selector); simplified.has_value()) {
            if (simplified->type == Selector::SimpleSelector::Type::TagName) {
                add_to_tag_name_bucket(simplified->name);
                return;
            }
            if (simplified->type == Selector::SimpleSelector::Type::Class) {
                add_to_class_bucket(simplified->name);
                return;
            }
            if (simplified->type == Selector::SimpleSelector::Type::Id) {
                add_to_id_bucket(simplified->name);
                return;
            }
        }
    }

    if (matching_rule.contains_pseudo_element && pseudo_element.has_value()) {
        if (Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
            rules_by_pseudo_element[to_underlying(pseudo_element.value())].append(matching_rule);
        } else {
            // NOTE: We don't cache rules for unknown pseudo-elements. They can't match anything anyway.
        }
    } else if (contains_root_pseudo_class) {
        root_rules.append(matching_rule);
    } else {
        for (auto const& simple_selector : matching_rule.selector.compound_selectors().last().simple_selectors) {
            if (simple_selector.type == Selector::SimpleSelector::Type::Attribute) {
                rules_by_attribute_name.ensure(simple_selector.attribute().qualified_name.name.lowercase_name).append(matching_rule);
                return;
            }
        }
        other_rules.append(matching_rule);
    }
}

void RuleCache::for_each_matching_rules(DOM::AbstractElement abstract_element, Function<IterationDecision(Vector<MatchingRule> const&)> callback) const
{
    for (auto const& class_name : abstract_element.element().class_names()) {
        if (auto it = rules_by_class.find(class_name); it != rules_by_class.end()) {
            if (callback(it->value) == IterationDecision::Break)
                return;
        }
    }
    if (auto id = abstract_element.element().id(); id.has_value()) {
        if (auto it = rules_by_id.find(id.value()); it != rules_by_id.end()) {
            if (callback(it->value) == IterationDecision::Break)
                return;
        }
    }
    if (auto it = rules_by_tag_name.find(abstract_element.element().lowercased_local_name()); it != rules_by_tag_name.end()) {
        if (callback(it->value) == IterationDecision::Break)
            return;
    }
    if (abstract_element.pseudo_element().has_value()) {
        if (Selector::PseudoElementSelector::is_known_pseudo_element_type(abstract_element.pseudo_element().value())) {
            if (callback(rules_by_pseudo_element.at(to_underlying(abstract_element.pseudo_element().value()))) == IterationDecision::Break)
                return;
        } else {
            // NOTE: We don't cache rules for unknown pseudo-elements. They can't match anything anyway.
        }
    }

    if (abstract_element.element().is_document_element()) {
        if (callback(root_rules) == IterationDecision::Break)
            return;
    }

    IterationDecision decision = IterationDecision::Continue;
    abstract_element.element().for_each_attribute([&](auto& name, auto&) {
        if (auto it = rules_by_attribute_name.find(name); it != rules_by_attribute_name.end()) {
            decision = callback(it->value);
        }
    });
    if (decision == IterationDecision::Break)
        return;

    (void)callback(other_rules);
}

}
