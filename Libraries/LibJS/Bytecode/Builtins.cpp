/*
 * Copyright (c) 2023, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/AST.h>
#include <LibJS/Bytecode/Builtins.h>

namespace JS::Bytecode {

Optional<Builtin> get_builtin(MemberExpression const& expression)
{
    if (expression.is_computed() || !expression.object().is_identifier() || !expression.property().is_identifier())
        return {};
    auto base_name = static_cast<Identifier const&>(expression.object()).string();
    auto property_name = static_cast<Identifier const&>(expression.property()).string();
#define CHECK_MEMBER_BUILTIN(name, snake_case_name, base, property, ...) \
    if (base_name == #base##sv && property_name == #property##sv)        \
        return Builtin::name;
    JS_ENUMERATE_BUILTINS(CHECK_MEMBER_BUILTIN)
#undef CHECK_MEMBER_BUILTIN
    return {};
}

}
