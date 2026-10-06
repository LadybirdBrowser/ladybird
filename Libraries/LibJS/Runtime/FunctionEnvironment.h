/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/FunctionObject.h>

namespace JS {

// 9.1.1.3 Function Environment Records, https://tc39.es/ecma262/#sec-function-environment-records
class JS_API FunctionEnvironment final : public DeclarativeEnvironment {
public:
    static bool is_environment_kind_of(Environment const& environment) { return environment.is_function_environment(); }

    // [[FunctionObject]]
    FunctionObject& function_object();
    FunctionObject const& function_object() const;
};

}
