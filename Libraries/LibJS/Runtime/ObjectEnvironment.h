/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Environment.h>

namespace JS {

// 9.1.1.2 Object Environment Records, https://tc39.es/ecma262/#sec-object-environment-records
class JS_API ObjectEnvironment final : public Environment {
public:
    static bool is_environment_kind_of(Environment const& environment) { return environment.is_object_environment(); }

    // [[BindingObject]], The binding object of this Environment Record.
    Object& binding_object();
};

}
