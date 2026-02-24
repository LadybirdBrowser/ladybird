/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bytecode generator.
//!
//! This module contains the `Generator` struct which manages all state
//! needed for bytecode generation from the AST.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use super::basic_block::{BasicBlock, SourceMapEntry};
use super::instruction::Instruction;
use super::operand::*;
use crate::ast::{LocalType, Utf16String};
use crate::u32_from_usize;

/// Identifies an operand that auto-frees its register when the last
/// clone is dropped.
///
/// Wraps `Rc<ScopedOperandInner>`. When the last `Rc` clone drops
/// and the operand is a non-reserved register, the `Drop` impl
/// returns it to the generator's register pool for reuse.
#[derive(Debug, Clone)]
pub struct ScopedOperand {
    pub(crate) inner: std::rc::Rc<ScopedOperandInner>,
}

pub(crate) struct ScopedOperandInner {
    operand: Operand,
    free_register_pool: Rc<RefCell<Vec<Register>>>,
}

impl std::fmt::Debug for ScopedOperandInner {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "ScopedOperandInner({:?})", self.operand)
    }
}

impl Drop for ScopedOperandInner {
    fn drop(&mut self) {
        if self.operand.is_register() && self.operand.index() >= Register::RESERVED_COUNT {
            self.free_register_pool
                .borrow_mut()
                .push(Register(self.operand.index()));
        }
    }
}

impl ScopedOperand {
    pub fn operand(&self) -> Operand {
        self.inner.operand
    }
}

impl PartialEq for ScopedOperand {
    fn eq(&self, other: &Self) -> bool {
        self.inner.operand == other.inner.operand
    }
}

pub use crate::ast::FunctionKind;

/// Block boundary types for unwind tracking.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlockBoundaryType {
    Break,
    Continue,
    ReturnToFinally,
    LeaveFinally,
    LeaveLexicalEnvironment,
}

/// A break/continue scope with its target label and language labels.
pub struct LabelableScope {
    pub bytecode_target: Label,
    pub language_label_set: Vec<Utf16String>,
    pub completion_register: Option<ScopedOperand>,
}

/// Codegen-time state for a try/finally scope.
///
/// Stored in `Generator::finally_contexts` Vec, referenced by index.
/// This avoids the deep-clone issues of an owned `Box` parent chain.
pub struct FinallyContext {
    pub completion_type: ScopedOperand,
    pub completion_value: ScopedOperand,
    pub finally_body: Label,
    pub exception_preamble: Label,
    pub parent_index: Option<usize>,
    pub registered_jumps: Vec<FinallyJump>,
    pub next_jump_index: i32,
    pub lexical_environment_at_entry: Option<ScopedOperand>,
}

impl FinallyContext {
    pub const NORMAL: i32 = 0;
    pub const THROW: i32 = 1;
    pub const RETURN: i32 = 2;
    pub const FIRST_JUMP_INDEX: i32 = 3;
}

/// A break/continue target registered with a FinallyContext.
pub struct FinallyJump {
    pub index: i32,
    pub target: Label,
}

/// A local variable name with metadata.
#[derive(Debug)]
pub struct LocalVariable {
    pub name: Utf16String,
    pub is_lexically_declared: bool,
    pub is_initialized_during_declaration_instantiation: bool,
}

/// The bytecode generator.
///
/// Manages all state needed for compiling an AST into bytecode.
pub struct Generator {
    // --- Basic block management ---
    pub basic_blocks: Vec<BasicBlock>,
    current_block_index: Label,

    // --- Register allocation ---
    next_register: u32,
    free_register_pool: Rc<RefCell<Vec<Register>>>,

    // --- Constant pool ---
    pub constants: Vec<ConstantValue>,

    // Cached constants for deduplication
    true_constant: Option<ScopedOperand>,
    false_constant: Option<ScopedOperand>,
    null_constant: Option<ScopedOperand>,
    undefined_constant: Option<ScopedOperand>,
    empty_constant: Option<ScopedOperand>,
    int32_constants: HashMap<i32, ScopedOperand>,
    string_constants: HashMap<Utf16String, ScopedOperand>,

    // --- String/identifier/property tables (with deduplication) ---
    pub string_table: Vec<Utf16String>,
    string_table_index: HashMap<Utf16String, StringTableIndex>,
    pub identifier_table: Vec<Utf16String>,
    identifier_table_index: HashMap<Utf16String, IdentifierTableIndex>,
    pub property_key_table: Vec<Utf16String>,
    property_key_table_index: HashMap<Utf16String, PropertyKeyTableIndex>,
    pub compiled_regexes: Vec<*mut std::ffi::c_void>,

    // --- Scope/unwind state ---
    pub boundaries: Vec<BlockBoundaryType>,
    pub continuable_scopes: Vec<LabelableScope>,
    pub breakable_scopes: Vec<LabelableScope>,
    pub pending_labels: Vec<Utf16String>,
    pub lexical_environment_register_stack: Vec<ScopedOperand>,
    pub home_objects: Vec<ScopedOperand>,

    // --- Finally context ---
    // FinallyContext objects are stored in this Vec and referenced by index.
    // This avoids the deep-clone issues of an owned Box parent chain.
    pub finally_contexts: Vec<FinallyContext>,
    pub current_finally_context: Option<usize>,

    // --- Various counters ---
    pub next_property_lookup_cache: u32,
    pub next_global_variable_cache: u32,
    pub next_template_object_cache: u32,
    pub next_object_shape_cache: u32,

    // --- Codegen state ---
    pub strict: bool,
    pub function_environment_needed: bool,
    pub enclosing_function_kind: FunctionKind,
    pub local_variables: Vec<LocalVariable>,
    pub initialized_locals: Vec<bool>,
    pub initialized_arguments: Vec<bool>,

    /// When set, function/class expressions will use this as their `.name`.
    /// Set by assignment/declaration codegen, consumed by function expression codegen.
    pub pending_lhs_name: Option<IdentifierTableIndex>,

    // Source location tracking
    pub current_source_start: u32,
    pub current_source_end: u32,

    // --- Completion register ---
    pub current_completion_register: Option<ScopedOperand>,
    pub must_propagate_completion: bool,

    // --- Accumulator and this ---
    accumulator: ScopedOperand,
    this_value: ScopedOperand,

    // --- Shared function data ---
    // Opaque pointers to SharedFunctionInstanceData objects.
    pub shared_function_data: Vec<*mut std::ffi::c_void>,

    // --- Class blueprints ---
    // Opaque pointers to heap-allocated ClassBlueprint objects.
    // Ownership transfers to the Executable during creation.
    pub class_blueprints: Vec<*mut std::ffi::c_void>,

    // --- Length identifier cache ---
    pub length_identifier: Option<PropertyKeyTableIndex>,

    // --- Unwind context ---
    // When set, newly created basic blocks inherit this handler index.
    pub current_unwind_handler: Option<Label>,

