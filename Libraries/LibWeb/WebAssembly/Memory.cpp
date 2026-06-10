/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Root.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWasm/Types.h>
#include <LibWeb/WebAssembly/Memory.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::WebAssembly {

GC_DEFINE_ALLOCATOR(Memory);

static u8* wasm_memory_buffer_data(void* context)
{
    return static_cast<Wasm::MemoryBuffer*>(context)->data();
}

static size_t wasm_memory_buffer_size(void* context)
{
    return static_cast<Wasm::MemoryBuffer*>(context)->size();
}

struct MemoryBufferCache {
    GC::Weak<Memory> memory;
    GC::Weak<JS::ArrayBuffer> primary_buffer;
    Vector<GC::Weak<JS::ArrayBuffer>> live_buffer_objects;
};

static Vector<MemoryBufferCache>& memory_buffer_caches()
{
    static NeverDestroyed<Vector<MemoryBufferCache>> caches;
    return *caches;
}

static void prune_memory_buffer_cache(MemoryBufferCache& cache)
{
    cache.live_buffer_objects.remove_all_matching([](auto const& buffer) { return !buffer; });
}

static void prune_memory_buffer_caches()
{
    memory_buffer_caches().remove_all_matching([](auto const& cache) {
        return !cache.memory;
    });
}

static MemoryBufferCache& memory_buffer_cache_for(Memory const& memory)
{
    auto& caches = memory_buffer_caches();
    prune_memory_buffer_caches();

    for (auto& cache : caches) {
        if (cache.memory.ptr() == &memory)
            return cache;
    }

    caches.append(MemoryBufferCache { const_cast<Memory&>(memory), {}, {} });
    return caches.last();
}

static GC::Ref<JS::ArrayBuffer> create_a_fixed_length_memory_buffer(JS::VM& vm, JS::Realm& buffer_realm, Memory& memory)
{
    auto* memory_instance = memory.cache().abstract_machine().store().get(memory.address());
    VERIFY(memory_instance);

    JS::ArrayBuffer* array_buffer;
    // https://webassembly.github.io/threads/js-api/index.html#create-a-fixed-length-memory-buffer
    // 3. If share is shared,
    if (memory.is_shared()) {
        // 1. Let block be a Shared Data Block which is identified with the underlying memory of memaddr.
        // 2. Let buffer be a new SharedArrayBuffer with the internal slots [[ArrayBufferData]] and [[ArrayBufferByteLength]].
        // 3. Set buffer.[[ArrayBufferData]] to block.
        // 4. Set buffer.[[ArrayBufferByteLength]] to the length of block.
        // NOTE: ArrayBufferByteLength should contain the original size regardless of growth.
        array_buffer = JS::ArrayBuffer::create(buffer_realm, JS::DataBlock::UnownedExternalBuffer(memory, &memory_instance->data(), wasm_memory_buffer_data, memory_instance->size()), JS::DataBlock::Shared::Yes);
        VERIFY(array_buffer->byte_length() == memory_instance->size());

        // 5. Perform ! SetIntegrityLevel(buffer, "frozen").
        MUST(array_buffer->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    } else {
        // 4. Otherwise,
        array_buffer = JS::ArrayBuffer::create(buffer_realm, JS::DataBlock::UnownedExternalBuffer(memory, &memory_instance->data(), wasm_memory_buffer_data, wasm_memory_buffer_size));
        array_buffer->set_detach_key(JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string));
    }

    return GC::Ref(*array_buffer);
}

