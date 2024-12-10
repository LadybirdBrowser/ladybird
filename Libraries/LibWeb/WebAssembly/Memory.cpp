/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/SharedArrayBufferConstructor.h>
#include <LibJS/Runtime/VM.h>
#include <LibWasm/Types.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MemoryPrototype.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Memory);

WebIDL::ExceptionOr<GC::Ref<Memory>> Memory::construct_impl(JS::Realm& realm, MemoryDescriptor& descriptor)
{
    auto& vm = realm.vm();

    // https://webassembly.github.io/threads/js-api/index.html#dom-memory-memory
    // 4. Let share be shared if descriptor["shared"] is true and unshared otherwise.
    // 5. If share is shared and maximum is empty, throw a TypeError exception.
    auto shared = descriptor.shared.value_or(false);
    if (shared && !descriptor.maximum.has_value())
        return vm.throw_completion<JS::TypeError>("Maximum has to be specified for shared memory."sv);

    Wasm::Limits limits { descriptor.initial, move(descriptor.maximum) };
    Wasm::MemoryType memory_type { move(limits) };

    auto& cache = Detail::get_cache(realm);
    auto address = cache.abstract_machine().store().allocate(memory_type);
    if (!address.has_value())
        return vm.throw_completion<JS::TypeError>("Wasm Memory allocation failed"sv);

    auto memory_object = realm.create<Memory>(realm, *address, shared ? Shared::Yes : Shared::No);

    return memory_object;
}

Memory::Memory(JS::Realm& realm, Wasm::MemoryAddress address, Shared shared)
    : Bindings::PlatformObject(realm)
    , m_address(address)
    , m_shared(shared)
{
    auto& cache = Detail::get_cache(realm);

    cache.abstract_machine().store().get(address)->successful_grow_hook = [this] {
        MUST(reset_the_memory_buffer());
    };
}

void Memory::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Memory, WebAssembly.Memory);
}

void Memory::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_buffer);
}

// https://webassembly.github.io/spec/js-api/#dom-memory-grow
WebIDL::ExceptionOr<u32> Memory::grow(u32 delta)
{
    auto& vm = this->vm();

    auto& context = Detail::get_cache(realm());
    auto* memory = context.abstract_machine().store().get(address());
    if (!memory)
        return vm.throw_completion<JS::RangeError>("Could not find the memory instance to grow"sv);

    auto previous_size = memory->size() / Wasm::Constants::page_size;
    if (!memory->grow(delta * Wasm::Constants::page_size, Wasm::MemoryInstance::GrowType::No, Wasm::MemoryInstance::InhibitGrowCallback::Yes))
        return vm.throw_completion<JS::RangeError>("Memory.grow() grows past the stated limit of the memory instance"sv);

    TRY(reset_the_memory_buffer());

    return previous_size;
}

// https://webassembly.github.io/spec/js-api/#refresh-the-memory-buffer
// FIXME: `refresh-the-memory-buffer` is a global abstract operation.
//        Implement it as a static function to align with the spec.
WebIDL::ExceptionOr<void> Memory::reset_the_memory_buffer()
{
    if (!m_buffer)
        return {};

    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    if (m_buffer->is_fixed_length()) {
        // https://webassembly.github.io/threads/js-api/index.html#refresh-the-memory-buffer
        // 1. If IsSharedArrayBuffer(buffer) is false,
        if (!m_buffer->is_shared_array_buffer()) {
            // 1. Perform ! DetachArrayBuffer(buffer, "WebAssembly.Memory").
            MUST(JS::detach_array_buffer(vm, *m_buffer, JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string)));
        }
    }

    m_buffer = TRY(create_a_fixed_length_memory_buffer(vm, realm, m_address, m_shared));

    return {};
}

// https://webassembly.github.io/spec/js-api/#dom-memory-buffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> Memory::buffer() const
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    if (!m_buffer)
        m_buffer = TRY(create_a_fixed_length_memory_buffer(vm, realm, m_address, m_shared));

    return GC::Ref(*m_buffer);
}

// https://webassembly.github.io/spec/js-api/#create-a-fixed-length-memory-buffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> Memory::create_a_fixed_length_memory_buffer(JS::VM& vm, JS::Realm& realm, Wasm::MemoryAddress address, Shared shared)
{
    auto& context = Detail::get_cache(realm);
    auto* memory = context.abstract_machine().store().get(address);
    if (!memory)
        return vm.throw_completion<JS::RangeError>("Could not find the memory instance"sv);

    JS::ArrayBuffer* array_buffer;
    // https://webassembly.github.io/threads/js-api/index.html#create-a-fixed-length-memory-buffer
    // 3. If share is shared,
    if (shared == Shared::Yes) {
        // 1. Let block be a Shared Data Block which is identified with the underlying memory of memaddr.
        auto bytes = memory->data();

        // 2. Let buffer be a new SharedArrayBuffer with the internal slots [[ArrayBufferData]] and [[ArrayBufferByteLength]].
        array_buffer = TRY(JS::allocate_shared_array_buffer(vm, realm.intrinsics().shared_array_buffer_constructor(), bytes.size()));
        bytes.span().copy_to(array_buffer->buffer().span());
        // 3. FIXME: Set buffer.[[ArrayBufferData]] to block.
        // 4. FIXME: Set buffer.[[ArrayBufferByteLength]] to the length of block.

        // 5. Perform ! SetIntegrityLevel(buffer, "frozen").
        MUST(array_buffer->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    }

    // 4. Otherwise,
    else {
        array_buffer = JS::ArrayBuffer::create(realm, &memory->data());
        array_buffer->set_detach_key(JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string));
    }

    return GC::Ref(*array_buffer);
}

}