    // --- AnnexB function names ---
    // Names approved for AnnexB.3.3 hoisting by the scope collector.
    // Populated during FDI, checked in switch case codegen.
    pub annexb_function_names: HashSet<Utf16String>,

    // --- Builtin abstract operations ---
    // When true, calls to known abstract operations (e.g. IsCallable, GetMethod)
    // are compiled to specialized bytecode instructions rather than normal calls.
    // Used for builtin JS files.
    pub builtin_abstract_operations_enabled: bool,

    // --- FFI context ---
    // These are set by the top-level compiler and passed through for
    // creating SharedFunctionInstanceData via FFI callbacks.
    pub vm_ptr: *mut std::ffi::c_void,
    pub source_code_ptr: *const std::ffi::c_void,
    pub source_len: usize,

    // --- Function table ---
    // Side table owning all FunctionData from the parser. Codegen
    // takes ownership of individual entries via `take()`.
    pub function_table: crate::ast::FunctionTable,
}

macro_rules! singleton_constant {
    ($self:expr_2021, $field:ident, $value:expr_2021) => {{
        if let Some(op) = &$self.$field {
            return op.clone();
        }
        let op = $self.append_constant($value);
        $self.$field = Some(op.clone());
        op
    }};
}

macro_rules! next_cache_method {
    ($method:ident, $field:ident) => {
        pub fn $method(&mut self) -> u32 {
            let index = self.$field;
            self.$field += 1;
            index
        }
    };
}

macro_rules! define_intern_method {
    ($method_name:ident, $index_type:ident, $table:ident, $cache:ident) => {
        pub fn $method_name(&mut self, s: &[u16]) -> $index_type {
            if let Some(&index) = self.$cache.get(s) {
                return index;
            }
            let index = $index_type(u32_from_usize(self.$table.len()));
            let key = Utf16String(s.to_vec());
            self.$table.push(key.clone());
            self.$cache.insert(key, index);
            index
        }
    };
}

impl Default for Generator {
    fn default() -> Self {
        Self::new()
    }
}

impl Generator {
    /// Create a new bytecode generator.
    pub fn new() -> Self {
        let free_register_pool = Rc::new(RefCell::new(Vec::new()));

        Self {
            basic_blocks: Vec::new(),
            current_block_index: Label(0),
            next_register: Register::RESERVED_COUNT,
            constants: Vec::new(),
            true_constant: None,
            false_constant: None,
            null_constant: None,
            undefined_constant: None,
            empty_constant: None,
            int32_constants: HashMap::new(),
            string_constants: HashMap::new(),
            string_table: Vec::new(),
            string_table_index: HashMap::new(),
            identifier_table: Vec::new(),
            identifier_table_index: HashMap::new(),
            property_key_table: Vec::new(),
            property_key_table_index: HashMap::new(),
            compiled_regexes: Vec::new(),
            boundaries: Vec::new(),
            continuable_scopes: Vec::new(),
            breakable_scopes: Vec::new(),
            pending_labels: Vec::new(),
            lexical_environment_register_stack: Vec::new(),
            home_objects: Vec::new(),
            finally_contexts: Vec::new(),
            current_finally_context: None,
            next_property_lookup_cache: 0,
            next_global_variable_cache: 0,
            next_template_object_cache: 0,
            next_object_shape_cache: 0,
            strict: false,
            function_environment_needed: true,
            enclosing_function_kind: FunctionKind::Normal,
            local_variables: Vec::new(),
            initialized_locals: Vec::new(),
            initialized_arguments: Vec::new(),
            pending_lhs_name: None,
            current_source_start: 0,
            current_source_end: 0,
            current_completion_register: None,
            must_propagate_completion: false,
            accumulator: ScopedOperand {
                inner: Rc::new(ScopedOperandInner {
                    operand: Operand::register(Register::ACCUMULATOR),
                    free_register_pool: free_register_pool.clone(),
                }),
            },
            this_value: ScopedOperand {
                inner: Rc::new(ScopedOperandInner {
                    operand: Operand::register(Register::THIS_VALUE),
                    free_register_pool: free_register_pool.clone(),
                }),
            },
            shared_function_data: Vec::new(),
            class_blueprints: Vec::new(),
            length_identifier: None,
            current_unwind_handler: None,
            annexb_function_names: HashSet::new(),
            builtin_abstract_operations_enabled: false,
            vm_ptr: std::ptr::null_mut(),
            source_code_ptr: std::ptr::null(),
            source_len: 0,
            function_table: crate::ast::FunctionTable::new(),
            free_register_pool,
        }
    }

    // --- Function kind queries ---

    pub fn is_in_generator_function(&self) -> bool {
        matches!(
            self.enclosing_function_kind,
            FunctionKind::Generator | FunctionKind::AsyncGenerator
        )
    }

    pub fn is_in_async_function(&self) -> bool {
        matches!(
            self.enclosing_function_kind,
            FunctionKind::Async | FunctionKind::AsyncGenerator
        )
    }

    pub fn is_in_async_generator_function(&self) -> bool {
        self.enclosing_function_kind == FunctionKind::AsyncGenerator
    }

    pub fn is_in_generator_or_async_function(&self) -> bool {
        self.enclosing_function_kind != FunctionKind::Normal
    }

    pub fn is_in_finalizer(&self) -> bool {
        self.boundaries.contains(&BlockBoundaryType::LeaveFinally)
    }

    // --- Register management ---

    /// Allocate a new register (or reuse a freed one).
    /// Always picks the lowest-numbered free register to ensure deterministic
    /// allocation regardless of operand drop order.
    pub fn allocate_register(&mut self) -> ScopedOperand {
        let reg = {
            let mut pool = self.free_register_pool.borrow_mut();
            if pool.is_empty() {
                let r = Register(self.next_register);
                self.next_register += 1;
                r
            } else {
                let min_index = pool.iter().enumerate().min_by_key(|(_, r)| r.0).unwrap().0;
                pool.remove(min_index)
            }
        };
        self.scoped_operand(Operand::register(reg))
    }

    /// Get a ScopedOperand for a local variable.
    pub fn local(&mut self, index: u32) -> ScopedOperand {
        self.scoped_operand(Operand::local(index))
    }

    /// Resolve a local binding (argument or variable) to a ScopedOperand.
    pub fn resolve_local(&mut self, index: u32, local_type: LocalType) -> ScopedOperand {
        match local_type {
            LocalType::Argument => self.scoped_operand(Operand::argument(index)),
            LocalType::Variable => self.local(index),
        }
    }

    /// Get the accumulator register.
    pub fn accumulator(&self) -> ScopedOperand {
        self.accumulator.clone()
    }

    /// Get the this_value register.
    pub fn this_value(&self) -> ScopedOperand {
        self.this_value.clone()
    }

    /// Get the exception register as a raw Operand (not ScopedOperand since
    /// it's a fixed register that should not be freed).
    pub fn exception_operand(&self) -> Operand {
        Operand::register(Register::EXCEPTION)
    }

