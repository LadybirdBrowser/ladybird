/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/SinglyLinkedList.h>
#include <LibJS/AST.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/BuiltinAbstractOperationsEnabled.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Op.h>
#include <LibJS/Bytecode/PutKind.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/Bytecode/StringTable.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibRegex/Regex.h>

namespace JS::Bytecode {

class Generator {
public:
    VM& vm() { return m_vm; }

    enum class SurroundingScopeKind {
        Global,
        Function,
        Block,
    };

    enum class MustPropagateCompletion {
        No,
        Yes,
    };

    static GC::Ref<Executable> generate_from_ast_node(VM&, ASTNode const&, FunctionKind = FunctionKind::Normal);
    static GC::Ref<Executable> generate_from_function(VM&, GC::Ref<SharedFunctionInstanceData const> shared_function_instance_data, BuiltinAbstractOperationsEnabled builtin_abstract_operations_enabled = BuiltinAbstractOperationsEnabled::No);

    void emit_function_declaration_instantiation(SharedFunctionInstanceData const& shared_function_instance_data);

    [[nodiscard]] ScopedOperand allocate_register();
    [[nodiscard]] ScopedOperand local(Identifier::Local const&);
    [[nodiscard]] ScopedOperand local(FunctionLocal const&);
    [[nodiscard]] ScopedOperand accumulator();
    [[nodiscard]] ScopedOperand this_value();

    void free_register(Register);

    void set_local_initialized(Identifier::Local const&);
    void set_local_initialized(FunctionLocal const&);
    [[nodiscard]] bool is_local_initialized(u32 local_index) const;
    [[nodiscard]] bool is_local_initialized(Identifier::Local const&) const;
    [[nodiscard]] bool is_local_lexically_declared(Identifier::Local const& local) const;

    class SourceLocationScope {
    public:
        SourceLocationScope(Generator&, ASTNode const& node);
        ~SourceLocationScope();

    private:
        Generator& m_generator;
        ASTNode const* m_previous_node { nullptr };
    };

    class UnwindContext {
    public:
        UnwindContext(Generator&, Optional<Label> handler);

        UnwindContext const* previous() const { return m_previous_context; }
        void set_handler(Label handler) { m_handler = handler; }
        Optional<Label> handler() const { return m_handler; }

        ~UnwindContext();

    private:
        Generator& m_generator;
        Optional<Label> m_handler {};
        UnwindContext const* m_previous_context { nullptr };
    };

    // Tracks a break/continue target registered with a FinallyContext.
    // The after-finally dispatch chain uses the index to route to the target.
    struct FinallyJump {
        i32 index;
        Label target;
    };

    // Codegen-time state for a try/finally scope. Each finally scope gets two
    // dedicated registers (completion_type and completion_value) that form an
    // explicit completion record. Every path into the finally body sets these
    // before jumping to finally_body:
    //
    //   - Normal exit from try/catch: completion_type = NORMAL
    //   - Exception (via handler table): completion_type = THROW, completion_value = exception
    //   - Return statement: completion_type = RETURN, completion_value = return value
    //   - Break/continue: completion_type = FIRST_JUMP_INDEX + n
    //
    // After the finally body executes, a dispatch chain of JumpStrictlyEquals
    // instructions checks completion_type and routes to the right continuation.
    struct FinallyContext {
        static constexpr i32 NORMAL = 0;
        static constexpr i32 THROW = 1;
        static constexpr i32 RETURN = 2;
        static constexpr i32 FIRST_JUMP_INDEX = 3;

        ScopedOperand completion_type;
        ScopedOperand completion_value;
        Label finally_body;
        Label exception_preamble;
        FinallyContext* parent { nullptr };
        Vector<FinallyJump> registered_jumps;
        i32 next_jump_index { FIRST_JUMP_INDEX };
        Optional<ScopedOperand> lexical_environment_at_entry;
    };

    FinallyContext* current_finally_context() { return m_current_finally_context; }
    void set_current_finally_context(FinallyContext* context) { m_current_finally_context = context; }

