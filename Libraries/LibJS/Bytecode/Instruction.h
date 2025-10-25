/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/Span.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/PutKind.h>
#include <LibJS/Forward.h>
#include <LibJS/SourceRange.h>

#define JS_ENUMERATE_GENERIC_BYTECODE_OPS(X) \
    X(Add)                                   \
    X(AddPrivateName)                        \
    X(ArrayAppend)                           \
    X(AsyncIteratorClose)                    \
    X(BitwiseAnd)                            \
    X(BitwiseNot)                            \
    X(BitwiseOr)                             \
    X(BitwiseXor)                            \
    X(BlockDeclarationInstantiation)         \
    X(Call)                                  \
    X(CallBuiltin)                           \
    X(CallConstruct)                         \
    X(CallConstructWithArgumentArray)        \
    X(CallDirectEval)                        \
    X(CallDirectEvalWithArgumentArray)       \
    X(CallWithArgumentArray)                 \
    X(Catch)                                 \
    X(ConcatString)                          \
    X(CopyObjectExcludingProperties)         \
    X(CreateArguments)                       \
    X(CreateLexicalEnvironment)              \
    X(CreatePrivateEnvironment)              \
    X(CreateRestParams)                      \
    X(CreateVariable)                        \
    X(CreateVariableEnvironment)             \
    X(Decrement)                             \
    X(DeleteById)                            \
    X(DeleteByIdWithThis)                    \
    X(DeleteByValue)                         \
    X(DeleteByValueWithThis)                 \
    X(DeleteVariable)                        \
    X(Div)                                   \
    X(Dump)                                  \
    X(EnterObjectEnvironment)                \
    X(Exp)                                   \
    X(GetBinding)                            \
    X(GetById)                               \
    X(GetByIdWithThis)                       \
    X(GetByValue)                            \
    X(GetByValueWithThis)                    \
    X(GetCalleeAndThisFromEnvironment)       \
    X(GetCompletionFields)                   \
    X(GetGlobal)                             \
    X(GetImportMeta)                         \
    X(GetInitializedBinding)                 \
    X(GetIterator)                           \
    X(GetLength)                             \
    X(GetLengthWithThis)                     \
    X(GetMethod)                             \
    X(GetNewTarget)                          \
    X(GetNextMethodFromIteratorRecord)       \
    X(GetObjectFromIteratorRecord)           \
    X(GetObjectPropertyIterator)             \
    X(GetPrivateById)                        \
    X(GreaterThan)                           \
    X(GreaterThanEquals)                     \
    X(HasPrivateId)                          \
    X(ImportCall)                            \
    X(In)                                    \
    X(Increment)                             \
    X(InitializeLexicalBinding)              \
    X(InitializeVariableBinding)             \
    X(InstanceOf)                            \
    X(IteratorClose)                         \
    X(IteratorNext)                          \
    X(IteratorNextUnpack)                    \
    X(IteratorToArray)                       \
    X(LeaveFinally)                          \
    X(LeaveLexicalEnvironment)               \
    X(LeavePrivateEnvironment)               \
    X(LeaveUnwindContext)                    \
    X(LeftShift)                             \
    X(LessThan)                              \
    X(LessThanEquals)                        \
    X(LooselyEquals)                         \
    X(LooselyInequals)                       \
    X(Mod)                                   \
    X(Mul)                                   \
    X(NewArray)                              \
    X(NewClass)                              \
    X(NewFunction)                           \
    X(NewObject)                             \
    X(NewPrimitiveArray)                     \
    X(NewRegExp)                             \
    X(NewTypeError)                          \
    X(Not)                                   \
    X(PostfixDecrement)                      \
    X(PostfixIncrement)                      \
    X(PrepareYield)                          \
    X(PutBySpread)                           \
    X(PutPrivateById)                        \
    X(ResolveSuperBase)                      \
    X(ResolveThisBinding)                    \
    X(RestoreScheduledJump)                  \
    X(RightShift)                            \
    X(SetCompletionType)                     \
    X(SetGlobal)                             \
    X(SetLexicalBinding)                     \
    X(SetVariableBinding)                    \
    X(StrictlyEquals)                        \
    X(StrictlyInequals)                      \
    X(Sub)                                   \
    X(SuperCallWithArgumentArray)            \
    X(Throw)                                 \
    X(ThrowIfNotObject)                      \
    X(ThrowIfNullish)                        \
    X(ThrowIfTDZ)                            \
    X(Typeof)                                \
    X(TypeofBinding)                         \
    X(UnaryMinus)                            \
    X(UnaryPlus)                             \
    X(UnsignedRightShift)

