/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Utf16FlyString.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// 9.1 Environment Records, https://tc39.es/ecma262/#sec-environment-records
// An Environment Record of the Rust runtime. The runtime dispatches each abstract method on the kind of the record, so
// these types have no virtual functions, and is<T>(), as<T>() and as_if<T>() recognize the kind with
// `static bool T::is_environment_kind_of(Environment const&)`.
class JS_API Environment : public EngineCell {
public:
    enum class InitializeBindingHint {
        Normal,
        SyncDispose,
        AsyncDispose,
    };

    ThrowCompletionOr<void> create_immutable_binding(VM&, Utf16FlyString const& name, bool strict);
    ThrowCompletionOr<void> initialize_binding(VM&, Utf16FlyString const& name, Value, InitializeBindingHint);
    ThrowCompletionOr<void> set_mutable_binding(VM&, Utf16FlyString const& name, Value, bool strict);
    ThrowCompletionOr<Value> get_binding_value(VM&, Utf16FlyString const& name, bool strict);
    ThrowCompletionOr<bool> delete_binding(VM&, Utf16FlyString const& name);

    // [[OuterEnv]]
    Environment* outer_environment();
    Environment const* outer_environment() const;

    // Function and module Environment Records are declarative Environment Records, too.
    enum class EngineEnvironmentKind : u8 {
        Declarative,
        Function,
        Module,
        Global,
        Object,
    };
    EngineEnvironmentKind engine_environment_kind() const;

    [[nodiscard]] bool is_declarative_environment() const
    {
        auto kind = engine_environment_kind();
        return kind == EngineEnvironmentKind::Declarative || kind == EngineEnvironmentKind::Function || kind == EngineEnvironmentKind::Module;
    }
    bool is_global_environment() const { return engine_environment_kind() == EngineEnvironmentKind::Global; }
    bool is_function_environment() const { return engine_environment_kind() == EngineEnvironmentKind::Function; }
    bool is_object_environment() const { return engine_environment_kind() == EngineEnvironmentKind::Object; }

    template<typename T>
    requires(IsBaseOf<Environment, T> && requires(Environment const& environment) { { T::is_environment_kind_of(environment) } -> SameAs<bool>; })
    bool fast_is() const
    {
        return T::is_environment_kind_of(*this);
    }
};

}