    template<typename OpType, typename... Args>
    requires(requires { OpType(declval<Args>()...); })
    void emit(Args&&... args)
    {
        VERIFY(!is_current_block_terminated());
        size_t slot_offset = m_current_basic_block->size();
        VERIFY(slot_offset <= NumericLimits<u32>::max());
        m_current_basic_block->set_last_instruction_start_offset(slot_offset);
        grow(sizeof(OpType));
        void* slot = m_current_basic_block->data() + slot_offset;
        new (slot) OpType(forward<Args>(args)...);
        static_cast<OpType*>(slot)->set_strict(m_strict);
        if constexpr (OpType::IsTerminator)
            m_current_basic_block->terminate({});
        m_current_basic_block->add_source_map_entry(static_cast<u32>(slot_offset), { m_current_ast_node->start_offset(), m_current_ast_node->end_offset() });
    }

    template<typename OpType, typename ExtraSlotType, typename... Args>
    requires(requires { OpType(declval<Args>()...); })
    void emit_with_extra_slots(size_t extra_slot_count, Args&&... args)
    {
        VERIFY(!is_current_block_terminated());

        size_t size_to_allocate = round_up_to_power_of_two(sizeof(OpType) + extra_slot_count * sizeof(ExtraSlotType), alignof(void*));
        size_t slot_offset = m_current_basic_block->size();
        VERIFY(slot_offset <= NumericLimits<u32>::max());
        m_current_basic_block->set_last_instruction_start_offset(slot_offset);
        grow(size_to_allocate);
        void* slot = m_current_basic_block->data() + slot_offset;
        new (slot) OpType(forward<Args>(args)...);
        static_cast<OpType*>(slot)->set_strict(m_strict);
        if constexpr (OpType::IsTerminator)
            m_current_basic_block->terminate({});
        m_current_basic_block->add_source_map_entry(static_cast<u32>(slot_offset), { m_current_ast_node->start_offset(), m_current_ast_node->end_offset() });
    }

    template<typename OpType, typename... Args>
    requires(requires { OpType(declval<Args>()...); })
    void emit_with_extra_operand_slots(size_t extra_operand_slots, Args&&... args)
    {
        emit_with_extra_slots<OpType, Operand>(extra_operand_slots, forward<Args>(args)...);
    }

    template<typename OpType, typename... Args>
    requires(requires { OpType(declval<Args>()...); })
    void emit_with_extra_value_slots(size_t extra_operand_slots, Args&&... args)
    {
        emit_with_extra_slots<OpType, Value>(extra_operand_slots, forward<Args>(args)...);
    }

    void emit_mov(ScopedOperand const& dst, ScopedOperand const& src)
    {
        // Optimize away when the source is the destination
        if (dst != src)
            emit<Op::Mov>(dst, src);
    }

    void emit_mov(Operand const& dst, Operand const& src)
    {
        emit<Op::Mov>(dst, src);
    }

    void emit_jump_if(ScopedOperand const& condition, Label true_target, Label false_target);

    void emit_todo(StringView message);

    struct ReferenceOperands {
        Optional<ScopedOperand> base {};                                 // [[Base]]
        Optional<ScopedOperand> referenced_name {};                      // [[ReferencedName]] as an operand
        Optional<PropertyKeyTableIndex> referenced_identifier {};        // [[ReferencedName]] as an identifier
        Optional<IdentifierTableIndex> referenced_private_identifier {}; // [[ReferencedName]] as a private identifier
        Optional<ScopedOperand> this_value {};                           // [[ThisValue]]
        Optional<ScopedOperand> loaded_value {};                         // Loaded value, if we've performed a load.
    };

    ReferenceOperands emit_load_from_reference(JS::ASTNode const&, Optional<ScopedOperand> preferred_dst = {});
    void emit_store_to_reference(JS::ASTNode const&, ScopedOperand value);
    void emit_store_to_reference(ReferenceOperands const&, ScopedOperand value);
    Optional<ScopedOperand> emit_delete_reference(JS::ASTNode const&);

    ReferenceOperands emit_super_reference(MemberExpression const&);

    void emit_set_variable(JS::Identifier const& identifier, ScopedOperand value, Bytecode::Op::BindingInitializationMode initialization_mode = Bytecode::Op::BindingInitializationMode::Set, Bytecode::Op::EnvironmentMode mode = Bytecode::Op::EnvironmentMode::Lexical);

    void push_home_object(ScopedOperand);
    void pop_home_object();
    void emit_new_function(ScopedOperand dst, JS::FunctionExpression const&, Optional<IdentifierTableIndex> lhs_name, bool is_method);

    u32 register_shared_function_data(GC::Ref<SharedFunctionInstanceData>);
    u32 register_class_blueprint(ClassBlueprint);

