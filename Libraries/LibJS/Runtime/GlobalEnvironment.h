/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Environment.h>

namespace JS {

// 9.1.1.4 Global Environment Records, https://tc39.es/ecma262/#sec-global-environment-records
class JS_API GlobalEnvironment final : public Environment {
public:
    static bool is_environment_kind_of(Environment const& environment) { return environment.is_global_environment(); }

    // [[GlobalThisValue]]
    Object& global_this_value();

    // [[DeclarativeRecord]]
    DeclarativeEnvironment& declarative_record();
};

}