// https://webassembly.github.io/spec/js-api/#create-a-resizable-memory-buffer
static JS::ThrowCompletionOr<GC::Ref<JS::ArrayBuffer>> create_a_resizable_memory_buffer(JS::VM& vm, JS::Realm& buffer_realm, Memory& memory, size_t max_size)
{
    auto* memory_instance = memory.cache().abstract_machine().store().get(memory.address());
    VERIFY(memory_instance);

    // 3. If maxsize > (65536 x 65536),
    if (max_size > (65536 * Wasm::Constants::page_size)) {
        // 1. Throw a RangeError exception.
        return vm.throw_completion<JS::RangeError>("Maximum memory length exceeds 65536 * 65536 bytes"sv);
    }

    // https://webassembly.github.io/threads/js-api/index.html#create-a-resizable-memory-buffer
    // 5. If share is shared,
    if (memory.is_shared()) {
        // 1. Let block be a Shared Data Block which is identified with the underlying memory of memaddr.
        // 2. Let buffer be a new SharedArrayBuffer with the internal slots [[ArrayBufferData]], [[ArrayBufferByteLength]], and [[ArrayBufferMaxByteLength]].
        // 3. Set buffer.[[ArrayBufferData]] to block.
        auto buffer = JS::ArrayBuffer::create(buffer_realm, JS::DataBlock::UnownedExternalBuffer(memory, &memory_instance->data(), wasm_memory_buffer_data, wasm_memory_buffer_size), JS::DataBlock::Shared::Yes);

        // AD-HOC: The threads proposal uses the memory type's minimum for both shared and
        //         non-shared memories, but the upstream spec uses the memory instance's current
        //         size. We assume the upstream spec is correct for both cases.
        // 4. Set buffer.[[ArrayBufferByteLength]] to min.
        VERIFY(buffer->byte_length() == memory_instance->size());

        // 5. Set buffer.[[ArrayBufferMaxByteLength]] to maxsize.
        buffer->set_max_byte_length(max_size);

        // 6. Perform ! SetIntegrityLevel(buffer, "frozen").
        MUST(buffer->set_integrity_level(JS::Object::IntegrityLevel::Frozen));

        // AD-HOC: Set buffer.[[ArrayBufferDetachKey]] to "WebAssembly.Memory".
        //         SharedArrayBuffers can't be detached, but this allows us to bail early from HostGrowSharedArrayBuffer
        //         for SharedArrayBuffers not associated with a WebAssembly memory.
        buffer->set_detach_key(JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string));

        // 7. Return buffer.
        return buffer;
    }

    // 6. Otherwise,
    // 1. Let block be a Data Block which is identified with the underlying memory of memaddr.
    // 4. Let buffer be a new ArrayBuffer with the internal slots [[ArrayBufferData]], [[ArrayBufferByteLength]], [[ArrayBufferMaxByteLength]], and [[ArrayBufferDetachKey]].
    // 5. Set buffer.[[ArrayBufferData]] to block.
    auto buffer = JS::ArrayBuffer::create(buffer_realm, JS::DataBlock::UnownedExternalBuffer(memory, &memory_instance->data(), wasm_memory_buffer_data, wasm_memory_buffer_size));

    // 2. Let length be the length of block.
    // 6. Set buffer.[[ArrayBufferByteLength]] to length.
    VERIFY(buffer->byte_length() == memory_instance->size());

    // 7. Set buffer.[[ArrayBufferMaxByteLength]] to maxsize.
    buffer->set_max_byte_length(max_size);

    // 8. Set buffer.[[ArrayBufferDetachKey]] to "WebAssembly.Memory".
    buffer->set_detach_key(JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string));

    // 9. Return buffer.
    return buffer;
}

static void set_buffer_object_for_realm(JS::Realm&, Memory&, GC::Ref<JS::ArrayBuffer>);

static GC::Ref<JS::ArrayBuffer> buffer_object_for_realm(JS::Realm& buffer_realm, Memory& memory)
{
    auto& cache = memory_buffer_cache_for(memory);

    if (cache.primary_buffer && &buffer_realm == &cache.primary_buffer->shape().realm())
        return GC::Ref { *cache.primary_buffer };

    prune_memory_buffer_cache(cache);
    for (auto const& buffer : cache.live_buffer_objects) {
        if (&buffer->shape().realm() == &buffer_realm)
            return GC::Ref { *buffer };
    }

    auto buffer = create_a_fixed_length_memory_buffer(buffer_realm.vm(), buffer_realm, memory);
    set_buffer_object_for_realm(buffer_realm, memory, buffer);
    return buffer;
}