    ScopedOperand emit_named_evaluation_if_anonymous_function(Expression const&, Optional<IdentifierTableIndex> lhs_name, Optional<ScopedOperand> preferred_dst = {}, bool is_method = false);

    void ensure_lexical_environment_register_initialized();
    [[nodiscard]] ScopedOperand current_lexical_environment_register() const;
    void push_lexical_environment_register(ScopedOperand const& environment);
    void pop_lexical_environment_register();

    void begin_continuable_scope(Label continue_target, Vector<FlyString> const& language_label_set, Optional<ScopedOperand> completion_register = {});
    void end_continuable_scope();
    void begin_breakable_scope(Label breakable_target, Vector<FlyString> const& language_label_set, Optional<ScopedOperand> completion_register = {});
    void end_breakable_scope();
    void set_current_breakable_scope_completion_register(ScopedOperand completion) { m_breakable_scopes.last().completion_register = completion; }

    [[nodiscard]] Label nearest_continuable_scope() const;
    [[nodiscard]] Label nearest_breakable_scope() const;

    void switch_to_basic_block(BasicBlock& block)
    {
        m_current_basic_block = &block;
    }

    [[nodiscard]] BasicBlock& current_block() { return *m_current_basic_block; }

    BasicBlock& make_block(String name = {})
    {
        if (name.is_empty())
            name = String::number(m_next_block++);
        auto block = BasicBlock::create(m_root_basic_blocks.size(), name);
        if (auto const* context = m_current_unwind_context) {
            if (context->handler().has_value())
                block->set_handler(*m_root_basic_blocks[context->handler().value().basic_block_index()]);
        }
        m_root_basic_blocks.append(move(block));
        return *m_root_basic_blocks.last();
    }

    bool is_current_block_terminated() const
    {
        return m_current_basic_block->is_terminated();
    }

    StringTableIndex intern_string(Utf16String string)
    {
        return m_string_table->insert(move(string));
    }

    RegexTableIndex intern_regex(ParsedRegex regex)
    {
        return m_regex_table->insert(move(regex));
    }

    IdentifierTableIndex intern_identifier(Utf16FlyString string)
    {
        return m_identifier_table->insert(move(string));
    }

    PropertyKeyTableIndex intern_property_key(PropertyKey key)
    {
        return m_property_key_table->insert(move(key));
    }

    Optional<IdentifierTableIndex> intern_identifier_for_expression(Expression const& expression);

    bool is_in_generator_or_async_function() const { return m_enclosing_function_kind == FunctionKind::Async || m_enclosing_function_kind == FunctionKind::Generator || m_enclosing_function_kind == FunctionKind::AsyncGenerator; }
    bool is_in_generator_function() const { return m_enclosing_function_kind == FunctionKind::Generator || m_enclosing_function_kind == FunctionKind::AsyncGenerator; }
    bool is_in_async_function() const { return m_enclosing_function_kind == FunctionKind::Async || m_enclosing_function_kind == FunctionKind::AsyncGenerator; }
    bool is_in_async_generator_function() const { return m_enclosing_function_kind == FunctionKind::AsyncGenerator; }

    enum class BindingMode {
        Lexical,
        Var,
        Global,
    };
    struct LexicalScope {
        SurroundingScopeKind kind;
    };

    // Returns true if a lexical environment was created.
    bool emit_block_declaration_instantiation(ScopeNode const&);

    void begin_variable_scope();
    void end_variable_scope();

    enum class BlockBoundaryType {
        Break,
        Continue,
        ReturnToFinally,
        LeaveFinally,
        LeaveLexicalEnvironment,
    };
    template<typename OpType>
    void perform_needed_unwinds()
    requires(OpType::IsTerminator && !IsSame<OpType, Op::Jump>)
    {
        auto environment_stack_offset = m_lexical_environment_register_stack.size();
        for (size_t i = m_boundaries.size(); i > 0; --i) {
            auto boundary = m_boundaries[i - 1];
            using enum BlockBoundaryType;
            switch (boundary) {
            case LeaveLexicalEnvironment:
                --environment_stack_offset;
                emit<Bytecode::Op::SetLexicalEnvironment>(m_lexical_environment_register_stack[environment_stack_offset - 1]);
                break;
            case Break:
            case Continue:
                break;
            case ReturnToFinally:
                // Stop unwinding here; emit_return handles chaining to the finally body.
                return;
            case LeaveFinally:
                break;
            };
        }
    }