    /// Copy a local variable into a fresh register to prevent later
    /// side effects from changing its value. Returns the operand unchanged
    /// if it is not a local.
    pub fn copy_if_needed_to_preserve_evaluation_order(
        &mut self,
        operand: &ScopedOperand,
    ) -> ScopedOperand {
        if operand.operand().is_local() {
            let reg = self.allocate_register();
            self.emit_mov(&reg, operand);
            reg
        } else {
            operand.clone()
        }
    }

    pub fn scoped_operand(&mut self, operand: Operand) -> ScopedOperand {
        ScopedOperand {
            inner: Rc::new(ScopedOperandInner {
                operand,
                free_register_pool: self.free_register_pool.clone(),
            }),
        }
    }

    // --- Constant pool ---

    fn append_constant(&mut self, value: ConstantValue) -> ScopedOperand {
        let index = u32_from_usize(self.constants.len());
        self.constants.push(value);
        self.scoped_operand(Operand::constant(index))
    }

    pub fn add_constant_number(&mut self, value: f64) -> ScopedOperand {
        // Deduplicate i32 values (but not -0.0, which has distinct semantics from +0.0)
        if value.fract() == 0.0
            && value >= i32::MIN as f64
            && value <= i32::MAX as f64
            && value.to_bits() != (-0.0_f64).to_bits()
        {
            let as_i32 = value as i32;
            if let Some(op) = self.int32_constants.get(&as_i32) {
                return op.clone();
            }
            let op = self.append_constant(ConstantValue::Number(value));
            self.int32_constants.insert(as_i32, op.clone());
            return op;
        }
        self.append_constant(ConstantValue::Number(value))
    }

    pub fn add_constant_boolean(&mut self, value: bool) -> ScopedOperand {
        if value {
            singleton_constant!(self, true_constant, ConstantValue::Boolean(true))
        } else {
            singleton_constant!(self, false_constant, ConstantValue::Boolean(false))
        }
    }

    pub fn add_constant_null(&mut self) -> ScopedOperand {
        singleton_constant!(self, null_constant, ConstantValue::Null)
    }

    pub fn add_constant_undefined(&mut self) -> ScopedOperand {
        singleton_constant!(self, undefined_constant, ConstantValue::Undefined)
    }

    pub fn add_constant_empty(&mut self) -> ScopedOperand {
        singleton_constant!(self, empty_constant, ConstantValue::Empty)
    }

    pub fn add_constant_string(&mut self, value: Utf16String) -> ScopedOperand {
        if let Some(op) = self.string_constants.get(&value) {
            return op.clone();
        }
        let op = self.append_constant(ConstantValue::String(value.clone()));
        self.string_constants.insert(value, op.clone());
        op
    }

    pub fn add_constant_bigint(&mut self, value: String) -> ScopedOperand {
        self.append_constant(ConstantValue::BigInt(value))
    }

    pub fn add_constant_raw_value(&mut self, value: u64) -> ScopedOperand {
        self.append_constant(ConstantValue::RawValue(value))
    }

    /// Get the constant value for a constant operand.
    pub fn get_constant(&self, operand: &ScopedOperand) -> Option<&ConstantValue> {
        if operand.operand().is_constant() {
            self.constants.get(operand.operand().index() as usize)
        } else {
            None
        }
    }

    // --- Table interning ---

    define_intern_method!(
        intern_string,
        StringTableIndex,
        string_table,
        string_table_index
    );
    define_intern_method!(
        intern_identifier,
        IdentifierTableIndex,
        identifier_table,
        identifier_table_index
    );
    define_intern_method!(
        intern_property_key,
        PropertyKeyTableIndex,
        property_key_table,
        property_key_table_index
    );

    /// If `operand` is a constant string that is not an array index, intern it
    /// as a property key and return the index. Uses split borrows to avoid
    /// cloning the string when it is already interned (the common case).
    pub fn try_constant_string_to_property_key(
        &mut self,
        operand: &ScopedOperand,
    ) -> Option<PropertyKeyTableIndex> {
        if !operand.operand().is_constant() {
            return None;
        }
        let idx = operand.operand().index() as usize;
        let s: &[u16] = match self.constants.get(idx) {
            Some(ConstantValue::String(s)) if !super::codegen::is_array_index(&s.0) => &s.0,
            _ => return None,
        };
        // Split borrow: s borrows self.constants, get() borrows self.property_key_table_index
        if let Some(&key_index) = self.property_key_table_index.get(s) {
            return Some(key_index);
        }
        // Cold path: not yet interned, must clone
        let owned = Utf16String(s.to_vec());
        let key_index = PropertyKeyTableIndex(u32_from_usize(self.property_key_table.len()));
        self.property_key_table.push(owned.clone());
        self.property_key_table_index.insert(owned, key_index);
        Some(key_index)
    }

    /// Register a SharedFunctionInstanceData (opaque pointer) and return its index.
    pub fn register_shared_function_data(&mut self, ptr: *mut std::ffi::c_void) -> u32 {
        let index = u32_from_usize(self.shared_function_data.len());
        self.shared_function_data.push(ptr);
        index
    }

    /// Register a ClassBlueprint (opaque pointer) and return its index.
    pub fn register_class_blueprint(&mut self, ptr: *mut std::ffi::c_void) -> u32 {
        let index = u32_from_usize(self.class_blueprints.len());
        self.class_blueprints.push(ptr);
        index
    }

    pub fn intern_regex(&mut self, compiled: *mut std::ffi::c_void) -> RegexTableIndex {
        let index = u32_from_usize(self.compiled_regexes.len());
        self.compiled_regexes.push(compiled);
        RegexTableIndex(index)
    }

    // --- Basic block management ---

    /// Create a new basic block and return its label.
    pub fn make_block(&mut self) -> Label {
        let index = self.basic_blocks.len();
        let mut block = BasicBlock::new(u32_from_usize(index));

        // Propagate exception handler from active unwind context.
        if let Some(handler) = self.current_unwind_handler {
            block.handler = Some(handler);
        }

        self.basic_blocks.push(block);
        Label(u32_from_usize(index))
    }

    /// Switch emission to the given basic block.
    pub fn switch_to_basic_block(&mut self, label: Label) {
        self.current_block_index = label;
    }

    /// Get the current basic block's label.
    pub fn current_block_index(&self) -> Label {
        self.current_block_index
    }

    /// Is the current block terminated?
    pub fn is_current_block_terminated(&self) -> bool {
        self.basic_blocks[self.current_block_index.basic_block_index()].terminated
    }

    /// Number of basic blocks.
    pub fn basic_block_count(&self) -> usize {
        self.basic_blocks.len()
    }

