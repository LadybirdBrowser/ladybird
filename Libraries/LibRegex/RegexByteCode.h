/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "RegexBytecodeStreamOptimizer.h"
#include "RegexMatch.h"

#include <AK/Concepts.h>
#include <AK/DisjointChunks.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/TypeCasts.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibUnicode/Forward.h>

namespace regex {

using ByteCodeValueType = u64;

#define ENUMERATE_OPCODES                          \
    __ENUMERATE_OPCODE(Compare)                    \
    __ENUMERATE_OPCODE(Jump)                       \
    __ENUMERATE_OPCODE(JumpNonEmpty)               \
    __ENUMERATE_OPCODE(ForkJump)                   \
    __ENUMERATE_OPCODE(ForkStay)                   \
    __ENUMERATE_OPCODE(ForkReplaceJump)            \
    __ENUMERATE_OPCODE(ForkReplaceStay)            \
    __ENUMERATE_OPCODE(FailForks)                  \
    __ENUMERATE_OPCODE(PopSaved)                   \
    __ENUMERATE_OPCODE(SaveLeftCaptureGroup)       \
    __ENUMERATE_OPCODE(SaveRightCaptureGroup)      \
    __ENUMERATE_OPCODE(SaveRightNamedCaptureGroup) \
    __ENUMERATE_OPCODE(CheckBegin)                 \
    __ENUMERATE_OPCODE(CheckEnd)                   \
    __ENUMERATE_OPCODE(CheckBoundary)              \
    __ENUMERATE_OPCODE(Save)                       \
    __ENUMERATE_OPCODE(Restore)                    \
    __ENUMERATE_OPCODE(GoBack)                     \
    __ENUMERATE_OPCODE(ClearCaptureGroup)          \
    __ENUMERATE_OPCODE(Repeat)                     \
    __ENUMERATE_OPCODE(ResetRepeat)                \
    __ENUMERATE_OPCODE(Checkpoint)                 \
    __ENUMERATE_OPCODE(Exit)

// clang-format off
enum class OpCodeId : ByteCodeValueType {
#define __ENUMERATE_OPCODE(x) x,
    ENUMERATE_OPCODES
#undef __ENUMERATE_OPCODE

    First = Compare,
    Last = Exit,
};
// clang-format on

#define ENUMERATE_CHARACTER_COMPARE_TYPES                    \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(Undefined)            \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(Inverse)              \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(TemporaryInverse)     \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(AnyChar)              \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(Char)                 \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(String)               \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(CharClass)            \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(CharRange)            \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(Reference)            \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(Property)             \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(GeneralCategory)      \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(Script)               \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(ScriptExtension)      \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(RangeExpressionDummy) \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(LookupTable)          \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(And)                  \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(Or)                   \
    __ENUMERATE_CHARACTER_COMPARE_TYPE(EndAndOr)

enum class CharacterCompareType : ByteCodeValueType {
#define __ENUMERATE_CHARACTER_COMPARE_TYPE(x) x,
    ENUMERATE_CHARACTER_COMPARE_TYPES
#undef __ENUMERATE_CHARACTER_COMPARE_TYPE
};

#define ENUMERATE_CHARACTER_CLASSES    \
    __ENUMERATE_CHARACTER_CLASS(Alnum) \
    __ENUMERATE_CHARACTER_CLASS(Cntrl) \
    __ENUMERATE_CHARACTER_CLASS(Lower) \
    __ENUMERATE_CHARACTER_CLASS(Space) \
    __ENUMERATE_CHARACTER_CLASS(Alpha) \
    __ENUMERATE_CHARACTER_CLASS(Digit) \
    __ENUMERATE_CHARACTER_CLASS(Print) \
    __ENUMERATE_CHARACTER_CLASS(Upper) \
    __ENUMERATE_CHARACTER_CLASS(Blank) \
    __ENUMERATE_CHARACTER_CLASS(Graph) \
    __ENUMERATE_CHARACTER_CLASS(Punct) \
    __ENUMERATE_CHARACTER_CLASS(Word)  \
    __ENUMERATE_CHARACTER_CLASS(Xdigit)

enum class CharClass : ByteCodeValueType {
#define __ENUMERATE_CHARACTER_CLASS(x) x,
    ENUMERATE_CHARACTER_CLASSES
#undef __ENUMERATE_CHARACTER_CLASS
};

#define ENUMERATE_BOUNDARY_CHECK_TYPES    \
    __ENUMERATE_BOUNDARY_CHECK_TYPE(Word) \
    __ENUMERATE_BOUNDARY_CHECK_TYPE(NonWord)

enum class BoundaryCheckType : ByteCodeValueType {
#define __ENUMERATE_BOUNDARY_CHECK_TYPE(x) x,
    ENUMERATE_BOUNDARY_CHECK_TYPES
#undef __ENUMERATE_BOUNDARY_CHECK_TYPE
};

struct CharRange {
    u32 from;
    u32 to;