    bool is_in_finalizer() const { return m_boundaries.contains_slow(BlockBoundaryType::LeaveFinally); }

    void generate_break();
    void generate_break(FlyString const& break_label);

    void generate_continue();
    void generate_continue(FlyString const& continue_label);

    template<typename OpType>
    void emit_return(ScopedOperand value)
    requires(IsOneOf<OpType, Op::Return, Op::Yield>)
    {
        perform_needed_unwinds<OpType>();
        if (m_current_finally_context) {
            auto& finally_context = *m_current_finally_context;
            emit_mov(finally_context.completion_value, value);
            emit_mov(finally_context.completion_type, add_constant(Value(FinallyContext::RETURN)));
            emit<Bytecode::Op::Jump>(finally_context.finally_body);
            return;
        }

        if constexpr (IsSame<OpType, Op::Return>)
            emit<Op::Return>(value);
        else
            emit<Op::Yield>(OptionalNone {}, value);
    }

    void start_boundary(BlockBoundaryType type) { m_boundaries.append(type); }
    void end_boundary(BlockBoundaryType type)
    {
        VERIFY(m_boundaries.last() == type);
        m_boundaries.take_last();
    }

    [[nodiscard]] ScopedOperand copy_if_needed_to_preserve_evaluation_order(ScopedOperand const&);

    [[nodiscard]] ScopedOperand get_this(Optional<ScopedOperand> preferred_dst = {});

    void emit_get_by_id(ScopedOperand dst, ScopedOperand base, PropertyKeyTableIndex property_identifier, Optional<IdentifierTableIndex> base_identifier = {});

    void emit_get_by_id_with_this(ScopedOperand dst, ScopedOperand base, PropertyKeyTableIndex, ScopedOperand this_value);

    void emit_get_by_value(ScopedOperand dst, ScopedOperand base, ScopedOperand property, Optional<IdentifierTableIndex> base_identifier = {});
    void emit_get_by_value_with_this(ScopedOperand dst, ScopedOperand base, ScopedOperand property, ScopedOperand this_value);

    void emit_put_by_id(Operand base, PropertyKeyTableIndex property, Operand src, PutKind kind, u32 cache_index, Optional<IdentifierTableIndex> base_identifier = {});

    void emit_put_by_value(ScopedOperand base, ScopedOperand property, ScopedOperand src, Bytecode::PutKind, Optional<IdentifierTableIndex> base_identifier);
    void emit_put_by_value_with_this(ScopedOperand base, ScopedOperand property, ScopedOperand this_value, ScopedOperand src, Bytecode::PutKind);

    void emit_iterator_value(ScopedOperand dst, ScopedOperand result);
    void emit_iterator_complete(ScopedOperand dst, ScopedOperand result);

    [[nodiscard]] size_t next_global_variable_cache() { return m_next_global_variable_cache++; }
    [[nodiscard]] size_t next_property_lookup_cache() { return m_next_property_lookup_cache++; }
    [[nodiscard]] size_t next_template_object_cache() { return m_next_template_object_cache++; }
    [[nodiscard]] u32 next_object_shape_cache() { return m_next_object_shape_cache++; }

    enum class DeduplicateConstant {
        Yes,
        No,
    };
    [[nodiscard]] ScopedOperand add_constant(Value);

    [[nodiscard]] Value get_constant(ScopedOperand const& operand) const
    {
        VERIFY(operand.operand().is_constant());
        return m_constants[operand.operand().index()];
    }

    [[nodiscard]] Optional<Value> try_get_constant(ScopedOperand const& operand) const
    {
        if (operand.operand().is_constant())
            return get_constant(operand);
        return {};
    }

    UnwindContext const* current_unwind_context() const { return m_current_unwind_context; }

    [[nodiscard]] bool is_finished() const { return m_finished; }

    [[nodiscard]] bool must_propagate_completion() const { return m_must_propagate_completion; }

    [[nodiscard]] Optional<ScopedOperand> current_completion_register() const { return m_current_completion_register; }

    class CompletionRegisterScope {
    public:
        CompletionRegisterScope(Generator& gen, ScopedOperand reg)
            : m_generator(gen)
            , m_previous(gen.m_current_completion_register)
        {
            gen.m_current_completion_register = reg;
        }
        ~CompletionRegisterScope() { m_generator.m_current_completion_register = m_previous; }

