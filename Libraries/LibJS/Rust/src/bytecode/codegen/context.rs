/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The state of a compile. [`CodegenContext`] holds everything with memory of its own, which every compile of a thread
//! reuses, and [`Generator`] the rest of what one compile needs.

use std::cell::Cell;
use std::mem::ManuallyDrop;
use std::sync::Arc;

use ak::Utf16FlyString;
use ak::Utf16String;

use crate::ast::FunctionKind;
use crate::ast::Name;
use crate::ast::SyntaxTree;
use crate::bytecode::constant::AbstractOperationKind;
use crate::bytecode::constant::WellKnownSymbolKind;
use crate::bytecode::constant_fold::ConstantPool;
use crate::bytecode::executable_data::ConstantValue;
use crate::bytecode::executable_data::ExecutableCacheCounts;
use crate::bytecode::executable_data::PendingClassBlueprint;
use crate::bytecode::instruction::Instruction;
use crate::bytecode::instruction::OpCode;
use crate::bytecode::instruction::typed as op;
use crate::bytecode::instruction::typed::TypedInstruction;
use crate::bytecode::operand::*;
use crate::bytecode::source_map::SourceOffset;
use crate::parser::EnclosingCode;
use crate::utf16::Utf16View;

use super::constants::ConstantTable;
use super::environment::BindingLocation;
use super::environment::EnclosingEnvironmentScope;
use super::environment::EnvironmentCoordinateScope;
use super::names::NameClass;
use super::names::NameTables;
use super::registers::ScopedOperand;

/// The most entries that a table keeps its memory for when the next compile takes over.
pub(super) const MAXIMUM_SPARE_CAPACITY: usize = 1024;

/// Feedback slot indices are encoded as u16. Sites beyond the last index share
/// it, which only merges their feedback.
const MAX_FEEDBACK_SLOT_COUNT: u32 = u16::MAX as u32 + 1;

const ENVIRONMENT_MODE_LEXICAL: u32 = libjs_abi::EnvironmentMode::Lexical as u32;

/// Everything of a compile that has memory of its own, which stays around for the next compile on the thread. It resets
/// by clearing and by moving on to a new generation, never by allocating anew.
#[derive(Default)]
pub struct CodegenContext {
    pub(super) stream: super::stream::Stream,
    pub(super) scope: super::scope::CodeScope,
    pub(super) constants: ConstantTable,
    pub(super) names: NameTables,

    pub(super) lexical_environment_register_stack: Vec<ScopedOperand>,
    pub(super) environment_coordinate_scope_stack: Vec<EnvironmentCoordinateScope>,
    pub(super) initialized_locals: Vec<bool>,
    pub(super) initialized_arguments: Vec<bool>,
    pub(super) annexb_function_names: Vec<Name>,

    /// The memo of what `binding_location_for_identifier()` found, by identifier, while `environment_generation` stays.
    pub(super) binding_locations: Vec<(u32, Option<BindingLocation>)>,
    /// What moves on whenever an environment or a binding of one comes or goes.
    pub(super) environment_generation: u32,
    /// The values of the NewPrimitiveArray being put together.
    pub(super) primitive_values: Vec<u64>,
}

impl CodegenContext {
    /// Drops what the last compile left, so that the next one starts from nothing.
    fn clear(&mut self) {
        self.lexical_environment_register_stack.clear();
        self.environment_coordinate_scope_stack.clear();
        self.initialized_locals.clear();
        self.initialized_arguments.clear();
        self.annexb_function_names.clear();
        self.environment_generation = self.environment_generation.wrapping_add(1);
        self.stream.clear();
        self.scope.clear();
        self.constants.clear();
        self.names.clear();
    }
}

thread_local! {
    /// The context of this thread. A compile nested in another one finds nothing here and starts from nothing.
    static SPARE_CONTEXT: Cell<Option<Box<CodegenContext>>> = const { Cell::new(None) };
}

