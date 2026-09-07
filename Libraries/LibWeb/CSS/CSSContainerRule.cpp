/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSContainerRule.h"
#include <LibGC/Heap.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSContainerRule);

GC::Ref<CSSContainerRule> CSSContainerRule::create(RustRule rule, CSSRuleList& rules)
{
    return GC::Heap::the().allocate<CSSContainerRule>(move(rule), rules);
}

CSSContainerRule::CSSContainerRule(RustRule rule, CSSRuleList& rules)
    : CSSConditionRule(rules, move(rule))
    , m_conditions(ContainerConditions::create(native_rule().payload().container))
{
}

CSSContainerRule::~CSSContainerRule() = default;

void CSSContainerRule::visit_edges(GC::Cell::Visitor& visitor)
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

static Utf16String serialized_condition_name(ContainerConditions::Condition const& condition)
{
    if (!condition.container_name.has_value())
        return {};

    Utf16StringBuilder builder;
    serialize_an_identifier(builder, *condition.container_name);
    return builder.to_string();
}

static Utf16String serialized_condition_query(ContainerConditions::Condition const& condition)
{
    return condition.container_query ? condition.container_query->to_string() : Utf16String {};
}

static void append_condition_text(Utf16StringBuilder& result, ContainerConditions::Condition const& condition)
{
    auto name = serialized_condition_name(condition);
    auto query = serialized_condition_query(condition);

    if (!name.is_empty()) {
        result.append(name.utf16_view());

        if (!query.is_empty())
            result.append_ascii(' ');
    }

    result.append(query.utf16_view());
}

// https://drafts.csswg.org/css-conditional-5/#the-csscontainerrule-interface
Utf16String CSSContainerRule::serialized_condition_text() const
{
    // The conditionText attribute (defined on the CSSConditionRule parent rule), on getting, must return a value as
    // follows:

    // 1. Let conditions be the result of getting the conditions attribute.
    auto const& conditions = m_conditions->entries();

    // 2. Let first be true.
    auto first = true;

    // 2. Let result be the empty string.
    Utf16StringBuilder result;

    // 3. For each condition in conditions:
    for (auto const& condition : conditions) {
        // 1. If first is false, append ", " to result.
        if (!first)
            result.append_ascii(", "sv);

        // 2. Set first to false.
        first = false;

        // 3. If condition's name is not empty:
        //     1. Append condition's name to result.
        //     2. If condition's query is not empty, append a single space to result.
        //
        // 4. Append condition's query to result.
        append_condition_text(result, condition);
    }

    // 5. Return result.
    return result.to_string();
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
    if (!m_conditions->matches(element))
        return false;

    if (auto const* parent_container_rule = find_parent_container_rule())
        return parent_container_rule->matches(element);

    return true;
}

bool CSSContainerRule::contains_size_feature() const
{
    if (m_conditions->contains_size_feature())
        return true;

    if (auto const* parent_container_rule = find_parent_container_rule())
        return parent_container_rule->contains_size_feature();

    return false;
}

bool CSSContainerRule::contains_style_feature() const
{
    if (m_conditions->contains_style_feature())
        return true;

    if (auto const* parent_container_rule = find_parent_container_rule())
        return parent_container_rule->contains_style_feature();

    return false;
}

void CSSContainerRule::mark_element_style_dependencies(DOM::AbstractElement& abstract_element) const
{
    if (contains_size_feature())
        abstract_element.element().set_style_depends_on_size_container_query();

    if (contains_style_feature())
        abstract_element.element().set_style_depends_on_style_container_query();
}

// https://drafts.csswg.org/cssom-1/#serialize-a-css-rule
Utf16String CSSContainerRule::serialized() const
{
    // AD-HOC: The spec does not define @container serialization, so this is based on CSSMediaRule::serialized().
    Utf16StringBuilder builder;
    builder.append_ascii("@container "sv);

    builder.append(serialized_condition_text());

    builder.append_ascii(" {\n"sv);

    for (size_t i = 0; i < css_rules().length(); i++) {
        auto rule = css_rules().item(i);
        auto result = rule->serialized();

        if (result.is_empty())
            continue;

        builder.append_ascii("  "sv);
        builder.append(result);
        builder.append_ascii('\n');
    }

    builder.append_ascii('}');

    return builder.to_string();
}

// https://drafts.csswg.org/css-conditional-5/#dom-csscontainerrule-containername
Utf16String CSSContainerRule::container_name() const
{
    // The containerName attribute, on getting, must return a value as follows:

    // 1. Let conditions be the result of getting the conditions attribute.
    auto const& conditions = m_conditions->entries();

    // 2. If the length of conditions is 1:
    if (conditions.size() == 1) {
        // 1. Return the only condition's name.
        return serialized_condition_name(conditions.first());
    }

    // 3. Return "".
    return {};
}

// https://drafts.csswg.org/css-conditional-5/#dom-csscontainerrule-containerquery
Utf16String CSSContainerRule::container_query() const
{
    // The containerQuery attribute, on getting, must return a value as follows:

    // 1. Let conditions be the result of getting the conditions attribute.
    auto const& conditions = m_conditions->entries();

    // 2. If the length of conditions is 1:
    if (conditions.size() == 1) {
        // 1. Return the only condition's query.
        return serialized_condition_query(conditions.first());
    }

    // 3. Return "".
    return {};
}

// https://drafts.csswg.org/css-conditional-5/#dom-csscontainerrule-conditions
Vector<CSSContainerCondition> CSSContainerRule::conditions() const
{
    // The conditions attribute, on getting, must return a value as follows:

    // 1. Let result be an empty list.
    Vector<CSSContainerCondition> result;

    // 2. For each <container-condition> condition specified in the rule:
    result.ensure_capacity(m_conditions->entries().size());
    for (auto const& condition : m_conditions->entries()) {
        // 1. Let dict be a new CSSContainerCondition with name set to the serialized <container-name> of condition if
        //    specified, or "" otherwise, and query set to the <container-query> specified in condition without any
        //    logical simplifications, so that the returned query will evaluate to the same result as the specified
        //    query in any conformant implementation of this specification (including implementations that implement
        //    future extensions allowed by the <general-enclosed> extensibility mechanism in this specification). In
        //    other words, token stream simplifications are allowed (such as reducing whitespace to a single space or
        //    omitting it in cases where it is known to be optional), but logical simplifications (such as removal of
        //    unneeded parentheses, or simplification based on evaluating results) are not allowed.
        CSSContainerCondition dict {
            .name = serialized_condition_name(condition),
            .query = serialized_condition_query(condition),
        };

        // 2. Append dict to result.
        result.unchecked_append(move(dict));
    }

    // 3. Return result.
    return result;
}

}