    CharRange(u64 value)
        : from(value >> 32)
        , to(value & 0xffffffff)
    {
    }

    CharRange(u32 from, u32 to)
        : from(from)
        , to(to)
    {
    }

    operator ByteCodeValueType() const { return ((u64)from << 32) | to; }
};

struct CompareTypeAndValuePair {
    CharacterCompareType type;
    ByteCodeValueType value;
};

class OpCode;

struct REGEX_API StringTable {
    StringTable();
    ~StringTable();
    StringTable(StringTable const&) = default;
    StringTable(StringTable&&) = default;
    StringTable& operator=(StringTable const&) = default;
    StringTable& operator=(StringTable&&) = default;

    ByteCodeValueType set(FlyString string)
    {
        u32 local_index = m_table.size() + 0x4242;
        ByteCodeValueType global_index;
        if (auto maybe_local_index = m_table.get(string); maybe_local_index.has_value()) {
            local_index = maybe_local_index.value();
            global_index = static_cast<ByteCodeValueType>(m_serial) << 32 | static_cast<ByteCodeValueType>(local_index);
        } else {
            global_index = static_cast<ByteCodeValueType>(m_serial) << 32 | static_cast<ByteCodeValueType>(local_index);
            m_table.set(string, global_index);
            m_inverse_table.set(global_index, string);
        }

        return global_index;
    }

    FlyString get(ByteCodeValueType index) const
    {
        return m_inverse_table.get(index).value();
    }

    u32 m_serial { 0 };
    HashMap<FlyString, ByteCodeValueType> m_table;
    HashMap<ByteCodeValueType, FlyString> m_inverse_table;
};

class REGEX_API ByteCode : public DisjointChunks<ByteCodeValueType> {
    using Base = DisjointChunks<ByteCodeValueType>;

public:
    using Base::append;

    ByteCode()
    {
        ensure_opcodes_initialized();
    }

    ByteCode(ByteCode const&) = default;
    ByteCode(ByteCode&&) = default;

    ByteCode(Base&&) = delete;
    ByteCode(Base const&) = delete;

    virtual ~ByteCode() = default;

    ByteCode& operator=(ByteCode const&) = default;
    ByteCode& operator=(ByteCode&&) = default;

    ByteCode& operator=(Base&& value) = delete;
    ByteCode& operator=(Base const& value) = delete;

    void extend(ByteCode&& other)
    {
        merge_string_tables_from({ &other, 1 });
        Base::extend(move(other));
    }

    void extend(ByteCode const& other)
    {
        merge_string_tables_from({ &other, 1 });
        Base::extend(other);
    }

    template<SameAs<Vector<ByteCodeValueType>> T>
    void extend(T other)
    {
        Base::append(move(other));
    }

    template<typename... Args>
    void empend(Args&&... args)
    {
        if (is_empty())
            Base::append({});
        Base::last_chunk().empend(forward<Args>(args)...);
    }
    template<typename T>
    void append(T&& value)
    {
        if (is_empty())
            Base::append({});
        Base::last_chunk().append(forward<T>(value));
    }
    template<typename T>
    void prepend(T&& value)
    {
        if (is_empty())
            return append(forward<T>(value));
        Base::first_chunk().prepend(forward<T>(value));
    }

    void append(Span<ByteCodeValueType const> value)
    {
        if (is_empty())
            Base::append({});
        auto& last = Base::last_chunk();
        last.ensure_capacity(value.size());
        for (auto v : value)
            last.unchecked_append(v);
    }

    void ensure_capacity(size_t capacity)
    {
        if (is_empty())
            Base::append({});
        Base::last_chunk().ensure_capacity(capacity);
    }

    FlyString get_string(size_t index) const { return m_string_table.get(index); }
    auto const& string_table() const { return m_string_table; }

    void last_chunk() const = delete;
    void first_chunk() const = delete;

