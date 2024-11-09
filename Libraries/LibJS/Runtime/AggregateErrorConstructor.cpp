/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/AggregateError.h>
#include <LibJS/Runtime/AggregateErrorConstructor.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ErrorConstructor.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>

namespace JS {

JS_DEFINE_ALLOCATOR(AggregateErrorConstructor);

AggregateErrorConstructor::AggregateErrorConstructor(Realm& realm)
    : NativeFunction(static_cast<Object&>(*realm.intrinsics().error_constructor()))
{
}

void AggregateErrorConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 20.5.7.2.1 AggregateError.prototype, https://tc39.es/ecma262/#sec-aggregate-error.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().aggregate_error_prototype(), 0);

    define_direct_property(vm.names.length, Value(2), Attribute::Configurable);
}

// 20.5.7.1.1 AggregateError ( errors, message [ , options ] ), https://tc39.es/ecma262/#sec-aggregate-error
ThrowCompletionOr<Value> AggregateErrorConstructor::call()
{
    // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
    return TRY(construct(*this));
}

// 20.5.7.1.1 AggregateError ( errors, message [ , options ] ), https://tc39.es/ecma262/#sec-aggregate-error
ThrowCompletionOr<NonnullGCPtr<Object>> AggregateErrorConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    auto errors = vm.argument(0);
    auto message = vm.argument(1);
    auto options = vm.argument(2);

    // 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%AggregateError.prototype%", « [[ErrorData]] »).
    auto aggregate_error = TRY(ordinary_create_from_constructor<AggregateError>(vm, new_target, &Intrinsics::aggregate_error_prototype));

    // 3. If message is not undefined, then
    if (!message.is_undefined()) {
        // a. Let msg be ? ToString(message).
        auto msg = TRY(message.to_byte_string(vm));

        // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message", msg).
        aggregate_error->create_non_enumerable_data_property_or_throw(vm.names.message, PrimitiveString::create(vm, msg));
    }

    // 4. Perform ? InstallErrorCause(O, options).
    TRY(aggregate_error->install_error_cause(options));

    // 5. Let errorsList be ? IteratorToList(? GetIterator(errors, sync)).
    auto errors_list = TRY(iterator_to_list(vm, TRY(get_iterator(vm, errors, IteratorHint::Sync))));

    // 6. Perform ! DefinePropertyOrThrow(O, "errors", PropertyDescriptor { [[Configurable]]: true, [[Enumerable]]: false, [[Writable]]: true, [[Value]]: CreateArrayFromList(errorsList) }).
    MUST(aggregate_error->define_property_or_throw(vm.names.errors, { .value = Array::create_from(realm, errors_list), .writable = true, .enumerable = false, .configurable = true }));

    // 7. Return O.
    return aggregate_error;
}

}