/// The state of generating code for one function.
pub struct Generator<'t> {
    pub(super) tree: &'t SyntaxTree,
    pub(super) context: ManuallyDrop<Box<CodegenContext>>,

    pub(super) current_source_offset: SourceOffset,
    pub(super) strict: bool,
    /// Whether the code can change the running lexical environment. Without that, nothing saves or restores it.
    pub(super) creates_environment: bool,
    pub(super) stack_limit: usize,

    pub(super) this_value_needs_environment_resolution: bool,
    pub(super) enclosing_function_kind: FunctionKind,
    pub(super) builtin_abstract_operations_enabled: bool,
    pub(super) eager_compile_direct_iifes: bool,
    pub(super) contains_direct_call_to_eval_in_non_strict_mode: bool,
    pub(super) enclosing_environment_scope: Option<Arc<EnclosingEnvironmentScope>>,
    pub(super) variable_environment_coordinate_scope_index: Option<usize>,
    pub(super) enclosing_code: Arc<EnclosingCode>,

    pub(super) class_blueprints: Vec<PendingClassBlueprint>,
    pub(super) length_identifier: Option<PropertyKeyTableIndex>,

    pub(super) next_property_lookup_cache: u32,
    pub(super) next_global_variable_cache: u32,
    pub(super) next_environment_coordinate_cache: u32,
    pub(super) next_template_object_cache: u32,
    pub(super) next_object_shape_cache: u32,
    pub(super) next_object_property_iterator_cache: u32,
    pub(super) next_environment_shape_cache: u32,
    pub(super) next_arith_feedback: u32,
    pub(super) next_value_feedback: u32,
    pub(super) next_call_feedback: u32,
    pub(super) next_keyed_feedback: u32,
}

macro_rules! next_cache_method {
    ($method:ident, $field:ident) => {
        #[inline(always)]
        pub(super) fn $method(&mut self) -> u32 {
            let index = self.$field;
            self.$field += 1;
            index
        }
    };
}

macro_rules! next_feedback_slot_method {
    ($method:ident, $field:ident) => {
        #[inline(always)]
        pub(super) fn $method(&mut self) -> u16 {
            let index = u16::try_from(self.$field).unwrap_or(u16::MAX);
            self.$field = (self.$field + 1).min(MAX_FEEDBACK_SLOT_COUNT);
            index
        }
    };
}

impl Drop for Generator<'_> {
    fn drop(&mut self) {
        // SAFETY: This is the last that anything sees of the context.
        let mut context = unsafe { ManuallyDrop::take(&mut self.context) };
        context.clear();
        SPARE_CONTEXT.set(Some(context));
    }
}

impl<'t> Generator<'t> {
    pub(super) fn new(tree: &'t SyntaxTree, enclosing_code: Arc<EnclosingCode>, stack_limit: usize) -> Self {
        let mut context = SPARE_CONTEXT.take().unwrap_or_default();
        context.names.begin_compile(&tree.script);
        Self {
            tree,
            context: ManuallyDrop::new(context),
            current_source_offset: SourceOffset::NONE,
            strict: false,
            creates_environment: true,
            stack_limit,
            this_value_needs_environment_resolution: true,
            enclosing_function_kind: FunctionKind::Normal,
            builtin_abstract_operations_enabled: false,
            eager_compile_direct_iifes: false,
            contains_direct_call_to_eval_in_non_strict_mode: false,
            enclosing_environment_scope: None,
            variable_environment_coordinate_scope_index: None,
            enclosing_code,
            class_blueprints: Vec::new(),
            length_identifier: None,
            next_property_lookup_cache: 0,
            next_global_variable_cache: 0,
            next_environment_coordinate_cache: 0,
            next_template_object_cache: 0,
            next_object_shape_cache: 0,
            next_object_property_iterator_cache: 0,
            next_environment_shape_cache: 0,
            next_arith_feedback: 0,
            next_value_feedback: 0,
            next_call_feedback: 0,
            next_keyed_feedback: 0,
        }
    }

    // --- Function kind queries ---