    void merge_string_tables_from(Span<ByteCode const> others)
    {
        for (auto const& other : others) {
            for (auto const& entry : other.m_string_table.m_table) {
                auto const result = m_string_table.m_inverse_table.set(entry.value, entry.key);
                if (result != HashSetResult::InsertedNewEntry) {
                    if (m_string_table.m_inverse_table.get(entry.value) == entry.key) // Already in inverse table.
                        continue;
                    dbgln("StringTable: Detected ID clash in string tables! ID {} seems to be reused", entry.value);
                    dbgln("Old: {}, New: {}", m_string_table.m_inverse_table.get(entry.value), entry.key);
                    VERIFY_NOT_REACHED();
                }
                m_string_table.m_table.set(entry.key, entry.value);
            }
            m_string_table.m_inverse_table.update(other.m_string_table.m_inverse_table);
        }
    }

    void insert_bytecode_compare_values(Vector<CompareTypeAndValuePair>&& pairs)
    {
        Optimizer::append_character_class(*this, move(pairs));
    }

    void insert_bytecode_check_boundary(BoundaryCheckType type)
    {
        ByteCode bytecode;
        bytecode.empend((ByteCodeValueType)OpCodeId::CheckBoundary);
        bytecode.empend((ByteCodeValueType)type);

        extend(move(bytecode));
    }

    void insert_bytecode_clear_capture_group(size_t index)
    {
        empend(static_cast<ByteCodeValueType>(OpCodeId::ClearCaptureGroup));
        empend(index);
    }

    void insert_bytecode_compare_string(StringView view)
    {
        empend(static_cast<ByteCodeValueType>(OpCodeId::Compare));
        empend(static_cast<u64>(1)); // number of arguments
        empend(2 + view.length());   // size of arguments
        empend(static_cast<ByteCodeValueType>(CharacterCompareType::String));
        insert_string(view);
    }

    void insert_bytecode_group_capture_left(size_t capture_groups_count)
    {
        empend(static_cast<ByteCodeValueType>(OpCodeId::SaveLeftCaptureGroup));
        empend(capture_groups_count);
    }

    void insert_bytecode_group_capture_right(size_t capture_groups_count)
    {
        empend(static_cast<ByteCodeValueType>(OpCodeId::SaveRightCaptureGroup));
        empend(capture_groups_count);
    }

    void insert_bytecode_group_capture_right(size_t capture_groups_count, FlyString name)
    {
        empend(static_cast<ByteCodeValueType>(OpCodeId::SaveRightNamedCaptureGroup));
        empend(m_string_table.set(move(name)));
        empend(capture_groups_count);
    }

    enum class LookAroundType {
        LookAhead,
        LookBehind,
        NegatedLookAhead,
        NegatedLookBehind,
    };
    void insert_bytecode_lookaround(ByteCode&& lookaround_body, LookAroundType type, size_t match_length = 0)
    {
        // FIXME: The save stack will grow infinitely with repeated failures
        //        as we do not discard that on failure (we don't necessarily know how many to pop with the current architecture).
        switch (type) {
        case LookAroundType::LookAhead: {
            // SAVE
            // FORKJUMP _BODY
            // POPSAVED
            // LABEL _BODY
            // REGEXP BODY
            // RESTORE
            empend((ByteCodeValueType)OpCodeId::Save);
            empend((ByteCodeValueType)OpCodeId::ForkJump);
            empend((ByteCodeValueType)1);
            empend((ByteCodeValueType)OpCodeId::PopSaved);
            extend(move(lookaround_body));
            empend((ByteCodeValueType)OpCodeId::Restore);
            return;
        }
        case LookAroundType::NegatedLookAhead: {
            // JUMP _A
            // LABEL _L
            // REGEXP BODY
            // FAIL
            // LABEL _A
            // SAVE
            // FORKJUMP _L
            // RESTORE
            auto body_length = lookaround_body.size();
            empend((ByteCodeValueType)OpCodeId::Jump);
            empend((ByteCodeValueType)body_length + 1); // JUMP to label _A
            extend(move(lookaround_body));
            empend((ByteCodeValueType)OpCodeId::FailForks);
            empend((ByteCodeValueType)OpCodeId::Save);
            empend((ByteCodeValueType)OpCodeId::ForkJump);
            empend((ByteCodeValueType) - (body_length + 4)); // JUMP to label _L
            empend((ByteCodeValueType)OpCodeId::Restore);
            return;
        }
        case LookAroundType::LookBehind:
            // SAVE
            // GOBACK match_length(BODY)
            // REGEXP BODY
            // RESTORE
            empend((ByteCodeValueType)OpCodeId::Save);
            empend((ByteCodeValueType)OpCodeId::GoBack);
            empend((ByteCodeValueType)match_length);
            extend(move(lookaround_body));
            empend((ByteCodeValueType)OpCodeId::Restore);
            return;
        case LookAroundType::NegatedLookBehind: {
            // JUMP _A
            // LABEL _L
            // GOBACK match_length(BODY)
            // REGEXP BODY
            // FAIL
            // LABEL _A
            // SAVE
            // FORKJUMP _L
            // RESTORE
            auto body_length = lookaround_body.size();
            empend((ByteCodeValueType)OpCodeId::Jump);
            empend((ByteCodeValueType)body_length + 3); // JUMP to label _A
            empend((ByteCodeValueType)OpCodeId::GoBack);
            empend((ByteCodeValueType)match_length);
            extend(move(lookaround_body));
            empend((ByteCodeValueType)OpCodeId::FailForks);
            empend((ByteCodeValueType)OpCodeId::Save);
            empend((ByteCodeValueType)OpCodeId::ForkJump);
            empend((ByteCodeValueType) - (body_length + 6)); // JUMP to label _L
            empend((ByteCodeValueType)OpCodeId::Restore);
            return;
        }
        }

        VERIFY_NOT_REACHED();
    }

