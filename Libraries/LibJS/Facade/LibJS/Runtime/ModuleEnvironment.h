/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/DeclarativeEnvironment.h>
#include <LibJS/Runtime/Environment.h>

namespace JS {

// 9.1.1.5 Module Environment Records, https://tc39.es/ecma262/#sec-module-environment-records
class JS_API ModuleEnvironment final : public DeclarativeEnvironment {
public:
    static bool is_environment_kind_of(Environment const& environment) { return environment.engine_environment_kind() == EngineEnvironmentKind::Module; }
};

// 9.1.2.6 NewModuleEnvironment ( E ), https://tc39.es/ecma262/#sec-newmoduleenvironment
JS_API GC::Ref<ModuleEnvironment> new_module_environment(GC::Ptr<Environment> outer_environment);

}