    /// Terminate all unterminated blocks with Yield (no continuation).
    /// Used for generator and async functions.
    pub fn terminate_unterminated_blocks_with_yield(&mut self) {
        let block_count = self.basic_block_count();
        for i in 0..block_count {
            let label = Label(u32_from_usize(i));
            if self.is_block_terminated(label) {
                continue;
            }
            self.switch_to_basic_block(label);
            let undef = self.add_constant_undefined();
            self.emit(Instruction::Yield {
                continuation_label: None,
                value: undef.operand(),
            });
        }
    }

    /// Is a specific block terminated?
    pub fn is_block_terminated(&self, label: Label) -> bool {
        self.basic_blocks[label.basic_block_index()].terminated
    }

    // --- Instruction emission ---

    /// Emit an instruction to the current basic block.
    pub fn emit(&mut self, instruction: Instruction) {
        if self.is_current_block_terminated() {
            return;
        }
        let source_map = SourceMapEntry {
            bytecode_offset: 0, // filled during flattening
            source_start: self.current_source_start,
            source_end: self.current_source_end,
        };
        let block = &mut self.basic_blocks[self.current_block_index.basic_block_index()];
        block.append(instruction, source_map);
    }

    /// Emit a Mov instruction (optimized away if src == dst).
    pub fn emit_mov(&mut self, dst: &ScopedOperand, src: &ScopedOperand) {
        if dst != src {
            self.emit(Instruction::Mov {
                dst: dst.operand(),
                src: src.operand(),
            });
        }
    }

    pub fn emit_mov_raw(&mut self, dst: Operand, src: Operand) {
        // NB: Unlike emit_mov (ScopedOperand version), this does NOT skip self-moves.
        //     This matches C++ emit_mov(Operand, Operand) which also emits unconditionally.
        self.emit(Instruction::Mov { dst, src });
    }

    /// Emit a conditional jump, with comparison fusion and constant folding.
    pub fn emit_jump_if(
        &mut self,
        condition: &ScopedOperand,
        true_target: Label,
        false_target: Label,
    ) {
        // OPTIMIZATION: If condition is a constant, emit an unconditional jump.
        if let Some(constant) = self.get_constant(condition)
            && let Some(is_truthy) = constant_to_boolean(constant)
        {
            self.emit(Instruction::Jump {
                target: if is_truthy { true_target } else { false_target },
            });
            return;
        }

        // OPTIMIZATION: If the condition is a register with ref_count == 1 and the last
        // instruction is a comparison whose dst matches condition, fuse into a JumpXxx.
        if condition.operand().is_register() && std::rc::Rc::strong_count(&condition.inner) == 1 {
            let block = &mut self.basic_blocks[self.current_block_index.basic_block_index()];
            if let Some((last_instruction, _)) = block.instructions.last() {
                let fused = match last_instruction {
                    Instruction::LessThan { dst, lhs, rhs } if *dst == condition.operand() => {
                        Some(Instruction::JumpLessThan {
                            lhs: *lhs,
                            rhs: *rhs,
                            true_target,
                            false_target,
                        })
                    }
                    Instruction::LessThanEquals { dst, lhs, rhs }
                        if *dst == condition.operand() =>
                    {
                        Some(Instruction::JumpLessThanEquals {
                            lhs: *lhs,
                            rhs: *rhs,
                            true_target,
                            false_target,
                        })
                    }
                    Instruction::GreaterThan { dst, lhs, rhs } if *dst == condition.operand() => {
                        Some(Instruction::JumpGreaterThan {
                            lhs: *lhs,
                            rhs: *rhs,
                            true_target,
                            false_target,
                        })
                    }
                    Instruction::GreaterThanEquals { dst, lhs, rhs }
                        if *dst == condition.operand() =>
                    {
                        Some(Instruction::JumpGreaterThanEquals {
                            lhs: *lhs,
                            rhs: *rhs,
                            true_target,
                            false_target,
                        })
                    }
                    Instruction::LooselyEquals { dst, lhs, rhs } if *dst == condition.operand() => {
                        Some(Instruction::JumpLooselyEquals {
                            lhs: *lhs,
                            rhs: *rhs,
                            true_target,
                            false_target,
                        })
                    }
                    Instruction::LooselyInequals { dst, lhs, rhs }
                        if *dst == condition.operand() =>
                    {
                        Some(Instruction::JumpLooselyInequals {
                            lhs: *lhs,
                            rhs: *rhs,
                            true_target,
                            false_target,
                        })
                    }
                    Instruction::StrictlyEquals { dst, lhs, rhs }
                        if *dst == condition.operand() =>
                    {
                        Some(Instruction::JumpStrictlyEquals {
                            lhs: *lhs,
                            rhs: *rhs,
                            true_target,
                            false_target,
                        })
                    }
                    Instruction::StrictlyInequals { dst, lhs, rhs }
                        if *dst == condition.operand() =>
                    {
                        Some(Instruction::JumpStrictlyInequals {
                            lhs: *lhs,
                            rhs: *rhs,
                            true_target,
                            false_target,
                        })
                    }
                    _ => None,
                };
                if let Some(fused_instruction) = fused {
                    // Remove the comparison instruction and emit the fused jump.
                    block.instructions.pop();
                    self.emit(fused_instruction);
                    return;
                }
            }
        }

        self.emit(Instruction::JumpIf {
            condition: condition.operand(),
            true_target,
            false_target,
        });
    }

    // --- Cache index allocation ---

    next_cache_method!(next_property_lookup_cache, next_property_lookup_cache);
    next_cache_method!(next_global_variable_cache, next_global_variable_cache);
    next_cache_method!(next_template_object_cache, next_template_object_cache);
    next_cache_method!(next_object_shape_cache, next_object_shape_cache);

    // --- Lexical environment helpers ---

    pub fn current_lexical_environment(&mut self) -> ScopedOperand {
        self.lexical_environment_register_stack
            .last()
            .cloned()
            .unwrap_or_else(|| {
                self.scoped_operand(Operand::register(Register::SAVED_LEXICAL_ENVIRONMENT))
            })
    }

    pub fn end_variable_scope(&mut self) {
        self.end_boundary(BlockBoundaryType::LeaveLexicalEnvironment);
        self.lexical_environment_register_stack.pop();
        if !self.is_current_block_terminated() {
            let parent = self.current_lexical_environment();
            self.emit(Instruction::SetLexicalEnvironment {
                environment: parent.operand(),
            });
        }
    }

    pub fn allocate_completion_register(&mut self) -> Option<ScopedOperand> {
        if self.must_propagate_completion {
            let reg = self.allocate_register();
            let undef = self.add_constant_undefined();
            self.emit_mov(&reg, &undef);
            Some(reg)
        } else {
            None
        }
    }

    pub fn push_new_lexical_environment(&mut self, capacity: u32) -> ScopedOperand {
        let parent = self.current_lexical_environment();
        let new_env = self.allocate_register();
        self.emit(Instruction::CreateLexicalEnvironment {
            dst: new_env.operand(),
            parent: parent.operand(),
            capacity,
        });
        self.lexical_environment_register_stack
            .push(new_env.clone());
        new_env
    }