    void insert_bytecode_alternation(ByteCode&& left, ByteCode&& right)
    {

        // FORKJUMP _ALT
        // REGEXP ALT2
        // JUMP  _END
        // LABEL _ALT
        // REGEXP ALT1
        // LABEL _END

        // Optimisation: Eliminate extra work by unifying common pre-and-postfix exprs.
        Optimizer::append_alternation(*this, move(left), move(right));
    }

    template<Integral T>
    static void transform_bytecode_repetition_min_max(ByteCode& bytecode_to_repeat, T minimum, Optional<T> maximum, size_t min_repetition_mark_id, size_t max_repetition_mark_id, bool greedy = true)
    {
        if (!maximum.has_value()) {
            if (minimum == 0)
                return transform_bytecode_repetition_any(bytecode_to_repeat, greedy);
            if (minimum == 1)
                return transform_bytecode_repetition_min_one(bytecode_to_repeat, greedy);
        }

        ByteCode new_bytecode;
        new_bytecode.insert_bytecode_repetition_n(bytecode_to_repeat, minimum, min_repetition_mark_id);

        if (maximum.has_value()) {
            // (REPEAT REGEXP MIN)
            // LABEL _MAX_LOOP            |
            // FORK END                   |
            // REGEXP                     |
            // REPEAT _MAX_LOOP MAX-MIN   | if max > min
            // FORK END                   |
            // REGEXP                     |
            // LABEL END                  |
            // RESET _MAX_LOOP            |
            auto jump_kind = static_cast<ByteCodeValueType>(greedy ? OpCodeId::ForkStay : OpCodeId::ForkJump);
            if (maximum.value() > minimum) {
                new_bytecode.empend(jump_kind);
                new_bytecode.empend((ByteCodeValueType)0); // Placeholder for the jump target.
                auto pre_loop_fork_jump_index = new_bytecode.size();
                new_bytecode.extend(bytecode_to_repeat);
                auto repetitions = maximum.value() - minimum;
                auto fork_jump_address = new_bytecode.size();
                if (repetitions > 1) {
                    new_bytecode.empend((ByteCodeValueType)OpCodeId::Repeat);
                    new_bytecode.empend(bytecode_to_repeat.size() + 2);
                    new_bytecode.empend(static_cast<ByteCodeValueType>(repetitions - 1));
                    new_bytecode.empend(max_repetition_mark_id);
                    new_bytecode.empend(jump_kind);
                    new_bytecode.empend((ByteCodeValueType)0); // Placeholder for the jump target.
                    auto post_loop_fork_jump_index = new_bytecode.size();
                    new_bytecode.extend(bytecode_to_repeat);
                    fork_jump_address = new_bytecode.size();

                    new_bytecode[post_loop_fork_jump_index - 1] = (ByteCodeValueType)(fork_jump_address - post_loop_fork_jump_index);

                    new_bytecode.empend((ByteCodeValueType)OpCodeId::ResetRepeat);
                    new_bytecode.empend((ByteCodeValueType)max_repetition_mark_id);
                }
                new_bytecode[pre_loop_fork_jump_index - 1] = (ByteCodeValueType)(fork_jump_address - pre_loop_fork_jump_index);
            }
        } else {
            // no maximum value set, repeat finding if possible:
            // (REPEAT REGEXP MIN)
            // LABEL _START
            // CHECKPOINT _C
            // REGEXP
            // JUMP_NONEMPTY _C _START FORK

            // Note: This is only safe because REPEAT will leave one iteration outside (see repetition_n)
            auto checkpoint = s_next_checkpoint_serial_id++;
            new_bytecode.insert(new_bytecode.size() - bytecode_to_repeat.size(), (ByteCodeValueType)OpCodeId::Checkpoint);
            new_bytecode.insert(new_bytecode.size() - bytecode_to_repeat.size(), (ByteCodeValueType)checkpoint);

            auto jump_kind = static_cast<ByteCodeValueType>(greedy ? OpCodeId::ForkJump : OpCodeId::ForkStay);
            new_bytecode.empend((ByteCodeValueType)OpCodeId::JumpNonEmpty);
            new_bytecode.empend(-bytecode_to_repeat.size() - 4 - 2); // Jump to the last iteration
            new_bytecode.empend(checkpoint);                         // if _C is not empty.
            new_bytecode.empend(jump_kind);
        }

        bytecode_to_repeat = move(new_bytecode);
    }

