/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

#define JS_PROTOTYPE_OBJECT(prototype_class, object_class, display_name_) \
    using Prototype = PrototypeObject<prototype_class, object_class>;     \
    JS_OBJECT(prototype_class, Prototype)                                 \
    static constexpr StringView display_name()                            \
    {                                                                     \
        return #display_name_##sv;                                        \
    }

template<typename PrototypeType, typename ObjectType>
class PrototypeObject : public Object {
    JS_OBJECT(PrototypeObject, Object);

public:
    virtual ~PrototypeObject() override = default;

    static ThrowCompletionOr<NonnullGCPtr<Object>> this_object(VM& vm)
    {
        auto this_value = vm.this_value();
        if (!this_value.is_object())
            return vm.throw_completion<TypeError>(ErrorType::NotAnObject, this_value);
        return this_value.as_object();
    }

    // Use typed_this_object() when the spec coerces |this| value to an object.
    static ThrowCompletionOr<NonnullGCPtr<ObjectType>> typed_this_object(VM& vm)
    {
        auto this_object = TRY(vm.this_value().to_object(vm));
        if (!is<ObjectType>(*this_object))
            return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, PrototypeType::display_name());
        return static_cast<ObjectType&>(*this_object);
    }

    // Use typed_this_value() when the spec does not coerce |this| value to an object.
    static ThrowCompletionOr<NonnullGCPtr<ObjectType>> typed_this_value(VM& vm)
    {
        auto this_value = vm.this_value();
        if (!this_value.is_object() || !is<ObjectType>(this_value.as_object()))
            return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, PrototypeType::display_name());
        return static_cast<ObjectType&>(this_value.as_object());
    }

protected:
    explicit PrototypeObject(Object& prototype)
        : Object(ConstructWithPrototypeTag::Tag, prototype)
    {
    }
};

}