static void set_buffer_object_for_realm(JS::Realm& buffer_realm, Memory& memory, GC::Ref<JS::ArrayBuffer> buffer)
{
    auto& cache = memory_buffer_cache_for(memory);

    if (!cache.primary_buffer || &buffer_realm == &cache.primary_buffer->shape().realm()) {
        cache.primary_buffer = buffer;
        return;
    }

    prune_memory_buffer_cache(cache);
    for (auto& live_buffer : cache.live_buffer_objects) {
        if (&live_buffer->shape().realm() == &buffer_realm) {
            live_buffer = buffer;
            return;
        }
    }

    cache.live_buffer_objects.append(buffer);
}

static void refresh_buffer_object(Memory& memory, GC::Ref<JS::ArrayBuffer> buffer)
{
    auto& vm = buffer->shape().realm().vm();

    // 4. Let buffer be memory.[[BufferObject]].

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
        set_buffer_object_for_realm(buffer->shape().realm(), memory, create_a_fixed_length_memory_buffer(vm, buffer->shape().realm(), memory));
    } else {
        // 1. Let block be a Data Block which is identified with the underlying memory of memaddr.
        auto& bytes = memory.cache().abstract_machine().store().get(memory.address())->data();

        // AD-HOC: Neither the main spec nor the threads proposal specify that the Data Block should be Shared for
        //         shared Wasm memories, but we do in fact want a Shared Data Block in that case.
        auto is_shared = buffer->is_shared_array_buffer() ? JS::DataBlock::Shared::Yes : JS::DataBlock::Shared::No;

        // 2. Set buffer.[[ArrayBufferData]] to block.
        // 3. Set buffer.[[ArrayBufferByteLength]] to the length of block.
        buffer->set_data_block({ JS::DataBlock::UnownedExternalBuffer(memory, &bytes, wasm_memory_buffer_data, wasm_memory_buffer_size), is_shared });
    }
}

static void refresh_buffer_objects(Memory& memory)
{
    auto& cache = memory_buffer_cache_for(memory);
    GC::RootVector<GC::Ref<JS::ArrayBuffer>> buffer_objects;

    if (cache.primary_buffer)
        buffer_objects.append(GC::Ref { *cache.primary_buffer });

    prune_memory_buffer_cache(cache);
    for (auto const& buffer : cache.live_buffer_objects) {
        bool already_appended = false;
        for (auto const& buffer_object : buffer_objects) {
            if (buffer_object.ptr() == buffer.ptr()) {
                already_appended = true;
                break;
            }
        }
        if (!already_appended)
            buffer_objects.append(GC::Ref { *buffer });
    }

    for (auto buffer : buffer_objects)
        refresh_buffer_object(memory, buffer);
}