    template<Integral T>
    void insert_bytecode_repetition_n(ByteCode& bytecode_to_repeat, T n, size_t repetition_mark_id)
    {
        // LABEL _LOOP
        // REGEXP
        // REPEAT _LOOP N-1
        // REGEXP
        if (n == 0)
            return;

        // Note: this bytecode layout allows callers to repeat the last REGEXP instruction without the
        // REPEAT instruction forcing another loop.
        extend(bytecode_to_repeat);

        if (n > 1) {
            empend(static_cast<ByteCodeValueType>(OpCodeId::Repeat));
            empend(bytecode_to_repeat.size());
            empend(static_cast<ByteCodeValueType>(n - 1));
            empend(repetition_mark_id);

            extend(bytecode_to_repeat);
        }
    }

    static void transform_bytecode_repetition_min_one(ByteCode& bytecode_to_repeat, bool greedy)
    {
        // LABEL _START = -bytecode_to_repeat.size()
        // CHECKPOINT _C
        // REGEXP
        // JUMP_NONEMPTY _C _START FORKSTAY (FORKJUMP -> Greedy)

        auto checkpoint = s_next_checkpoint_serial_id++;
        bytecode_to_repeat.prepend((ByteCodeValueType)checkpoint);
        bytecode_to_repeat.prepend((ByteCodeValueType)OpCodeId::Checkpoint);

        bytecode_to_repeat.empend((ByteCodeValueType)OpCodeId::JumpNonEmpty);
        bytecode_to_repeat.empend(-bytecode_to_repeat.size() - 3); // Jump to the _START label...
        bytecode_to_repeat.empend(checkpoint);                     // ...if _C is not empty

        if (greedy)
            bytecode_to_repeat.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));
        else
            bytecode_to_repeat.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkStay));
    }

    static void transform_bytecode_repetition_any(ByteCode& bytecode_to_repeat, bool greedy)
    {
        // LABEL _START
        // FORKJUMP _END  (FORKSTAY -> Greedy)
        // CHECKPOINT _C
        // REGEXP
        // JUMP_NONEMPTY _C _START JUMP
        // LABEL _END

        // LABEL _START = m_bytes.size();
        ByteCode bytecode;

        if (greedy)
            bytecode.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkStay));
        else
            bytecode.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));

        bytecode.empend(bytecode_to_repeat.size() + 2 + 4); // Jump to the _END label

        auto checkpoint = s_next_checkpoint_serial_id++;
        bytecode.empend(static_cast<ByteCodeValueType>(OpCodeId::Checkpoint));
        bytecode.empend(static_cast<ByteCodeValueType>(checkpoint));

        bytecode.extend(bytecode_to_repeat);

        bytecode.empend(static_cast<ByteCodeValueType>(OpCodeId::JumpNonEmpty));
        bytecode.empend(-bytecode.size() - 3); // Jump(...) to the _START label...
        bytecode.empend(checkpoint);           // ...only if _C passes.
        bytecode.empend((ByteCodeValueType)OpCodeId::Jump);
        // LABEL _END = bytecode.size()

        bytecode_to_repeat = move(bytecode);
    }

    static void transform_bytecode_repetition_zero_or_one(ByteCode& bytecode_to_repeat, bool greedy)
    {
        // FORKJUMP _END (FORKSTAY -> Greedy)
        // REGEXP
        // LABEL _END
        ByteCode bytecode;

        if (greedy)
            bytecode.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkStay));
        else
            bytecode.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));

        bytecode.empend(bytecode_to_repeat.size()); // Jump to the _END label

        bytecode.extend(move(bytecode_to_repeat));
        // LABEL _END = bytecode.size()

        bytecode_to_repeat = move(bytecode);
    }

    OpCode& get_opcode(MatchState& state) const;

    static void reset_checkpoint_serial_id() { s_next_checkpoint_serial_id = 0; }

