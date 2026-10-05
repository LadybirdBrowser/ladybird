/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// 9.1.1.1 Declarative Environment Records, https://tc39.es/ecma262/#sec-declarative-environment-records
class JS_API DeclarativeEnvironment : public Environment {
public:
    static bool is_environment_kind_of(Environment const& environment) { return environment.is_declarative_environment(); }

    // This is not a method defined in the spec! Do not use this in any LibJS (or other spec related) code.
    [[nodiscard]] Vector<Utf16FlyString> bindings() const;

    bool binding_is_mutable_by_name(Utf16FlyString const&) const;
};

}