    // --- Boundary management ---

    pub fn start_boundary(&mut self, ty: BlockBoundaryType) {
        self.boundaries.push(ty);
    }

    pub fn end_boundary(&mut self, ty: BlockBoundaryType) {
        assert_eq!(self.boundaries.last(), Some(&ty));
        self.boundaries.pop();
    }

    // --- Break/continue scope management ---

    pub fn begin_breakable_scope(
        &mut self,
        target: Label,
        label_set: Vec<Utf16String>,
        completion: Option<ScopedOperand>,
    ) {
        self.breakable_scopes.push(LabelableScope {
            bytecode_target: target,
            language_label_set: label_set,
            completion_register: completion,
        });
        self.start_boundary(BlockBoundaryType::Break);
    }

    pub fn end_breakable_scope(&mut self) {
        self.end_boundary(BlockBoundaryType::Break);
        self.breakable_scopes.pop();
    }

    pub fn begin_continuable_scope(
        &mut self,
        target: Label,
        label_set: Vec<Utf16String>,
        completion: Option<ScopedOperand>,
    ) {
        self.continuable_scopes.push(LabelableScope {
            bytecode_target: target,
            language_label_set: label_set,
            completion_register: completion,
        });
        self.start_boundary(BlockBoundaryType::Continue);
    }

    pub fn end_continuable_scope(&mut self) {
        self.end_boundary(BlockBoundaryType::Continue);
        self.continuable_scopes.pop();
    }

    pub fn set_current_breakable_scope_completion_register(&mut self, completion: ScopedOperand) {
        self.breakable_scopes
            .last_mut()
            .expect("no active breakable scope")
            .completion_register = Some(completion);
    }

    pub fn find_breakable_scope(&self, label: Option<&[u16]>) -> Option<&LabelableScope> {
        if let Some(label) = label {
            self.breakable_scopes
                .iter()
                .rev()
                .find(|s| s.language_label_set.iter().any(|l| l == label))
        } else {
            self.breakable_scopes.last()
        }
    }

    pub fn find_continuable_scope(&self, label: Option<&[u16]>) -> Option<&LabelableScope> {
        if let Some(label) = label {
            self.continuable_scopes
                .iter()
                .rev()
                .find(|s| s.language_label_set.iter().any(|l| l == label))
        } else {
            self.continuable_scopes.last()
        }
    }

    // --- FinallyContext support ---

    /// Push a new FinallyContext and set it as current. Returns its index.
    pub fn push_finally_context(&mut self, ctx: FinallyContext) -> usize {
        let index = self.finally_contexts.len();
        self.finally_contexts.push(ctx);
        self.current_finally_context = Some(index);
        index
    }

    /// Check if there is an outer ReturnToFinally boundary between `boundary_index`
    /// and the matching break/continue boundary.
    fn has_outer_finally_before_target(&self, is_break: bool, boundary_index: usize) -> bool {
        for j in (0..boundary_index.saturating_sub(1)).rev() {
            let inner = self.boundaries[j];
            if (is_break && inner == BlockBoundaryType::Break)
                || (!is_break && inner == BlockBoundaryType::Continue)
            {
                return false;
            }
            if inner == BlockBoundaryType::ReturnToFinally {
                return true;
            }
        }
        false
    }

    /// Register a jump target with the current FinallyContext.
    /// Assigns a unique completion_type index and emits code to set it and jump to finally.
    pub fn register_jump_in_finally_context(&mut self, target: Label) {
        let index = self
            .current_finally_context
            .expect("no active finally context");
        let ctx = &mut self.finally_contexts[index];
        let jump_index = ctx.next_jump_index;
        ctx.next_jump_index += 1;
        ctx.registered_jumps.push(FinallyJump {
            index: jump_index,
            target,
        });
        let completion_type = ctx.completion_type.clone();
        let finally_body = ctx.finally_body;
        let index_const = self.add_constant_i32(jump_index);
        self.emit_mov(&completion_type, &index_const);
        self.emit(Instruction::Jump {
            target: finally_body,
        });
    }

    /// For break/continue through nested finally: create a trampoline block.
    fn emit_trampoline_through_finally(&mut self) {
        let trampoline_block = self.make_block();
        self.register_jump_in_finally_context(trampoline_block);
        self.switch_to_basic_block(trampoline_block);
        // Pop to the parent FinallyContext (simulating the inner finally completing).
        let index = self
            .current_finally_context
            .expect("no active finally context");
        self.current_finally_context = self.finally_contexts[index].parent_index;
    }

    /// Generate a break, walking boundaries and handling FinallyContext.
    pub fn generate_break(&mut self, label: Option<&[u16]>) {
        if let Some(label) = label {
            self.generate_labelled_jump(true, label);
        } else {
            self.generate_scoped_jump(true);
        }
    }

    /// Generate a continue, walking boundaries and handling FinallyContext.
    pub fn generate_continue(&mut self, label: Option<&[u16]>) {
        if let Some(label) = label {
            self.generate_labelled_jump(false, label);
        } else {
            self.generate_scoped_jump(false);
        }
    }

    /// Walk boundaries for unlabelled break/continue.
    fn generate_scoped_jump(&mut self, is_break: bool) {
        let saved_ctx = self.current_finally_context;
        let env_stack_len = self.lexical_environment_register_stack.len();
        let mut env_offset = env_stack_len;

        let mut i = self.boundaries.len();
        while i > 0 {
            i -= 1;
            let boundary = self.boundaries[i];
            match boundary {
                BlockBoundaryType::Break if is_break => {
                    let target_scope = self
                        .breakable_scopes
                        .last()
                        .expect("no active breakable scope");
                    let target = target_scope.bytecode_target;
                    let completion = target_scope.completion_register.clone();
                    if let (Some(cur), Some(tgt)) =
                        (self.current_completion_register.clone(), completion)
                        && cur != tgt
                    {
                        self.emit_mov(&tgt, &cur);
                    }
                    self.emit(Instruction::Jump { target });
                    self.current_finally_context = saved_ctx;
                    return;
                }
                BlockBoundaryType::Continue if !is_break => {
                    let target_scope = self
                        .continuable_scopes
                        .last()
                        .expect("no active continuable scope");
                    let target = target_scope.bytecode_target;
                    let completion = target_scope.completion_register.clone();
                    if let (Some(cur), Some(tgt)) =
                        (self.current_completion_register.clone(), completion)
                        && cur != tgt
                    {
                        self.emit_mov(&tgt, &cur);
                    }
                    self.emit(Instruction::Jump { target });
                    self.current_finally_context = saved_ctx;
                    return;
                }
                BlockBoundaryType::LeaveLexicalEnvironment => {
                    env_offset -= 1;
                    let env = self.lexical_environment_register_stack[env_offset - 1].clone();
                    self.emit(Instruction::SetLexicalEnvironment {
                        environment: env.operand(),
                    });
                }
                BlockBoundaryType::ReturnToFinally => {
                    if !self.has_outer_finally_before_target(is_break, i + 1) {
                        let target_scope = if is_break {
                            self.breakable_scopes
                                .last()
                                .expect("no active breakable scope")
                        } else {
                            self.continuable_scopes
                                .last()
                                .expect("no active continuable scope")
                        };
                        let target = target_scope.bytecode_target;
                        let completion = target_scope.completion_register.clone();
                        if let (Some(cur), Some(tgt)) =
                            (self.current_completion_register.clone(), completion)
                            && cur != tgt
                        {
                            self.emit_mov(&tgt, &cur);
                        }
                        self.register_jump_in_finally_context(target);
                        self.current_finally_context = saved_ctx;
                        return;
                    }
                    self.emit_trampoline_through_finally();
                }
                _ => {}
            }
        }
        self.current_finally_context = saved_ctx;
    }