WebIDL::ExceptionOr<GC::Ref<Memory>> Memory::create(NonnullRefPtr<Detail::WebAssemblyCache> cache, MemoryDescriptor const& descriptor)
{
    // https://webassembly.github.io/threads/js-api/index.html#dom-memory-memory
    // 3. If maximum is not empty and maximum < initial, throw a RangeError exception.
    if (descriptor.maximum.has_value() && descriptor.maximum.value() < descriptor.initial) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Initial is larger than maximum."sv };
    }

    // 4. Let share be shared if descriptor["shared"] is true and unshared otherwise.
    // 5. If share is shared and maximum is empty, throw a TypeError exception.
    auto shared = descriptor.shared;
    if (shared && !descriptor.maximum.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Maximum has to be specified for shared memory."sv };

    Wasm::Limits limits { Wasm::AddressType::I32, descriptor.initial, descriptor.maximum.map([](auto x) -> u64 { return x; }) };
    Wasm::MemoryType memory_type { move(limits) };

    auto address = cache->abstract_machine().store().allocate(memory_type);
    if (!address.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Wasm Memory allocation failed"sv };

    auto memory_object = Memory::create(cache, *address, shared ? Shared::Yes : Shared::No);

    return memory_object;
}

GC::Ref<Memory> Memory::create(NonnullRefPtr<Detail::WebAssemblyCache> cache, Wasm::MemoryAddress address, Shared shared)
{
    auto memory = GC::Heap::the().allocate<Memory>(cache, address, shared);

    // https://webassembly.github.io/spec/js-api/#initialize-a-memory-object
    auto exists = cache->memory_instances().contains(address);
    VERIFY(!exists);

    cache->add_memory_instance(address, memory);

    return memory;
}

WebIDL::ExceptionOr<GC::Ref<Memory>> Memory::construct_impl(JS::Realm& realm, MemoryDescriptor const& descriptor)
{
    auto cache = Detail::get_cache(realm);
    return Memory::create(move(cache), descriptor);
}

Memory::Memory(NonnullRefPtr<Detail::WebAssemblyCache> cache, Wasm::MemoryAddress address, Shared shared)
    : m_cache(move(cache))
    , m_address(address)
    , m_shared(shared)
{
    auto& cache_ref = *m_cache;
    m_cache->abstract_machine().store().get(address)->successful_grow_hook = [&cache_ref, address] {
        refresh_the_memory_buffer(cache_ref, address);
    };
}

void Memory::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_cache->visit_edges(visitor);
}

// https://webassembly.github.io/spec/js-api/#dom-memory-grow
WebIDL::ExceptionOr<u32> Memory::grow(u32 delta)
{
    auto* memory = cache().abstract_machine().store().get(address());
    VERIFY(memory);

    auto previous_size = memory->size() / Wasm::Constants::page_size;
    if (!memory->grow(delta * Wasm::Constants::page_size, Wasm::MemoryInstance::GrowType::No, Wasm::MemoryInstance::InhibitGrowCallback::Yes))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Memory.grow() grows past the stated limit of the memory instance"sv };

    refresh_the_memory_buffer(cache(), m_address);

    return previous_size;
}

// https://webassembly.github.io/threads/js-api/index.html#dom-memory-tofixedlengthbuffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> Memory::to_fixed_length_buffer(JS::Realm& realm)
{
    auto& vm = realm.vm();
    auto& buffer_realm = realm;

    // 1. Let buffer be this.[[BufferObject]].
    auto buffer = buffer_object_for_realm(buffer_realm, *this);

    // 2. Let memaddr be this.[[Memory]].
    // 3. If IsSharedArrayBuffer(buffer) is false,
    if (!is_shared()) {
        // 1. If IsFixedLengthArrayBuffer(buffer) is true, return buffer.
        if (buffer->is_fixed_length())
            return buffer;

        // 2. Otherwise,
        // 1. Let fixedBuffer be the result of creating a fixed length memory buffer from memaddr.
        auto fixed_buffer = create_a_fixed_length_memory_buffer(vm, buffer_realm, *this);

        // 2. Perform ! DetachArrayBuffer(buffer, "WebAssembly.Memory").
        MUST(JS::detach_array_buffer(vm, *buffer, JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string)));

        // 3. Set this.[[BufferObject]] to fixedBuffer.
        set_buffer_object_for_realm(buffer_realm, *this, fixed_buffer);

        // 4. Return fixedBuffer.
        return fixed_buffer;
    }

    // 4. Otherwise,
    // 1. Let map be the surrounding agent's associated Memory object cache.
    // 2. Assert: map[memaddr] exists.
    // 3. Let newMemory be map[memaddr].
    auto new_memory = cache().get_memory_instance(address());
    VERIFY(new_memory.has_value());

    // 4. Let newBufferObject be newMemory.[[BufferObject]].
    auto new_buffer_object = buffer_object_for_realm(buffer_realm, *new_memory.value());

    // 5. Set this.[[BufferObject]] to newBufferObject.
    set_buffer_object_for_realm(buffer_realm, *this, new_buffer_object);

    // 6. Return newBufferObject.
    return new_buffer_object;
}

