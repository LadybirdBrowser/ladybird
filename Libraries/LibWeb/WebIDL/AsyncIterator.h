/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebIDL {

class WEB_API AsyncIterator : public Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(AsyncIterator, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AsyncIterator);

public:
    virtual ~AsyncIterator() override;

    // https://webidl.spec.whatwg.org/#ref-for-dfn-asynchronous-iterator-prototype-object%E2%91%A2
    template<typename AsyncIteratorInterface>
    static JS::ThrowCompletionOr<GC::Ptr<JS::Object>> next(JS::Realm& realm, StringView interface_name)
    {
        auto validation_result = validate_this<AsyncIteratorInterface>(realm, interface_name);

        return validation_result.visit(
            [](GC::Ref<AsyncIteratorInterface> iterator) {
                return iterator->iterator_next_impl();
            },
            [](JS::ThrowCompletionOr<GC::Ptr<JS::Object>> result) {
                return result;
            });
    }

    // https://webidl.spec.whatwg.org/#ref-for-asynchronous-iterator-return
    template<typename AsyncIteratorInterface>
    static JS::ThrowCompletionOr<GC::Ptr<JS::Object>> return_(JS::Realm& realm, StringView interface_name, JS::Value value)
    {
        auto return_promise_capability = WebIDL::create_promise(realm);
        auto validation_result = validate_this<AsyncIteratorInterface>(realm, interface_name, return_promise_capability);

        return validation_result.visit(
            [&](GC::Ref<AsyncIteratorInterface> iterator) {
                return iterator->iterator_return_impl(return_promise_capability, value);
            },
            [](JS::ThrowCompletionOr<GC::Ptr<JS::Object>> result) {
                return result;
            });
    }

protected:
    AsyncIterator(JS::Realm&, JS::Object::PropertyKind);

    virtual void visit_edges(Cell::Visitor&) override;

    virtual GC::Ref<WebIDL::Promise> next_iteration_result(JS::Realm&) = 0;
    virtual GC::Ref<WebIDL::Promise> iterator_return(JS::Realm&, JS::Value);

private:
    template<typename AsyncIteratorInterface>
    static Variant<JS::Completion, GC::Ref<JS::Object>, GC::Ref<AsyncIteratorInterface>> validate_this(JS::Realm& realm, StringView interface_name, GC::Ptr<WebIDL::Promise> this_validation_promise_capability = {})
    {
        // NOTE: This defines the steps to validate `this` that are common between "next" and "return".
        auto& vm = realm.vm();

        // 1. Let interface be the interface for which the asynchronous iterator prototype object exists.
        // 2. Let thisValidationPromiseCapability be ! NewPromiseCapability(%Promise%).
        if (!this_validation_promise_capability)
            this_validation_promise_capability = WebIDL::create_promise(realm);

        // 3. Let thisValue be the this value.
        auto this_value = vm.this_value();

        // 4. Let object be Completion(ToObject(thisValue)).
        // 5. IfAbruptRejectPromise(object, thisValidationPromiseCapability).
        auto object = TRY_OR_REJECT(vm, this_validation_promise_capability, this_value.to_object(vm));

        // FIXME: 6. If object is a platform object, then perform a security check, passing:
        //     * the platform object object,
        //     * the identifier "next", and
        //     * the type "method".
        //
        //     If this threw an exception e, then:
        //         Perform ! Call(thisValidationPromiseCapability.[[Reject]], undefined, « e »).
        //         Return thisValidationPromiseCapability.[[Promise]].

        // 7. If object is not a default asynchronous iterator object for interface, then:
        auto* iterator = as_if<AsyncIteratorInterface>(*object);

        if (!iterator) {
            // 1. Let error be a new TypeError.
            auto error = vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, interface_name);

            // 2. Perform ! Call(thisValidationPromiseCapability.[[Reject]], undefined, « error »).
            // 3. Return thisValidationPromiseCapability.[[Promise]].
            TRY_OR_MUST_REJECT(vm, this_validation_promise_capability, error);
            VERIFY_NOT_REACHED();
        }

        return GC::Ref { *iterator };
    }

    JS::ThrowCompletionOr<GC::Ptr<JS::Object>> iterator_next_impl();
    JS::ThrowCompletionOr<GC::Ptr<JS::Object>> iterator_return_impl(GC::Ref<WebIDL::Promise> return_promise_capability, JS::Value);

    JS::Object::PropertyKind m_kind { JS::Object::PropertyKind::Value };
    GC::Ptr<JS::Promise> m_ongoing_promise;
    bool m_is_finished { false };
};

}
