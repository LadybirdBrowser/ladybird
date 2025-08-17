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
JS::ThrowCompletionOr<u32> Memory::grow(u32 delta)
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

// https://webassembly.github.io/threads/js-api/index.html#dom-memory-tofixedlengthbuffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> Memory::to_fixed_length_buffer()
{
    auto& vm = this->vm();

    // 1. Let buffer be this.[[BufferObject]].
    // 2. Let memaddr be this.[[Memory]].
    // 3. If IsSharedArrayBuffer(buffer) is false,
    if (m_shared == Shared::No) {
        // 1. If IsFixedLengthArrayBuffer(buffer) is true, return buffer.
        if (m_buffer->is_fixed_length())
            return GC::Ref(*m_buffer);

        // 2. Otherwise,
        // 1. Let fixedBuffer be the result of creating a fixed length memory buffer from memaddr.
        auto fixed_buffer = create_a_fixed_length_memory_buffer(vm, realm(), m_address, m_shared);

        // 2. Perform ! DetachArrayBuffer(buffer, "WebAssembly.Memory").
        MUST(JS::detach_array_buffer(vm, *m_buffer, JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string)));

        // 3. Set this.[[BufferObject]] to fixedBuffer.
        m_buffer = fixed_buffer;

        // 4. Return fixedBuffer.
        return fixed_buffer;
    }

    // 4. Otherwise,
    // 1. Let map be the surrounding agent's associated Memory object cache.
    auto& cache = Detail::get_cache(realm());

    // 2. Assert: map[memaddr] exists.
    // 3. Let newMemory be map[memaddr].
    auto new_memory = cache.get_memory_instance(m_address);
    VERIFY(new_memory.has_value());

    // 4. Let newBufferObject be newMemory.[[BufferObject]].
    auto new_buffer_object = new_memory.value()->m_buffer;

    // 5. Set this.[[BufferObject]] to newBufferObject.
    m_buffer = new_buffer_object;

    // 6. Return newBufferObject.
    return GC::Ref(*new_buffer_object);
}

// https://webassembly.github.io/spec/js-api/#dom-memory-toresizablebuffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> Memory::to_resizable_buffer()
{
    auto& vm = this->vm();

    // 1. Let buffer be this.[[BufferObject]].
    // 2. If IsFixedLengthArrayBuffer(buffer) is false, return buffer.
    if (!m_buffer->is_fixed_length())
        return GC::Ref(*m_buffer);

    // 3. Let memaddr be this.[[Memory]].
    // 4. Let store be the surrounding agent’s associated store.
    auto& store = Detail::get_cache(realm()).abstract_machine().store();

    // 5. Let memtype be mem_type(store, memaddr).
    auto mem_type = store.get(m_address)->type();

    // 6. If memtype has a max,
    //        1. Let maxsize be the max value in memtype.
    // 7. Otherwise,
    //        1. Let maxsize be 65536 × 65536.
    size_t max_size = mem_type.limits().max().value_or(65536) * Wasm::Constants::page_size;

    // 8. Let resizableBuffer be the result of creating a resizable memory buffer from memaddr and maxsize.
    auto resizable_buffer = TRY(create_a_resizable_memory_buffer(vm, realm(), m_address, m_shared, max_size));

    // https://webassembly.github.io/threads/js-api/index.html#dom-memory-toresizablebuffer
    // 5. If IsSharedArrayBuffer(buffer) is false,
    // 9. Perform ! DetachArrayBuffer(buffer, "WebAssembly.Memory").
    if (!m_buffer->is_shared_array_buffer())
        MUST(JS::detach_array_buffer(vm, *m_buffer, JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string)));

    // 10. Set this.[[BufferObject]] to resizableBuffer.
    m_buffer = resizable_buffer;

    // 11. Return resizeableBuffer.
    return resizable_buffer;
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
        buffer = create_a_fixed_length_memory_buffer(vm, realm, address, memory.value()->m_shared);
    } else {
        // 1. Let block be a Data Block which is identified with the underlying memory of memaddr.
        auto& bytes = cache.abstract_machine().store().get(address)->data();

        // 2. Set buffer.[[ArrayBufferData]] to block.
        // 3. Set buffer.[[ArrayBufferByteLength]] to the length of block.
        buffer->set_data_block({ JS::DataBlock::UnownedFixedLengthByteBuffer(&bytes) });
    }
}

