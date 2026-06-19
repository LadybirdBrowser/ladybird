/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/String.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

// https://drafts.csswg.org/css-values-5/#substitution-context
struct SubstitutionContext {
    enum class DependencyType : u8 {
        Property,
        Attribute,
        Function,
    };
    DependencyType dependency_type;
    String first;
    Optional<String> second {};

    bool is_cyclic { false };

    bool operator==(SubstitutionContext const&) const;
    String to_string() const;
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
[[nodiscard]] Optional<ArbitrarySubstitutionFunction> to_arbitrary_substitution_function(FlyString const& name);

bool contains_guaranteed_invalid_value(ReadonlySpan<ComponentValue>);

[[nodiscard]] Vector<ComponentValue> substitute_arbitrary_substitution_functions(DOM::AbstractElement&, GuardedSubstitutionContexts&, ArbitrarySubstitutionReplacementContext const&, ReadonlySpan<ComponentValue>, Optional<SubstitutionContext> = {});

using DeclarationValueList = Vector<ReadonlySpan<ComponentValue>>;

struct IfArgsBranch {
    ReadonlySpan<ComponentValue> condition;
    Optional<ReadonlySpan<ComponentValue>> value;
};

using IfArgs = Vector<IfArgsBranch>;
using ArbitrarySubstitutionFunctionArguments = Variant<DeclarationValueList, IfArgs>;
// The returned argument spans borrow from the input component value list.
[[nodiscard]] Optional<ArbitrarySubstitutionFunctionArguments> parse_according_to_argument_grammar(ArbitrarySubstitutionFunction, ReadonlySpan<ComponentValue>);

[[nodiscard]] Vector<ComponentValue> replace_an_arbitrary_substitution_function(DOM::AbstractElement&, GuardedSubstitutionContexts&, ArbitrarySubstitutionReplacementContext const&, ArbitrarySubstitutionFunction, ArbitrarySubstitutionFunctionArguments const&);

}
