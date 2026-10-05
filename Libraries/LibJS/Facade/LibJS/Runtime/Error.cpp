/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

static JSObject* error_to_abi(Error const& error)
{
    return object_to_abi(error);
}

Error& Error::create_of_engine_error_kind(Realm& realm, u8 engine_error_kind)
{
    auto& error = object_from_abi(js_error_create(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), engine_error_kind));
    return static_cast<Error&>(error);
}

GC::Ref<Error> Error::create(Realm& realm)
{
    return create_of_engine_error_kind(realm, JS_ERROR_KIND_ERROR);
}

GC::Ref<Error> Error::create(Realm& realm, Utf16String message)
{
    auto error = Error::create(realm);
    error->set_message(move(message));
    return error;
}

GC::Ref<Error> Error::create(Realm& realm, Utf16View message)
{
    auto error = Error::create(realm);
    error->set_message(message);
    return error;
}

GC::Ref<Error> Error::create(Realm& realm, Object& prototype)
{
    auto& error = object_from_abi(js_error_create_with_prototype(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), object_to_abi(prototype)));
    return static_cast<Error&>(error);
}

GC::Ref<Error> Error::create(Realm& realm, Object& prototype, Utf16String message)
{
    auto error = Error::create(realm, prototype);
    error->set_message(move(message));
    return error;
}

Utf16String Error::stack_string(CompactTraceback compact) const
{
    return own_error_data().stack_string(compact);
}

// 20.5.8.1 InstallErrorCause ( O, options ), https://tc39.es/ecma262/#sec-installerrorcause
ThrowCompletionOr<void> Error::install_error_cause(Value options)
{
    return completion_from_abi<void>(js_error_install_error_cause(vm_to_abi(vm()), error_to_abi(*this), value_to_abi(options)));
}

void Error::set_message(Utf16String message)
{
    js_error_set_owned_message(vm_to_abi(vm()), error_to_abi(*this), owned_utf16_string_to_abi(move(message)));
}

void Error::set_message(Utf16View message)
{
    js_error_set_message(vm_to_abi(vm()), error_to_abi(*this), utf16_view_to_abi(message));
}

ErrorData& Error::own_error_data() const
{
    auto const* error_data = js_error_data_of(error_to_abi(*this));
    VERIFY(error_data);
    return *const_cast<ErrorData*>(reinterpret_cast<ErrorData const*>(error_data));
}

#define DEFINE_NATIVE_ERROR(ClassName, engine_error_kind)                                      \
    GC::Ref<ClassName> ClassName::create(Realm& realm)                                         \
    {                                                                                          \
        return static_cast<ClassName&>(create_of_engine_error_kind(realm, engine_error_kind)); \
    }                                                                                          \
                                                                                               \
    GC::Ref<ClassName> ClassName::create(Realm& realm, Utf16String message)                    \
    {                                                                                          \
        auto error = ClassName::create(realm);                                                 \
        error->set_message(move(message));                                                     \
        return error;                                                                          \
    }                                                                                          \
                                                                                               \
    GC::Ref<ClassName> ClassName::create(Realm& realm, Utf16View message)                      \
    {                                                                                          \
        auto error = ClassName::create(realm);                                                 \
        error->set_message(message);                                                           \
        return error;                                                                          \
    }

DEFINE_NATIVE_ERROR(EvalError, JS_ERROR_KIND_EVAL_ERROR)
DEFINE_NATIVE_ERROR(InternalError, JS_ERROR_KIND_INTERNAL_ERROR)
DEFINE_NATIVE_ERROR(RangeError, JS_ERROR_KIND_RANGE_ERROR)
DEFINE_NATIVE_ERROR(ReferenceError, JS_ERROR_KIND_REFERENCE_ERROR)
DEFINE_NATIVE_ERROR(SyntaxError, JS_ERROR_KIND_SYNTAX_ERROR)
DEFINE_NATIVE_ERROR(TypeError, JS_ERROR_KIND_TYPE_ERROR)
DEFINE_NATIVE_ERROR(URIError, JS_ERROR_KIND_URI_ERROR)
#undef DEFINE_NATIVE_ERROR

}