// https://webassembly.github.io/threads/js-api/#dom-memory-buffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> Memory::buffer() const
{
    // 1. Let memaddr be this.[[Memory]].
    // 2. Let block be a Data Block which is identified with the underlying memory of memaddr.
    // 3. If block is a Shared Data Block,
    if (m_shared == Shared::Yes) {
        // 1. Let map be the surrounding agent's associated Memory object cache.
        // 2. Assert: map[memaddr] exists.
        // 3. Let newMemory be map[memaddr].
        auto& cache = Detail::get_cache(realm());
        auto new_memory = cache.get_memory_instance(m_address);
        VERIFY(new_memory.has_value());

        // 4. Let newBufferObject be newMemory.[[BufferObject]].
        auto new_buffer_object = new_memory.value()->m_buffer;

        // 5. Set this.[[BufferObject]] to newBufferObject.
        m_buffer = new_buffer_object;

        // 6. Return newBufferObject.
        return GC::Ref(*new_buffer_object);
    }
    // 4. Otherwise,
    else {
        // 1. Return this.[[BufferObject]].
        return GC::Ref(*m_buffer);
    }
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

// https://webassembly.github.io/spec/js-api/#create-a-resizable-memory-buffer
JS::ThrowCompletionOr<GC::Ref<JS::ArrayBuffer>> Memory::create_a_resizable_memory_buffer(JS::VM& vm, JS::Realm& realm, Wasm::MemoryAddress address, Shared shared, size_t max_size)
{
    auto& context = Detail::get_cache(realm);
    auto* memory = context.abstract_machine().store().get(address);
    VERIFY(memory);

    // 3. If maxsize > (65536 × 65536),
    if (max_size > (65536 * Wasm::Constants::page_size)) {
        // 1. Throw a RangeError exception.
        return vm.throw_completion<JS::RangeError>("Maximum memory length exceeds 65536 * 65536 bytes"sv);
    }

    // https://webassembly.github.io/threads/js-api/index.html#create-a-resizable-memory-buffer
    // 5. If share is shared,
    if (shared == Shared::Yes) {
        // 1. Let block be a Shared Data Block which is identified with the underlying memory of memaddr.
        // 2. Let buffer be a new SharedArrayBuffer with the internal slots [[ArrayBufferData]], [[ArrayBufferByteLength]], and [[ArrayBufferMaxByteLength]].
        // 3. Set buffer.[[ArrayBufferData]] to block.
        auto buffer = JS::ArrayBuffer::create(realm, &memory->data());

        // AD-HOC: The threads proposal uses the memory type's minimum for both shared and
        //         non-shared memories, but the upstream spec uses the memory instance's current
        //         size. We assume the upstream spec is correct for both cases.
        // 4. Set buffer.[[ArrayBufferByteLength]] to min.
        VERIFY(buffer->byte_length() == memory->size());

        // 5. Set buffer.[[ArrayBufferMaxByteLength]] to maxsize.
        buffer->set_max_byte_length(max_size);

        // 6. Perform ! SetIntegrityLevel(buffer, "frozen").
        MUST(buffer->set_integrity_level(IntegrityLevel::Frozen));

        // 7. Return buffer.
        return buffer;
    }
    // 6. Otherwise,
    else {
        // 1. Let block be a Data Block which is identified with the underlying memory of memaddr.
        // 4. Let buffer be a new ArrayBuffer with the internal slots [[ArrayBufferData]], [[ArrayBufferByteLength]], [[ArrayBufferMaxByteLength]], and [[ArrayBufferDetachKey]].
        // 5. Set buffer.[[ArrayBufferData]] to block.
        auto buffer = JS::ArrayBuffer::create(realm, &memory->data());

        // 2. Let length be the length of block.
        // 6. Set buffer.[[ArrayBufferByteLength]] to length.
        VERIFY(buffer->byte_length() == memory->size());

        // 7. Set buffer.[[ArrayBufferMaxByteLength]] to maxsize.
        buffer->set_max_byte_length(max_size);

        // 8. Set buffer.[[ArrayBufferDetachKey]] to "WebAssembly.Memory".
        buffer->set_detach_key(JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string));

        // 9. Return buffer.
        return buffer;
    }
}

}
