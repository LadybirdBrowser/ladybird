/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

class FunctionObject : public Object {
    JS_OBJECT(FunctionObject, Object);

public:
    virtual ~FunctionObject() = default;

    // Table 5: Additional Essential Internal Methods of Function Objects, https://tc39.es/ecma262/#table-additional-essential-internal-methods-of-function-objects

    virtual ThrowCompletionOr<Value> internal_call(Value this_argument, ReadonlySpan<Value> arguments_list) = 0;
    virtual ThrowCompletionOr<GC::Ref<Object>> internal_construct([[maybe_unused]] ReadonlySpan<Value> arguments_list, [[maybe_unused]] FunctionObject& new_target) { VERIFY_NOT_REACHED(); }

    virtual FlyString const& name() const = 0;

    void set_function_name(Variant<PropertyKey, PrivateName> const& name_arg, Optional<StringView> const& prefix = {});
    void set_function_length(double length);

    virtual bool is_strict_mode() const { return false; }

    virtual bool has_constructor() const { return false; }

    // [[Realm]]
    virtual Realm* realm() const { return nullptr; }

    virtual Vector<FlyString> const& local_variables_names() const { VERIFY_NOT_REACHED(); }

    virtual NonnullRefPtr<FunctionParameters const> const& formal_parameters() const { VERIFY_NOT_REACHED(); }

protected:
    explicit FunctionObject(Realm&, Object* prototype, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);
    explicit FunctionObject(Object& prototype, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);

private:
    virtual bool is_function() const override { return true; }
};

}
