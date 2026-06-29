/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibWeb/CSS/CSSFunctionRule.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

// https://drafts.csswg.org/css-values-5/#substitution-context
// The types of substitution contexts are currently:

// "property", followed by a property name, and optionally a custom function.
struct PropertySubstitutionContextDependency {
    Utf16String property_name;
    GC::Ptr<CSSFunctionRule const> custom_function { nullptr };
    bool operator==(PropertySubstitutionContextDependency const& other) const = default;
};

// "attribute", followed by an attribute name.
struct AttributeSubstitutionContextDependency {
    Utf16String attribute_name;
    bool operator==(AttributeSubstitutionContextDependency const& other) const = default;
};

// "function", followed by a custom function.
struct FunctionSubstitutionContextDependency {
    GC::Ref<CSSFunctionRule const> custom_function;
    bool operator==(FunctionSubstitutionContextDependency const& other) const = default;
};

struct SubstitutionContext {
    Variant<PropertySubstitutionContextDependency, AttributeSubstitutionContextDependency, FunctionSubstitutionContextDependency> dependency;

    bool is_cyclic { false };

    bool operator==(SubstitutionContext const& other) const { return dependency == other.dependency; }
};

class GuardedSubstitutionContexts {
public:
    void guard(SubstitutionContext&);
    void unguard(SubstitutionContext const&);
    bool mark_existing_as_cyclic(SubstitutionContext const&);

private:
    Vector<SubstitutionContext&> m_contexts;
};

struct ArbitrarySubstitutionReplacementContext {
    ComputedProperties const* computed_style_for_custom_property_resolution { nullptr };
};

enum class ArbitrarySubstitutionFunction : u8 {
    Attr,
    Env,
    If,
    Inherit,
    Var,
};
[[nodiscard]] Optional<ArbitrarySubstitutionFunction> to_arbitrary_substitution_function(Utf16View name);

bool contains_guaranteed_invalid_value(ReadonlySpan<ComponentValue>);
bool contains_attr_tainted_value(ReadonlySpan<ComponentValue>);

[[nodiscard]] Vector<ComponentValue> substitute_arbitrary_substitution_functions(AbstractOrHypotheticalElement&, GuardedSubstitutionContexts&, ArbitrarySubstitutionReplacementContext const&, ReadonlySpan<ComponentValue>, Optional<SubstitutionContext> = {});

using DeclarationValueList = Vector<ReadonlySpan<ComponentValue>>;

struct IfArgsBranch {
    ReadonlySpan<ComponentValue> condition;
    Optional<ReadonlySpan<ComponentValue>> value;
};

using IfArgs = Vector<IfArgsBranch>;
using ArbitrarySubstitutionFunctionArguments = Variant<DeclarationValueList, IfArgs>;
// The returned argument spans borrow from the input component value list.
[[nodiscard]] Optional<ArbitrarySubstitutionFunctionArguments> parse_according_to_argument_grammar(ArbitrarySubstitutionFunction, ReadonlySpan<ComponentValue>);

[[nodiscard]] Vector<ComponentValue> replace_an_arbitrary_substitution_function(AbstractOrHypotheticalElement&, GuardedSubstitutionContexts&, ArbitrarySubstitutionReplacementContext const&, ArbitrarySubstitutionFunction, ArbitrarySubstitutionFunctionArguments const&);

}