// https://webassembly.github.io/spec/js-api/#dom-memory-toresizablebuffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> Memory::to_resizable_buffer(JS::Realm& realm)
{
    auto& vm = realm.vm();
    auto& buffer_realm = realm;

    // 1. Let buffer be this.[[BufferObject]].
    auto buffer = buffer_object_for_realm(buffer_realm, *this);

    // 2. If IsFixedLengthArrayBuffer(buffer) is false, return buffer.
    if (!buffer->is_fixed_length())
        return buffer;

    // 3. Let memaddr be this.[[Memory]].
    // 4. Let store be the surrounding agent's associated store.
    auto& store = cache().abstract_machine().store();

    // 5. Let memtype be mem_type(store, memaddr).
    auto mem_type = store.get(address())->type();

    // 6. If memtype has a max,
    //        1. Let maxsize be the max value in memtype.
    // 7. Otherwise,
    //        1. Let maxsize be 65536 x 65536.
    size_t max_size = mem_type.limits().max().value_or(65536) * Wasm::Constants::page_size;

    // 8. Let resizableBuffer be the result of creating a resizable memory buffer from memaddr and maxsize.
    auto resizable_buffer = TRY(create_a_resizable_memory_buffer(vm, buffer_realm, *this, max_size));

    // https://webassembly.github.io/threads/js-api/index.html#dom-memory-toresizablebuffer
    // 5. If IsSharedArrayBuffer(buffer) is false,
    // 9. Perform ! DetachArrayBuffer(buffer, "WebAssembly.Memory").
    if (!buffer->is_shared_array_buffer())
        MUST(JS::detach_array_buffer(vm, *buffer, JS::PrimitiveString::create(vm, "WebAssembly.Memory"_string)));

    // 10. Set this.[[BufferObject]] to resizableBuffer.
    set_buffer_object_for_realm(buffer_realm, *this, resizable_buffer);

    // 11. Return resizeableBuffer.
    return resizable_buffer;
}

// https://webassembly.github.io/threads/js-api/#dom-memory-buffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> Memory::buffer(JS::Realm& realm)
{
    auto& buffer_realm = realm;

    // 1. Let memaddr be this.[[Memory]].
    // 2. Let block be a Data Block which is identified with the underlying memory of memaddr.
    // 3. If block is a Shared Data Block,
    if (is_shared()) {
        // 1. Let map be the surrounding agent's associated Memory object cache.
        // 2. Assert: map[memaddr] exists.
        // 3. Let newMemory be map[memaddr].
        auto new_memory = cache().get_memory_instance(address());
        VERIFY(new_memory.has_value());

        // 4. Let newBufferObject be newMemory.[[BufferObject]].
        auto new_buffer_object = buffer_object_for_realm(buffer_realm, *new_memory.value());

        // 5. Set this.[[BufferObject]] to newBufferObject.
        set_buffer_object_for_realm(buffer_realm, *this, new_buffer_object);

        // 6. Return newBufferObject.
        return new_buffer_object;
    }

    // 4. Otherwise,
    // 1. Return this.[[BufferObject]].
    return buffer_object_for_realm(buffer_realm, *this);
}

bool memory_has_buffer_object(Memory const& memory, JS::ArrayBuffer const& buffer)
{
    auto& cache = memory_buffer_cache_for(memory);

    if (cache.primary_buffer.ptr() == &buffer)
        return true;

    prune_memory_buffer_cache(cache);
    for (auto const& live_buffer : cache.live_buffer_objects) {
        if (live_buffer.ptr() == &buffer)
            return true;
    }

    return false;
}

// https://webassembly.github.io/spec/js-api/#refresh-the-memory-buffer
void refresh_the_memory_buffer(Detail::WebAssemblyCache& cache, Wasm::MemoryAddress address)
{
    // 1. Let map be the surrounding agent's associated Memory object cache.
    // 2. Assert: map[memaddr] exists.
    // 3. Let memory be map[memaddr].
    auto memory = cache.get_memory_instance(address);
    VERIFY(memory.has_value());

    refresh_buffer_objects(*memory.value());
}

}