    /// Walk boundaries for labelled break/continue.
    fn generate_labelled_jump(&mut self, is_break: bool, label: &[u16]) {
        let saved_ctx = self.current_finally_context;
        let env_stack_len = self.lexical_environment_register_stack.len();
        let mut env_offset = env_stack_len;

        let jumpable_scopes: Vec<(Label, Vec<Utf16String>, Option<ScopedOperand>)> = if is_break {
            self.breakable_scopes
                .iter()
                .rev()
                .map(|s| {
                    (
                        s.bytecode_target,
                        s.language_label_set.clone(),
                        s.completion_register.clone(),
                    )
                })
                .collect()
        } else {
            self.continuable_scopes
                .iter()
                .rev()
                .map(|s| {
                    (
                        s.bytecode_target,
                        s.language_label_set.clone(),
                        s.completion_register.clone(),
                    )
                })
                .collect()
        };

        let mut current_boundary = self.boundaries.len();

        for (target, label_set, completion) in &jumpable_scopes {
            while current_boundary > 0 {
                current_boundary -= 1;
                let boundary = self.boundaries[current_boundary];
                match boundary {
                    BlockBoundaryType::LeaveLexicalEnvironment => {
                        env_offset -= 1;
                        let env = self.lexical_environment_register_stack[env_offset - 1].clone();
                        self.emit(Instruction::SetLexicalEnvironment {
                            environment: env.operand(),
                        });
                    }
                    BlockBoundaryType::ReturnToFinally => {
                        if !self.has_outer_finally_before_target(is_break, current_boundary + 1)
                            && label_set.iter().any(|l| l == label)
                        {
                            if let (Some(cur), Some(tgt)) =
                                (self.current_completion_register.clone(), completion.clone())
                                && cur != tgt
                            {
                                self.emit_mov(&tgt, &cur);
                            }
                            self.register_jump_in_finally_context(*target);
                            self.current_finally_context = saved_ctx;
                            return;
                        }
                        self.emit_trampoline_through_finally();
                    }
                    b if (is_break && b == BlockBoundaryType::Break)
                        || (!is_break && b == BlockBoundaryType::Continue) =>
                    {
                        break;
                    }
                    _ => {}
                }
            }

            if label_set.iter().any(|l| l == label) {
                if let (Some(cur), Some(tgt)) =
                    (self.current_completion_register.clone(), completion.clone())
                    && cur != tgt
                {
                    self.emit_mov(&tgt, &cur);
                }
                self.emit(Instruction::Jump { target: *target });
                self.current_finally_context = saved_ctx;
                return;
            }
        }
        self.current_finally_context = saved_ctx;
    }

    /// Walk the boundary stack and emit SetLexicalEnvironment instructions
    /// for each LeaveLexicalEnvironment boundary, restoring the parent
    /// environment. Stops at ReturnToFinally since the finally handler
    /// takes care of further unwinding.
    pub fn perform_needed_unwinds(&mut self) {
        let mut env_stack_offset = self.lexical_environment_register_stack.len();
        for i in (0..self.boundaries.len()).rev() {
            match self.boundaries[i] {
                BlockBoundaryType::LeaveLexicalEnvironment => {
                    env_stack_offset -= 1;
                    let parent_env =
                        self.lexical_environment_register_stack[env_stack_offset - 1].clone();
                    self.emit(Instruction::SetLexicalEnvironment {
                        environment: parent_env.operand(),
                    });
                }
                BlockBoundaryType::ReturnToFinally => {
                    return;
                }
                _ => {}
            }
        }
    }

    /// Generate a return, routing through FinallyContext if needed.
    pub fn generate_return(&mut self, value: &ScopedOperand) {
        self.perform_needed_unwinds();
        if let Some(index) = self.current_finally_context {
            let ctx = &self.finally_contexts[index];
            let completion_value = ctx.completion_value.clone();
            let completion_type = ctx.completion_type.clone();
            let finally_body = ctx.finally_body;
            self.emit_mov(&completion_value, value);
            let ret_const = self.add_constant_i32(FinallyContext::RETURN);
            self.emit_mov(&completion_type, &ret_const);
            self.emit(Instruction::Jump {
                target: finally_body,
            });
        } else if self.is_in_generator_or_async_function() {
            self.emit(Instruction::Yield {
                continuation_label: None,
                value: value.operand(),
            });
        } else {
            self.emit(Instruction::Return {
                value: value.operand(),
            });
        }
    }

    pub fn add_constant_i32(&mut self, val: i32) -> ScopedOperand {
        self.add_constant_number(val as f64)
    }

    // --- Local variable initialization tracking ---

    pub fn is_local_initialized(&self, index: u32) -> bool {
        self.initialized_locals
            .get(index as usize)
            .copied()
            .unwrap_or(false)
    }

    pub fn is_local_lexically_declared(&self, index: u32) -> bool {
        self.local_variables
            .get(index as usize)
            .is_some_and(|v| v.is_lexically_declared)
    }

    pub fn mark_local_initialized(&mut self, index: u32) {
        let index = index as usize;
        if index >= self.initialized_locals.len() {
            self.initialized_locals.resize(index + 1, false);
        }
        self.initialized_locals[index] = true;
    }

    pub fn is_argument_initialized(&self, index: u32) -> bool {
        self.initialized_arguments
            .get(index as usize)
            .copied()
            .unwrap_or(false)
    }

    pub fn mark_argument_initialized(&mut self, index: u32) {
        let index = index as usize;
        if index >= self.initialized_arguments.len() {
            self.initialized_arguments.resize(index + 1, false);
        }
        self.initialized_arguments[index] = true;
    }

    // --- Compile/assemble/link pipeline ---

