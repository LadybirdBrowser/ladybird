/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class ErrorConstructor final : public NativeFunction {
public:
    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_ERROR_CONSTRUCTOR; }
};

#define DECLARE_NATIVE_ERROR_CONSTRUCTOR(ConstructorName, layout_class_id)                                           \
    class ConstructorName final : public NativeFunction {                                                            \
    public:                                                                                                          \
        static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == layout_class_id; } \
    };

DECLARE_NATIVE_ERROR_CONSTRUCTOR(EvalErrorConstructor, JS_LAYOUT_CLASS_ID_EVAL_ERROR_CONSTRUCTOR)
DECLARE_NATIVE_ERROR_CONSTRUCTOR(InternalErrorConstructor, JS_LAYOUT_CLASS_ID_INTERNAL_ERROR_CONSTRUCTOR)
DECLARE_NATIVE_ERROR_CONSTRUCTOR(RangeErrorConstructor, JS_LAYOUT_CLASS_ID_RANGE_ERROR_CONSTRUCTOR)
DECLARE_NATIVE_ERROR_CONSTRUCTOR(ReferenceErrorConstructor, JS_LAYOUT_CLASS_ID_REFERENCE_ERROR_CONSTRUCTOR)
DECLARE_NATIVE_ERROR_CONSTRUCTOR(SyntaxErrorConstructor, JS_LAYOUT_CLASS_ID_SYNTAX_ERROR_CONSTRUCTOR)
DECLARE_NATIVE_ERROR_CONSTRUCTOR(TypeErrorConstructor, JS_LAYOUT_CLASS_ID_TYPE_ERROR_CONSTRUCTOR)
DECLARE_NATIVE_ERROR_CONSTRUCTOR(URIErrorConstructor, JS_LAYOUT_CLASS_ID_URI_ERROR_CONSTRUCTOR)
#undef DECLARE_NATIVE_ERROR_CONSTRUCTOR

}
