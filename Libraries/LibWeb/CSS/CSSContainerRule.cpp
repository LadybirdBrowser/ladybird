/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSContainerRule.h"
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/CSSContainerRule.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSContainerRule);

GC::Ref<CSSContainerRule> CSSContainerRule::create(JS::Realm& realm, Vector<Condition>&& conditions, CSSRuleList& rules)
{
    return realm.create<CSSContainerRule>(realm, move(conditions), rules);
}

CSSContainerRule::CSSContainerRule(JS::Realm& realm, Vector<Condition>&& conditions, CSSRuleList& rules)
    : CSSConditionRule(realm, rules, Type::Container)
    , m_conditions(move(conditions))
{
}

CSSContainerRule::~CSSContainerRule() = default;

void CSSContainerRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSContainerRule);
    Base::initialize(realm);
}

void CSSContainerRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cached_parent_container_rule);
}

void CSSContainerRule::clear_caches()
{
    Base::clear_caches();
    m_cached_parent_container_rule = nullptr;
    m_parent_container_rule_cache_valid = false;
}

// https://drafts.csswg.org/css-conditional-5/#the-csscontainerrule-interface
String CSSContainerRule::condition_text() const
{
    // The conditionText attribute (defined on the CSSConditionRule parent rule), on getting, must return a value as
    // follows:

    // 1. Let conditions be the result of getting the conditions attribute.
    auto conditions = this->conditions();

    // 2. Let first be true.
    auto first = true;

    // 2. Let result be the empty string.
    StringBuilder result;

    // 3. For each condition in conditions:
    for (auto const& condition : conditions) {
        // 1. If first is false, append ", " to result.
        if (!first)
            result.append(", "sv);

        // 2. Set first to false.
        first = false;

        // 3. If condition's name is not empty:
        if (!condition.name.is_empty()) {
            // 1. Append condition's name to result.
            result.append(condition.name);

            // 2. If condition's query is not empty, append a single space to result.
            if (!condition.query.is_empty())
                result.append(' ');
        }

        // 4. Append condition's query to result.
        result.append(condition.query);
    }

    // 5. Return result.
    return result.to_string_without_validation();
}

bool CSSContainerRule::condition_matches() const
{
    // NB: @container is processed differently from other CSSConditionRules. As such this is never called.
    VERIFY_NOT_REACHED();
}

static bool has_ancestor_container_with_name(DOM::AbstractElement const& element, FlyString const& container_name)
{
    Optional<FlyString> optional_container_name { container_name };
    for (auto* container = element.element().flat_tree_parent_element(); container; container = container->flat_tree_parent_element()) {
        if (container_name_matches(*container, optional_container_name))
            return true;
    }

    return false;
}

bool CSSContainerRule::conditions_match(DOM::AbstractElement const& element) const
{
    for (auto const& condition : m_conditions) {
        if (condition.container_query) {
            if (condition.container_query->evaluate(element, condition.container_name) == MatchResult::True)
                return true;
            continue;
        }

        if (condition.container_name.has_value() && has_ancestor_container_with_name(element, *condition.container_name))
            return true;
    }

    return false;
}

CSSContainerRule const* CSSContainerRule::find_parent_container_rule() const
{
    if (m_parent_container_rule_cache_valid)
        return m_cached_parent_container_rule.ptr();

    m_cached_parent_container_rule = nullptr;
    for (auto const* rule = parent_rule(); rule; rule = rule->parent_rule()) {
        if (auto const* container_rule = as_if<CSSContainerRule>(*rule)) {
            m_cached_parent_container_rule = container_rule;
            break;
        }
    }
    m_parent_container_rule_cache_valid = true;

    return m_cached_parent_container_rule.ptr();
}

bool CSSContainerRule::matches(DOM::AbstractElement const& element) const
{
    if (!conditions_match(element))
        return false;

    if (auto const* parent_container_rule = find_parent_container_rule())
        return parent_container_rule->matches(element);

    return true;
}

// https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
String CSSContainerRule::serialized() const
{
    // AD-HOC: The spec does not define @container serialization, so this is based on CSSMediaRule::serialized().
    StringBuilder builder;
    builder.append("@container "sv);

    builder.append(condition_text());

    builder.append(" {\n"sv);

    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        auto result = rule->css_text();

        if (result.is_empty())
            continue;

        builder.append("  "sv);
        builder.append(result);
        builder.append('\n');
    }

    builder.append('}');

    return builder.to_string_without_validation();
}

// https://drafts.csswg.org/css-conditional-5/#dom-csscontainerrule-containername
String CSSContainerRule::container_name() const
{
    // The containerName attribute, on getting, must return a value as follows:

    // 1. Let conditions be the result of getting the conditions attribute.
    auto conditions = this->conditions();

    // 2. If the length of conditions is 1:
    if (conditions.size() == 1) {
        // 1. Return the only condition's name.
        return conditions.first().name;
    }

    // 3. Return "".
    return ""_string;
}

// https://drafts.csswg.org/css-conditional-5/#dom-csscontainerrule-containerquery
String CSSContainerRule::container_query() const
{
    // The containerQuery attribute, on getting, must return a value as follows:

    // 1. Let conditions be the result of getting the conditions attribute.
    auto conditions = this->conditions();

    // 2. If the length of conditions is 1:
    if (conditions.size() == 1) {
        // 1. Return the only condition's query.
        return conditions.first().query;
    }

    // 3. Return "".
    return ""_string;
}

// https://drafts.csswg.org/css-conditional-5/#dom-csscontainerrule-conditions
Vector<CSSContainerCondition> CSSContainerRule::conditions() const
{
    // The conditions attribute, on getting, must return a value as follows:

    // 1. Let result be an empty list.
    Vector<CSSContainerCondition> result;

    // 2. For each <container-condition> condition specified in the rule:
    result.ensure_capacity(m_conditions.size());
    for (auto const& condition : m_conditions) {
        // 1. Let dict be a new CSSContainerCondition with name set to the serialized <container-name> of condition if
        //    specified, or "" otherwise, and query set to the <container-query> specified in condition without any
        //    logical simplifications, so that the returned query will evaluate to the same result as the specified
        //    query in any conformant implementation of this specification (including implementations that implement
        //    future extensions allowed by the <general-enclosed> extensibility mechanism in this specification). In
        //    other words, token stream simplifications are allowed (such as reducing whitespace to a single space or
        //    omitting it in cases where it is known to be optional), but logical simplifications (such as removal of
        //    unneeded parentheses, or simplification based on evaluating results) are not allowed.
        CSSContainerCondition dict {
            .name = condition.container_name.has_value() ? serialize_an_identifier(condition.container_name.value()) : ""_string,
            .query = condition.container_query ? condition.container_query->to_string() : ""_string,
        };

        // 2. Append dict to result.
        result.unchecked_append(move(dict));
    }

    // 3. Return result.
    return result;
}

void CSSContainerRule::for_each_effective_rule(TraversalOrder order, Function<void(CSSRule const&)> const& callback) const
{
    // https://drafts.csswg.org/css-conditional-5/#container-rule
    // Global, name-defining at-rules such as @keyframes or @font-face or @layer that are defined inside container
    // queries are not constrained by the container query conditions.
    //
    // NB: For other rules, we always treat them as effective now, and reject them later when our @container conditions
    //     are evaluated.
    CSSGroupingRule::for_each_effective_rule(order, callback);
}

}