private:
    void insert_string(StringView view)
    {
        empend((ByteCodeValueType)view.length());
        for (size_t i = 0; i < view.length(); ++i)
            empend((ByteCodeValueType)view[i]);
    }

    void ensure_opcodes_initialized();
    ALWAYS_INLINE OpCode& get_opcode_by_id(OpCodeId id) const;
    static OwnPtr<OpCode> s_opcodes[(size_t)OpCodeId::Last + 1];
    static bool s_opcodes_initialized;
    static size_t s_next_checkpoint_serial_id;
    StringTable m_string_table;
};

#define ENUMERATE_EXECUTION_RESULTS                          \
    __ENUMERATE_EXECUTION_RESULT(Continue)                   \
    __ENUMERATE_EXECUTION_RESULT(Fork_PrioHigh)              \
    __ENUMERATE_EXECUTION_RESULT(Fork_PrioLow)               \
    __ENUMERATE_EXECUTION_RESULT(Failed)                     \
    __ENUMERATE_EXECUTION_RESULT(Failed_ExecuteLowPrioForks) \
    __ENUMERATE_EXECUTION_RESULT(Succeeded)

enum class ExecutionResult : u8 {
#define __ENUMERATE_EXECUTION_RESULT(x) x,
    ENUMERATE_EXECUTION_RESULTS
#undef __ENUMERATE_EXECUTION_RESULT
};

StringView execution_result_name(ExecutionResult result);
StringView opcode_id_name(OpCodeId opcode_id);
StringView boundary_check_type_name(BoundaryCheckType);
StringView character_compare_type_name(CharacterCompareType result);
StringView character_class_name(CharClass ch_class);

class REGEX_API OpCode {
public:
    OpCode() = default;
    virtual ~OpCode() = default;

    virtual OpCodeId opcode_id() const = 0;
    virtual size_t size() const = 0;
    virtual ExecutionResult execute(MatchInput const& input, MatchState& state) const = 0;

    ALWAYS_INLINE ByteCodeValueType argument(size_t offset) const
    {
        return m_bytecode->at(state().instruction_position + 1 + offset);
    }

    ALWAYS_INLINE StringView name() const;
    static StringView name(OpCodeId);

    ALWAYS_INLINE void set_state(MatchState const& state) { m_state = &state; }

    ALWAYS_INLINE void set_bytecode(ByteCode& bytecode) { m_bytecode = &bytecode; }

    ALWAYS_INLINE MatchState const& state() const
    {
        VERIFY(m_state);
        return *m_state;
    }

    ByteString to_byte_string() const
    {
        return ByteString::formatted("[{:#02X}] {}", (int)opcode_id(), name(opcode_id()));
    }

    virtual ByteString arguments_string() const = 0;

    ALWAYS_INLINE ByteCode const& bytecode() const { return *m_bytecode; }

protected:
    ByteCode* m_bytecode { nullptr };
    MatchState const* m_state { nullptr };
};

class OpCode_Exit final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::Exit; }
    ALWAYS_INLINE size_t size() const override { return 1; }
    ByteString arguments_string() const override { return ByteString::empty(); }
};

class OpCode_FailForks final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::FailForks; }
    ALWAYS_INLINE size_t size() const override { return 1; }
    ByteString arguments_string() const override { return ByteString::empty(); }
};

class OpCode_PopSaved final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::PopSaved; }
    ALWAYS_INLINE size_t size() const override { return 1; }
    ByteString arguments_string() const override { return ByteString::empty(); }
};

class OpCode_Save final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::Save; }
    ALWAYS_INLINE size_t size() const override { return 1; }
    ByteString arguments_string() const override { return ByteString::empty(); }
};

class OpCode_Restore final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::Restore; }
    ALWAYS_INLINE size_t size() const override { return 1; }
    ByteString arguments_string() const override { return ByteString::empty(); }
};

class OpCode_GoBack final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::GoBack; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE size_t count() const { return argument(0); }
    ByteString arguments_string() const override { return ByteString::formatted("count={}", count()); }
};

class OpCode_Jump final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::Jump; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE ssize_t offset() const { return argument(0); }
    ByteString arguments_string() const override
    {
        return ByteString::formatted("offset={} [&{}]", offset(), state().instruction_position + size() + offset());
    }
};

