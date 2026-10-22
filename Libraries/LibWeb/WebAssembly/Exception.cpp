/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Exception.h>
#include <LibWeb/WebAssembly/BindingsGlue.h>
#include <LibWeb/WebAssembly/Exception.h>
#include <LibWeb/WebAssembly/Tag.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Exception);

GC::Ref<Exception> Exception::create(u64 exception, Optional<Utf16String> stack)
{
    // https://webassembly.github.io/spec/js-api/#initialize-an-exception-object
    // 1. Let map be the surrounding agent's associated exception object cache.
    // 2. Assert: map[exnAddr] doesn't exist.
    auto& cache = Detail::WebAssemblyCache::the();
    VERIFY(!cache.exception_object(exception));

    // 3. Set exn.[[Address]] to exnAddr.
    // 4. Set map[exnAddr] to exn.
    // 5. Let store be the surrounding agent's associated store.
    // 6. Let tagaddr be exn_tag(store, exnAddr).
    // 7. Let payload be exn_read(store, exnAddr).
    // 8. Set exn.[[Type]] to tagaddr.
    // 9. Set exn.[[Payload]] to payload.
    // NB: The payload of an exception never changes, so getArg() reads it from the store.
    auto tag = cache.store().exception_tag(exception);
    auto object = GC::Heap::the().allocate<Exception>(exception, tag, move(stack));
    cache.set_exception_object(exception, object);
    return object;
}

// https://webassembly.github.io/spec/js-api/#create-an-exception-object
GC::Ref<Exception> Exception::from_address(u64 exception)
{
    // 1. Let map be the surrounding agent's associated exception object cache.
    // 2. If map[exnAddr] exists,
    //     1. Return map[exnAddr].
    if (auto object = Detail::WebAssemblyCache::the().exception_object(exception))
        return *object;

    // 3. Let exn be a new Exception.
    // 4. Initialize exn from exnAddr.
    // 5. Set exn.[[Stack]] to undefined.
    // 6. Return exn.
    return create(exception, {});
}

Exception::Exception(u64 exception, Wasm::Runtime::Entity tag, Optional<Utf16String> stack)
    : m_exception(exception)
    , m_tag(tag)
    , m_stack(move(stack))
{
}

void Exception::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    // NB: An exception reference is the address of the exception's cell.
    visitor.visit(reinterpret_cast<GC::ForeignCell*>(static_cast<FlatPtr>(m_exception)));
    visitor.visit(m_tag);
}

// https://webassembly.github.io/spec/js-api/#dom-exception-is
bool Exception::is(Tag const& tag) const
{
    // 1. If this.[[Type]] is not equal to exceptionTag.[[Address]], return false.
    // 2. Return true.
    return m_tag.ptr() == tag.address();
}

// https://webassembly.github.io/spec/js-api/#dom-exception-stack
Variant<Utf16String, Empty> Exception::stack() const
{
    // 1. Return this.[[Stack]].
    if (m_stack.has_value())
        return *m_stack;
    return Empty {};
}

}

namespace Web::Bindings {

// https://webassembly.github.io/spec/js-api/#dom-exception-exception
WebIDL::ExceptionOr<GC::Ref<WebAssembly::Exception>> construct_exception(JS::Realm& realm, WebAssembly::Tag& exception_tag, GC::RootVector<JS::Value> const& payload, ExceptionOptions const& options)
{
    auto& vm = realm.vm();
    auto& cache = WebAssembly::Detail::WebAssemblyCache::the();
    auto& store = cache.store();

    // 1. Let JSTagAddr be the result of getting the JavaScript exception tag.
    // 2. If exceptionTag.[[Address]] is equal to JSTagAddr,
    //     1. Throw a TypeError.
    if (exception_tag.address() == cache.js_tag())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot construct an exception with the JavaScript tag"_utf16 };

    // 3. Let store be the surrounding agent's associated store.
    // 4. Let [types] → [] be tag_type(store, exceptionTag.[[Address]]).
    auto types = store.tag_parameters(exception_tag.address());

    // 5. If types's size is not payload's size,
    //     1. Throw a TypeError.
    if (types.size() != payload.size())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("The tag takes {} values, not {}", types.size(), payload.size()) };

    // 6. Let wasmPayload be « ».
    // NB: The converted values may refer to new cells, which this vector keeps alive until they are in the exception.
    Wasm::Runtime::Values wasm_payload;

    // 7. For each value and resultType of payload and types, paired linearly,
    for (size_t index = 0; index < payload.size(); ++index) {
        // 1. If resultType is v128 or exnref,
        //     1. Throw a TypeError.
        // 2. Append ? ToWebAssemblyValue(value, resultType) to wasmPayload.
        // NB: ToWebAssemblyValue throws the TypeError for v128 and exnref.
        wasm_payload.append(TRY(WebAssembly::Detail::to_webassembly_value(vm, payload[index], types[index])));
    }

    // 8. Let (store, exceptionAddr) be exn_alloc(store, exceptionTag.[[Address]], wasmPayload).
    // 9. Set the surrounding agent's associated store to store.
    auto exception = store.allocate_exception(exception_tag.address(), wasm_payload);

    // 10. Initialize this from exceptionAddr.
    // 11. If options["traceStack"] is true,
    //     1. Set this.[[Stack]] to either a DOMString representation of the current call stack or undefined.
    // 12. Otherwise,
    //     1. Set this.[[Stack]] to undefined.
    Optional<Utf16String> stack;
    if (options.trace_stack) {
        auto error = JS::Error::create(realm);
        stack = error->stack_string();
    }
    return WebAssembly::Exception::create(exception, move(stack));
}

// https://webassembly.github.io/spec/js-api/#dom-exception-getarg
WebIDL::ExceptionOr<JS::Value> get_arg(WebAssembly::Exception& exception, WebAssembly::Tag& exception_tag, u32 index)
{
    auto& store = WebAssembly::Detail::WebAssemblyCache::the().store();

    // 1. If this.[[Type]] is not equal to exceptionTag.[[Address]],
    //     1. Throw a TypeError.
    if (exception.tag_address() != exception_tag.address())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The exception does not have this tag"_utf16 };

    // 2. Let payload be this.[[Payload]].
    auto payload = store.exception_values(exception.address());

    // 3. If index ≥ payload's size,
    //     1. Throw a RangeError.
    if (index >= payload.size())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, Utf16String::formatted("The exception has {} values", payload.size()) };

    // 4. Let [types] → [] be tag_type(store, exceptionTag.[[Address]]).
    auto types = store.tag_parameters(exception_tag.address());

    // 5. If types[index] matches v128 or exnref,
    //     1. Throw a TypeError.
    // 6. Return ToJSValue(payload[index]).
    // NB: ToJSValue throws the TypeError for v128 and exnref.
    return TRY(WebAssembly::Detail::to_js_value(JS::VM::the(), payload[index], types[index]));
}

WebAssembly::Exception* exception_from_value(JS::Value value)
{
    return value.is_object() ? Bindings::impl_from<WebAssembly::Exception>(&value.as_object()) : nullptr;
}

}
