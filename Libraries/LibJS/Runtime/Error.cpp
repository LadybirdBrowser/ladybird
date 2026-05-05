/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Error);

GC::Ref<Error> Error::create(Realm& realm)
{
    return realm.create<Error>(realm.intrinsics().error_prototype());
}

GC::Ref<Error> Error::create(Realm& realm, Utf16String message)
{
    auto error = Error::create(realm);
    error->set_message(move(message));
    return error;
}

GC::Ref<Error> Error::create(Realm& realm, StringView message)
{
    return create(realm, Utf16String::from_utf8(message));
}

Utf16String Error::stack_string(CompactTraceback compact) const
{
    return ErrorData::stack_string(compact);
}

Error::Error(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , ErrorData(prototype.vm())
{
}

void Error::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    ErrorData::visit_edges(visitor);
}

size_t Error::external_memory_size() const
{
    return Object::external_memory_size() + ErrorData::external_memory_size();
}

// 20.5.8.1 InstallErrorCause ( O, options ), https://tc39.es/ecma262/#sec-installerrorcause
ThrowCompletionOr<void> Error::install_error_cause(Value options)
{
    auto& vm = this->vm();

    // 1. If Type(options) is Object and ? HasProperty(options, "cause") is true, then
    if (options.is_object() && TRY(options.as_object().has_property(vm.names.cause))) {
        // a. Let cause be ? Get(options, "cause").
        auto cause = TRY(options.as_object().get(vm.names.cause));

        // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "cause", cause).
        create_non_enumerable_data_property_or_throw(vm.names.cause, cause);
    }

    // 2. Return unused.
    return {};
}

void Error::set_message(Utf16String message)
{
    auto& vm = this->vm();

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_direct_property(vm.names.message, PrimitiveString::create(vm, move(message)), attr);
}

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    GC_DEFINE_ALLOCATOR(ClassName);                                                      \
    GC::Ref<ClassName> ClassName::create(Realm& realm)                                   \
    {                                                                                    \
        return realm.create<ClassName>(realm.intrinsics().snake_name##_prototype());     \
    }                                                                                    \
                                                                                         \
    GC::Ref<ClassName> ClassName::create(Realm& realm, Utf16String message)              \
    {                                                                                    \
        auto error = ClassName::create(realm);                                           \
        error->set_message(move(message));                                               \
        return error;                                                                    \
    }                                                                                    \
                                                                                         \
    GC::Ref<ClassName> ClassName::create(Realm& realm, StringView message)               \
    {                                                                                    \
        return create(realm, Utf16String::from_utf8(message));                           \
    }                                                                                    \
                                                                                         \
    ClassName::ClassName(Object& prototype)                                              \
        : Error(prototype)                                                               \
    {                                                                                    \
    }

JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE

}