        CompletionRegisterScope(CompletionRegisterScope const&) = delete;
        CompletionRegisterScope& operator=(CompletionRegisterScope const&) = delete;

    private:
        Generator& m_generator;
        Optional<ScopedOperand> m_previous;
    };

    [[nodiscard]] bool builtin_abstract_operations_enabled() const { return m_builtin_abstract_operations_enabled; }

    void generate_builtin_abstract_operation(Identifier const& builtin_identifier, ReadonlySpan<CallExpression::Argument> arguments, ScopedOperand const& dst);
    Optional<ScopedOperand> maybe_generate_builtin_constant(Identifier const& builtin_identifier);

private:
    VM& m_vm;

    static GC::Ref<Executable> compile(VM&, ASTNode const&, FunctionKind, GC::Ptr<SharedFunctionInstanceData const>, MustPropagateCompletion, BuiltinAbstractOperationsEnabled, Vector<LocalVariable> local_variable_names);

    enum class JumpType {
        Continue,
        Break,
    };
    void generate_scoped_jump(JumpType);
    void generate_labelled_jump(JumpType, FlyString const& label);

    [[nodiscard]] bool has_outer_finally_before_target(JumpType, size_t boundary_index) const;
    void register_jump_in_finally_context(Label target);
    void emit_trampoline_through_finally(JumpType);

    Generator(VM&, GC::Ptr<SharedFunctionInstanceData const>, MustPropagateCompletion, BuiltinAbstractOperationsEnabled);
    ~Generator() = default;

    void grow(size_t);

    // Returns true if a fused instruction was emitted.
    [[nodiscard]] bool fuse_compare_and_jump(ScopedOperand const& condition, Label true_target, Label false_target);

    struct LabelableScope {
        Label bytecode_target;
        Vector<FlyString> language_label_set;
        Optional<ScopedOperand> completion_register;
    };

    Strict m_strict { Strict::No };

    BasicBlock* m_current_basic_block { nullptr };
    ASTNode const* m_current_ast_node { nullptr };
    UnwindContext const* m_current_unwind_context { nullptr };

    Vector<NonnullOwnPtr<BasicBlock>> m_root_basic_blocks;
    NonnullOwnPtr<StringTable> m_string_table;
    NonnullOwnPtr<IdentifierTable> m_identifier_table;
    NonnullOwnPtr<PropertyKeyTable> m_property_key_table;
    NonnullOwnPtr<RegexTable> m_regex_table;
    GC::RootVector<Value> m_constants;

    mutable Optional<ScopedOperand> m_true_constant;
    mutable Optional<ScopedOperand> m_false_constant;
    mutable Optional<ScopedOperand> m_null_constant;
    mutable Optional<ScopedOperand> m_undefined_constant;
    mutable Optional<ScopedOperand> m_empty_constant;
    mutable HashMap<i32, ScopedOperand> m_int32_constants;
    mutable HashMap<Utf16String, ScopedOperand> m_string_constants;

    ScopedOperand m_accumulator;
    ScopedOperand m_this_value;
    Vector<Register> m_free_registers;

    u32 m_next_register { Register::reserved_register_count };
    u32 m_next_block { 1 };
    u32 m_next_property_lookup_cache { 0 };
    u32 m_next_global_variable_cache { 0 };
    u32 m_next_template_object_cache { 0 };
    u32 m_next_object_shape_cache { 0 };
    FunctionKind m_enclosing_function_kind { FunctionKind::Normal };
    Vector<LabelableScope> m_continuable_scopes;
    Vector<LabelableScope> m_breakable_scopes;
    Vector<BlockBoundaryType> m_boundaries;
    Vector<ScopedOperand> m_home_objects;
    Vector<ScopedOperand> m_lexical_environment_register_stack;
    FinallyContext* m_current_finally_context { nullptr };

    HashTable<u32> m_initialized_locals;
    HashTable<u32> m_initialized_arguments;
    Vector<LocalVariable> m_local_variables;

    Optional<ScopedOperand> m_current_completion_register {};

    bool m_finished { false };
    bool m_must_propagate_completion { true };
    bool m_builtin_abstract_operations_enabled { false };

    GC::Ptr<SharedFunctionInstanceData const> m_shared_function_instance_data;

    Vector<GC::Root<SharedFunctionInstanceData>> m_shared_function_data;
    Vector<ClassBlueprint> m_class_blueprints;

    Optional<PropertyKeyTableIndex> m_length_identifier;
};

}