#define JS_ENUMERATE_SIMPLE_JUMP_OPS(X) \
    X(Jump)                             \
    X(JumpIf)                           \
    X(JumpTrue)                         \
    X(JumpFalse)                        \
    X(JumpNullish)                      \
    X(JumpUndefined)

// FIXME: Ideally generate these form JS_ENUMERATE_COMPARISON_OPS and
//       Jump##op_TitleCase
#define JS_ENUMERATE_COMPARISON_JUMP_OPS(X) \
    X(JumpGreaterThan)                      \
    X(JumpGreaterThanEquals)                \
    X(JumpLessThan)                         \
    X(JumpLessThanEquals)                   \
    X(JumpLooselyEquals)                    \
    X(JumpLooselyInequals)                  \
    X(JumpStrictlyEquals)                   \
    X(JumpStrictlyInequals)

// FIXME: Find a way to define these with JS_ENUMERATE_PUT_KINDS and
//        Put##kind##ById, Put##kind##ByNumericId, Put##kind##ByValue,
//        Put##kind##ByValueWithThis, Put##kind##ByIdWithThis,
//        Put##kind##ByNumericIdWithThis
#define JS_ENUMERATE_PUT_OPS(X)        \
    X(PutNormalById)                   \
    X(PutNormalByNumericId)            \
    X(PutNormalByValue)                \
    X(PutNormalByValueWithThis)        \
    X(PutNormalByIdWithThis)           \
    X(PutNormalByNumericIdWithThis)    \
    X(PutGetterById)                   \
    X(PutGetterByNumericId)            \
    X(PutGetterByValue)                \
    X(PutGetterByValueWithThis)        \
    X(PutGetterByIdWithThis)           \
    X(PutGetterByNumericIdWithThis)    \
    X(PutSetterById)                   \
    X(PutSetterByNumericId)            \
    X(PutSetterByValue)                \
    X(PutSetterByValueWithThis)        \
    X(PutSetterByIdWithThis)           \
    X(PutSetterByNumericIdWithThis)    \
    X(PutPrototypeById)                \
    X(PutPrototypeByNumericId)         \
    X(PutPrototypeByValue)             \
    X(PutPrototypeByValueWithThis)     \
    X(PutPrototypeByIdWithThis)        \
    X(PutPrototypeByNumericIdWithThis) \
    X(PutOwnById)                      \
    X(PutOwnByNumericId)               \
    X(PutOwnByValue)                   \
    X(PutOwnByValueWithThis)           \
    X(PutOwnByIdWithThis)              \
    X(PutOwnByNumericIdWithThis)

#define ENUMERATE_BYTECODE_OPS(O)        \
    JS_ENUMERATE_GENERIC_BYTECODE_OPS(O) \
    JS_ENUMERATE_SIMPLE_JUMP_OPS(O)      \
    JS_ENUMERATE_COMPARISON_JUMP_OPS(O)  \
    JS_ENUMERATE_PUT_OPS(O)              \
    O(Mov)                               \
    O(End)                               \
    O(EnterUnwindContext)                \
    O(ContinuePendingUnwind)             \
    O(ScheduleJump)                      \
    O(Await)                             \
    O(Return)                            \
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
            __Last
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

    Executable const* executable() const { return m_executable; }

private:
    Instruction const& dereference() const { return *reinterpret_cast<Instruction const*>(m_ptr); }

    u8 const* m_begin { nullptr };
    u8 const* m_end { nullptr };
    u8 const* m_ptr { nullptr };
    GC::Ptr<Executable const> m_executable;
};

}
