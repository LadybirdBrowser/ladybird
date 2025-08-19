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

    cache.abstract_machine().store().get(address)->successful_grow_hook = [realm = GC::Ref(realm), address] {
        refresh_the_memory_buffer(realm->vm(), realm, address);
    };
}

// https://webassembly.github.io/spec/js-api/#initialize-a-memory-object
void Memory::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(Memory, WebAssembly.Memory);
    Base::initialize(realm);

    auto& vm = realm.vm();

    // https://webassembly.github.io/spec/js-api/#initialize-a-memory-object
    // 1. Let map be the surrounding agent’s associated Memory object cache.
    // 2. Assert: map[memaddr] doesn’t exist.
    auto& cache = Detail::get_cache(realm);
    auto exists = cache.memory_instances().contains(m_address);
    VERIFY(!exists);

    // 3. Let buffer be the result of creating a fixed length memory buffer from memaddr.
    auto buffer = create_a_fixed_length_memory_buffer(vm, realm, m_address, m_shared);

    // 4. Set memory.[[Memory]] to memaddr.
    // NOTE: This is already set by the Memory constructor.

    // 5. Set memory.[[BufferObject]] to buffer.
    m_buffer = buffer;

    // 6. Set map[memaddr] to memory.
    cache.add_memory_instance(m_address, *this);
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
    VERIFY(memory);

    auto previous_size = memory->size() / Wasm::Constants::page_size;
    if (!memory->grow(delta * Wasm::Constants::page_size, Wasm::MemoryInstance::GrowType::No, Wasm::MemoryInstance::InhibitGrowCallback::Yes))
        return vm.throw_completion<JS::RangeError>("Memory.grow() grows past the stated limit of the memory instance"sv);

    refresh_the_memory_buffer(vm, realm(), m_address);

    return previous_size;
}

// https://webassembly.github.io/spec/js-api/#refresh-the-memory-buffer
void Memory::refresh_the_memory_buffer(JS::VM& vm, JS::Realm& realm, Wasm::MemoryAddress address)
{
    // 1. Let map be the surrounding agent’s associated Memory object cache.
    // 2. Assert: map[memaddr] exists.
    // 3. Let memory be map[memaddr].
    auto& cache = Detail::get_cache(realm);
    auto memory = cache.get_memory_instance(address);
    VERIFY(memory.has_value());

    // 4. Let buffer be memory.[[BufferObject]].
    auto& buffer = memory.value()->m_buffer;

    // 5. If IsFixedLengthArrayBuffer(buffer) is true,
    if (buffer->is_fixed_length()) {
        // https://webassembly.github.io/threads/js-api/index.html#refresh-the-memory-buffer
        // 1. If IsSharedArrayBuffer(buffer) is false,
        if (!buffer->is_shared_array_buffer()) {
            // 1. Perform ! DetachArrayBuffer(buffer, "WebAssembly.Memory").
            MUST(JS::detach_array_buffer(vm, *buffer, JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string)));
        }

        // 2. Let newBuffer be the result of creating a fixed length memory buffer from memaddr.
        // 3. Set memory.[[BufferObject]] to newBuffer.
    }

    buffer = create_a_fixed_length_memory_buffer(vm, realm, address, memory.value()->m_shared);
}

// https://webassembly.github.io/spec/js-api/#dom-memory-buffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> Memory::buffer() const
{
    return GC::Ref(*m_buffer);
}

// https://webassembly.github.io/spec/js-api/#create-a-fixed-length-memory-buffer
GC::Ref<JS::ArrayBuffer> Memory::create_a_fixed_length_memory_buffer(JS::VM& vm, JS::Realm& realm, Wasm::MemoryAddress address, Shared shared)
{
    auto& context = Detail::get_cache(realm);
    auto* memory = context.abstract_machine().store().get(address);
    VERIFY(memory);

    JS::ArrayBuffer* array_buffer;
    // https://webassembly.github.io/threads/js-api/index.html#create-a-fixed-length-memory-buffer
    // 3. If share is shared,
    if (shared == Shared::Yes) {
        // 1. Let block be a Shared Data Block which is identified with the underlying memory of memaddr.
        // 2. Let buffer be a new SharedArrayBuffer with the internal slots [[ArrayBufferData]] and [[ArrayBufferByteLength]].
        // 3. Set buffer.[[ArrayBufferData]] to block.
        array_buffer = JS::ArrayBuffer::create(realm, &memory->data(), JS::DataBlock::Shared::Yes);

        // 4. Set buffer.[[ArrayBufferByteLength]] to the length of block.
        VERIFY(array_buffer->byte_length() == memory->size());

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