    pub(super) fn is_in_generator_function(&self) -> bool {
        matches!(
            self.enclosing_function_kind,
            FunctionKind::Generator | FunctionKind::AsyncGenerator
        )
    }

    pub(super) fn is_in_async_generator_function(&self) -> bool {
        self.enclosing_function_kind == FunctionKind::AsyncGenerator
    }

    pub(super) fn is_in_generator_or_async_function(&self) -> bool {
        self.enclosing_function_kind != FunctionKind::Normal
    }

    // --- Register management ---

    /// Allocate a new register (or reuse a freed one).
    #[inline(always)]
    pub(super) fn allocate_register(&mut self) -> ScopedOperand {
        ScopedOperand::plain(self.held_register())
    }

    /// An operand that is not an allocated register, which stays valid for as long as the code runs.
    #[inline(always)]
    pub(super) fn scoped_operand(&mut self, operand: Operand) -> ScopedOperand {
        ScopedOperand::plain(operand)
    }

    // --- Constant pool ---

    #[inline(always)]
    pub(super) fn add_constant_number(&mut self, value: f64) -> ScopedOperand {
        ScopedOperand::plain(self.context.constants.add_number(value))
    }

    #[inline(always)]
    pub(super) fn add_constant_i32(&mut self, value: i32) -> ScopedOperand {
        self.add_constant_number(f64::from(value))
    }

    #[inline(always)]
    pub(super) fn add_constant_boolean(&mut self, value: bool) -> ScopedOperand {
        ScopedOperand::plain(self.context.constants.add_boolean(value))
    }

    #[inline(always)]
    pub(super) fn add_constant_null(&mut self) -> ScopedOperand {
        ScopedOperand::plain(self.context.constants.add_null())
    }

    #[inline(always)]
    pub(super) fn add_constant_undefined(&mut self) -> ScopedOperand {
        ScopedOperand::plain(self.context.constants.add_undefined())
    }

    #[inline(always)]
    pub(super) fn add_constant_string(&mut self, value: Utf16String) -> ScopedOperand {
        ScopedOperand::plain(self.context.constants.add_string(value))
    }

    /// A constant of the string `text`.
    #[inline(always)]
    pub(super) fn add_constant_string_view(&mut self, text: Utf16View<'_>) -> Operand {
        let context = &mut *self.context;
        context.constants.add_string_view(text, &mut context.names)
    }

    pub(super) fn add_constant_bigint(&mut self, value: String) -> ScopedOperand {
        ScopedOperand::plain(self.context.constants.add_bigint(value))
    }

    pub(super) fn add_constant_well_known_symbol(&mut self, symbol: WellKnownSymbolKind) -> ScopedOperand {
        ScopedOperand::plain(self.context.constants.add_well_known_symbol(symbol))
    }

    pub(super) fn add_constant_abstract_operation(&mut self, operation: AbstractOperationKind) -> ScopedOperand {
        ScopedOperand::plain(self.context.constants.add_abstract_operation(operation))
    }

    /// Get the constant value for a constant operand.
    #[inline(always)]
    pub(super) fn get_constant(&self, operand: &ScopedOperand) -> Option<&ConstantValue> {
        self.context.constants.get(operand.operand())
    }

    // --- Table interning ---

    #[inline(always)]
    pub(super) fn intern_identifier(&mut self, name: Name) -> IdentifierTableIndex {
        self.context.names.intern_identifier(self.tree, name)
    }

    pub(super) fn intern_identifier_text(&mut self, text: Utf16View<'_>) -> IdentifierTableIndex {
        self.context.names.intern_identifier_text(self.tree, text)
    }

    #[inline(always)]
    pub(super) fn intern_property_key(&mut self, name: Name) -> PropertyKeyTableIndex {
        self.context.names.intern_property_key(self.tree, name)
    }

    pub(super) fn intern_property_key_text(&mut self, text: Utf16View<'_>) -> PropertyKeyTableIndex {
        self.context.names.intern_property_key_text(self.tree, text)
    }

