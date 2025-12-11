/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RegexByteCode.h"
#include "RegexDebug.h"

#include <AK/BinarySearch.h>
#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <LibUnicode/CharacterTypes.h>

// U+2028 LINE SEPARATOR
constexpr static u32 const LineSeparator { 0x2028 };
// U+2029 PARAGRAPH SEPARATOR
constexpr static u32 const ParagraphSeparator { 0x2029 };

namespace regex {

template<typename ByteCode>
StringView OpCode<ByteCode>::name() const
{
    return name(opcode_id());
}

StringView execution_result_name(ExecutionResult result)
{
    switch (result) {
#define __ENUMERATE_EXECUTION_RESULT(x) \
    case ExecutionResult::x:            \
        return #x##sv;
        ENUMERATE_EXECUTION_RESULTS
#undef __ENUMERATE_EXECUTION_RESULT
    default:
        VERIFY_NOT_REACHED();
        return "<Unknown>"sv;
    }
}

StringView opcode_id_name(OpCodeId opcode)
{
    switch (opcode) {
#define __ENUMERATE_OPCODE(x) \
    case OpCodeId::x:         \
        return #x##sv;

        ENUMERATE_OPCODES

#undef __ENUMERATE_OPCODE
    default:
        VERIFY_NOT_REACHED();
        return "<Unknown>"sv;
    }
}

StringView fork_if_condition_name(ForkIfCondition condition)
{
    switch (condition) {
#define __ENUMERATE_FORK_IF_CONDITION(x) \
    case ForkIfCondition::x:             \
        return #x##sv;
        ENUMERATE_FORK_IF_CONDITIONS
#undef __ENUMERATE_FORK_IF_CONDITION
    default:
        return "<Unknown>"sv;
    }
}

StringView boundary_check_type_name(BoundaryCheckType ty)
{
    switch (ty) {
#define __ENUMERATE_BOUNDARY_CHECK_TYPE(x) \
    case BoundaryCheckType::x:             \
        return #x##sv;
        ENUMERATE_BOUNDARY_CHECK_TYPES
#undef __ENUMERATE_BOUNDARY_CHECK_TYPE
    default:
        VERIFY_NOT_REACHED();
        return "<Unknown>"sv;
    }
}

StringView character_compare_type_name(CharacterCompareType ch_compare_type)
{
    switch (ch_compare_type) {
#define __ENUMERATE_CHARACTER_COMPARE_TYPE(x) \
    case CharacterCompareType::x:             \
        return #x##sv;
        ENUMERATE_CHARACTER_COMPARE_TYPES
#undef __ENUMERATE_CHARACTER_COMPARE_TYPE
    default:
        VERIFY_NOT_REACHED();
        return "<Unknown>"sv;
    }
}

StringView character_class_name(CharClass ch_class)
{
    switch (ch_class) {
#define __ENUMERATE_CHARACTER_CLASS(x) \
    case CharClass::x:                 \
        return #x##sv;
        ENUMERATE_CHARACTER_CLASSES
#undef __ENUMERATE_CHARACTER_CLASS
    default:
        VERIFY_NOT_REACHED();
        return "<Unknown>"sv;
    }
}

static void advance_string_position(MatchState& state, RegexStringView view, Optional<u32> code_point = {})
{
    ++state.string_position;

    if (view.unicode()) {
        if (!code_point.has_value() && (state.string_position_in_code_units < view.length_in_code_units()))
            code_point = view.code_point_at(state.string_position_in_code_units);
        if (code_point.has_value())
            state.string_position_in_code_units += view.length_of_code_point(*code_point);
    } else {
        ++state.string_position_in_code_units;
    }
}

static void advance_string_position(MatchState& state, RegexStringView, RegexStringView advance_by)
{
    state.string_position += advance_by.length();
    state.string_position_in_code_units += advance_by.length_in_code_units();
}

static void reverse_string_position(MatchState& state, RegexStringView view, size_t amount)
{
    VERIFY(state.string_position >= amount);
    state.string_position -= amount;

    if (view.unicode())
        state.string_position_in_code_units = view.code_unit_offset_of(state.string_position);
    else
        state.string_position_in_code_units -= amount;
}

static void save_string_position(MatchInput const& input, MatchState const& state)
{
    input.saved_positions.append(state.string_position);
    input.saved_forks_since_last_save.append(state.forks_since_last_save);
    input.saved_code_unit_positions.append(state.string_position_in_code_units);
}

static bool restore_string_position(MatchInput const& input, MatchState& state)
{
    if (input.saved_positions.is_empty())
        return false;

    state.string_position = input.saved_positions.take_last();
    state.string_position_in_code_units = input.saved_code_unit_positions.take_last();
    state.forks_since_last_save = input.saved_forks_since_last_save.take_last();
    return true;
}

OwnPtr<OpCode<ByteCode>> ByteCode::s_opcodes[(size_t)OpCodeId::Last + 1];
bool ByteCode::s_opcodes_initialized { false };

OwnPtr<OpCode<FlatByteCode>> FlatByteCode::s_opcodes[(size_t)OpCodeId::Last + 1];
bool FlatByteCode::s_opcodes_initialized { false };

size_t ByteCode::s_next_checkpoint_serial_id { 0 };
u32 s_next_string_table_serial { 1 };
static u32 s_next_string_set_table_serial { 1 };

StringSetTable::StringSetTable()
    : m_serial(s_next_string_set_table_serial++)
{
}

StringSetTable::~StringSetTable()
{
    if (m_serial == s_next_string_set_table_serial - 1 && m_u8_tries.is_empty())
        --s_next_string_set_table_serial;
}

StringSetTable::StringSetTable(StringSetTable const& other)
    : m_serial(s_next_string_set_table_serial++)
{
    for (auto const& entry : other.m_u8_tries)
        m_u8_tries.set(entry.key, MUST(const_cast<StringSetTrie&>(entry.value).deep_copy()));
    for (auto const& entry : other.m_u16_tries)
        m_u16_tries.set(entry.key, MUST(const_cast<StringSetTrie&>(entry.value).deep_copy()));
}

StringSetTable& StringSetTable::operator=(StringSetTable const& other)
{
    if (this != &other) {
        m_u8_tries.clear();
        m_u16_tries.clear();
        for (auto const& entry : other.m_u8_tries)
            m_u8_tries.set(entry.key, MUST(const_cast<StringSetTrie&>(entry.value).deep_copy()));
        for (auto const& entry : other.m_u16_tries)
            m_u16_tries.set(entry.key, MUST(const_cast<StringSetTrie&>(entry.value).deep_copy()));
    }
    return *this;
}

void ByteCode::ensure_opcodes_initialized()
{
    if (s_opcodes_initialized)
        return;
    for (u32 i = (u32)OpCodeId::First; i <= (u32)OpCodeId::Last; ++i) {
        switch ((OpCodeId)i) {
#define __ENUMERATE_OPCODE(OpCode)                        \
    case OpCodeId::OpCode:                                \
        s_opcodes[i] = make<OpCode_##OpCode<ByteCode>>(); \
        break;

            ENUMERATE_OPCODES

#undef __ENUMERATE_OPCODE
        }
    }
    s_opcodes_initialized = true;
}

void FlatByteCode::ensure_opcodes_initialized()
{
    if (s_opcodes_initialized)
        return;
    for (u32 i = (u32)OpCodeId::First; i <= (u32)OpCodeId::Last; ++i) {
        switch ((OpCodeId)i) {
#define __ENUMERATE_OPCODE(OpCode)                            \
    case OpCodeId::OpCode:                                    \
        s_opcodes[i] = make<OpCode_##OpCode<FlatByteCode>>(); \
        break;

            ENUMERATE_OPCODES

#undef __ENUMERATE_OPCODE
        }
    }
    s_opcodes_initialized = true;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_Exit<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    if (state.string_position > input.view.length() || state.instruction_position >= bytecode().size())
        return ExecutionResult::Succeeded;

    return ExecutionResult::Failed;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_Save<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    save_string_position(input, state);
    state.forks_since_last_save = 0;
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_Restore<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    if (!restore_string_position(input, state))
        return ExecutionResult::Failed;
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_GoBack<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    if (count() > state.string_position)
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    reverse_string_position(state, input.view, count());
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_SetStepBack<ByteCode>::execute(MatchInput const&, MatchState& state) const
{
    state.step_backs.append(step());
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_IncStepBack<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    if (state.step_backs.is_empty())
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    size_t last_step_back = static_cast<size_t>(++state.step_backs.last());

    if (last_step_back > state.string_position)
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    reverse_string_position(state, input.view, last_step_back);
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_CheckStepBack<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    if (state.step_backs.is_empty())
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    if (input.saved_positions.is_empty())
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    // NOTE: Fail if the step-back would move before the lookbehind start.
    if (static_cast<size_t>(state.step_backs.last()) > input.saved_positions.last())
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    // NOTE: Restores the string position saved before executing a lookbehind.
    state.string_position = input.saved_positions.last();
    state.string_position_in_code_units = input.saved_code_unit_positions.last();
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_CheckSavedPosition<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    if (input.saved_positions.is_empty())
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    if (state.string_position != input.saved_positions.last())
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    state.step_backs.take_last();
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_FailForks<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    input.fail_counter += state.forks_since_last_save;
    return ExecutionResult::Failed_ExecuteLowPrioForks;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_PopSaved<ByteCode>::execute(MatchInput const& input, MatchState&) const
{
    if (input.saved_positions.is_empty() || input.saved_code_unit_positions.is_empty())
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    input.saved_positions.take_last();
    input.saved_code_unit_positions.take_last();
    return ExecutionResult::Failed_ExecuteLowPrioForks;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_Jump<ByteCode>::execute(MatchInput const&, MatchState& state) const
{
    state.instruction_position += offset();
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_ForkJump<ByteCode>::execute(MatchInput const&, MatchState& state) const
{
    state.fork_at_position = state.instruction_position + size() + offset();
    state.forks_since_last_save++;
    return ExecutionResult::Fork_PrioHigh;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_ForkReplaceJump<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    state.fork_at_position = state.instruction_position + size() + offset();
    input.fork_to_replace = state.instruction_position;
    state.forks_since_last_save++;
    return ExecutionResult::Fork_PrioHigh;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_ForkStay<ByteCode>::execute(MatchInput const&, MatchState& state) const
{
    state.fork_at_position = state.instruction_position + size() + offset();
    state.forks_since_last_save++;
    return ExecutionResult::Fork_PrioLow;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_ForkReplaceStay<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    state.fork_at_position = state.instruction_position + size() + offset();
    input.fork_to_replace = state.instruction_position;
    return ExecutionResult::Fork_PrioLow;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_ForkIf<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    auto next_step = [&](bool do_fork) -> ExecutionResult {
        switch (form()) {
        case OpCodeId::ForkJump:
            if (do_fork) {
                state.fork_at_position = state.instruction_position + size() + offset();
                state.forks_since_last_save++;
                return ExecutionResult::Fork_PrioHigh;
            }
            return ExecutionResult::Continue;
        case OpCodeId::ForkReplaceJump:
            if (do_fork) {
                state.fork_at_position = state.instruction_position + size() + offset();
                input.fork_to_replace = state.instruction_position;
                state.forks_since_last_save++;
                return ExecutionResult::Fork_PrioHigh;
            }
            return ExecutionResult::Continue;
        case OpCodeId::ForkStay:
            if (do_fork) {
                state.fork_at_position = state.instruction_position + size() + offset();
                state.forks_since_last_save++;
                return ExecutionResult::Fork_PrioLow;
            }
            state.instruction_position += offset();
            return ExecutionResult::Continue;
        case OpCodeId::ForkReplaceStay:
            if (do_fork) {
                state.fork_at_position = state.instruction_position + size() + offset();
                input.fork_to_replace = state.instruction_position;
                return ExecutionResult::Fork_PrioLow;
            }
            state.instruction_position += offset();
            return ExecutionResult::Continue;
        default:
            VERIFY_NOT_REACHED();
        }
    };

    switch (condition()) {
    case ForkIfCondition::AtStartOfLine:
        return next_step(!input.in_the_middle_of_a_line);
    case ForkIfCondition::Invalid:
    default:
        VERIFY_NOT_REACHED();
    }
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_CheckBegin<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    auto is_at_line_boundary = [&] {
        if (state.string_position == 0)
            return true;

        if (input.regex_options.has_flag_set(AllFlags::Multiline) && input.regex_options.has_flag_set(AllFlags::Internal_ConsiderNewline)) {
            auto input_view = input.view.substring_view(state.string_position - 1, 1).code_point_at(0);
            return input_view == '\r' || input_view == '\n' || input_view == LineSeparator || input_view == ParagraphSeparator;
        }

        return false;
    }();

    if (is_at_line_boundary && (input.regex_options & AllFlags::MatchNotBeginOfLine))
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    if ((is_at_line_boundary && !(input.regex_options & AllFlags::MatchNotBeginOfLine))
        || (!is_at_line_boundary && (input.regex_options & AllFlags::MatchNotBeginOfLine))
        || (is_at_line_boundary && (input.regex_options & AllFlags::Global)))
        return ExecutionResult::Continue;

    return ExecutionResult::Failed_ExecuteLowPrioForks;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_CheckBoundary<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    auto isword = [](auto ch) { return is_ascii_alphanumeric(ch) || ch == '_'; };
    auto is_word_boundary = [&] {
        if (state.string_position == input.view.length()) {
            return (state.string_position > 0 && isword(input.view.code_point_at(state.string_position_in_code_units - 1)));
        }

        if (state.string_position == 0) {
            return (isword(input.view.code_point_at(0)));
        }

        return !!(isword(input.view.code_point_at(state.string_position_in_code_units)) ^ isword(input.view.code_point_at(state.string_position_in_code_units - 1)));
    };
    switch (type()) {
    case BoundaryCheckType::Word: {
        if (is_word_boundary())
            return ExecutionResult::Continue;
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    }
    case BoundaryCheckType::NonWord: {
        if (!is_word_boundary())
            return ExecutionResult::Continue;
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    }
    }
    VERIFY_NOT_REACHED();
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_CheckEnd<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    auto is_at_line_boundary = [&] {
        if (state.string_position == input.view.length())
            return true;

        if (input.regex_options.has_flag_set(AllFlags::Multiline) && input.regex_options.has_flag_set(AllFlags::Internal_ConsiderNewline)) {
            auto input_view = input.view.substring_view(state.string_position, 1).code_point_at(0);
            return input_view == '\r' || input_view == '\n' || input_view == LineSeparator || input_view == ParagraphSeparator;
        }

        return false;
    }();
    if (is_at_line_boundary && (input.regex_options & AllFlags::MatchNotEndOfLine))
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    if ((is_at_line_boundary && !(input.regex_options & AllFlags::MatchNotEndOfLine))
        || (!is_at_line_boundary && (input.regex_options & AllFlags::MatchNotEndOfLine || input.regex_options & AllFlags::MatchNotBeginOfLine)))
        return ExecutionResult::Continue;

    return ExecutionResult::Failed_ExecuteLowPrioForks;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_ClearCaptureGroup<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    if (input.match_index < state.capture_group_matches_size()) {
        auto group = state.mutable_capture_group_matches(input.match_index);
        group[id() - 1].reset();
    }
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_SaveLeftCaptureGroup<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    if (input.match_index >= state.capture_group_matches_size()) {
        state.flat_capture_group_matches.ensure_capacity((input.match_index + 1) * state.capture_group_count);
        for (size_t i = state.capture_group_matches_size(); i <= input.match_index; ++i)
            for (size_t j = 0; j < state.capture_group_count; ++j)
                state.flat_capture_group_matches.append({});
    }

    state.mutable_capture_group_matches(input.match_index).at(id() - 1).left_column = state.string_position;
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_SaveRightCaptureGroup<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    auto& match = state.capture_group_matches(input.match_index).at(id() - 1);
    auto start_position = match.left_column;
    if (state.string_position < start_position) {
        dbgln("Right capture group {} is before left capture group {}!", state.string_position, start_position);
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    }

    auto length = state.string_position - start_position;

    if (start_position < match.column && state.step_backs.is_empty())
        return ExecutionResult::Continue;

    VERIFY(start_position + length <= input.view.length_in_code_units());

    auto captured_text = input.view.substring_view(start_position, length);

    // NOTE: Don't overwrite existing capture with empty match at the same position. The ECMA-262 RepeatMatcher
    // continuation chain effectively preserves captures when an empty match occurs at the position where the
    // existing capture ended.
    // See: https://tc39.es/ecma262/#step-repeatmatcher-done
    auto& existing_capture = state.mutable_capture_group_matches(input.match_index).at(id() - 1);
    if (length == 0 && !existing_capture.view.is_null() && existing_capture.view.length() > 0) {
        auto existing_end_position = existing_capture.global_offset - input.global_offset + existing_capture.view.length();
        if (existing_end_position == state.string_position) {
            return ExecutionResult::Continue;
        }
    }

    state.mutable_capture_group_matches(input.match_index).at(id() - 1) = { captured_text, input.line, start_position, input.global_offset + start_position };

    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_SaveRightNamedCaptureGroup<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    auto& match = state.capture_group_matches(input.match_index).at(id() - 1);
    auto start_position = match.left_column;
    if (state.string_position < start_position)
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    auto length = state.string_position - start_position;

    if (start_position < match.column)
        return ExecutionResult::Continue;

    VERIFY(start_position + length <= input.view.length_in_code_units());

    auto view = input.view.substring_view(start_position, length);

    // Same logic as in SaveRightCaptureGroup above.
    // https://tc39.es/ecma262/#step-repeatmatcher-done
    auto& existing_capture = state.mutable_capture_group_matches(input.match_index).at(id() - 1);
    if (length == 0 && !existing_capture.view.is_null() && existing_capture.view.length() > 0) {
        auto existing_end_position = existing_capture.global_offset - input.global_offset + existing_capture.view.length();
        if (existing_end_position == state.string_position) {
            return ExecutionResult::Continue;
        }
    }

    state.mutable_capture_group_matches(input.match_index).at(id() - 1) = { view, name_string_table_index(), input.line, start_position, input.global_offset + start_position };

    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_RSeekTo<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    auto ch = argument(0);
    auto last_position = exchange(state.string_position_before_rseek, state.string_position);
    auto last_position_in_code_units = exchange(state.string_position_in_code_units_before_rseek, state.string_position_in_code_units);
    auto next = input.view.find_index_of_previous(ch, last_position, last_position_in_code_units);
    if (!next.has_value())
        return ExecutionResult::Failed_ExecuteLowPrioForksButNoFurtherPossibleMatches;
    state.string_position = next->code_point_index;
    state.string_position_in_code_units = next->code_unit_index;
    return ExecutionResult::Continue;
}

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE ExecutionResult CompareInternals<ByteCode, IsSimple>::execute(MatchInput const& input, MatchState& state) const
{
    auto const argument_count = IsSimple ? 1 : argument(0);
    auto has_single_argument = argument_count == 1;

    bool inverse { false };
    bool temporary_inverse { false };
    bool reset_temp_inverse { false };
    struct DisjunctionState {
        bool active { false };
        bool is_conjunction { false };
        bool is_subtraction { false };
        bool is_and_operation { false };
        bool fail { false };
        bool inverse_matched { false };
        size_t subtraction_operand_index { 0 };
        size_t initial_position;
        size_t initial_code_unit_position;
        Optional<size_t> last_accepted_position {};
        Optional<size_t> last_accepted_code_unit_position {};
    };

    Vector<DisjunctionState, 4> disjunction_states;
    disjunction_states.unchecked_empend();

    auto current_disjunction_state = [&]() -> DisjunctionState& { return disjunction_states.last(); };

    auto current_inversion_state = [&]() -> bool {
        if constexpr (IsSimple)
            return false;
        else
            return temporary_inverse ^ inverse;
    };

    size_t string_position = state.string_position;
    bool inverse_matched { false };
    bool had_zero_length_match { false };

    state.string_position_before_match = state.string_position;

    bool has_string_set = false;
    bool string_set_matched = false;
    size_t best_match_position = state.string_position;
    size_t best_match_position_in_code_units = state.string_position_in_code_units;

    size_t offset { state.instruction_position + (IsSimple ? 2 : 3) };
    CharacterCompareType last_compare_type = CharacterCompareType::Undefined;

    auto const* bytecode_data = bytecode().flat_data().data();

    for (size_t i = 0; i < argument_count; ++i) {
        if (state.string_position > string_position)
            break;

        if (has_string_set) {
            state.string_position = string_position;
            state.string_position_in_code_units = current_disjunction_state().initial_code_unit_position;
        }

        auto compare_type = (CharacterCompareType)bytecode_data[offset++];

        if constexpr (!IsSimple) {
            if (reset_temp_inverse) {
                reset_temp_inverse = false;
                if (compare_type != CharacterCompareType::Property || last_compare_type != CharacterCompareType::StringSet) {
                    temporary_inverse = false;
                }
            } else {
                reset_temp_inverse = true;
            }

            last_compare_type = compare_type;
        }

        switch (compare_type) {
        case CharacterCompareType::Inverse:
            inverse = !inverse;
            continue;
        case CharacterCompareType::TemporaryInverse:
            // If "TemporaryInverse" is given, negate the current inversion state only for the next opcode.
            // it follows that this cannot be the last compare element.
            VERIFY(!IsSimple);
            VERIFY(i != argument_count - 1);

            temporary_inverse = true;
            reset_temp_inverse = false;
            continue;
        case CharacterCompareType::Char: {
            u32 ch = bytecode_data[offset++];

            // We want to compare a string that is longer or equal in length to the available string
            if (input.view.length() <= state.string_position)
                return ExecutionResult::Failed_ExecuteLowPrioForks;

            compare_char(input, state, ch, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::AnyChar: {
            // We want to compare a string that is definitely longer than the available string
            if (input.view.length() <= state.string_position)
                return ExecutionResult::Failed_ExecuteLowPrioForks;

            auto input_view = input.view.substring_view(state.string_position, 1).code_point_at(0);
            auto is_equivalent_to_newline = input_view == '\n'
                || (input.regex_options.has_flag_set(AllFlags::Internal_ECMA262DotSemantics)
                        ? (input_view == '\r' || input_view == LineSeparator || input_view == ParagraphSeparator)
                        : false);

            if (!is_equivalent_to_newline || (input.regex_options.has_flag_set(AllFlags::SingleLine) && input.regex_options.has_flag_set(AllFlags::Internal_ConsiderNewline))) {
                if (current_inversion_state())
                    inverse_matched = true;
                else
                    advance_string_position(state, input.view, input_view);
            }
            break;
        }
        case CharacterCompareType::String: {
            VERIFY(!current_inversion_state());

            auto string_index = bytecode_data[offset++];
            auto string = bytecode().get_u16_string(string_index);

            // We want to compare a string that is definitely longer than the available string
            if (input.view.unicode()) {
                if (input.view.length() < state.string_position + string.length_in_code_points())
                    return ExecutionResult::Failed_ExecuteLowPrioForks;
            } else {
                if (input.view.length() < state.string_position_in_code_units + string.length_in_code_units())
                    return ExecutionResult::Failed_ExecuteLowPrioForks;
            }

            auto view = RegexStringView(string);
            view.set_unicode(input.view.unicode());
            if (compare_string(input, state, view, had_zero_length_match)) {
                if (current_inversion_state())
                    inverse_matched = true;
            }
            break;
        }
        case CharacterCompareType::CharClass: {
            if (input.view.length_in_code_units() <= state.string_position_in_code_units)
                return ExecutionResult::Failed_ExecuteLowPrioForks;

            auto character_class = (CharClass)bytecode_data[offset++];
            auto ch = input.view.unicode_aware_code_point_at(state.string_position_in_code_units);

            compare_character_class(input, state, character_class, ch, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::LookupTable: {
            if (input.view.length() <= state.string_position)
                return ExecutionResult::Failed_ExecuteLowPrioForks;

            auto count_sensitive = bytecode_data[offset++];
            auto count_insensitive = bytecode_data[offset++];
            auto sensitive_range_data = bytecode().flat_data().slice(offset, count_sensitive);
            offset += count_sensitive;
            auto insensitive_range_data = bytecode().flat_data().slice(offset, count_insensitive);
            offset += count_insensitive;

            bool const insensitive = input.regex_options & AllFlags::Insensitive;
            auto ch = input.view.unicode_aware_code_point_at(state.string_position_in_code_units);

            if (insensitive)
                ch = to_ascii_lowercase(ch);

            auto const ranges = insensitive && !insensitive_range_data.is_empty() ? insensitive_range_data : sensitive_range_data;
            auto const* matching_range = binary_search(ranges, ch, nullptr, [](auto needle, CharRange range) {
                if (needle >= range.from && needle <= range.to)
                    return 0;
                if (needle > range.to)
                    return 1;
                return -1;
            });

            if (matching_range) {
                if (current_inversion_state())
                    inverse_matched = true;
                else
                    advance_string_position(state, input.view, ch);
            }
            break;
        }
        case CharacterCompareType::CharRange: {
            if (input.view.length() <= state.string_position)
                return ExecutionResult::Failed_ExecuteLowPrioForks;

            auto value = (CharRange)bytecode_data[offset++];

            auto from = value.from;
            auto to = value.to;
            auto ch = input.view.unicode_aware_code_point_at(state.string_position_in_code_units);

            compare_character_range(input, state, from, to, ch, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::Reference: {
            auto reference_number = ((size_t)bytecode_data[offset++]) - 1;
            if (input.match_index >= state.capture_group_matches_size()) {
                had_zero_length_match = true;
                if (current_inversion_state())
                    inverse_matched = true;
                break;
            }

            auto groups = state.capture_group_matches(input.match_index);

            if (groups.size() <= reference_number) {
                had_zero_length_match = true;
                if (current_inversion_state())
                    inverse_matched = true;
                break;
            }

            auto str = groups.at(reference_number).view;

            // We want to compare a string that is definitely longer than the available string
            if (input.view.length() < state.string_position + str.length())
                return ExecutionResult::Failed_ExecuteLowPrioForks;

            if (compare_string(input, state, str, had_zero_length_match)) {
                if (current_inversion_state())
                    inverse_matched = true;
            }
            break;
        }
        case CharacterCompareType::NamedReference: {
            auto reference_number = ((size_t)bytecode_data[offset++]) - 1;

            if (input.match_index >= state.capture_group_matches_size()) {
                had_zero_length_match = true;
                if (current_inversion_state())
                    inverse_matched = true;
                break;
            }

            auto groups = state.capture_group_matches(input.match_index);

            if (groups.size() <= reference_number) {
                had_zero_length_match = true;
                if (current_inversion_state())
                    inverse_matched = true;
                break;
            }

            RegexStringView str {};

            auto reference_name_index = bytecode().get_group_name_index(reference_number);

            if (reference_name_index.has_value()) {
                auto target_name_string = bytecode().get_string(reference_name_index.value());

                for (size_t i = 0; i < groups.size(); ++i) {
                    if (groups[i].view.is_null())
                        continue;

                    auto group_name_index = bytecode().get_group_name_index(i);

                    if (group_name_index.has_value()) {
                        auto group_name_string = bytecode().get_string(group_name_index.value());

                        if (group_name_string == target_name_string) {
                            str = groups[i].view;
                            break;
                        }
                    }
                }
            }

            if (input.view.length() < state.string_position + str.length()) {
                return ExecutionResult::Failed_ExecuteLowPrioForks;
            }

            if (compare_string(input, state, str, had_zero_length_match)) {
                if (current_inversion_state())
                    inverse_matched = true;
            }
            break;
        }
        case CharacterCompareType::Property: {
            auto property = static_cast<Unicode::Property>(bytecode_data[offset++]);
            compare_property(input, state, property, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::GeneralCategory: {
            auto general_category = static_cast<Unicode::GeneralCategory>(bytecode_data[offset++]);
            compare_general_category(input, state, general_category, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::Script: {
            auto script = static_cast<Unicode::Script>(bytecode_data[offset++]);
            compare_script(input, state, script, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::ScriptExtension: {
            auto script = static_cast<Unicode::Script>(bytecode_data[offset++]);
            compare_script_extension(input, state, script, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::StringSet: {
            has_string_set = true;
            auto string_set_index = bytecode_data[offset++];

            bool matched = false;
            size_t longest_match_length = 0;

            auto find_longest_match = [&](auto const& view, auto const& trie) {
                auto const* current = &trie;
                size_t current_code_unit_offset = state.string_position_in_code_units;

                if (current->has_metadata() && current->metadata_value()) {
                    matched = true;
                    longest_match_length = 0;
                }

                while (true) {
                    u32 value;

                    if constexpr (IsSame<decltype(view), Utf16View const&>) {
                        if (current_code_unit_offset >= view.length_in_code_units())
                            break;
                        value = view.code_unit_at(current_code_unit_offset);
                    } else {
                        if (current_code_unit_offset >= input.view.length_in_code_units())
                            break;
                        value = input.view.code_point_at(current_code_unit_offset);
                    }

                    if (input.regex_options & AllFlags::Insensitive) {
                        bool found_child = false;
                        for (auto const& [key, child] : current->children()) {
                            if (to_ascii_lowercase(key) == to_ascii_lowercase(value)) {
                                current = static_cast<StringSetTrie const*>(child.ptr());
                                current_code_unit_offset++;
                                found_child = true;
                                break;
                            }
                        }
                        if (!found_child)
                            break;
                    } else {
                        auto it = current->children().find(value);
                        if (it == current->children().end())
                            break;

                        current = static_cast<StringSetTrie const*>(it->value.ptr());
                        current_code_unit_offset++;
                    }

                    auto is_terminal = current->has_metadata() && current->metadata_value();
                    if (is_terminal) {
                        size_t match_length_in_code_points;
                        if constexpr (IsSame<decltype(view), Utf16View const&>) {
                            size_t code_points = 0;
                            for (size_t i = state.string_position_in_code_units; i < current_code_unit_offset;) {
                                auto code_point = view.code_point_at(i);
                                i += code_point >= 0x10000 ? 2 : 1;
                                code_points++;
                            }
                            match_length_in_code_points = code_points;
                        } else {
                            size_t code_points = 0;
                            for (size_t i = state.string_position_in_code_units; i < current_code_unit_offset;) {
                                auto code_point = input.view.code_point_at(i);
                                if (code_point <= 0x7F)
                                    i += 1;
                                else if (code_point <= 0x7FF)
                                    i += 2;
                                else if (code_point <= 0xFFFF)
                                    i += 3;
                                else
                                    i += 4;
                                code_points++;
                            }
                            match_length_in_code_points = code_points;
                        }

                        if (match_length_in_code_points > longest_match_length) {
                            matched = true;
                            longest_match_length = match_length_in_code_points;
                        }
                    }
                }
            };

            if (input.view.u16_view().is_null()) {
                auto const& trie = bytecode().string_set_table().get_u8_trie(string_set_index);
                StringView view;
                find_longest_match(view, trie);
            } else {
                auto const& view = input.view.u16_view();
                auto const& trie = bytecode().string_set_table().get_u16_trie(string_set_index);
                find_longest_match(view, trie);
            }

            if (matched) {
                if (longest_match_length == 0)
                    had_zero_length_match = true;
                if (current_inversion_state()) {
                    inverse_matched = true;
                } else {
                    state.string_position += longest_match_length;
                    if (input.view.unicode()) {
                        state.string_position_in_code_units = input.view.code_unit_offset_of(state.string_position);
                    } else {
                        state.string_position_in_code_units = state.string_position;
                    }
                }
            }
            break;
        }
        case CharacterCompareType::And:
            VERIFY(!IsSimple);
            if constexpr (!IsSimple) {
                disjunction_states.append({
                    .active = true,
                    .is_conjunction = current_inversion_state(),
                    .is_and_operation = true,
                    .fail = current_inversion_state(),
                    .inverse_matched = current_inversion_state(),
                    .initial_position = state.string_position,
                    .initial_code_unit_position = state.string_position_in_code_units,
                });
            }
            continue;
        case CharacterCompareType::Subtract:
            VERIFY(!IsSimple);
            if constexpr (!IsSimple) {
                disjunction_states.append({
                    .active = true,
                    .is_conjunction = true,
                    .is_subtraction = true,
                    .fail = true,
                    .inverse_matched = false,
                    .initial_position = state.string_position,
                    .initial_code_unit_position = state.string_position_in_code_units,
                });
            }
            continue;
        case CharacterCompareType::Or:
            VERIFY(!IsSimple);
            if constexpr (!IsSimple) {
                disjunction_states.append({
                    .active = true,
                    .is_conjunction = !current_inversion_state(),
                    .fail = !current_inversion_state(),
                    .inverse_matched = !current_inversion_state(),
                    .initial_position = state.string_position,
                    .initial_code_unit_position = state.string_position_in_code_units,
                });
            }
            continue;
        case CharacterCompareType::EndAndOr: {
            VERIFY(!IsSimple);
            if constexpr (!IsSimple) {
                auto disjunction_state = disjunction_states.take_last();
                if (!disjunction_state.fail) {
                    state.string_position = disjunction_state.last_accepted_position.value_or(disjunction_state.initial_position);
                    state.string_position_in_code_units = disjunction_state.last_accepted_code_unit_position.value_or(disjunction_state.initial_code_unit_position);
                } else if (has_string_set) {
                    string_set_matched = false;
                    best_match_position = disjunction_state.initial_position;
                    best_match_position_in_code_units = disjunction_state.initial_code_unit_position;
                }
                inverse_matched = disjunction_state.inverse_matched || disjunction_state.fail;
            }
            break;
        }
        default:
            warnln("Undefined comparison: {}", (int)compare_type);
            VERIFY_NOT_REACHED();
            break;
        }

        if constexpr (!IsSimple) {
            auto& new_disjunction_state = current_disjunction_state();
            if (current_inversion_state() && (!inverse || new_disjunction_state.active) && !inverse_matched) {
                advance_string_position(state, input.view);
                inverse_matched = true;
            }
        }

        if (has_string_set && state.string_position > best_match_position) {
            best_match_position = state.string_position;
            best_match_position_in_code_units = state.string_position_in_code_units;
            string_set_matched = true;
        }

        if constexpr (!IsSimple) {
            auto& new_disjunction_state = current_disjunction_state();
            if (!has_single_argument && new_disjunction_state.active) {
                auto failed = (!had_zero_length_match && string_position == state.string_position) || state.string_position > input.view.length();

                if (!failed && new_disjunction_state.is_and_operation
                    && new_disjunction_state.last_accepted_position.has_value()
                    && new_disjunction_state.last_accepted_position.value() != state.string_position) {

                    failed = true;
                }

                if (!failed) {
                    new_disjunction_state.last_accepted_position = state.string_position;
                    new_disjunction_state.last_accepted_code_unit_position = state.string_position_in_code_units;
                    new_disjunction_state.inverse_matched |= inverse_matched;
                }

                if (new_disjunction_state.is_subtraction) {
                    if (new_disjunction_state.subtraction_operand_index == 0) {
                        new_disjunction_state.fail = failed && new_disjunction_state.fail;
                    } else if (!failed && (!has_string_set || state.string_position >= best_match_position)) {
                        new_disjunction_state.fail = true;
                    }
                    new_disjunction_state.subtraction_operand_index++;
                } else if (new_disjunction_state.is_conjunction) {
                    new_disjunction_state.fail = failed && new_disjunction_state.fail;
                } else {
                    new_disjunction_state.fail = failed || new_disjunction_state.fail;
                }

                state.string_position = new_disjunction_state.initial_position;
                state.string_position_in_code_units = new_disjunction_state.initial_code_unit_position;
                inverse_matched = false;
            }
        }
    }

    if constexpr (!IsSimple) {
        if (!has_single_argument) {
            auto& new_disjunction_state = current_disjunction_state();
            if (new_disjunction_state.active && !new_disjunction_state.fail) {
                state.string_position = new_disjunction_state.last_accepted_position.value_or(new_disjunction_state.initial_position);
                state.string_position_in_code_units = new_disjunction_state.last_accepted_code_unit_position.value_or(new_disjunction_state.initial_code_unit_position);
            }
        }
    }

    if (has_string_set && string_set_matched) {
        if (has_single_argument || best_match_position > string_position) {
            state.string_position = best_match_position;
            state.string_position_in_code_units = best_match_position_in_code_units;
        }
    }

    if (current_inversion_state() && !inverse_matched && state.string_position == string_position)
        advance_string_position(state, input.view);

    if ((!had_zero_length_match && string_position == state.string_position) || state.string_position > input.view.length())
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    return ExecutionResult::Continue;
}

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE void CompareInternals<ByteCode, IsSimple>::compare_char(MatchInput const& input, MatchState& state, u32 ch1, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    // FIXME: Figure out how to do this if unicode() without performing a substring split first.
    auto input_view = input.view.unicode()
        ? input.view.substring_view(state.string_position, 1).code_point_at(0)
        : input.view.unicode_aware_code_point_at(state.string_position_in_code_units);

    bool equal;
    if (input.regex_options & AllFlags::Insensitive) {
        if (input.view.unicode()) {
            auto lhs = String::from_code_point(input_view);
            auto rhs = String::from_code_point(ch1);

            equal = lhs.equals_ignoring_case(rhs);
        } else {
            equal = to_ascii_lowercase(input_view) == to_ascii_lowercase(ch1);
        }
    } else {
        equal = input_view == ch1;
    }

    if (equal) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, ch1);
    }
}

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE bool CompareInternals<ByteCode, IsSimple>::compare_string(MatchInput const& input, MatchState& state, RegexStringView str, bool& had_zero_length_match)
{
    if (state.string_position + str.length() > input.view.length()) {
        if (str.is_empty()) {
            had_zero_length_match = true;
            return true;
        }
        return false;
    }

    if (str.length() == 0) {
        had_zero_length_match = true;
        return true;
    }

    if (str.length() == 1) {
        auto inverse_matched = false;
        compare_char(input, state, str.code_point_at(0), false, inverse_matched);
        return !inverse_matched;
    }

    auto subject = input.view.substring_view(state.string_position, str.length());
    bool equals;
    if (input.regex_options & AllFlags::Insensitive)
        equals = subject.equals_ignoring_case(str);
    else
        equals = subject.equals(str);

    if (equals)
        advance_string_position(state, input.view, str);

    return equals;
}

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE void CompareInternals<ByteCode, IsSimple>::compare_character_class(MatchInput const& input, MatchState& state, CharClass character_class, u32 ch, bool inverse, bool& inverse_matched)
{
    if (matches_character_class(character_class, ch, input.regex_options & AllFlags::Insensitive)) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, ch);
    }
}

template<typename ByteCode, bool IsSimple>
bool CompareInternals<ByteCode, IsSimple>::matches_character_class(CharClass character_class, u32 ch, bool insensitive)
{
    constexpr auto is_space_or_line_terminator = [](u32 code_point) {
        if ((code_point == 0x0a) || (code_point == 0x0d) || (code_point == 0x2028) || (code_point == 0x2029))
            return true;
        if ((code_point == 0x09) || (code_point == 0x0b) || (code_point == 0x0c) || (code_point == 0xfeff))
            return true;
        return Unicode::code_point_has_space_separator_general_category(code_point);
    };

    switch (character_class) {
    case CharClass::Alnum:
        return is_ascii_alphanumeric(ch);
    case CharClass::Alpha:
        return is_ascii_alpha(ch);
    case CharClass::Blank:
        return is_ascii_blank(ch);
    case CharClass::Cntrl:
        return is_ascii_control(ch);
    case CharClass::Digit:
        return is_ascii_digit(ch);
    case CharClass::Graph:
        return is_ascii_graphical(ch);
    case CharClass::Lower:
        return is_ascii_lower_alpha(ch) || (insensitive && is_ascii_upper_alpha(ch));
    case CharClass::Print:
        return is_ascii_printable(ch);
    case CharClass::Punct:
        return is_ascii_punctuation(ch);
    case CharClass::Space:
        return is_space_or_line_terminator(ch);
    case CharClass::Upper:
        return is_ascii_upper_alpha(ch) || (insensitive && is_ascii_lower_alpha(ch));
    case CharClass::Word:
        return is_ascii_alphanumeric(ch) || ch == '_';
    case CharClass::Xdigit:
        return is_ascii_hex_digit(ch);
    }

    VERIFY_NOT_REACHED();
}

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE void CompareInternals<ByteCode, IsSimple>::compare_character_range(MatchInput const& input, MatchState& state, u32 from, u32 to, u32 ch, bool inverse, bool& inverse_matched)
{
    if (input.regex_options & AllFlags::Insensitive) {
        from = to_ascii_lowercase(from);
        to = to_ascii_lowercase(to);
        ch = to_ascii_lowercase(ch);
    }

    if (ch >= from && ch <= to) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, ch);
    }
}

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE void CompareInternals<ByteCode, IsSimple>::compare_property(MatchInput const& input, MatchState& state, Unicode::Property property, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view.code_point_at(state.string_position_in_code_units);
    bool equal = Unicode::code_point_has_property(code_point, property);

    if (equal) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, code_point);
    }
}

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE void CompareInternals<ByteCode, IsSimple>::compare_general_category(MatchInput const& input, MatchState& state, Unicode::GeneralCategory general_category, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view.code_point_at(state.string_position_in_code_units);
    bool equal = Unicode::code_point_has_general_category(code_point, general_category);

    if (equal) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, code_point);
    }
}

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE void CompareInternals<ByteCode, IsSimple>::compare_script(MatchInput const& input, MatchState& state, Unicode::Script script, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view.code_point_at(state.string_position_in_code_units);
    bool equal = Unicode::code_point_has_script(code_point, script);

    if (equal) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, code_point);
    }
}

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE void CompareInternals<ByteCode, IsSimple>::compare_script_extension(MatchInput const& input, MatchState& state, Unicode::Script script, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view.code_point_at(state.string_position_in_code_units);
    bool equal = Unicode::code_point_has_script_extension(code_point, script);

    if (equal) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, code_point);
    }
}

template<typename ByteCode>
ByteString OpCode_Compare<ByteCode>::arguments_string() const
{
    return ByteString::formatted("argc={}, args={} ", arguments_count(), arguments_size());
}

template<typename ByteCode, bool IsSimple>
Vector<CompareTypeAndValuePair> CompareInternals<ByteCode, IsSimple>::flat_compares() const
{
    Vector<CompareTypeAndValuePair> result;

    size_t offset { state().instruction_position + (IsSimple ? 2 : 3) };
    auto argument_count = IsSimple ? 1 : argument(0);
    auto& bytecode = this->bytecode();

    for (size_t i = 0; i < argument_count; ++i) {
        auto compare_type = (CharacterCompareType)bytecode[offset++];

        if (compare_type == CharacterCompareType::Char) {
            auto ch = bytecode[offset++];
            result.append({ compare_type, ch });
        } else if (compare_type == CharacterCompareType::Reference) {
            auto ref = bytecode[offset++];
            result.append({ compare_type, ref });
        } else if (compare_type == CharacterCompareType::NamedReference) {
            auto ref = bytecode[offset++];
            result.append({ compare_type, ref });
        } else if (compare_type == CharacterCompareType::String) {
            auto string_index = bytecode[offset++];
            result.append({ compare_type, string_index });
        } else if (compare_type == CharacterCompareType::CharClass) {
            auto character_class = bytecode[offset++];
            result.append({ compare_type, character_class });
        } else if (compare_type == CharacterCompareType::CharRange) {
            auto value = bytecode[offset++];
            result.append({ compare_type, value });
        } else if (compare_type == CharacterCompareType::LookupTable) {
            auto count_sensitive = bytecode[offset++];
            auto count_insensitive = bytecode[offset++];
            for (size_t i = 0; i < count_sensitive; ++i)
                result.append({ CharacterCompareType::CharRange, bytecode[offset++] });
            offset += count_insensitive; // Skip insensitive ranges
        } else if (compare_type == CharacterCompareType::GeneralCategory
            || compare_type == CharacterCompareType::Property
            || compare_type == CharacterCompareType::Script
            || compare_type == CharacterCompareType::ScriptExtension
            || compare_type == CharacterCompareType::StringSet) {
            auto value = bytecode[offset++];
            result.append({ compare_type, value });
        } else {
            result.append({ compare_type, 0 });
        }
    }
    return result;
}

template<typename ByteCode>
ByteString OpCode_CompareSimple<ByteCode>::arguments_string() const
{
    StringBuilder builder;
    auto type = (CharacterCompareType)argument(1);
    builder.append(character_compare_type_name(type));
    switch (type) {
    case CharacterCompareType::Char: {
        auto ch = argument(2);
        if (is_ascii_printable(ch))
            builder.append(ByteString::formatted(" '{:c}'", static_cast<char>(ch)));
        else
            builder.append(ByteString::formatted(" 0x{:x}", ch));
        break;
    }
    case CharacterCompareType::String: {
        auto string_index = argument(2);
        auto string = this->bytecode().get_u16_string(string_index);
        builder.appendff(" \"{}\"", string);
        break;
    }
    case CharacterCompareType::CharClass: {
        auto character_class = (CharClass)argument(2);
        builder.appendff(" {}", character_class_name(character_class));
        break;
    }
    case CharacterCompareType::Reference: {
        auto ref = argument(2);
        builder.appendff(" number={}", ref);
        break;
    }
    case CharacterCompareType::NamedReference: {
        auto ref = argument(2);
        builder.appendff(" named_number={}", ref);
        break;
    }
    case CharacterCompareType::GeneralCategory:
    case CharacterCompareType::Property:
    case CharacterCompareType::Script:
    case CharacterCompareType::ScriptExtension:
    case CharacterCompareType::StringSet: {
        builder.appendff(" value={}", argument(2));
        break;
    }
    case CharacterCompareType::LookupTable: {
        auto count_sensitive = argument(2);
        auto count_insensitive = argument(3);
        for (size_t j = 0; j < count_sensitive; ++j) {
            auto range = (CharRange)argument(4 + j);
            builder.appendff(" {:x}-{:x}", range.from, range.to);
        }
        if (count_insensitive > 0) {
            builder.append(" [insensitive ranges:"sv);
            for (size_t j = 0; j < count_insensitive; ++j) {
                auto range = (CharRange)argument(4 + count_sensitive + j);
                builder.appendff("  {:x}-{:x}", range.from, range.to);
            }
            builder.append(" ]"sv);
        }
        break;
    }
    case CharacterCompareType::CharRange: {
        auto value = argument(2);
        auto range = (CharRange)value;
        builder.appendff(" {:x}-{:x}", range.from, range.to);
        break;
    }
    default:
        break;
    }

    return builder.to_byte_string();
}

template<typename ByteCode>
Vector<ByteString> OpCode_Compare<ByteCode>::variable_arguments_to_byte_string(Optional<MatchInput const&> input) const
{
    Vector<ByteString> result;

    size_t offset { state().instruction_position + 3 };
    RegexStringView const& view = input.has_value() ? input.value().view : StringView {};

    auto argument_count = arguments_count();
    auto& bytecode = this->bytecode();

    for (size_t i = 0; i < argument_count; ++i) {
        auto compare_type = (CharacterCompareType)bytecode[offset++];
        result.empend(ByteString::formatted("type={} [{}]", (size_t)compare_type, character_compare_type_name(compare_type)));

        auto string_start_offset = state().string_position_before_match;

        if (compare_type == CharacterCompareType::Char) {
            auto ch = bytecode[offset++];
            auto is_ascii = is_ascii_printable(ch);
            if (is_ascii)
                result.empend(ByteString::formatted(" value='{:c}'", static_cast<char>(ch)));
            else
                result.empend(ByteString::formatted(" value={:x}", ch));

            if (!view.is_null() && view.length() > string_start_offset) {
                if (is_ascii) {
                    result.empend(ByteString::formatted(
                        " compare against: '{}'",
                        view.substring_view(string_start_offset, string_start_offset > view.length() ? 0 : 1).to_byte_string()));
                } else {
                    auto str = view.substring_view(string_start_offset, string_start_offset > view.length() ? 0 : 1).to_byte_string();
                    u8 buf[8] { 0 };
                    __builtin_memcpy(buf, str.characters(), min(str.length(), sizeof(buf)));
                    result.empend(ByteString::formatted(" compare against: {:x},{:x},{:x},{:x},{:x},{:x},{:x},{:x}",
                        buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]));
                }
            }
        } else if (compare_type == CharacterCompareType::Reference) {
            auto ref = bytecode[offset++];
            result.empend(ByteString::formatted(" number={}", ref));
            if (input.has_value()) {
                if (state().capture_group_matches_size() > input->match_index) {
                    auto match = state().capture_group_matches(input->match_index);
                    if (match.size() > ref) {
                        auto& group = match[ref];
                        result.empend(ByteString::formatted(" left={}", group.left_column));
                        result.empend(ByteString::formatted(" right={}", group.left_column + group.view.length_in_code_units()));
                        result.empend(ByteString::formatted(" contents='{}'", group.view));
                    } else {
                        result.empend(ByteString::formatted(" (invalid ref, max={})", match.size() - 1));
                    }
                } else {
                    result.empend(ByteString::formatted(" (invalid index {}, max={})", input->match_index, state().capture_group_matches_size() - 1));
                }
            }
        } else if (compare_type == CharacterCompareType::NamedReference) {
            auto ref = bytecode[offset++];
            result.empend(ByteString::formatted(" named_number={}", ref));
            if (input.has_value()) {
                if (state().capture_group_matches_size() > input->match_index) {
                    auto match = state().capture_group_matches(input->match_index);
                    if (match.size() > ref) {
                        auto& group = match[ref];
                        result.empend(ByteString::formatted(" left={}", group.left_column));
                        result.empend(ByteString::formatted(" right={}", group.left_column + group.view.length_in_code_units()));
                        result.empend(ByteString::formatted(" contents='{}'", group.view));
                    } else {
                        result.empend(ByteString::formatted(" (invalid ref {}, max={})", ref, match.size() - 1));
                    }
                } else {
                    result.empend(ByteString::formatted(" (invalid index {}, max={})", input->match_index, state().capture_group_matches_size() - 1));
                }
            }
        } else if (compare_type == CharacterCompareType::String) {
            auto id = bytecode[offset++];
            auto string = this->bytecode().get_u16_string(id);
            result.empend(ByteString::formatted(" value=\"{}\"", string));
            if (!view.is_null() && view.length() > state().string_position)
                result.empend(ByteString::formatted(
                    " compare against: \"{}\"",
                    input.value().view.substring_view(string_start_offset, string_start_offset + string.length_in_code_units() > view.length() ? 0 : string.length_in_code_units()).to_byte_string()));
        } else if (compare_type == CharacterCompareType::CharClass) {
            auto character_class = (CharClass)bytecode[offset++];
            result.empend(ByteString::formatted(" ch_class={} [{}]", (size_t)character_class, character_class_name(character_class)));
            if (!view.is_null() && view.length() > state().string_position)
                result.empend(ByteString::formatted(
                    " compare against: '{}'",
                    input.value().view.substring_view(string_start_offset, state().string_position > view.length() ? 0 : 1).to_byte_string()));
        } else if (compare_type == CharacterCompareType::CharRange) {
            auto value = (CharRange)bytecode[offset++];
            result.empend(ByteString::formatted(" ch_range={:x}-{:x}", value.from, value.to));
            if (!view.is_null() && view.length() > state().string_position)
                result.empend(ByteString::formatted(
                    " compare against: '{}'",
                    input.value().view.substring_view(string_start_offset, state().string_position > view.length() ? 0 : 1).to_byte_string()));
        } else if (compare_type == CharacterCompareType::LookupTable) {
            auto count_sensitive = bytecode[offset++];
            auto count_insensitive = bytecode[offset++];
            for (size_t j = 0; j < count_sensitive; ++j) {
                auto range = (CharRange)bytecode[offset++];
                result.append(ByteString::formatted(" {:x}-{:x}", range.from, range.to));
            }
            if (count_insensitive > 0) {
                result.append(" [insensitive ranges:");
                for (size_t j = 0; j < count_insensitive; ++j) {
                    auto range = (CharRange)bytecode[offset++];
                    result.append(ByteString::formatted("  {:x}-{:x}", range.from, range.to));
                }
                result.append(" ]");
            }

            if (!view.is_null() && view.length() > state().string_position)
                result.empend(ByteString::formatted(
                    " compare against: '{}'",
                    input.value().view.substring_view(string_start_offset, state().string_position > view.length() ? 0 : 1).to_byte_string()));
        } else if (compare_type == CharacterCompareType::GeneralCategory
            || compare_type == CharacterCompareType::Property
            || compare_type == CharacterCompareType::Script
            || compare_type == CharacterCompareType::ScriptExtension
            || compare_type == CharacterCompareType::StringSet) {
            auto value = bytecode[offset++];
            result.empend(ByteString::formatted(" value={}", value));
        }
    }
    return result;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_Repeat<ByteCode>::execute(MatchInput const&, MatchState& state) const
{
    VERIFY(count() > 0);

    if (id() >= state.repetition_marks.size())
        state.repetition_marks.resize(id() + 1);
    auto& repetition_mark = state.repetition_marks.mutable_at(id());

    if (repetition_mark == count() - 1) {
        repetition_mark = 0;
    } else {
        state.instruction_position -= offset() + size();
        ++repetition_mark;
    }

    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_ResetRepeat<ByteCode>::execute(MatchInput const&, MatchState& state) const
{
    if (id() >= state.repetition_marks.size())
        state.repetition_marks.resize(id() + 1);

    state.repetition_marks.mutable_at(id()) = 0;
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_Checkpoint<ByteCode>::execute(MatchInput const&, MatchState& state) const
{
    auto id = this->id();
    if (id >= state.checkpoints.size())
        state.checkpoints.resize(id + 1);

    state.checkpoints[id] = state.string_position + 1;
    return ExecutionResult::Continue;
}

template<typename ByteCode>
ALWAYS_INLINE ExecutionResult OpCode_JumpNonEmpty<ByteCode>::execute(MatchInput const& input, MatchState& state) const
{
    u64 current_position = state.string_position;
    auto checkpoint_position = state.checkpoints.get(checkpoint()).value_or(0);

    if (checkpoint_position != 0 && checkpoint_position != current_position + 1) {
        auto form = this->form();

        if (form == OpCodeId::Jump) {
            state.instruction_position += offset();
            return ExecutionResult::Continue;
        }

        state.fork_at_position = state.instruction_position + size() + offset();

        if (form == OpCodeId::ForkJump) {
            state.forks_since_last_save++;
            return ExecutionResult::Fork_PrioHigh;
        }

        if (form == OpCodeId::ForkStay) {
            state.forks_since_last_save++;
            return ExecutionResult::Fork_PrioLow;
        }

        if (form == OpCodeId::ForkReplaceStay) {
            input.fork_to_replace = state.instruction_position;
            return ExecutionResult::Fork_PrioLow;
        }

        if (form == OpCodeId::ForkReplaceJump) {
            input.fork_to_replace = state.instruction_position;
            return ExecutionResult::Fork_PrioHigh;
        }
    }

    if (form() == OpCodeId::Jump && state.string_position < input.view.length())
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    return ExecutionResult::Continue;
}

template class OpCode_Compare<ByteCode>;
template class OpCode<ByteCode>;
template class CompareInternals<ByteCode, true>;
template class CompareInternals<ByteCode, false>;
template class OpCode_Compare<FlatByteCode>;
template class OpCode<FlatByteCode>;
template class CompareInternals<FlatByteCode, true>;
template class CompareInternals<FlatByteCode, false>;

}