    /// Compile all basic blocks into a flat bytecode buffer.
    ///
    /// This performs:
    /// 1. Operand rewriting (offset indices for the runtime layout)
    /// 2. Compute block byte offsets using encoded_size()
    /// 3. Patch labels in typed instructions (block index → byte offset)
    /// 4. Encode to bytes and build source map + exception handlers
    pub fn assemble(&mut self) -> AssembledBytecode {
        // If any block is unterminated, ensure the undefined constant exists
        // for the assembly-time End(undefined) fallthrough. This must happen
        // before computing number_of_constants so operand rewriting accounts
        // for it (matching C++ compile()).
        let has_unterminated = self.basic_blocks.iter().any(|b| !b.terminated);
        let undefined_constant_operand = if has_unterminated {
            Some(self.add_constant_undefined().operand())
        } else {
            None
        };

        let number_of_registers = self.next_register;
        let number_of_locals = u32_from_usize(self.local_variables.len());
        let number_of_constants = u32_from_usize(self.constants.len());

        // Phase 1: Operand rewriting
        for block in &mut self.basic_blocks {
            for (instruction, _) in &mut block.instructions {
                instruction.visit_operands(&mut |op: &mut Operand| {
                    match op.operand_type() {
                        OperandType::Register => {} // stays as-is
                        OperandType::Local => op.offset_index_by(number_of_registers),
                        OperandType::Constant => {
                            op.offset_index_by(number_of_registers + number_of_locals)
                        }
                        OperandType::Argument => op.offset_index_by(
                            number_of_registers + number_of_locals + number_of_constants,
                        ),
                    }
                });
            }
        }

        // Phase 2: Compute block byte offsets, applying assembly-time optimizations.
        // These match the C++ Generator.cpp:compile() optimizations:
        //   - Skip Jump-to-next-block
        //   - Replace Jump-to-Return/End-only-block with inline Return/End
        //   - Replace JumpIf-where-one-target-is-next-block with JumpTrue/JumpFalse
        let num_blocks = self.basic_blocks.len();
        let mut block_offsets: Vec<usize> = Vec::with_capacity(num_blocks);
        // Per-instruction skip flags: skip_flags[block_index][instruction_index] = replacement action
        #[derive(Clone, Copy)]
        enum InstAction {
            Emit,
            Skip,
            JumpToReturn(Operand),
            JumpToEnd(Operand),
            EmitJumpTrue { condition: Operand, target: Label },
            EmitJumpFalse { condition: Operand, target: Label },
        }
        let mut actions: Vec<Vec<InstAction>> = Vec::with_capacity(num_blocks);
        let mut offset: usize = 0;

        for block_index in 0..num_blocks {
            block_offsets.push(offset);
            let block = &self.basic_blocks[block_index];
            let mut block_actions = Vec::with_capacity(block.instructions.len());
            for (instruction, _) in block.instructions.iter() {
                match instruction {
                    Instruction::Jump { target } => {
                        let target_block = target.0 as usize;
                        // OPTIMIZATION: Don't emit jumps that just jump to the next block.
                        if target_block == block_index + 1 {
                            // If this block would become empty, we handle it by
                            // not advancing offset (matching C++ behavior of removing
                            // the block_start_offset entry and reusing it).
                            block_actions.push(InstAction::Skip);
                            continue;
                        }
                        // OPTIMIZATION: For jumps to a return-or-end-only block, inline
                        // the Return/End instead of emitting the Jump.
                        let target_blk = &self.basic_blocks[target_block];
                        if target_blk.terminated && target_blk.instructions.len() == 1 {
                            match &target_blk.instructions[0].0 {
                                Instruction::Return { value } => {
                                    let replacement = Instruction::Return { value: *value };
                                    block_actions.push(InstAction::JumpToReturn(*value));
                                    offset += replacement.encoded_size();
                                    continue;
                                }
                                Instruction::End { value } => {
                                    let replacement = Instruction::End { value: *value };
                                    block_actions.push(InstAction::JumpToEnd(*value));
                                    offset += replacement.encoded_size();
                                    continue;
                                }
                                _ => {}
                            }
                        }
                        block_actions.push(InstAction::Emit);
                        offset += instruction.encoded_size();
                    }
                    Instruction::JumpIf {
                        condition,
                        true_target,
                        false_target,
                    } => {
                        let true_block = true_target.0 as usize;
                        let false_block = false_target.0 as usize;
                        // OPTIMIZATION: Replace JumpIf where one target is next block
                        // with JumpTrue or JumpFalse.
                        if true_block == block_index + 1 {
                            block_actions.push(InstAction::EmitJumpFalse {
                                condition: *condition,
                                target: *false_target,
                            });
                            let replacement = Instruction::JumpFalse {
                                condition: *condition,
                                target: *false_target,
                            };
                            offset += replacement.encoded_size();
                            continue;
                        }
                        if false_block == block_index + 1 {
                            block_actions.push(InstAction::EmitJumpTrue {
                                condition: *condition,
                                target: *true_target,
                            });
                            let replacement = Instruction::JumpTrue {
                                condition: *condition,
                                target: *true_target,
                            };
                            offset += replacement.encoded_size();
                            continue;
                        }
                        block_actions.push(InstAction::Emit);
                        offset += instruction.encoded_size();
                    }
                    _ => {
                        block_actions.push(InstAction::Emit);
                        offset += instruction.encoded_size();
                    }
                }
            }
            // Unterminated blocks get an implicit End(undefined) appended.
            if !block.terminated {
                let dummy_end = Instruction::End {
                    value: Operand::constant(0),
                };
                offset += dummy_end.encoded_size();
            }
            actions.push(block_actions);
        }

        // Check if any block became empty due to skip, and adjust its offset
        // to match the next block's offset (C++ pops basic_block_start_offsets.last()).
        // We handle this by keeping block_offsets as-is since labels referencing
        // an empty block will resolve to the same byte offset as the next block.

        // Phase 3: Patch labels (block index → byte offset)
        for block in &mut self.basic_blocks {
            for (instruction, _) in &mut block.instructions {
                instruction.visit_labels(&mut |label: &mut Label| {
                    let block_index = label.0 as usize;
                    label.0 = u32_from_usize(block_offsets[block_index]);
                });
            }
        }

        // Phase 4: Encode to bytes with optimizations applied
        let mut bytecode: Vec<u8> = Vec::with_capacity(offset);
        let mut source_map: Vec<SourceMapEntry> = Vec::new();
        let mut exception_handlers: Vec<ExceptionHandler> = Vec::new();
        // Track which blocks actually produced instructions (matching C++ behavior
        // of popping basic_block_start_offsets when a block becomes empty).
        let mut basic_block_start_offsets: Vec<usize> = Vec::with_capacity(num_blocks);

        for (block_index, block) in self.basic_blocks.iter().enumerate() {
            basic_block_start_offsets.push(bytecode.len());
            let block_start = bytecode.len();
            let handler = block.handler;
            let block_actions = &actions[block_index];

            for (instruction_index, (instruction, sm)) in block.instructions.iter().enumerate() {
                let action = block_actions[instruction_index];
                match action {
                    InstAction::Skip => {
                        // If this skip makes the block empty, remove it from
                        // basic_block_start_offsets (matching C++ take_last()).
                        if basic_block_start_offsets.last() == Some(&bytecode.len()) {
                            basic_block_start_offsets.pop();
                        }
                    }
                    InstAction::Emit => {
                        let instruction_offset = bytecode.len();
                        source_map.push(SourceMapEntry {
                            bytecode_offset: u32_from_usize(instruction_offset),
                            source_start: sm.source_start,
                            source_end: sm.source_end,
                        });
                        instruction.encode(self.strict, &mut bytecode);
                    }
                    InstAction::JumpToReturn(value) => {
                        let instruction_offset = bytecode.len();
                        source_map.push(SourceMapEntry {
                            bytecode_offset: u32_from_usize(instruction_offset),
                            source_start: sm.source_start,
                            source_end: sm.source_end,
                        });
                        let replacement = Instruction::Return { value };
                        replacement.encode(self.strict, &mut bytecode);
                    }
                    InstAction::JumpToEnd(value) => {
                        let instruction_offset = bytecode.len();
                        source_map.push(SourceMapEntry {
                            bytecode_offset: u32_from_usize(instruction_offset),
                            source_start: sm.source_start,
                            source_end: sm.source_end,
                        });
                        let replacement = Instruction::End { value };
                        replacement.encode(self.strict, &mut bytecode);
                    }
                    InstAction::EmitJumpFalse {
                        condition,
                        mut target,
                    } => {
                        // Patch label for the target
                        let target_block = target.0 as usize;
                        target.0 = u32_from_usize(block_offsets[target_block]);
                        let instruction_offset = bytecode.len();
                        source_map.push(SourceMapEntry {
                            bytecode_offset: u32_from_usize(instruction_offset),
                            source_start: sm.source_start,
                            source_end: sm.source_end,
                        });
                        let replacement = Instruction::JumpFalse { condition, target };
                        replacement.encode(self.strict, &mut bytecode);
                    }
                    InstAction::EmitJumpTrue {
                        condition,
                        mut target,
                    } => {
                        let target_block = target.0 as usize;
                        target.0 = u32_from_usize(block_offsets[target_block]);
                        let instruction_offset = bytecode.len();
                        source_map.push(SourceMapEntry {
                            bytecode_offset: u32_from_usize(instruction_offset),
                            source_start: sm.source_start,
                            source_end: sm.source_end,
                        });
                        let replacement = Instruction::JumpTrue { condition, target };
                        replacement.encode(self.strict, &mut bytecode);
                    }
                }
            }

            // Unterminated blocks get an implicit End(undefined).
            if !block.terminated {
                let mut undef_rewritten =
                    undefined_constant_operand.expect("undefined constant must exist");
                undef_rewritten.offset_index_by(number_of_registers + number_of_locals);
                let end_instruction = Instruction::End {
                    value: undef_rewritten,
                };
                let instruction_offset = bytecode.len();
                source_map.push(SourceMapEntry {
                    bytecode_offset: u32_from_usize(instruction_offset),
                    source_start: 0,
                    source_end: 0,
                });
                end_instruction.encode(self.strict, &mut bytecode);
            }

            // Close exception handler range
            if let Some(handler_label) = handler {
                exception_handlers.push(ExceptionHandler {
                    start_offset: u32_from_usize(block_start),
                    end_offset: u32_from_usize(bytecode.len()),
                    handler_offset: u32_from_usize(
                        block_offsets[handler_label.basic_block_index()],
                    ),
                });
            }
        }

        // Merge adjacent exception handlers with the same handler offset
        // (matching C++ Generator.cpp behavior).
        let mut merged_handlers: Vec<ExceptionHandler> = Vec::new();
        for handler in &exception_handlers {
            if let Some(last) = merged_handlers.last_mut()
                && last.end_offset == handler.start_offset
                && last.handler_offset == handler.handler_offset
            {
                last.end_offset = handler.end_offset;
                continue;
            }
            merged_handlers.push(handler.clone());
        }
        merged_handlers.sort_by_key(|h| h.start_offset);

        AssembledBytecode {
            bytecode,
            source_map,
            exception_handlers: merged_handlers,
            basic_block_start_offsets,
            number_of_registers,
        }
    }
}

