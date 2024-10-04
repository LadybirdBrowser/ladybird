/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class ErrorPrototype final : public PrototypeObject<ErrorPrototype, Error> {
    JS_PROTOTYPE_OBJECT(ErrorPrototype, Error, Error);
    JS_DECLARE_ALLOCATOR(ErrorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~ErrorPrototype() override = default;

private:
    explicit ErrorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(stack_getter);
    JS_DECLARE_NATIVE_FUNCTION(stack_setter);
};

#define DECLARE_NATIVE_ERROR_PROTOTYPE(ClassName, snake_name, PrototypeName, ConstructorName) \
    class PrototypeName final : public PrototypeObject<PrototypeName, ClassName> {            \
        JS_PROTOTYPE_OBJECT(PrototypeName, ClassName, ClassName);                             \
        JS_DECLARE_ALLOCATOR(PrototypeName);                                                  \
                                                                                              \
    public:                                                                                   \
        virtual void initialize(Realm&) override;                                             \
        virtual ~PrototypeName() override = default;                                          \
                                                                                              \
    private:                                                                                  \
        explicit PrototypeName(Realm&);                                                       \
    };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    DECLARE_NATIVE_ERROR_PROTOTYPE(ClassName, snake_name, PrototypeName, ConstructorName)
JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE

}
