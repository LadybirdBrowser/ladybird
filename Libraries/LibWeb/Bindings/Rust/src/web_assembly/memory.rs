/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Memory, https://webassembly.github.io/spec/js-api/#memories, and its shared memories,
//! https://webassembly.github.io/threads/js-api/
//!
//! A Memory object belongs to the agent, as its memory does, while its [[BufferObject]] belongs to a realm: the object
//! has a buffer object of its own in every realm that asks for one, and holds each as the internal slot the
//! specification gives it: "[[BufferObject]] : an ArrayBuffer whose Data Block is identified with the above memory
//! address".

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{
    ArrayBuffer, ArrayBufferShared, DataBlock, DataBlockStorage, ExternalPrimitiveStorage, ForeignCellSlot, Gc,
    GcRefCell, IntegrityLevel, MarkedVec, Must, PrimitiveString, RawNativeFunctionResult, Realm, ThrowCompletionOr,
    Trace, Value, Vm, detach_array_buffer, same_value,
};
use libwasm_engine::runtime::MemoryAddress;
use libwasm_engine::runtime::memory::MemoryInstance;
use libwasm_engine::types::{AddressType, Limits, MemoryType};

use crate::bindings::interface::{Ref, Wrappable, allocate, cell_if_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::web_assembly::{Agent, EntityRef, PAGE_SIZE, host_object, set_host_object};

/// https://webassembly.github.io/spec/js-api/#dictdef-memorydescriptor
pub(crate) struct MemoryDescriptor {
    pub(crate) initial: u32,
    pub(crate) maximum: Option<u32>,
    pub(crate) shared: bool,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Memory {
    // [[Memory]]
    address: EntityRef<MemoryInstance>,
    #[gc(untraced)]
    shared: bool,
    // [[BufferObject]], one per realm.
    buffer_objects: GcRefCell<Vec<Gc<ArrayBuffer>>>,
}

impl Wrappable for Memory {}

fn detach_key(vm: &Vm) -> Value {
    Value::from_string(PrimitiveString::create_from_utf8(vm, "WebAssembly.Memory"))
}

impl Memory {
    // https://webassembly.github.io/spec/js-api/#dom-memory-memory
    pub(crate) fn construct(cx: &mut CallContext, descriptor: MemoryDescriptor) -> WebIdlResult<Ref<Self>> {
        // https://webassembly.github.io/threads/js-api/index.html#dom-memory-memory
        // 1. Let initial be descriptor["initial"].
        let initial = descriptor.initial;

        // 2. If descriptor["maximum"] exists, let maximum be descriptor["maximum"]; otherwise, let maximum be empty.
        let maximum = descriptor.maximum;

        // 3. If maximum is not empty and maximum < initial, throw a RangeError exception.
        if maximum.is_some_and(|maximum| maximum < initial) {
            return Err(Exception::range_error("Initial is larger than maximum."));
        }

        // 4. Let share be shared if descriptor["shared"] is true and unshared otherwise.
        let share = descriptor.shared;

        // 5. If share is shared and maximum is empty, throw a TypeError exception.
        if share && maximum.is_none() {
            return Err(Exception::type_error("Maximum has to be specified for shared memory."));
        }

        // 6. Let memtype be ({ min initial, max maximum }, share).
        let memtype = MemoryType {
            limits: Limits {
                min: u64::from(initial),
                max: maximum.map(u64::from),
                address_type: AddressType::I32,
                shared: share,
            },
        };

        // 7. Let store be the surrounding agent's associated store.
        // 8. Let (store, memaddr) be mem_alloc(store, memtype). If allocation fails, throw a RangeError exception.
        // 9. Set the surrounding agent's associated store to store.
        let memory = Agent::the(cx.vm())
            .store()
            .allocate_memory(memtype)
            .ok_or_else(|| Exception::range_error("Wasm Memory allocation failed"))?;

        // 10. Initialize this from memaddr.
        Ok(Self::initialize(cx.vm(), memory))
    }

    // https://webassembly.github.io/spec/js-api/#initialize-a-memory-object
    fn initialize(vm: &Vm, memory: MemoryAddress) -> Ref<Self> {
        // 1. Let map be the surrounding agent's associated Memory object cache.
        assert!(host_object::<()>(memory.cell()).is_none());

        // NB: The buffer of a realm is created when the realm first asks for it.
        let shared = Agent::the(vm).store().memory(memory).ty.limits.shared;
        let object = allocate(vm, |_| Self {
            address: EntityRef(memory),
            shared,
            buffer_objects: GcRefCell::default(),
        });

        set_host_object(memory.cell(), object);
        object
    }

    /// The Memory object of the memory, https://webassembly.github.io/spec/js-api/#create-a-memory-object
    pub(crate) fn from_address(vm: &Vm, memory: MemoryAddress) -> Ref<Self> {
        // 1. Let map be the surrounding agent's associated Memory object cache.
        if let Some(object) = host_object(memory.cell()) {
            return object;
        }

        Self::initialize(vm, memory)
    }

    pub(crate) fn address(&self) -> MemoryAddress {
        self.address.0
    }

    fn size(&self, vm: &Vm) -> usize {
        Agent::the(vm).store().memory(self.address()).size()
    }

    fn external_storage(this: Ref<Self>, vm: &Vm, fixed_length: bool) -> ExternalPrimitiveStorage {
        let handle = Agent::the(vm).store().memory(this.address()).primitive_storage_handle();
        let owner = ForeignCellSlot::empty();
        // SAFETY: The memory object is a live cell of the heap, which the slot then keeps alive.
        unsafe { owner.set(Some(NonNull::new_unchecked(this.as_ptr().cast()))) };
        ExternalPrimitiveStorage::new(owner, handle, fixed_length.then(|| this.size(vm)))
    }

    // https://webassembly.github.io/threads/js-api/index.html#create-a-fixed-length-memory-buffer
    fn create_a_fixed_length_memory_buffer(this: Ref<Self>, vm: &Vm, realm: Gc<Realm>) -> Gc<ArrayBuffer> {
        // 3. If share is shared,
        if this.shared {
            // 1. Let block be a Shared Data Block which is identified with the underlying memory of memaddr.
            // 2. Let buffer be a new SharedArrayBuffer with the internal slots [[ArrayBufferData]] and [[ArrayBufferByteLength]].
            // 3. Set buffer.[[ArrayBufferData]] to block.
            // 4. Set buffer.[[ArrayBufferByteLength]] to the length of block.
            // NOTE: ArrayBufferByteLength should contain the original size regardless of growth.
            let block = DataBlock::external(Self::external_storage(this, vm, true), ArrayBufferShared::Yes);
            let buffer = ArrayBuffer::create_from_data_block(vm, realm, block);

            // 5. Perform ! SetIntegrityLevel(buffer, "frozen").
            buffer.set_integrity_level(vm, IntegrityLevel::Frozen).must();

            return buffer;
        }

        // 4. Otherwise,
        //     1. Let block be a Data Block which is identified with the underlying memory of memaddr.
        //     3. Set buffer.[[ArrayBufferData]] to block.
        //     4. Set buffer.[[ArrayBufferByteLength]] to the length of block.
        let block = DataBlock::external(Self::external_storage(this, vm, true), ArrayBufferShared::No);
        let buffer = ArrayBuffer::create_from_data_block(vm, realm, block);
        buffer.set_detach_key(detach_key(vm));
        buffer
    }

    // https://webassembly.github.io/spec/js-api/#create-a-resizable-memory-buffer
    fn create_a_resizable_memory_buffer(
        this: Ref<Self>,
        vm: &Vm,
        realm: Gc<Realm>,
        max_size: u64,
    ) -> WebIdlResult<Gc<ArrayBuffer>> {
        // 3. If maxsize > (65536 x 65536),
        if max_size > 65536 * PAGE_SIZE {
            // 1. Throw a RangeError exception.
            return Err(Exception::range_error(
                "Maximum memory length exceeds 65536 * 65536 bytes",
            ));
        }

        // https://webassembly.github.io/threads/js-api/index.html#create-a-resizable-memory-buffer
        // 5. If share is shared,
        if this.shared {
            // 1. Let block be a Shared Data Block which is identified with the underlying memory of memaddr.
            // 2. Let buffer be a new SharedArrayBuffer with the internal slots [[ArrayBufferData]], [[ArrayBufferByteLength]], and [[ArrayBufferMaxByteLength]].
            // 3. Set buffer.[[ArrayBufferData]] to block.
            // AD-HOC: The threads proposal uses the memory type's minimum for both shared and
            //         non-shared memories, but the upstream spec uses the memory instance's current
            //         size. We assume the upstream spec is correct for both cases.
            // 4. Set buffer.[[ArrayBufferByteLength]] to min.
            let block = DataBlock::external(Self::external_storage(this, vm, false), ArrayBufferShared::Yes);
            let buffer = ArrayBuffer::create_from_data_block(vm, realm, block);

            // 5. Set buffer.[[ArrayBufferMaxByteLength]] to maxsize.
            buffer.set_max_byte_length(max_size as usize);

            // 6. Perform ! SetIntegrityLevel(buffer, "frozen").
            buffer.set_integrity_level(vm, IntegrityLevel::Frozen).must();

            // AD-HOC: Set buffer.[[ArrayBufferDetachKey]] to "WebAssembly.Memory".
            //         SharedArrayBuffers can't be detached, but this allows us to bail early from HostGrowSharedArrayBuffer
            //         for SharedArrayBuffers not associated with a WebAssembly memory.
            buffer.set_detach_key(detach_key(vm));

            // 7. Return buffer.
            return Ok(buffer);
        }

        // 6. Otherwise,
        // 1. Let block be a Data Block which is identified with the underlying memory of memaddr.
        // 4. Let buffer be a new ArrayBuffer with the internal slots [[ArrayBufferData]], [[ArrayBufferByteLength]], [[ArrayBufferMaxByteLength]], and [[ArrayBufferDetachKey]].
        // 5. Set buffer.[[ArrayBufferData]] to block.
        // 2. Let length be the length of block.
        // 6. Set buffer.[[ArrayBufferByteLength]] to length.
        let block = DataBlock::external(Self::external_storage(this, vm, false), ArrayBufferShared::No);
        let buffer = ArrayBuffer::create_from_data_block(vm, realm, block);

        // 7. Set buffer.[[ArrayBufferMaxByteLength]] to maxsize.
        buffer.set_max_byte_length(max_size as usize);

        // 8. Set buffer.[[ArrayBufferDetachKey]] to "WebAssembly.Memory".
        buffer.set_detach_key(detach_key(vm));

        // 9. Return buffer.
        Ok(buffer)
    }

    /// The [[BufferObject]] of the memory in the realm, which the realm's first use creates.
    fn buffer_object(this: Ref<Self>, vm: &Vm, realm: Gc<Realm>) -> Gc<ArrayBuffer> {
        let buffer = this
            .buffer_objects
            .borrow()
            .iter()
            .copied()
            .find(|buffer| buffer.shape().realm() == realm);
        if let Some(buffer) = buffer {
            return buffer;
        }
        let buffer = Self::create_a_fixed_length_memory_buffer(this, vm, realm);
        this.set_buffer_object(vm, buffer);
        buffer
    }

    /// Sets the [[BufferObject]] of the memory in the realm of the buffer.
    fn set_buffer_object(&self, vm: &Vm, buffer: Gc<ArrayBuffer>) {
        let realm = buffer.shape().realm();
        let mut buffer_objects = self.buffer_objects.borrow_mut(vm);
        buffer_objects.retain(|other| other.shape().realm() != realm);
        buffer_objects.push(buffer);
    }

    fn has_buffer_object(&self, buffer: Gc<ArrayBuffer>) -> bool {
        self.buffer_objects.borrow().contains(&buffer)
    }

    // https://webassembly.github.io/spec/js-api/#dom-memory-grow
    pub(crate) fn grow(this: &Ref<Self>, cx: &mut CallContext, delta: u32) -> WebIdlResult<u32> {
        // 1. Let memaddr be this.[[Memory]].
        Self::grow_the_memory_buffer(*this, cx.vm(), u64::from(delta))
    }

    // https://webassembly.github.io/spec/js-api/#grow-the-memory-buffer
    fn grow_the_memory_buffer(this: Ref<Self>, vm: &Vm, delta: u64) -> WebIdlResult<u32> {
        // 1. Let store be the surrounding agent's associated store.
        // 5. Set the surrounding agent's associated store to store.
        let previous_size = Agent::the(vm)
            .store()
            .memory_mut(this.address())
            .grow(delta)
            .ok_or_else(|| {
                Exception::range_error("Memory.grow() grows past the stated limit of the memory instance")
            })?;

        refresh_the_memory_buffer(vm, this);

        Ok(previous_size as u32)
    }

    // https://webassembly.github.io/threads/js-api/index.html#dom-memory-tofixedlengthbuffer
    pub(crate) fn to_fixed_length_buffer(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<Gc<ArrayBuffer>> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 1. Let buffer be this.[[BufferObject]].
        let buffer = Self::buffer_object(*this, vm, realm);

        // 2. Let memaddr be this.[[Memory]].
        // 3. If IsSharedArrayBuffer(buffer) is false,
        if !this.shared {
            // 1. If IsFixedLengthArrayBuffer(buffer) is true, return buffer.
            if buffer.is_fixed_length() {
                return Ok(buffer);
            }

            // 2. Otherwise,
            // 1. Let fixedBuffer be the result of creating a fixed length memory buffer from memaddr.
            let fixed_buffer = Self::create_a_fixed_length_memory_buffer(*this, vm, realm);

            // 2. Perform ! DetachArrayBuffer(buffer, "WebAssembly.Memory").
            detach_array_buffer(vm, buffer, Some(detach_key(vm))).must();

            // 3. Set this.[[BufferObject]] to fixedBuffer.
            this.set_buffer_object(vm, fixed_buffer);

            // 4. Return fixedBuffer.
            return Ok(fixed_buffer);
        }

        // 4. Otherwise,
        // 1. Let map be the surrounding agent's associated Memory object cache.
        // 2. Assert: map[memaddr] exists.
        // 3. Let newMemory be map[memaddr].
        let new_memory: Ref<Self> = host_object(this.address().cell()).expect("the memory has a Memory object");

        // 4. Let newBufferObject be newMemory.[[BufferObject]].
        let new_buffer_object = Self::buffer_object(new_memory, vm, realm);

        // 5. Set this.[[BufferObject]] to newBufferObject.
        this.set_buffer_object(vm, new_buffer_object);

        // 6. Return newBufferObject.
        Ok(new_buffer_object)
    }

    // https://webassembly.github.io/spec/js-api/#dom-memory-toresizablebuffer
    pub(crate) fn to_resizable_buffer(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<Gc<ArrayBuffer>> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 1. Let buffer be this.[[BufferObject]].
        let buffer = Self::buffer_object(*this, vm, realm);

        // 2. If IsFixedLengthArrayBuffer(buffer) is false, return buffer.
        if !buffer.is_fixed_length() {
            return Ok(buffer);
        }

        // 3. Let memaddr be this.[[Memory]].
        // 4. Let store be the surrounding agent's associated store.
        // 5. Let memtype be mem_type(store, memaddr).
        let maximum = Agent::the(vm).store().memory(this.address()).ty.limits.max;

        // 6. If memtype has a max,
        //        1. Let maxsize be the max value in memtype.
        // 7. Otherwise,
        //        1. Let maxsize be 65536 x 65536.
        let max_size = maximum.unwrap_or(65536) * PAGE_SIZE;

        // 8. Let resizableBuffer be the result of creating a resizable memory buffer from memaddr and maxsize.
        let resizable_buffer = Self::create_a_resizable_memory_buffer(*this, vm, realm, max_size)?;

        // https://webassembly.github.io/threads/js-api/index.html#dom-memory-toresizablebuffer
        // 5. If IsSharedArrayBuffer(buffer) is false,
        // 9. Perform ! DetachArrayBuffer(buffer, "WebAssembly.Memory").
        if !buffer.is_shared_array_buffer() {
            detach_array_buffer(vm, buffer, Some(detach_key(vm))).must();
        }

        // 10. Set this.[[BufferObject]] to resizableBuffer.
        this.set_buffer_object(vm, resizable_buffer);

        // 11. Return resizeableBuffer.
        Ok(resizable_buffer)
    }

    // https://webassembly.github.io/threads/js-api/#dom-memory-buffer
    pub(crate) fn buffer(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<Gc<ArrayBuffer>> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 1. Let memaddr be this.[[Memory]].
        // 2. Let block be a Data Block which is identified with the underlying memory of memaddr.
        // 3. If block is a Shared Data Block,
        if this.shared {
            // 1. Let map be the surrounding agent's associated Memory object cache.
            // 2. Assert: map[memaddr] exists.
            // 3. Let newMemory be map[memaddr].
            let new_memory: Ref<Self> = host_object(this.address().cell()).expect("the memory has a Memory object");

            // 4. Let newBufferObject be newMemory.[[BufferObject]].
            let new_buffer_object = Self::buffer_object(new_memory, vm, realm);

            // 5. Set this.[[BufferObject]] to newBufferObject.
            this.set_buffer_object(vm, new_buffer_object);

            // 6. Return newBufferObject.
            return Ok(new_buffer_object);
        }

        // 4. Otherwise,
        // 1. Return this.[[BufferObject]].
        Ok(Self::buffer_object(*this, vm, realm))
    }
}

// https://webassembly.github.io/spec/js-api/#refresh-the-memory-buffer
pub(crate) fn refresh_the_memory_buffer(vm: &Vm, memory: Ref<Memory>) {
    // 1. Let map be the surrounding agent's associated Memory object cache.
    // 2. Assert: map[memaddr] exists.
    // 3. Let memory be map[memaddr].
    // 4. Let buffer be memory.[[BufferObject]].
    // NB: Replacing a buffer changes the list, so the loop runs over a copy of it.
    let buffer_objects = MarkedVec::new(vm);
    buffer_objects.extend_from_slice(&memory.buffer_objects.borrow());
    for &buffer in buffer_objects.values().iter() {
        // 5. If IsFixedLengthArrayBuffer(buffer) is true,
        if buffer.is_fixed_length() {
            // https://webassembly.github.io/threads/js-api/index.html#refresh-the-memory-buffer
            // 1. If IsSharedArrayBuffer(buffer) is false,
            if !buffer.is_shared_array_buffer() {
                // 1. Perform ! DetachArrayBuffer(buffer, "WebAssembly.Memory").
                detach_array_buffer(vm, buffer, Some(detach_key(vm))).must();
            }

            // 2. Let newBuffer be the result of creating a fixed length memory buffer from memaddr.
            // 3. Set memory.[[BufferObject]] to newBuffer.
            let new_buffer = Memory::create_a_fixed_length_memory_buffer(memory, vm, buffer.shape().realm());
            memory.set_buffer_object(vm, new_buffer);
        } else {
            // 1. Let block be a Data Block which is identified with the underlying memory of memaddr.
            // AD-HOC: Neither the main spec nor the threads proposal specify that the Data Block should be Shared for
            //         shared Wasm memories, but we do in fact want a Shared Data Block in that case.
            let is_shared = if buffer.is_shared_array_buffer() {
                ArrayBufferShared::Yes
            } else {
                ArrayBufferShared::No
            };

            // 2. Set buffer.[[ArrayBufferData]] to block.
            // 3. Set buffer.[[ArrayBufferByteLength]] to the length of block.
            buffer.set_data_block(
                vm,
                DataBlock::external(Memory::external_storage(memory, vm, false), is_shared),
            );
        }
    }
}

/// The Memory object whose [[BufferObject]] the buffer is, if its [[ArrayBufferDetachKey]] is "WebAssembly.Memory".
fn memory_of_buffer(vm: &Vm, buffer: Gc<ArrayBuffer>) -> Option<Ref<Memory>> {
    if !same_value(buffer.detach_key(), detach_key(vm)) {
        return None;
    }
    // 3. For each memaddr → mem in all Memory object caches,
    //     1. If SameValue(mem.[[BufferObject]], buffer) is true,
    // NB: The memory whose [[BufferObject]] the buffer is owns the buffer's storage.
    let owner = buffer.with_data_block(|block| match &block.byte_buffer {
        DataBlockStorage::External(storage) => Some(storage.owner()),
        _ => None,
    })?;
    // SAFETY: The owner of external storage is a live cell.
    let memory = unsafe { cell_if_abi::<Memory>(owner.as_ptr()) }?;
    // 2. Assert: buffer is the [[BufferObject]] of exactly one value in map.
    assert!(memory.has_buffer_object(buffer));
    Some(memory)
}

/// The steps that HostResizeArrayBuffer and HostGrowSharedArrayBuffer share: whether the buffer is that of a memory,
/// and if it is, growing the memory to the new length.
fn grow_memory_of_buffer(vm: &Vm, buffer: Gc<ArrayBuffer>, new_length: usize) -> ThrowCompletionOr<bool> {
    let Some(memory) = memory_of_buffer(vm, buffer) else {
        return Ok(false);
    };
    let byte_length = buffer.byte_length();

    // 1. Assert: buffer.[[ArrayBufferByteLength]] modulo 65536 is 0.
    assert_eq!(byte_length as u64 % PAGE_SIZE, 0);

    // 2. Let lengthDelta be newLength - buffer.[[ArrayBufferByteLength]].
    // 3. If lengthDelta < 0 or lengthDelta modulo 65536 is not 0,
    if new_length < byte_length || !((new_length - byte_length) as u64).is_multiple_of(PAGE_SIZE) {
        // 1. Throw a RangeError exception.
        return vm.throw_completion_with_message(
            libjs_rust::api::ErrorKind::RangeError,
            "WebAssembly.Memory buffers must be resized by a multiple of the page size".to_string(),
        );
    }

    // 4. Let delta be lengthDelta ÷ 65536.
    let delta = (new_length - byte_length) as u64 / PAGE_SIZE;

    // 5. Grow the memory buffer associated with memaddr by delta.
    let cx = CallContext::new(vm, buffer.shape().realm());
    Memory::grow_the_memory_buffer(memory, vm, delta).map_err(|exception| exception.throw(&cx))?;
    Ok(true)
}

/// HostResizeArrayBuffer(buffer, newLength), https://webassembly.github.io/spec/js-api/#HostResizeArrayBuffer, which
/// writes whether it handled the buffer.
///
/// # Safety
///
/// The pointers must be the VM, a live ArrayBuffer, and a writable bool.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_web_assembly_host_resize_array_buffer(
    vm: *mut c_void,
    buffer: *mut c_void,
    new_length: usize,
    handled: *mut bool,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live buffer.
    let (vm, buffer) = unsafe {
        (
            &*vm.cast::<Vm>(),
            Gc::from_non_null(NonNull::new_unchecked(buffer.cast())),
        )
    };
    // 1. If buffer.[[ArrayBufferDetachKey]] is "WebAssembly.Memory",
    //     4. Return handled.
    // 2. Otherwise, return unhandled.
    let result = grow_memory_of_buffer(vm, buffer, new_length);
    if let Ok(was_handled) = result {
        // SAFETY: The caller passes a writable bool.
        unsafe { *handled = was_handled };
    }
    RawNativeFunctionResult::from_completion(result.map(|_| Value::UNDEFINED))
}

/// HostGrowSharedArrayBuffer(buffer, newLength), https://webassembly.github.io/threads/js-api/#abstract-operation-hostgrowsharedarraybuffer,
/// which writes whether it handled the buffer.
///
/// # Safety
///
/// The pointers must be the VM, a live SharedArrayBuffer, and a writable bool.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_web_assembly_host_grow_shared_array_buffer(
    vm: *mut c_void,
    buffer: *mut c_void,
    new_length: usize,
    handled: *mut bool,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the VM and a live buffer.
    let (vm, buffer) = unsafe {
        (
            &*vm.cast::<Vm>(),
            Gc::from_non_null(NonNull::new_unchecked(buffer.cast::<ArrayBuffer>())),
        )
    };
    // AD-HOC: If buffer.[[ArrayBufferDetachKey]] is "WebAssembly.Memory",
    let current_byte_length = buffer.byte_length();
    let result = grow_memory_of_buffer(vm, buffer, new_length).and_then(|was_handled| {
        if !was_handled {
            // AD-HOC: Otherwise, return unhandled.
            return Ok(false);
        }

        // 4. If newLength < buffer.[[ArrayBufferByteLength]] or newLength > buffer.[[ArrayBufferMaxByteLength]], throw a RangeError exception.
        // NB: The step cannot throw. Step 3.1.3 threw for a newLength below the byte length, and the buffer's
        //     [[ArrayBufferMaxByteLength]] is the memory's maximum, past which step 3.1.5 failed to grow.
        if new_length < current_byte_length {
            return vm.throw_completion(
                libjs_rust::api::ErrorKind::RangeError,
                libjs_rust::api::ErrorType::ByteLengthLessThanPreviousByteLength,
                &[&new_length, &current_byte_length],
            );
        }
        if new_length > buffer.max_byte_length() {
            return vm.throw_completion(
                libjs_rust::api::ErrorKind::RangeError,
                libjs_rust::api::ErrorType::ByteLengthExceedsMaxByteLength,
                &[&new_length, &buffer.max_byte_length()],
            );
        }

        // FIXME: 5. Let agentRecord be the surrounding agent’s Agent Record.
        // FIXME: 6. Let isLittleEndian be agentRecord.[[LittleEndian]] .
        // FIXME: 7. Let event be an implementation defined ReadModifyWriteSharedMemory event, whose [[Order]] is "SeqCst", [[Payload]] is NumericToRawBytes(Bigint, newLength, isLittleEndian), [[Block]] is buffer.[[ArrayBufferByteLengthData]], [[ByteIndex]] is 0, and [[ElementSize]] is 8.
        // FIXME: 8. Let eventsRecord be the EventsRecord in agentRecord.[[CandidateExecution]].[[EventsRecords]] that corresponds to the surrounding agent’s signifier agentRecord.[[Signifier]].
        // FIXME: 9. Append event to eventsRecord.[[EventsList]].
        assert_eq!(buffer.byte_length(), new_length);

        // 10. Return handled.
        Ok(true)
    });
    if let Ok(was_handled) = result {
        // SAFETY: The caller passes a writable bool.
        unsafe { *handled = was_handled };
    }
    RawNativeFunctionResult::from_completion(result.map(|_| Value::UNDEFINED))
}

impl Memory {
    /// The Memory object that the value is, if it is the wrapper of one.
    pub(crate) fn from_value(value: Value) -> Option<Ref<Self>> {
        crate::web_assembly::implementation_of_value::<Self>(value)
    }
}
