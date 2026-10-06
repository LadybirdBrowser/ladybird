/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/ErrorData.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

// An Error object of the runtime. Its [[ErrorData]] lives inside it rather than at its address, so an Error converts
// to its ErrorData, where the C++ runtime's Error derives from it.
class JS_API Error : public Object {
public:
    static GC::Ref<Error> create(Realm&);
    static GC::Ref<Error> create(Realm&, Utf16String message);
    static GC::Ref<Error> create(Realm&, Utf16View message);

    // For an embedder's error type, whose instances are Error objects that inherit from a prototype of its own.
    static GC::Ref<Error> create(Realm&, Object& prototype);
    static GC::Ref<Error> create(Realm&, Object& prototype, Utf16String message);

    [[nodiscard]] Utf16String stack_string(CompactTraceback compact = CompactTraceback::No) const;

    ThrowCompletionOr<void> install_error_cause(Value options);

    void set_message(Utf16String);
    void set_message(Utf16View);

    operator ErrorData&() { return own_error_data(); }
    operator ErrorData const&() const { return own_error_data(); }

    static bool is_engine_class_of(Object const& object) { return object.is_of_engine_class_or_subclass(JS_LAYOUT_CLASS_ID_ERROR); }

protected:
    // A new error, without a message, of one of the runtime's error kinds (a JS_ERROR_KIND_*) in the realm.
    static Error& create_of_engine_error_kind(Realm&, u8 engine_error_kind);

private:
    ErrorData& own_error_data() const;
};

// NOTE: Making these inherit from Error is not required by the spec but
//       our way of implementing the [[ErrorData]] internal slot, which is
//       used in Object.prototype.toString().
#define DECLARE_NATIVE_ERROR(ClassName, layout_class_id)                                                             \
    class JS_API ClassName final : public Error {                                                                    \
    public:                                                                                                          \
        static GC::Ref<ClassName> create(Realm&);                                                                    \
        static GC::Ref<ClassName> create(Realm&, Utf16String message);                                               \
        static GC::Ref<ClassName> create(Realm&, Utf16View message);                                                 \
                                                                                                                     \
        static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == layout_class_id; } \
    };

DECLARE_NATIVE_ERROR(EvalError, JS_LAYOUT_CLASS_ID_EVAL_ERROR)
DECLARE_NATIVE_ERROR(InternalError, JS_LAYOUT_CLASS_ID_INTERNAL_ERROR)
DECLARE_NATIVE_ERROR(RangeError, JS_LAYOUT_CLASS_ID_RANGE_ERROR)
DECLARE_NATIVE_ERROR(ReferenceError, JS_LAYOUT_CLASS_ID_REFERENCE_ERROR)
DECLARE_NATIVE_ERROR(SyntaxError, JS_LAYOUT_CLASS_ID_SYNTAX_ERROR)
DECLARE_NATIVE_ERROR(TypeError, JS_LAYOUT_CLASS_ID_TYPE_ERROR)
DECLARE_NATIVE_ERROR(URIError, JS_LAYOUT_CLASS_ID_URI_ERROR)
#undef DECLARE_NATIVE_ERROR

}