    pub(super) fn intern_string_text(&mut self, text: Utf16View<'_>) -> StringTableIndex {
        self.context.names.intern_string_text(self.tree, text)
    }

    #[inline(always)]
    pub(super) fn name_class(&mut self, name: Name) -> NameClass {
        self.context.names.class_of(self.tree, name)
    }

    /// Whether generating code for something any deeper in the tree makes no sense: the code may overflow the stack, or
    /// is a lost cause already.
    #[inline(always)]
    pub(super) fn should_stop_generating(&mut self) -> bool {
        let marker = 0u8;
        (&raw const marker as usize) < self.stack_limit
    }

    /// If `operand` is a constant string that is not an array index, intern it as a property key and return the index.
    pub(super) fn try_constant_string_to_property_key(
        &mut self,
        operand: &ScopedOperand,
    ) -> Option<PropertyKeyTableIndex> {
        let context = &mut *self.context;
        let ConstantValue::String(string) = context.constants.get(operand.operand())? else {
            return None;
        };
        if crate::bytecode::constant_fold::is_array_index(&string.to_utf16()) {
            return None;
        }
        Some(
            context
                .names
                .intern_property_key_text(self.tree, Utf16View::of_string(string)),
        )
    }

    // --- Instruction emission ---

    /// Emits an instruction.
    #[inline(always)]
    pub(super) fn emit<I: TypedInstruction>(&mut self, instruction: I) {
        if changes_environment_state(I::OPCODE) {
            self.note_environment_change(&instruction.to_instruction());
        }
        self.put(instruction);
    }

    /// Keep coordinate scopes in lockstep with the actual declarative environment shape. Most bindings are created
    /// explicitly, while CreateArguments can implicitly create an `arguments` binding.
    #[cold]
    fn note_environment_change(&mut self, instruction: &Instruction) {
        match instruction {
            Instruction::CreateVariable {
                identifier,
                mode,
                is_global,
                ..
            } => {
                if !is_global {
                    let name = self.context.names.identifier_fly_string(self.tree, *identifier);
                    if *mode == ENVIRONMENT_MODE_LEXICAL {
                        self.record_environment_binding(name);
                    } else {
                        self.record_variable_environment_binding(name);
                    }
                }
            }
            Instruction::CreateMutableBinding { identifier, .. }
            | Instruction::CreateImmutableBinding { identifier, .. } => {
                let name = self.context.names.identifier_fly_string(self.tree, *identifier);
                self.record_environment_binding(name);
            }
            Instruction::CreateArguments { dst: None, .. } => {
                self.record_environment_binding(Utf16FlyString::from_utf8("arguments"));
            }
            Instruction::CreateArguments { .. } | Instruction::GetLexicalEnvironment { .. } => {}
            _ => assert!(
                self.creates_environment,
                "scope analysis missed an environment of this code"
            ),
        }
    }

    /// Emit a Mov instruction (optimized away if src == dst).
    #[inline(always)]
    pub(super) fn emit_mov(&mut self, dst: &ScopedOperand, src: &ScopedOperand) {
        if dst != src {
            self.emit(op::Mov {
                dst: dst.operand(),
                src: src.operand(),
            });
        }
    }

    /// Emit a conditional jump, with comparison fusion and constant folding.
    pub(super) fn emit_jump_if(&mut self, condition: &ScopedOperand, true_target: Label, false_target: Label) {
        self.emit(op::JumpIf {
            condition: condition.operand(),
            true_target,
            false_target,
        });
    }

    // --- Cache index allocation ---