class OpCode_ForkJump : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::ForkJump; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE ssize_t offset() const { return argument(0); }
    ByteString arguments_string() const override
    {
        return ByteString::formatted("offset={} [&{}], sp: {}", offset(), state().instruction_position + size() + offset(), state().string_position);
    }
};

class OpCode_ForkReplaceJump final : public OpCode_ForkJump {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::ForkReplaceJump; }
};

class OpCode_ForkStay : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::ForkStay; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE ssize_t offset() const { return argument(0); }
    ByteString arguments_string() const override
    {
        return ByteString::formatted("offset={} [&{}], sp: {}", offset(), state().instruction_position + size() + offset(), state().string_position);
    }
};

class OpCode_ForkReplaceStay final : public OpCode_ForkStay {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::ForkReplaceStay; }
};

class OpCode_CheckBegin final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::CheckBegin; }
    ALWAYS_INLINE size_t size() const override { return 1; }
    ByteString arguments_string() const override { return ByteString::empty(); }
};

class OpCode_CheckEnd final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::CheckEnd; }
    ALWAYS_INLINE size_t size() const override { return 1; }
    ByteString arguments_string() const override { return ByteString::empty(); }
};

class OpCode_CheckBoundary final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::CheckBoundary; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE size_t arguments_count() const { return 1; }
    ALWAYS_INLINE BoundaryCheckType type() const { return static_cast<BoundaryCheckType>(argument(0)); }
    ByteString arguments_string() const override { return ByteString::formatted("kind={} ({})", (long unsigned int)argument(0), boundary_check_type_name(type())); }
};

class OpCode_ClearCaptureGroup final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::ClearCaptureGroup; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE size_t id() const { return argument(0); }
    ByteString arguments_string() const override { return ByteString::formatted("id={}", id()); }
};

class OpCode_SaveLeftCaptureGroup final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::SaveLeftCaptureGroup; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE size_t id() const { return argument(0); }
    ByteString arguments_string() const override { return ByteString::formatted("id={}", id()); }
};

class OpCode_SaveRightCaptureGroup final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::SaveRightCaptureGroup; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE size_t id() const { return argument(0); }
    ByteString arguments_string() const override { return ByteString::formatted("id={}", id()); }
};

class OpCode_SaveRightNamedCaptureGroup final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::SaveRightNamedCaptureGroup; }
    ALWAYS_INLINE size_t size() const override { return 3; }
    ALWAYS_INLINE FlyString name() const { return m_bytecode->get_string(name_string_table_index()); }
    ALWAYS_INLINE size_t name_string_table_index() const { return argument(0); }
    ALWAYS_INLINE size_t length() const { return name().bytes_as_string_view().length(); }
    ALWAYS_INLINE size_t id() const { return argument(1); }
    ByteString arguments_string() const override
    {
        return ByteString::formatted("name_id={}, id={}", argument(0), id());
    }
};

class REGEX_API OpCode_Compare final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::Compare; }
    ALWAYS_INLINE size_t size() const override { return arguments_size() + 3; }
    ALWAYS_INLINE size_t arguments_count() const { return argument(0); }
    ALWAYS_INLINE size_t arguments_size() const { return argument(1); }
    ByteString arguments_string() const override;
    Vector<ByteString> variable_arguments_to_byte_string(Optional<MatchInput const&> input = {}) const;
    Vector<CompareTypeAndValuePair> flat_compares() const;
    static bool matches_character_class(CharClass, u32, bool insensitive);

private:
    ALWAYS_INLINE static void compare_char(MatchInput const& input, MatchState& state, u32 ch1, bool inverse, bool& inverse_matched);
    ALWAYS_INLINE static bool compare_string(MatchInput const& input, MatchState& state, RegexStringView str, bool& had_zero_length_match);
    ALWAYS_INLINE static void compare_character_class(MatchInput const& input, MatchState& state, CharClass character_class, u32 ch, bool inverse, bool& inverse_matched);
    ALWAYS_INLINE static void compare_character_range(MatchInput const& input, MatchState& state, u32 from, u32 to, u32 ch, bool inverse, bool& inverse_matched);
    ALWAYS_INLINE static void compare_property(MatchInput const& input, MatchState& state, Unicode::Property property, bool inverse, bool& inverse_matched);
    ALWAYS_INLINE static void compare_general_category(MatchInput const& input, MatchState& state, Unicode::GeneralCategory general_category, bool inverse, bool& inverse_matched);
    ALWAYS_INLINE static void compare_script(MatchInput const& input, MatchState& state, Unicode::Script script, bool inverse, bool& inverse_matched);
    ALWAYS_INLINE static void compare_script_extension(MatchInput const& input, MatchState& state, Unicode::Script script, bool inverse, bool& inverse_matched);
};

