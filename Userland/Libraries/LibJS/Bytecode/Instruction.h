/*
 * Copyright (c) 2021-2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/Span.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Forward.h>
#include <LibJS/SourceRange.h>

#define ENUMERATE_BYTECODE_OPS(O)      \
    O(Add)                             \
    O(AddPrivateName)                  \
    O(ArrayAppend)                     \
    O(AsyncIteratorClose)              \
    O(Await)                           \
    O(BitwiseAnd)                      \
    O(BitwiseNot)                      \
    O(BitwiseOr)                       \
    O(BitwiseXor)                      \
    O(BlockDeclarationInstantiation)   \
    O(Call)                            \
    O(CallWithArgumentArray)           \
    O(Catch)                           \
    O(ConcatString)                    \
    O(ContinuePendingUnwind)           \
    O(CopyObjectExcludingProperties)   \
    O(CreateArguments)                 \
    O(CreateLexicalEnvironment)        \
    O(CreatePrivateEnvironment)        \
    O(CreateRestParams)                \
    O(CreateVariable)                  \
    O(CreateVariableEnvironment)       \
    O(Decrement)                       \
    O(DeleteById)                      \
    O(DeleteByIdWithThis)              \
    O(DeleteByValue)                   \
    O(DeleteByValueWithThis)           \
    O(DeleteVariable)                  \
    O(Div)                             \
    O(Dump)                            \
    O(End)                             \
    O(EnterObjectEnvironment)          \
    O(EnterUnwindContext)              \
    O(Exp)                             \
    O(GetArgument)                     \
    O(GetById)                         \
    O(GetByIdWithThis)                 \
    O(GetByValue)                      \
    O(GetByValueWithThis)              \
    O(GetCalleeAndThisFromEnvironment) \
    O(GetGlobal)                       \
    O(GetImportMeta)                   \
    O(GetIterator)                     \
    O(GetLength)                       \
    O(GetLengthWithThis)               \
    O(GetMethod)                       \
    O(GetNewTarget)                    \
    O(GetNextMethodFromIteratorRecord) \
    O(GetObjectFromIteratorRecord)     \
    O(GetObjectPropertyIterator)       \
    O(GetPrivateById)                  \
    O(GetBinding)                      \
    O(GreaterThan)                     \
    O(GreaterThanEquals)               \
    O(HasPrivateId)                    \
    O(ImportCall)                      \
    O(In)                              \
    O(Increment)                       \
    O(InitializeLexicalBinding)        \
    O(InitializeVariableBinding)       \
    O(InstanceOf)                      \
    O(IteratorClose)                   \
    O(IteratorNext)                    \
    O(IteratorToArray)                 \
    O(Jump)                            \
    O(JumpFalse)                       \
    O(JumpGreaterThan)                 \
    O(JumpGreaterThanEquals)           \
    O(JumpIf)                          \
    O(JumpLessThan)                    \
    O(JumpLessThanEquals)              \
    O(JumpLooselyEquals)               \
    O(JumpLooselyInequals)             \
    O(JumpNullish)                     \
    O(JumpStrictlyEquals)              \
    O(JumpStrictlyInequals)            \
    O(JumpTrue)                        \
    O(JumpUndefined)                   \
    O(LeaveFinally)                    \
    O(LeaveLexicalEnvironment)         \
    O(LeavePrivateEnvironment)         \
    O(LeaveUnwindContext)              \
    O(LeftShift)                       \
    O(LessThan)                        \
    O(LessThanEquals)                  \
    O(LooselyEquals)                   \
    O(LooselyInequals)                 \
    O(Mod)                             \
    O(Mov)                             \
    O(Mul)                             \
    O(NewArray)                        \
    O(NewClass)                        \
    O(NewFunction)                     \
    O(NewObject)                       \
    O(NewPrimitiveArray)               \
    O(NewRegExp)                       \
    O(NewTypeError)                    \
    O(Not)                             \
    O(PrepareYield)                    \
    O(PostfixDecrement)                \
    O(PostfixIncrement)                \
    O(PutById)                         \
    O(PutByIdWithThis)                 \
    O(PutByValue)                      \
    O(PutByValueWithThis)              \
    O(PutPrivateById)                  \
    O(ResolveSuperBase)                \
    O(ResolveThisBinding)              \
    O(RestoreScheduledJump)            \
    O(Return)                          \
    O(RightShift)                      \
    O(ScheduleJump)                    \
    O(SetArgument)                     \
    O(SetLexicalBinding)               \
    O(SetVariableBinding)              \
    O(StrictlyEquals)                  \
    O(StrictlyInequals)                \
    O(Sub)                             \
    O(SuperCallWithArgumentArray)      \
    O(Throw)                           \
    O(ThrowIfNotObject)                \
    O(ThrowIfNullish)                  \
    O(ThrowIfTDZ)                      \
    O(Typeof)                          \
    O(TypeofBinding)                   \
    O(UnaryMinus)                      \
    O(UnaryPlus)                       \
    O(UnsignedRightShift)              \
    O(Yield)

namespace JS::Bytecode {

class alignas(void*) Instruction {
public:
    constexpr static bool IsTerminator = false;
    static constexpr bool IsVariableLength = false;

    enum class Type {
#define __BYTECODE_OP(op) \
    op,
        ENUMERATE_BYTECODE_OPS(__BYTECODE_OP)
#undef __BYTECODE_OP
    };

    Type type() const { return m_type; }
    size_t length() const;
    ByteString to_byte_string(Bytecode::Executable const&) const;
    void visit_labels(Function<void(Label&)> visitor);
    void visit_operands(Function<void(Operand&)> visitor);
    static void destroy(Instruction&);

protected:
    explicit Instruction(Type type)
        : m_type(type)
    {
    }

    void visit_labels_impl(Function<void(Label&)>) { }
    void visit_operands_impl(Function<void(Operand&)>) { }

private:
    Type m_type {};
};

class InstructionStreamIterator {
public:
    InstructionStreamIterator(ReadonlyBytes bytes, Executable const* executable = nullptr, size_t offset = 0)
        : m_begin(bytes.data())
        , m_end(bytes.data() + bytes.size())
        , m_ptr(bytes.data() + offset)
        , m_executable(executable)
    {
    }

    size_t offset() const { return m_ptr - m_begin; }
    bool at_end() const { return m_ptr >= m_end; }

    Instruction const& operator*() const { return dereference(); }

    ALWAYS_INLINE void operator++()
    {
        m_ptr += dereference().length();
    }

    UnrealizedSourceRange source_range() const;
    RefPtr<SourceCode> source_code() const;

    Executable const* executable() const { return m_executable; }

private:
    Instruction const& dereference() const { return *reinterpret_cast<Instruction const*>(m_ptr); }

    u8 const* m_begin { nullptr };
    u8 const* m_end { nullptr };
    u8 const* m_ptr { nullptr };
    GCPtr<Executable const> m_executable;
};

}