    next_cache_method!(next_property_lookup_cache, next_property_lookup_cache);
    next_cache_method!(next_global_variable_cache, next_global_variable_cache);
    next_cache_method!(next_environment_coordinate_cache, next_environment_coordinate_cache);
    next_cache_method!(next_template_object_cache, next_template_object_cache);
    next_cache_method!(next_object_shape_cache, next_object_shape_cache);
    next_cache_method!(next_object_property_iterator_cache, next_object_property_iterator_cache);
    next_cache_method!(next_environment_shape_cache, next_environment_shape_cache);
    next_feedback_slot_method!(next_arith_feedback, next_arith_feedback);
    next_feedback_slot_method!(next_value_feedback, next_value_feedback);
    next_feedback_slot_method!(next_call_feedback, next_call_feedback);
    next_feedback_slot_method!(next_keyed_feedback, next_keyed_feedback);

    // --- Local variable initialization tracking ---

    #[inline(always)]
    pub(super) fn is_local_initialized(&self, index: u32) -> bool {
        self.context
            .initialized_locals
            .get(index as usize)
            .copied()
            .unwrap_or(false)
    }

    /// Whether a local starts out uninitialized. Besides what a declaration binds lexically, that goes for the names of a
    /// binding pattern of a parameter or a catch clause: initializers in the pattern can read them before they are bound.
    #[inline(always)]
    pub(super) fn local_has_temporal_dead_zone(&self, index: u32) -> bool {
        self.context.scope.is_local_lexical(index)
    }

    pub(super) fn mark_local_initialized(&mut self, index: u32) {
        let index = index as usize;
        if index >= self.context.initialized_locals.len() {
            self.context.initialized_locals.resize(index + 1, false);
        }
        self.context.initialized_locals[index] = true;
    }

    #[inline(always)]
    pub(super) fn is_argument_initialized(&self, index: u32) -> bool {
        self.context
            .initialized_arguments
            .get(index as usize)
            .copied()
            .unwrap_or(false)
    }

    pub(super) fn mark_argument_initialized(&mut self, index: u32) {
        let index = index as usize;
        if index >= self.context.initialized_arguments.len() {
            self.context.initialized_arguments.resize(index + 1, false);
        }
        self.context.initialized_arguments[index] = true;
    }

    pub(super) fn cache_counts(&self) -> ExecutableCacheCounts {
        ExecutableCacheCounts {
            property_lookup: self.next_property_lookup_cache,
            global_variable: self.next_global_variable_cache,
            environment_coordinate: self.next_environment_coordinate_cache,
            template_object: self.next_template_object_cache,
            object_shape: self.next_object_shape_cache,
            object_property_iterator: self.next_object_property_iterator_cache,
            environment_shape: self.next_environment_shape_cache,
            arith_feedback: self.next_arith_feedback,
            value_feedback: self.next_value_feedback,
            call_feedback: self.next_call_feedback,
            keyed_feedback: self.next_keyed_feedback,
        }
    }
}

impl ConstantPool for Generator<'_> {
    type Operand = ScopedOperand;

    fn get_constant(&self, operand: &ScopedOperand) -> Option<&ConstantValue> {
        Generator::get_constant(self, operand)
    }

    fn add_constant_number(&mut self, value: f64) -> ScopedOperand {
        Generator::add_constant_number(self, value)
    }

    fn add_constant_boolean(&mut self, value: bool) -> ScopedOperand {
        Generator::add_constant_boolean(self, value)
    }

    fn add_constant_string(&mut self, value: Utf16String) -> ScopedOperand {
        Generator::add_constant_string(self, value)
    }

    fn add_constant_bigint(&mut self, value: String) -> ScopedOperand {
        Generator::add_constant_bigint(self, value)
    }
}

/// Whether an instruction changes which environments there are, or which bindings they have.
#[inline(always)]
const fn changes_environment_state(opcode: OpCode) -> bool {
    matches!(
        opcode,
        OpCode::CreateVariable
            | OpCode::CreateMutableBinding
            | OpCode::CreateImmutableBinding
            | OpCode::CreateArguments
            | OpCode::CreateLexicalEnvironment
            | OpCode::CreateVariableEnvironment
            | OpCode::EnterObjectEnvironment
            | OpCode::SetLexicalEnvironment
            | OpCode::GetLexicalEnvironment
    )
}