class OpCode_Repeat : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::Repeat; }
    ALWAYS_INLINE size_t size() const override { return 4; }
    ALWAYS_INLINE size_t offset() const { return argument(0); }
    ALWAYS_INLINE u64 count() const { return argument(1); }
    ALWAYS_INLINE size_t id() const { return argument(2); }
    ByteString arguments_string() const override
    {
        auto reps = id() < state().repetition_marks.size() ? state().repetition_marks.at(id()) : 0;
        return ByteString::formatted("offset={} [&{}] count={} id={} rep={}, sp: {}",
            static_cast<ssize_t>(offset()),
            state().instruction_position - offset(),
            count() + 1,
            id(),
            reps + 1,
            state().string_position);
    }
};

class OpCode_ResetRepeat : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::ResetRepeat; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE size_t id() const { return argument(0); }
    ByteString arguments_string() const override
    {
        auto reps = id() < state().repetition_marks.size() ? state().repetition_marks.at(id()) : 0;
        return ByteString::formatted("id={} rep={}", id(), reps + 1);
    }
};

class OpCode_Checkpoint final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::Checkpoint; }
    ALWAYS_INLINE size_t size() const override { return 2; }
    ALWAYS_INLINE size_t id() const { return argument(0); }
    ByteString arguments_string() const override { return ByteString::formatted("id={}", id()); }
};

class OpCode_JumpNonEmpty final : public OpCode {
public:
    ExecutionResult execute(MatchInput const& input, MatchState& state) const override;
    ALWAYS_INLINE OpCodeId opcode_id() const override { return OpCodeId::JumpNonEmpty; }
    ALWAYS_INLINE size_t size() const override { return 4; }
    ALWAYS_INLINE ssize_t offset() const { return argument(0); }
    ALWAYS_INLINE ssize_t checkpoint() const { return argument(1); }
    ALWAYS_INLINE OpCodeId form() const { return (OpCodeId)argument(2); }
    ByteString arguments_string() const override
    {
        return ByteString::formatted("{} offset={} [&{}], cp={}",
            opcode_id_name(form()),
            offset(), state().instruction_position + size() + offset(),
            checkpoint());
    }
};

ALWAYS_INLINE OpCode& ByteCode::get_opcode(regex::MatchState& state) const
{
    OpCodeId opcode_id;
    if (auto opcode_ptr = static_cast<DisjointChunks<ByteCodeValueType> const&>(*this).find(state.instruction_position))
        opcode_id = (OpCodeId)*opcode_ptr;
    else
        opcode_id = OpCodeId::Exit;

    auto& opcode = get_opcode_by_id(opcode_id);
    opcode.set_state(state);
    return opcode;
}

ALWAYS_INLINE OpCode& ByteCode::get_opcode_by_id(OpCodeId id) const
{
    VERIFY(id >= OpCodeId::First && id <= OpCodeId::Last);

    auto& opcode = s_opcodes[(u32)id];
    opcode->set_bytecode(*const_cast<ByteCode*>(this));
    return *opcode;
}

template<typename T>
bool is(OpCode const&);

template<typename T>
ALWAYS_INLINE bool is(OpCode const&)
{
    return false;
}

template<typename T>
ALWAYS_INLINE bool is(OpCode const* opcode)
{
    return is<T>(*opcode);
}

template<>
ALWAYS_INLINE bool is<OpCode_ForkStay>(OpCode const& opcode)
{
    return opcode.opcode_id() == OpCodeId::ForkStay;
}

template<>
ALWAYS_INLINE bool is<OpCode_Exit>(OpCode const& opcode)
{
    return opcode.opcode_id() == OpCodeId::Exit;
}

template<>
ALWAYS_INLINE bool is<OpCode_Compare>(OpCode const& opcode)
{
    return opcode.opcode_id() == OpCodeId::Compare;
}

template<typename T>
ALWAYS_INLINE T const& to(OpCode const& opcode)
{
    return as<T>(opcode);
}

template<typename T>
ALWAYS_INLINE T* to(OpCode* opcode)
{
    return as<T>(opcode);
}

template<typename T>
ALWAYS_INLINE T const* to(OpCode const* opcode)
{
    return as<T>(opcode);
}

template<typename T>
ALWAYS_INLINE T& to(OpCode& opcode)
{
    return as<T>(opcode);
}

}
