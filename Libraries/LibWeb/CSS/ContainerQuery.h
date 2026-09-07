/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Query.h>
#include <LibWeb/CSS/RustQueryHandle.h>

namespace Web::CSS {

struct ContainerQueryFeatureRequirements {
    bool requires_width_container : 1 { false };
    bool requires_height_container : 1 { false };
    bool requires_inline_size_container : 1 { false };
    bool requires_block_size_container : 1 { false };
    bool requires_style_container : 1 { false };
    bool requires_scroll_state_container : 1 { false };
    bool has_unknown_or_unsupported_feature : 1 { false };

    bool contains_size_feature() const
    {
        return requires_width_container
            || requires_height_container
            || requires_inline_size_container
            || requires_block_size_container;
    }

    bool contains_style_feature() const { return requires_style_container; }
};

// https://drafts.csswg.org/css-conditional-5/#container-rule
class WEB_API ContainerQuery final : public RefCounted<ContainerQuery> {
public:
    static NonnullRefPtr<ContainerQuery> create(RustQueryHandle);

    bool contains_size_feature() const { return m_feature_requirements.contains_size_feature(); }
    bool contains_style_feature() const { return m_feature_requirements.contains_style_feature(); }
    MatchResult evaluate(DOM::AbstractElement const&, Optional<Utf16FlyString> const& container_name) const;
    Utf16String to_string() const;

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    explicit ContainerQuery(RustQueryHandle);

    RustQueryHandle m_rust_query_handle;
    ContainerQueryFeatureRequirements m_feature_requirements;
};

// Document-thread bindings for an immutable Rust container-condition list.
class WEB_API ContainerConditions final : public RefCounted<ContainerConditions> {
public:
    struct Condition {
        Optional<Utf16FlyString> container_name;
        RefPtr<ContainerQuery> container_query;
    };

    static NonnullRefPtr<ContainerConditions> create(Parser::ValueParserFFI::ContainerConditionsData const*);
    ~ContainerConditions();
    Parser::ValueParserFFI::ContainerConditionsData const* handle() const { return m_data; }

    Vector<Condition> const& entries() const;
    bool matches(DOM::AbstractElement const&) const;
    bool contains_size_feature() const;
    bool contains_style_feature() const;
    void mark_element_style_dependencies(DOM::AbstractElement&) const;

private:
    explicit ContainerConditions(Parser::ValueParserFFI::ContainerConditionsData const*);
    Parser::ValueParserFFI::ContainerConditionsData const* m_data;
    mutable Optional<Vector<Condition>> m_entries;
};

bool container_name_matches(DOM::Element const&, Optional<Utf16FlyString> const& container_name);
bool evaluate_native_container_condition(Parser::ValueParserFFI::FfiQueryHandle const*, Utf16View name, DOM::AbstractElement const&);
MatchResult evaluate_style_query(RustQueryHandle const&, AbstractOrHypotheticalElement);
void prepare_for_style_query_evaluation();
bool style_query_cycle_detected();

}