/// Result of assembling bytecode from basic blocks.
pub struct AssembledBytecode {
    pub bytecode: Vec<u8>,
    pub source_map: Vec<SourceMapEntry>,
    pub exception_handlers: Vec<ExceptionHandler>,
    pub basic_block_start_offsets: Vec<usize>,
    pub number_of_registers: u32,
}

/// Exception handler range (with byte offsets, post-linking).
#[derive(Debug, Clone)]
pub struct ExceptionHandler {
    pub start_offset: u32,
    pub end_offset: u32,
    pub handler_offset: u32,
}

/// A typed constant value stored in the constant pool.
///
/// The actual NaN-boxed encoding happens at the FFI boundary when
/// creating the `Bytecode::Executable`.
#[derive(Debug, Clone)]
pub enum ConstantValue {
    Number(f64),
    Boolean(bool),
    Null,
    Undefined,
    Empty,
    String(Utf16String),
    BigInt(String),
    /// An opaque pre-encoded JS::Value (e.g. well-known symbol, intrinsic function).
    RawValue(u64),
}

/// Convert a constant value to a boolean, matching JS `ToBoolean`.
/// Returns `None` for opaque `RawValue` constants whose truthiness
/// cannot be determined at compile time.
pub fn constant_to_boolean(value: &ConstantValue) -> Option<bool> {
    match value {
        ConstantValue::Boolean(b) => Some(*b),
        ConstantValue::Null | ConstantValue::Undefined | ConstantValue::Empty => Some(false),
        ConstantValue::Number(n) => Some(*n != 0.0 && !n.is_nan()),
        ConstantValue::String(s) => Some(!s.is_empty()),
        ConstantValue::BigInt(s) => parse_bigint(s).map(|bi| bi != num_bigint::BigInt::ZERO),
        ConstantValue::RawValue(_) => None,
    }
}

/// Parse a BigInt string to an arbitrary-precision BigInt.
/// Handles decimal, 0b binary, 0o octal, and 0x hex prefixes.
pub fn parse_bigint(s: &str) -> Option<num_bigint::BigInt> {
    use num_bigint::BigInt;
    if s.len() > 2 {
        let (prefix, rest) = s.split_at(2);
        match prefix {
            "0b" | "0B" => return BigInt::parse_bytes(rest.as_bytes(), 2),
            "0o" | "0O" => return BigInt::parse_bytes(rest.as_bytes(), 8),
            "0x" | "0X" => return BigInt::parse_bytes(rest.as_bytes(), 16),
            _ => {}
        }
    }
    s.parse::<BigInt>().ok()
}

/// Use `preferred_dst` if available, otherwise allocate a fresh register.
pub fn choose_dst(
    generator: &mut Generator,
    preferred_dst: Option<&ScopedOperand>,
) -> ScopedOperand {
    match preferred_dst {
        Some(dst) => dst.clone(),
        None => generator.allocate_register(),
    }
}
