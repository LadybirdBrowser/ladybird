/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StackInfo.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Interpreter.h>
#include <LibWasm/Export.h>

namespace Wasm {

enum class Outcome : u64 {
    // 0..Constants::max_allowed_executed_instructions_per_call -> next IP.
    Continue = Constants::max_allowed_executed_instructions_per_call + 1,
    Return,
};

struct WASM_API BytecodeInterpreter final : public Interpreter {
    explicit BytecodeInterpreter(StackInfo const& stack_info)
        : m_stack_info(stack_info)
    {
    }

    virtual void interpret(Configuration&) final;

    virtual ~BytecodeInterpreter() override = default;
    virtual bool did_trap() const final { return !m_trap.has<Empty>(); }
    virtual Trap trap() const final
    {
        return m_trap.get<Trap>();
    }
    virtual void clear_trap() final { m_trap = Empty {}; }
    virtual void visit_external_resources(HostVisitOps const& host) override
    {
        if (auto ptr = m_trap.get_pointer<Trap>())
            if (auto data = ptr->data.get_pointer<ExternallyManagedTrap>())
                host.visit_trap(*data);
    }

    struct CallFrameHandle {
        explicit CallFrameHandle(BytecodeInterpreter& interpreter, Configuration& configuration)
            : m_configuration_handle(configuration)
            , m_interpreter(interpreter)
        {
        }

        ~CallFrameHandle() = default;

        Configuration::CallFrameHandle m_configuration_handle;
        BytecodeInterpreter& m_interpreter;
    };

    enum class CallAddressSource {
        DirectCall,
        IndirectCall,
        DirectTailCall,
        IndirectTailCall,
    };

    enum class CallType {
        UsingRegisters,
        UsingCallRecord,
        UsingStack,
    };

    template<bool HasCompiledList, bool HasDynamicInsnLimit, bool HaveDirectThreadingInfo>
    void interpret_impl(Configuration&, Expression const&);

    template<bool NeedsStackAdjustment>
    InstructionPointer branch_to_label(Configuration&, LabelIndex, InstructionPointer current_ip, bool actually_branching = true);
    template<typename ReadT, typename PushT, SourceAddressMix>
    bool load_and_push(Configuration&, Instruction const&, SourcesAndDestination const&);
    template<typename PopT, typename StoreT>
    bool pop_and_store(Configuration&, Instruction const&, SourcesAndDestination const&);
    template<typename StoreT>
    bool store_value(Configuration&, Instruction const&, StoreT, size_t address_source, SourcesAndDestination const&);
    template<size_t N>
    bool pop_and_store_lane_n(Configuration&, Instruction const&, SourcesAndDestination const&);
    template<size_t M, size_t N, template<typename> typename SetSign>
    bool load_and_push_mxn(Configuration&, Instruction const&, SourcesAndDestination const&);
    template<size_t N>
    bool load_and_push_lane_n(Configuration&, Instruction const&, SourcesAndDestination const&);
    template<size_t N>
    bool load_and_push_zero_n(Configuration&, Instruction const&, SourcesAndDestination const&);
    template<size_t M>
    bool load_and_push_m_splat(Configuration&, Instruction const&, SourcesAndDestination const&);
    template<size_t M, template<size_t> typename NativeType>
    void set_top_m_splat(Configuration&, NativeType<M>, SourcesAndDestination const&);
    template<size_t M, template<size_t> typename NativeType>
    void pop_and_push_m_splat(Configuration&, Instruction const&, SourcesAndDestination const&);
    template<typename M, template<typename> typename SetSign, typename VectorType = Native128ByteVectorOf<M, SetSign>>
    VectorType pop_vector(Configuration&, size_t source, SourcesAndDestination const&);
    bool store_to_memory(Configuration&, Instruction::MemoryArgument const&, ReadonlyBytes data, u32 base);
    Outcome call_address(Configuration&, FunctionAddress, SourcesAndDestination const&, CallAddressSource = CallAddressSource::DirectCall, CallType = CallType::UsingStack);

    template<typename T>
    bool store_to_memory(MemoryInstance&, u64 address, T value);

    template<typename PopTypeLHS, typename PushType, typename Operator, SourceAddressMix, typename PopTypeRHS = PopTypeLHS, typename... Args>
    bool binary_numeric_operation(Configuration&, SourcesAndDestination const&, Args&&...);

    template<typename PopType, typename PushType, typename Operator, SourceAddressMix, size_t input_arg = 0, typename... Args>
    bool unary_operation(Configuration&, SourcesAndDestination const&, Args&&...);

    ALWAYS_INLINE bool set_trap(StringView reason)
    {
        m_trap = Trap { ByteString(reason) };
        return true;
    }

    template<typename T>
    T read_value(ReadonlyBytes data);

    ALWAYS_INLINE bool trap_if_not(bool value, StringView reason)
    {
        if (!value) [[unlikely]] {
            m_trap = Trap { ByteString(reason) };
            return true;
        }
        return false;
    }

    template<typename... Rest>
    ALWAYS_INLINE bool trap_if_not(bool value, StringView reason, CheckedFormatString<StringView, Rest...> format, Rest const&... args)
    {
        if (!value) [[unlikely]] {
            m_trap = Trap { ByteString::formatted(move(format), reason, args...) };
            return true;
        }
        return false;
    }

protected:
    Variant<Trap, Empty> m_trap;
    StackInfo const& m_stack_info;
};

}
