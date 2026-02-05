/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

private:
    Vector<SubstitutionContext&> m_contexts;
};

enum class ArbitrarySubstitutionFunction : u8 {
    Attr,
    Env,
    Var,
};
[[nodiscard]] Optional<ArbitrarySubstitutionFunction> to_arbitrary_substitution_function(FlyString const& name);

bool contains_guaranteed_invalid_value(Vector<ComponentValue> const&);

[[nodiscard]] Vector<ComponentValue> substitute_arbitrary_substitution_functions(DOM::AbstractElement&, GuardedSubstitutionContexts&, Vector<ComponentValue> const&, Optional<SubstitutionContext> = {});

using ArbitrarySubstitutionFunctionArguments = Vector<Vector<ComponentValue>>;
[[nodiscard]] Optional<ArbitrarySubstitutionFunctionArguments> parse_according_to_argument_grammar(ArbitrarySubstitutionFunction, Vector<ComponentValue> const&);

[[nodiscard]] Vector<ComponentValue> replace_an_arbitrary_substitution_function(DOM::AbstractElement&, GuardedSubstitutionContexts&, ArbitrarySubstitutionFunction, ArbitrarySubstitutionFunctionArguments const&);

}
