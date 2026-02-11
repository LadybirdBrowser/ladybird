/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibJS/Bytecode/Builtins.h>
#include <LibJS/LocalVariable.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

class FunctionObject : public Object {
    JS_OBJECT(FunctionObject, Object);

public:
    virtual ~FunctionObject() = default;

    // Table 5: Additional Essential Internal Methods of Function Objects, https://tc39.es/ecma262/#table-additional-essential-internal-methods-of-function-objects

    virtual void get_stack_frame_size([[maybe_unused]] size_t& registers_and_locals_count, [[maybe_unused]] size_t& constants_count, [[maybe_unused]] size_t& argument_count) { }
    virtual ThrowCompletionOr<Value> internal_call(ExecutionContext&, Value this_argument) = 0;
    virtual ThrowCompletionOr<GC::Ref<Object>> internal_construct(ExecutionContext&, [[maybe_unused]] FunctionObject& new_target) { VERIFY_NOT_REACHED(); }

    void set_function_name(Variant<PropertyKey, PrivateName> const& name_arg, Optional<StringView> const& prefix = {});
    void set_function_length(double length);

    virtual bool is_strict_mode() const { return false; }

    virtual bool has_constructor() const { return false; }

    // [[Realm]]
    virtual Realm* realm() const { return nullptr; }

    virtual Vector<LocalVariable> const& local_variables_names() const { VERIFY_NOT_REACHED(); }

    virtual Utf16String name_for_call_stack() const = 0;

    bool is_array_prototype_next_builtin() const { return m_builtin.has_value() && *m_builtin == Bytecode::Builtin::ArrayIteratorPrototypeNext; }
    bool is_map_prototype_next_builtin() const { return m_builtin.has_value() && *m_builtin == Bytecode::Builtin::MapIteratorPrototypeNext; }
    bool is_set_prototype_next_builtin() const { return m_builtin.has_value() && *m_builtin == Bytecode::Builtin::SetIteratorPrototypeNext; }
    bool is_string_prototype_next_builtin() const { return m_builtin.has_value() && *m_builtin == Bytecode::Builtin::StringIteratorPrototypeNext; }

    Optional<Bytecode::Builtin> builtin() const { return m_builtin; }

protected:
    explicit FunctionObject(Realm&, Object* prototype, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);
    explicit FunctionObject(Object& prototype, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);

    [[nodiscard]] GC::Ref<PrimitiveString> make_function_name(Variant<PropertyKey, PrivateName> const&, Optional<StringView> const& prefix);

    Optional<Bytecode::Builtin> m_builtin;

private:
    virtual bool is_function() const override { return true; }
};

template<>
inline bool Object::fast_is<FunctionObject>() const { return is_function(); }

}
