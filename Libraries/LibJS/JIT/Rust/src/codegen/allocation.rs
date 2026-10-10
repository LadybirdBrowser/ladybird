/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Allocation in JIT code, per the allocation contract (see
//! `ObjectAllocationInfo`): a cell from the size class's local free list,
//! entirely inline, and a call of the runtime out of line when the list is
//! empty or the heap wants to collect garbage.

use super::Codegen;
use super::call::ConstantField;
use super::slow_path_calls::slow_path_saved_registers;
use crate::CompileFailure;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;
use crate::snapshot::CellId;

/// The runtime call that allocates what an allocation node allocates.
#[derive(Clone, Copy)]
enum AllocationCall {
    /// `ObjectAllocationInfo::slow_path` with the shape and the properties
    /// to make room for.
    Object(CellId, u32),
    /// `ArrayAllocationInfo::slow_path` with the element count.
    Array(u32),
    /// `FunctionAllocationInfo::slow_path` with the closure template and
    /// the environments in the node's inputs.
    Function(CellId),
    /// `RuntimeInfo::create_lexical_environment` with the parent in the
    /// node's input, the environment shape cache and the capacity.
    Environment(u32, u32),
}

/// The out of line part of an allocation: the runtime call.
pub(super) struct DeferredAllocation {
    node: NodeId,
    call: AllocationCall,
    entry: Label,
    resume: Label,
    /// Constants the fast path stores into the new object's inline named
    /// properties for `InitializeNamed` nodes, which emit nothing then, as
    /// (byte offset in the cell, value).
    constant_fields: Vec<(u32, u64)>,
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    fn allocation_temp(&self, node: NodeId, index: usize) -> Gpr {
        Gpr(self.allocation.node(node).temps[index])
    }

    /// `dst` = the value with tag `tag` of the cell pointer `src`, which is
    /// `src` with what the two differ in flipped: cells are in the heap
    /// region, which is aligned to its size, so a cell pointer is the
    /// region's base or its offset, and the value is the tag or the offset.
    /// NB: Flipping a value would keep any bits between its offset and its
    ///     tag, which a forged value may have set, so values are only ever
    ///     decoded with `emit_unbox_cell()`, which masks them.
    pub(super) fn box_cell_with_tag(&mut self, dst: Gpr, src: Gpr, tag: u16) {
        let difference = (u64::from(tag) << value::TAG_SHIFT) | self.runtime.heap_region_base;
        if dst == src {
            self.masm.xor64_imm(dst, src, difference);
        } else {
            self.masm.move_imm64(dst, difference);
            self.masm.xor64(dst, dst, src);
        }
    }

    /// `dst = the object value of the cell pointer in dst`.
    pub(super) fn box_object(&mut self, dst: Gpr) {
        self.box_cell_with_tag(dst, dst, value::OBJECT_TAG);
    }

    /// `dst = the cell value of the cell pointer src`, for cells that are no
    /// objects, like environments.
    pub(super) fn box_cell(&mut self, dst: Gpr, src: Gpr) {
        self.masm.and64_imm(dst, src, self.runtime.heap_region_offset_mask);
        self.masm.or64_imm(dst, dst, self.runtime.shifted_is_cell_pattern);
    }

    /// The entry and resume labels of the out-of-line call that allocates
    /// for `node` where its inline allocation cannot (see
    /// `emit_deferred_allocations()`).
    fn defer_allocation(&mut self, node: NodeId, call: AllocationCall) -> (Label, Label) {
        let entry = self.masm.new_label();
        let resume = self.masm.new_label();
        self.deferred_allocations.push(DeferredAllocation {
            node,
            call,
            entry,
            resume,
            constant_fields: Vec::new(),
        });
        (entry, resume)
    }

    pub(super) fn emit_allocate_object(
        &mut self,
        node: NodeId,
        shape: CellId,
        reserve: u32,
    ) -> Result<(), CompileFailure> {
        let info = &self.runtime.object_allocation;
        let (entry, resume) = self.defer_allocation(node, AllocationCall::Object(shape, reserve));
        // NB: The cell's fields up to the inline storage come from the
        //     template, patched with the shape and the inline capacity.
        let template_bytes = 8 * info.template.len() as u64;
        let fields_fit = u64::from(info.inline_storage_offset) <= template_bytes
            && info.inline_storage_offset.is_multiple_of(8)
            && info.named_properties_offset.is_multiple_of(8)
            && info.shape_offset + 8 <= info.inline_storage_offset
            && info.inline_capacity_offset < info.inline_storage_offset;
        let size_class = info.size_class_for(reserve).copied().filter(|_| fields_fit);
        let Some(size_class) = size_class else {
            self.masm.jump(entry);
            self.masm.bind(resume);
            return Ok(());
        };

        let (cell, heap, next) = (
            self.output(node),
            self.allocation_temp(node, 0),
            self.allocation_temp(node, 1),
        );
        let info = self.runtime.object_allocation.clone();
        let at = |base: Gpr, offset: u32| Address::new(base, offset as i32);

        self.emit_check_heap_threshold(size_class.cell_size, cell, heap, entry);
        self.emit_pop_free_list(size_class.allocator, cell, next, entry);
        self.emit_count_allocation(size_class.cell_size, size_class.cell_size, heap, true);

        // The template, with the shape, the inline capacity and every inline
        // value undefined. The pointer to the inline storage is stored last.
        let mut image = info
            .template
            .iter()
            .flat_map(|word| word.to_le_bytes())
            .take(info.inline_storage_offset as usize)
            .collect::<Vec<u8>>();
        let shape_offset = info.shape_offset as usize;
        image[shape_offset..shape_offset + 8].copy_from_slice(&shape.0.to_le_bytes());
        image[info.inline_capacity_offset as usize] =
            u8::try_from(size_class.inline_capacity).map_err(|_| CompileFailure::CodeGeneration)?;
        let mut fields: Vec<ConstantField> = (0u32..)
            .zip(image.as_chunks::<8>().0)
            .map(|(index, word)| (8 * index, 8, u64::from_le_bytes(*word)))
            .filter(|(offset, _, _)| *offset != info.named_properties_offset)
            .collect();
        // NB: Of several initializations of a property, the last one wins.
        //     If it is a constant, the allocation stores it, and none of
        //     them runs; otherwise they all run.
        let initialized = self.initializations_without_gc(node);
        let last_initialization = |index: u32| initialized.iter().rev().find(|(offset, _, _)| *offset == index);
        for index in 0..size_class.inline_capacity {
            let value = match last_initialization(index) {
                // NB: Stored right after allocating, before anything could
                //     look at the object.
                Some((_, _, None)) => continue,
                Some((_, _, Some(bits))) => *bits,
                None => value::UNDEFINED,
            };
            fields.push((info.inline_storage_offset + 8 * index, 8, value));
        }
        let mut constant_fields = Vec::new();
        for (offset, initialization, _) in &initialized {
            let Some((_, last, Some(bits))) = last_initialization(*offset) else {
                continue;
            };
            self.absorbed_initializations.push(*initialization);
            if last == initialization {
                constant_fields.push((info.inline_storage_offset + 8 * offset, *bits));
            }
        }
        self.deferred_allocations
            .last_mut()
            .expect("the allocation's slow path was deferred")
            .constant_fields = constant_fields;
        self.emit_constant_fields(cell, next, &mut fields)?;
        self.masm
            .load_effective_address(next, &at(cell, info.inline_storage_offset));
        self.masm.store64(&at(cell, info.named_properties_offset), next);
        self.box_object(cell);
        self.masm.bind(resume);
        Ok(())
    }

    /// Calls the runtime for every allocation node that did not allocate
    /// inline, saving the registers the call clobbers.
    pub(super) fn emit_deferred_allocations(&mut self) {
        for allocation in std::mem::take(&mut self.deferred_allocations) {
            self.annotate(super::CodeAnnotation::Deferred(allocation.node));
            let output = self.output(allocation.node);
            self.masm.bind(allocation.entry);
            let saved = slow_path_saved_registers::<M>()
                .without(output)
                .iter()
                .collect::<Vec<_>>();
            self.emit_save_registers(&saved);
            let arguments = M::ARGUMENT_GPRS;
            match allocation.call {
                AllocationCall::Object(shape, reserve) => {
                    self.masm.move64(arguments[0], self.pinned.vm);
                    self.masm.move_imm64(arguments[1], shape.0);
                    self.masm.move_imm32(arguments[2], reserve);
                    self.masm.call_absolute(self.runtime.object_allocation.slow_path);
                }
                AllocationCall::Array(count) => {
                    self.masm.move64(arguments[0], self.pinned.vm);
                    self.masm.move_imm32(arguments[1], count);
                    self.masm.call_absolute(self.runtime.array_allocation.slow_path);
                }
                AllocationCall::Function(sample) => {
                    // NB: The inputs may be in any argument register but
                    //     the first two.
                    let scratch = self.pinned.scratch;
                    self.masm.move64(scratch, self.input(allocation.node, 1));
                    self.masm.move64(arguments[2], self.input(allocation.node, 0));
                    self.masm.move64(arguments[3], scratch);
                    self.masm.move64(arguments[0], self.pinned.vm);
                    self.masm.move_imm64(arguments[1], sample.0);
                    self.masm.call_absolute(self.runtime.function_allocation.slow_path);
                }
                AllocationCall::Environment(shape_cache, capacity) => {
                    let parent = self.input(allocation.node, 0);
                    let scratch = self.pinned.scratch;
                    self.emit_unbox_cell(scratch, parent);
                    self.masm.move64(arguments[1], scratch);
                    self.masm.move64(arguments[0], self.pinned.vm);
                    self.masm.move_imm64(arguments[2], self.executables[0].cell.0);
                    self.masm.move_imm32(arguments[3], shape_cache);
                    self.masm.move_imm32(arguments[4], capacity);
                    self.masm.call_absolute(self.runtime.create_lexical_environment);
                }
            }
            for (offset, bits) in &allocation.constant_fields {
                self.masm.move_imm64(self.pinned.scratch, *bits);
                self.masm
                    .store64(&Address::new(M::RETURN_GPRS[0], *offset as i32), self.pinned.scratch);
            }
            self.masm.move64(output, M::RETURN_GPRS[0]);
            if let AllocationCall::Environment(..) = allocation.call {
                self.box_cell(output, output);
            } else {
                self.box_object(output);
            }
            self.emit_restore_registers(&saved, None);
            self.masm.jump(allocation.resume);
        }
    }

    /// The `InitializeNamed` nodes right after the `AllocateObject` `node`
    /// in its block, before anything that could collect garbage or look at
    /// the object (a call, an allocation or an exit): their offsets, and
    /// their values if they are constants.
    fn initializations_without_gc(&self, node: NodeId) -> Vec<(u32, NodeId, Option<u64>)> {
        let body = &self.graph.block(self.block).body;
        let position = body
            .iter()
            .position(|id| *id == node)
            .expect("the node is in the block");
        let mut initialized = Vec::new();
        for id in &body[position + 1..] {
            let next = self.graph.node(*id);
            match next.op {
                Op::InitializeNamed { offset } if next.inputs[0] == node => {
                    initialized.push((offset, *id, self.graph.constant_value(next.inputs[1])));
                }
                _ => {
                    let properties = next.op.properties();
                    if properties.is_call
                        || properties.allocates
                        || next.op.can_eager_exit()
                        || properties.can_lazy_exit
                    {
                        break;
                    }
                }
            }
        }
        initialized
    }

    /// Stores into a named property of an object being initialized, which
    /// is a plain data property (see `Op::InitializeNamed`).
    pub(super) fn emit_initialize_named(&mut self, node: NodeId, offset: u32) -> Result<(), CompileFailure> {
        // NB: The allocation stored constants already.
        if self.absorbed_initializations.contains(&node) {
            return Ok(());
        }
        let storage = self.allocation_temp(node, 0);
        let object = self.input(node, 0);
        // Objects allocated in a size class keep their properties inline.
        let inline_storage = match self.graph.node(self.graph.node(node).inputs[0]).op {
            Op::AllocateObject { reserve, .. } => self.runtime.object_allocation.size_class_for(reserve).is_some(),
            _ => false,
        };
        self.emit_unbox_cell(storage, object);
        let slot = if inline_storage {
            let offset = u64::from(self.runtime.object_allocation.inline_storage_offset) + 8 * u64::from(offset);
            Address::new(storage, super::checked_i32(offset)?)
        } else {
            let storage_field = Address::new(storage, self.runtime.offsets.object_named_properties as i32);
            self.masm.load64(storage, &storage_field);
            self.named_property_address(storage, offset)?
        };
        self.masm.store64(&slot, self.input(node, 1));
        Ok(())
    }

    /// Branches to `slow` if taking a cell of `cell_size` bytes would take
    /// the heap past its threshold for the next collection, which the slow
    /// path starts. Clobbers `temp` and `heap`.
    pub(super) fn emit_check_heap_threshold(&mut self, cell_size: u32, temp: Gpr, heap: Gpr, slow: Label) {
        let info = &self.runtime.object_allocation;
        let (heap_address, allocated, threshold) = (
            info.heap,
            info.heap_allocated_bytes_offset as i32,
            info.heap_threshold_offset as i32,
        );
        self.masm.move_imm64(heap, heap_address);
        self.masm.load64(temp, &Address::new(heap, threshold));
        self.masm.sub64_imm(temp, temp, i64::from(cell_size));
        // NB: Both counts are far below 2^63.
        self.masm
            .branch64_memory(Condition::GreaterThan, &Address::new(heap, allocated), temp, slow);
    }

    /// Counts cells taken from local free lists: `collected_bytes` towards
    /// the next collection, and `total_bytes` in the heap's total. `heap`
    /// holds the heap if `emit_check_heap_threshold()` left it there, and
    /// is clobbered otherwise.
    pub(super) fn emit_count_allocation(
        &mut self,
        collected_bytes: u32,
        total_bytes: u32,
        heap: Gpr,
        heap_loaded: bool,
    ) {
        let info = &self.runtime.object_allocation;
        let (heap_address, allocated, total) = (
            info.heap,
            info.heap_allocated_bytes_offset as i32,
            info.heap_total_allocated_bytes_offset as i32,
        );
        let byte_count = |bytes: u32| i32::try_from(bytes).expect("cells are smaller than 2 GiB");
        if !heap_loaded {
            self.masm.move_imm64(heap, heap_address);
        }
        self.masm
            .add64_to_memory_imm(&Address::new(heap, allocated), byte_count(collected_bytes));
        self.masm
            .add64_to_memory_imm(&Address::new(heap, total), byte_count(total_bytes));
    }

    /// Pops a cell off the local free list of `allocator` into `cell`, or
    /// branches to `slow` if it is empty. Clobbers `temp` and the scratch
    /// register.
    pub(super) fn emit_pop_free_list(&mut self, allocator: u64, cell: Gpr, temp: Gpr, slow: Label) {
        let scratch = self.pinned.scratch;
        self.masm.move_imm64(scratch, allocator);
        self.emit_pop_free_list_of(scratch, cell, temp, slow);
    }

    /// Pops a cell off the local free list of the allocator in `allocator`
    /// into `cell`, or branches to `slow` if it is empty. Clobbers `temp`.
    pub(super) fn emit_pop_free_list_of(&mut self, allocator: Gpr, cell: Gpr, temp: Gpr, slow: Label) {
        let info = &self.runtime.object_allocation;
        let head = Address::new(allocator, info.local_free_list_offset as i32);
        let (next, link_mask) = (info.freelist_next_offset as i32, u64::from(info.freelist_link_mask));
        self.masm.load64(cell, &head);
        self.masm.branch_test64(Condition::Zero, cell, link_mask, slow);
        // NB: The next cell is the cell's address with the link's bits under
        //     the mask, so it is in the cell's block whatever the link is.
        self.masm.load64(temp, &Address::new(cell, next));
        self.masm.xor64(temp, temp, cell);
        self.masm.and64_imm(temp, temp, link_mask);
        self.masm.xor64(temp, temp, cell);
        self.masm.store64(&head, temp);
    }

    /// The array of `count` packed elements, all undefined (see
    /// `ArrayAllocationInfo`).
    pub(super) fn emit_allocate_array(&mut self, node: NodeId, count: u32) -> Result<(), CompileFailure> {
        // Larger arrays are rare, and filling their storage takes longer
        // than the runtime call.
        const MAX_INLINE_ELEMENTS: u32 = 64;
        let info = self.runtime.array_allocation.clone();
        let (entry, resume) = self.defer_allocation(node, AllocationCall::Array(count));
        let storage_size_class = info
            .storage_size_classes
            .iter()
            .find(|size_class| size_class.capacity >= count)
            .copied();
        let objects = self.runtime.object_allocation.clone();
        let layout = self.runtime.layout;
        let template_bytes = 8 * info.template.len() as u64;
        let array_fields = [
            objects.named_properties_offset,
            layout.object_indexed_elements,
            layout.object_indexed_storage_kind,
            layout.object_indexed_array_like_size,
        ];
        let fits = info.allocator != 0
            && count <= MAX_INLINE_ELEMENTS
            && (count == 0 || storage_size_class.is_some())
            && template_bytes <= u64::from(info.cell_size)
            && array_fields
                .iter()
                .all(|offset| u64::from(*offset) + 8 <= template_bytes)
            && objects.named_properties_offset.is_multiple_of(8)
            && layout.object_indexed_elements.is_multiple_of(8)
            && info.storage_values_offset as usize == 8 * info.storage_template.len()
            && info.storage_capacity_offset + 4 <= info.storage_values_offset;
        if !fits {
            self.masm.jump(entry);
            self.masm.bind(resume);
            return Ok(());
        }

        let array = self.output(node);
        let (storage, temp) = (self.allocation_temp(node, 0), self.allocation_temp(node, 1));
        let scratch = self.pinned.scratch;
        let at = |base: Gpr, offset: u32| Address::new(base, offset as i32);

        // Only the array cell counts towards the next collection.
        self.emit_check_heap_threshold(info.cell_size, array, temp, entry);
        // NB: Both free lists must have a cell before either gives one.
        let storage_size_class = storage_size_class.filter(|_| count != 0);
        if let Some(size_class) = storage_size_class {
            self.masm.move_imm64(scratch, size_class.allocator);
            self.masm.load64(temp, &at(scratch, objects.local_free_list_offset));
            self.masm
                .branch_test64(Condition::Zero, temp, u64::from(objects.freelist_link_mask), entry);
        }
        self.emit_pop_free_list(info.allocator, array, temp, entry);
        if let Some(size_class) = storage_size_class {
            self.emit_pop_free_list(size_class.allocator, storage, temp, entry);
        }
        let storage_cell_size = storage_size_class.map_or(0, |size_class| size_class.cell_size);
        self.emit_count_allocation(info.cell_size, info.cell_size + storage_cell_size, temp, false);

        // The array, with its packed elements if it has any.
        let mut image = info
            .template
            .iter()
            .flat_map(|word| word.to_le_bytes())
            .collect::<Vec<u8>>();
        if count != 0 {
            image[layout.object_indexed_storage_kind as usize] = layout.indexed_storage_kind_packed;
            let size = layout.object_indexed_array_like_size as usize;
            image[size..size + 4].copy_from_slice(&count.to_le_bytes());
        }
        let mut fields: Vec<ConstantField> = (0u32..)
            .zip(image.as_chunks::<8>().0)
            .map(|(index, word)| (8 * index, 8, u64::from_le_bytes(*word)))
            .filter(|(offset, _, _)| {
                *offset != objects.named_properties_offset && (count == 0 || *offset != layout.object_indexed_elements)
            })
            .collect();
        self.emit_constant_fields(array, temp, &mut fields)?;
        self.masm
            .load_effective_address(temp, &at(array, objects.inline_storage_offset));
        self.masm.store64(&at(array, objects.named_properties_offset), temp);

        if let Some(size_class) = storage_size_class {
            let mut fields: Vec<ConstantField> = (0u32..)
                .zip(&info.storage_template)
                .map(|(index, word)| (8 * index, 8, *word))
                .collect();
            for index in 0..size_class.capacity {
                let element = if index < count { value::UNDEFINED } else { value::EMPTY };
                fields.push((info.storage_values_offset + 8 * index, 8, element));
            }
            self.emit_constant_fields(storage, temp, &mut fields)?;
            self.masm.move_imm32(temp, size_class.capacity);
            self.masm.store32(&at(storage, info.storage_capacity_offset), temp);
            self.masm
                .load_effective_address(temp, &at(storage, info.storage_values_offset));
            self.masm.store64(&at(array, layout.object_indexed_elements), temp);
        }
        self.box_object(array);
        self.masm.bind(resume);
        Ok(())
    }

    /// Stores into an element of an array being initialized (see
    /// `Op::InitializeElement`).
    pub(super) fn emit_initialize_element(&mut self, node: NodeId, index: u32) -> Result<(), CompileFailure> {
        let elements = self.allocation_temp(node, 0);
        let slot = self.named_property_address(elements, index)?;
        self.emit_unbox_cell(elements, self.input(node, 0));
        let elements_field = Address::new(elements, self.runtime.layout.object_indexed_elements as i32);
        self.masm.load64(elements, &elements_field);
        self.masm.store64(&slot, self.input(node, 1));
        Ok(())
    }

    /// A lexical environment of the compiled function's
    /// `CreateLexicalEnvironment` with environment shape cache `shape_cache`
    /// (see `LexicalEnvironmentTemplateSnapshot`).
    pub(super) fn emit_allocate_environment(
        &mut self,
        node: NodeId,
        shape_cache: u32,
        capacity: u32,
    ) -> Result<(), CompileFailure> {
        let template = self.executables[0]
            .lexical_environment_templates
            .get(shape_cache as usize)
            .and_then(Option::as_ref)
            .cloned()
            .ok_or(CompileFailure::CodeGeneration)?;
        let (entry, resume) = self.defer_allocation(node, AllocationCall::Environment(shape_cache, capacity));
        let template_bytes = 8 * template.words.len() as u64;
        let stored = [template.binding_values_offset, template.outer_offset];
        let fits = template.allocator != 0
            && template_bytes <= u64::from(template.cell_size)
            && stored
                .iter()
                .all(|offset| offset.is_multiple_of(8) && u64::from(*offset) + 8 <= template_bytes);
        if !fits {
            self.masm.jump(entry);
            self.masm.bind(resume);
            return Ok(());
        }

        // NB: The output may be in the register of the input.
        let output = self.output(node);
        let parent = self.input(node, 0);
        let (environment, temp) = (self.allocation_temp(node, 0), self.allocation_temp(node, 1));
        let at = |base: Gpr, offset: u32| Address::new(base, offset as i32);

        self.emit_check_heap_threshold(template.cell_size, environment, temp, entry);
        self.emit_pop_free_list(template.allocator, environment, temp, entry);
        self.emit_count_allocation(template.cell_size, template.cell_size, temp, false);

        let mut fields: Vec<ConstantField> = (0u32..)
            .zip(&template.words)
            .map(|(index, word)| (8 * index, 8, *word))
            .filter(|(offset, _, _)| !stored.contains(offset))
            .collect();
        self.emit_constant_fields(environment, temp, &mut fields)?;
        if template.inline_binding_values {
            self.masm
                .load_effective_address(temp, &Address::new(environment, super::checked_i32(template_bytes)?));
            self.masm
                .store64(&at(environment, template.binding_values_offset), temp);
        } else {
            self.masm
                .store_imm64(&at(environment, template.binding_values_offset), 0);
        }
        self.emit_unbox_cell(temp, parent);
        self.masm.store64(&at(environment, template.outer_offset), temp);
        self.masm.move64(output, environment);
        self.box_cell(output, output);
        self.masm.bind(resume);
        Ok(())
    }

    /// A closure of the compiled function's `NewFunction` of
    /// `shared_function_data_index` (see `FunctionAllocationInfo`).
    pub(super) fn emit_allocate_function(
        &mut self,
        node: NodeId,
        shared_function_data_index: u32,
    ) -> Result<(), CompileFailure> {
        let info = self.runtime.function_allocation;
        let closure = self.executables[0]
            .closure_templates
            .get(shared_function_data_index as usize)
            .and_then(Option::as_ref)
            .cloned()
            .ok_or(CompileFailure::CodeGeneration)?;
        let (entry, resume) = self.defer_allocation(node, AllocationCall::Function(closure.sample));
        let objects = self.runtime.object_allocation.clone();
        let offsets = self.runtime.offsets;
        let stored = [
            objects.named_properties_offset,
            offsets.ecmascript_function_environment,
            offsets.ecmascript_function_private_environment,
        ];
        let template_bytes = 8 * closure.words.len() as u64;
        let fits = info.allocator != 0
            && template_bytes <= u64::from(info.cell_size)
            && stored
                .iter()
                .all(|offset| offset.is_multiple_of(8) && u64::from(*offset) + 8 <= template_bytes);
        if !fits {
            self.masm.jump(entry);
            self.masm.bind(resume);
            return Ok(());
        }

        // NB: The output may be in the register of an input.
        let output = self.output(node);
        let (environment, private_environment) = (self.input(node, 0), self.input(node, 1));
        let (function, temp) = (self.allocation_temp(node, 0), self.allocation_temp(node, 1));
        let at = |base: Gpr, offset: u32| Address::new(base, offset as i32);

        self.emit_check_heap_threshold(info.cell_size, function, temp, entry);
        self.emit_pop_free_list(info.allocator, function, temp, entry);
        self.emit_count_allocation(info.cell_size, info.cell_size, temp, false);

        let mut fields: Vec<ConstantField> = (0u32..)
            .zip(&closure.words)
            .map(|(index, word)| (8 * index, 8, *word))
            .filter(|(offset, _, _)| !stored.contains(offset))
            .collect();
        self.emit_constant_fields(function, temp, &mut fields)?;
        self.masm
            .load_effective_address(temp, &at(function, objects.inline_storage_offset));
        self.masm.store64(&at(function, objects.named_properties_offset), temp);
        self.masm
            .store64(&at(function, offsets.ecmascript_function_environment), environment);
        self.masm.store64(
            &at(function, offsets.ecmascript_function_private_environment),
            private_environment,
        );
        self.box_cell_with_tag(output, function, value::OBJECT_TAG);
        self.masm.bind(resume);
        Ok(())
    }
}
