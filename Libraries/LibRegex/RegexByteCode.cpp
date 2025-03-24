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

StringView OpCode::name(OpCodeId opcode_id)
{
    switch (opcode_id) {
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

StringView OpCode::name() const
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
            code_point = view[state.string_position_in_code_units];
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

OwnPtr<OpCode> ByteCode::s_opcodes[(size_t)OpCodeId::Last + 1];
bool ByteCode::s_opcodes_initialized { false };
size_t ByteCode::s_next_checkpoint_serial_id { 0 };

void ByteCode::ensure_opcodes_initialized()
{
    if (s_opcodes_initialized)
        return;
    for (u32 i = (u32)OpCodeId::First; i <= (u32)OpCodeId::Last; ++i) {
        switch ((OpCodeId)i) {
#define __ENUMERATE_OPCODE(OpCode)              \
    case OpCodeId::OpCode:                      \
        s_opcodes[i] = make<OpCode_##OpCode>(); \
        break;

            ENUMERATE_OPCODES

#undef __ENUMERATE_OPCODE
        }
    }
    s_opcodes_initialized = true;
}

ALWAYS_INLINE ExecutionResult OpCode_Exit::execute(MatchInput const& input, MatchState& state) const
{
    if (state.string_position > input.view.length() || state.instruction_position >= m_bytecode->size())
        return ExecutionResult::Succeeded;

    return ExecutionResult::Failed;
}

ALWAYS_INLINE ExecutionResult OpCode_Save::execute(MatchInput const& input, MatchState& state) const
{
    save_string_position(input, state);
    state.forks_since_last_save = 0;
    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_Restore::execute(MatchInput const& input, MatchState& state) const
{
    if (!restore_string_position(input, state))
        return ExecutionResult::Failed;
    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_GoBack::execute(MatchInput const& input, MatchState& state) const
{
    if (count() > state.string_position)
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    reverse_string_position(state, input.view, count());
    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_FailForks::execute(MatchInput const& input, MatchState& state) const
{
    input.fail_counter += state.forks_since_last_save;
    return ExecutionResult::Failed_ExecuteLowPrioForks;
}

ALWAYS_INLINE ExecutionResult OpCode_PopSaved::execute(MatchInput const& input, MatchState&) const
{
    if (input.saved_positions.is_empty() || input.saved_code_unit_positions.is_empty())
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    input.saved_positions.take_last();
    input.saved_code_unit_positions.take_last();
    return ExecutionResult::Failed_ExecuteLowPrioForks;
}

ALWAYS_INLINE ExecutionResult OpCode_Jump::execute(MatchInput const&, MatchState& state) const
{
    state.instruction_position += offset();
    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_ForkJump::execute(MatchInput const&, MatchState& state) const
{
    state.fork_at_position = state.instruction_position + size() + offset();
    state.forks_since_last_save++;
    return ExecutionResult::Fork_PrioHigh;
}

ALWAYS_INLINE ExecutionResult OpCode_ForkReplaceJump::execute(MatchInput const& input, MatchState& state) const
{
    state.fork_at_position = state.instruction_position + size() + offset();
    input.fork_to_replace = state.instruction_position;
    state.forks_since_last_save++;
    return ExecutionResult::Fork_PrioHigh;
}

ALWAYS_INLINE ExecutionResult OpCode_ForkStay::execute(MatchInput const&, MatchState& state) const
{
    state.fork_at_position = state.instruction_position + size() + offset();
    state.forks_since_last_save++;
    return ExecutionResult::Fork_PrioLow;
}

ALWAYS_INLINE ExecutionResult OpCode_ForkReplaceStay::execute(MatchInput const& input, MatchState& state) const
{
    state.fork_at_position = state.instruction_position + size() + offset();
    input.fork_to_replace = state.instruction_position;
    return ExecutionResult::Fork_PrioLow;
}

ALWAYS_INLINE ExecutionResult OpCode_CheckBegin::execute(MatchInput const& input, MatchState& state) const
{
    auto is_at_line_boundary = [&] {
        if (state.string_position == 0)
            return true;

        if (input.regex_options.has_flag_set(AllFlags::Multiline) && input.regex_options.has_flag_set(AllFlags::Internal_ConsiderNewline)) {
            auto input_view = input.view.substring_view(state.string_position - 1, 1)[0];
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

ALWAYS_INLINE ExecutionResult OpCode_CheckBoundary::execute(MatchInput const& input, MatchState& state) const
{
    auto isword = [](auto ch) { return is_ascii_alphanumeric(ch) || ch == '_'; };
    auto is_word_boundary = [&] {
        if (state.string_position == input.view.length()) {
            return (state.string_position > 0 && isword(input.view[state.string_position_in_code_units - 1]));
        }

        if (state.string_position == 0) {
            return (isword(input.view[0]));
        }

        return !!(isword(input.view[state.string_position_in_code_units]) ^ isword(input.view[state.string_position_in_code_units - 1]));
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

ALWAYS_INLINE ExecutionResult OpCode_CheckEnd::execute(MatchInput const& input, MatchState& state) const
{
    auto is_at_line_boundary = [&] {
        if (state.string_position == input.view.length())
            return true;

        if (input.regex_options.has_flag_set(AllFlags::Multiline) && input.regex_options.has_flag_set(AllFlags::Internal_ConsiderNewline)) {
            auto input_view = input.view.substring_view(state.string_position, 1)[0];
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

ALWAYS_INLINE ExecutionResult OpCode_ClearCaptureGroup::execute(MatchInput const& input, MatchState& state) const
{
    if (input.match_index < state.capture_group_matches.size()) {
        auto& group = state.capture_group_matches.mutable_at(input.match_index);
        auto group_id = id();
        if (group_id >= group.size())
            group.resize(group_id + 1);

        group[group_id].reset();
    }
    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_SaveLeftCaptureGroup::execute(MatchInput const& input, MatchState& state) const
{
    if (input.match_index >= state.capture_group_matches.size()) {
        state.capture_group_matches.ensure_capacity(input.match_index);
        auto capacity = state.capture_group_matches.capacity();
        for (size_t i = state.capture_group_matches.size(); i <= capacity; ++i)
            state.capture_group_matches.empend();
    }

    if (id() >= state.capture_group_matches.at(input.match_index).size()) {
        state.capture_group_matches.mutable_at(input.match_index).ensure_capacity(id());
        auto capacity = state.capture_group_matches.at(input.match_index).capacity();
        for (size_t i = state.capture_group_matches.at(input.match_index).size(); i <= capacity; ++i)
            state.capture_group_matches.mutable_at(input.match_index).empend();
    }

    state.capture_group_matches.mutable_at(input.match_index).at(id()).left_column = state.string_position;
    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_SaveRightCaptureGroup::execute(MatchInput const& input, MatchState& state) const
{
    auto& match = state.capture_group_matches.mutable_at(input.match_index).at(id());
    auto start_position = match.left_column;
    if (state.string_position < start_position) {
        dbgln("Right capture group {} is before left capture group {}!", state.string_position, start_position);
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    }

    auto length = state.string_position - start_position;

    if (start_position < match.column)
        return ExecutionResult::Continue;

    VERIFY(start_position + length <= input.view.length());

    match = { input.view.substring_view(start_position, length), input.line, start_position, input.global_offset + start_position };

    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_SaveRightNamedCaptureGroup::execute(MatchInput const& input, MatchState& state) const
{
    auto& match = state.capture_group_matches.mutable_at(input.match_index).at(id());
    auto start_position = match.left_column;
    if (state.string_position < start_position)
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    auto length = state.string_position - start_position;

    if (start_position < match.column)
        return ExecutionResult::Continue;

    VERIFY(start_position + length <= input.view.length());

    auto view = input.view.substring_view(start_position, length);

    match = { view, name(), input.line, start_position, input.global_offset + start_position };

    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_Compare::execute(MatchInput const& input, MatchState& state) const
{
    auto argument_count = arguments_count();
    auto has_single_argument = argument_count == 1;

    bool inverse { false };
    bool temporary_inverse { false };
    bool reset_temp_inverse { false };
    struct DisjunctionState {
        bool active { false };
        bool is_conjunction { false };
        bool fail { false };
        bool inverse_matched { false };
        size_t initial_position;
        size_t initial_code_unit_position;
        Optional<size_t> last_accepted_position {};
        Optional<size_t> last_accepted_code_unit_position {};
    };

    Vector<DisjunctionState, 4> disjunction_states;
    disjunction_states.empend();

    auto current_disjunction_state = [&]() -> DisjunctionState& { return disjunction_states.last(); };

    auto current_inversion_state = [&]() -> bool { return temporary_inverse ^ inverse; };

    size_t string_position = state.string_position;
    bool inverse_matched { false };
    bool had_zero_length_match { false };

    state.string_position_before_match = state.string_position;

    size_t offset { state.instruction_position + 3 };
    for (size_t i = 0; i < argument_count; ++i) {
        if (state.string_position > string_position)
            break;

        if (reset_temp_inverse) {
            reset_temp_inverse = false;
            temporary_inverse = false;
        } else {
            reset_temp_inverse = true;
        }

        auto compare_type = (CharacterCompareType)m_bytecode->at(offset++);

        switch (compare_type) {
        case CharacterCompareType::Inverse:
            inverse = !inverse;
            continue;
        case CharacterCompareType::TemporaryInverse:
            // If "TemporaryInverse" is given, negate the current inversion state only for the next opcode.
            // it follows that this cannot be the last compare element.
            VERIFY(i != arguments_count() - 1);

            temporary_inverse = true;
            reset_temp_inverse = false;
            continue;
        case CharacterCompareType::Char: {
            u32 ch = m_bytecode->at(offset++);

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

            auto input_view = input.view.substring_view(state.string_position, 1)[0];
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

            auto const& length = m_bytecode->at(offset++);

            // We want to compare a string that is definitely longer than the available string
            if (input.view.length() < state.string_position + length)
                return ExecutionResult::Failed_ExecuteLowPrioForks;

            Optional<ByteString> str;
            Utf16Data utf16;
            Vector<u32> data;
            data.ensure_capacity(length);
            for (size_t i = offset; i < offset + length; ++i)
                data.unchecked_append(m_bytecode->at(i));

            auto view = input.view.construct_as_same(data, str, utf16);
            offset += length;
            if (compare_string(input, state, view, had_zero_length_match)) {
                if (current_inversion_state())
                    inverse_matched = true;
            }
            break;
        }
        case CharacterCompareType::CharClass: {
            if (input.view.length() <= state.string_position_in_code_units)
                return ExecutionResult::Failed_ExecuteLowPrioForks;

            auto character_class = (CharClass)m_bytecode->at(offset++);
            auto ch = input.view[state.string_position_in_code_units];

            compare_character_class(input, state, character_class, ch, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::LookupTable: {
            if (input.view.length() <= state.string_position)
                return ExecutionResult::Failed_ExecuteLowPrioForks;

            auto count = m_bytecode->at(offset++);
            auto range_data = m_bytecode->template spans<4>().slice(offset, count);
            offset += count;

            auto ch = input.view[state.string_position_in_code_units];

            auto const* matching_range = binary_search(range_data, ch, nullptr, [insensitive = input.regex_options & AllFlags::Insensitive](auto needle, CharRange range) {
                auto upper_case_needle = needle;
                auto lower_case_needle = needle;
                if (insensitive) {
                    upper_case_needle = to_ascii_uppercase(needle);
                    lower_case_needle = to_ascii_lowercase(needle);
                }

                if (lower_case_needle >= range.from && lower_case_needle <= range.to)
                    return 0;
                if (upper_case_needle >= range.from && upper_case_needle <= range.to)
                    return 0;
                if (lower_case_needle > range.to || upper_case_needle > range.to)
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

            auto value = (CharRange)m_bytecode->at(offset++);

            auto from = value.from;
            auto to = value.to;
            auto ch = input.view[state.string_position_in_code_units];

            compare_character_range(input, state, from, to, ch, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::Reference: {
            auto reference_number = (size_t)m_bytecode->at(offset++);
            auto& groups = state.capture_group_matches.at(input.match_index);
            if (groups.size() <= reference_number)
                return ExecutionResult::Failed_ExecuteLowPrioForks;

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
        case CharacterCompareType::Property: {
            auto property = static_cast<Unicode::Property>(m_bytecode->at(offset++));
            compare_property(input, state, property, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::GeneralCategory: {
            auto general_category = static_cast<Unicode::GeneralCategory>(m_bytecode->at(offset++));
            compare_general_category(input, state, general_category, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::Script: {
            auto script = static_cast<Unicode::Script>(m_bytecode->at(offset++));
            compare_script(input, state, script, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::ScriptExtension: {
            auto script = static_cast<Unicode::Script>(m_bytecode->at(offset++));
            compare_script_extension(input, state, script, current_inversion_state(), inverse_matched);
            break;
        }
        case CharacterCompareType::And:
            disjunction_states.append({
                .active = true,
                .is_conjunction = current_inversion_state(),
                .fail = current_inversion_state(),
                .inverse_matched = current_inversion_state(),
                .initial_position = state.string_position,
                .initial_code_unit_position = state.string_position_in_code_units,
            });
            continue;
        case CharacterCompareType::Or:
            disjunction_states.append({
                .active = true,
                .is_conjunction = !current_inversion_state(),
                .fail = !current_inversion_state(),
                .inverse_matched = !current_inversion_state(),
                .initial_position = state.string_position,
                .initial_code_unit_position = state.string_position_in_code_units,
            });
            continue;
        case CharacterCompareType::EndAndOr: {
            auto disjunction_state = disjunction_states.take_last();
            if (!disjunction_state.fail) {
                state.string_position = disjunction_state.last_accepted_position.value_or(disjunction_state.initial_position);
                state.string_position_in_code_units = disjunction_state.last_accepted_code_unit_position.value_or(disjunction_state.initial_code_unit_position);
            }
            inverse_matched = disjunction_state.inverse_matched || disjunction_state.fail;
            break;
        }
        default:
            warnln("Undefined comparison: {}", (int)compare_type);
            VERIFY_NOT_REACHED();
            break;
        }

        auto& new_disjunction_state = current_disjunction_state();
        if (current_inversion_state() && (!inverse || new_disjunction_state.active) && !inverse_matched) {
            advance_string_position(state, input.view);
            inverse_matched = true;
        }

        if (!has_single_argument && new_disjunction_state.active) {
            auto failed = (!had_zero_length_match && string_position == state.string_position) || state.string_position > input.view.length();

            if (!failed) {
                new_disjunction_state.last_accepted_position = state.string_position;
                new_disjunction_state.last_accepted_code_unit_position = state.string_position_in_code_units;
                new_disjunction_state.inverse_matched |= inverse_matched;
            }

            if (new_disjunction_state.is_conjunction)
                new_disjunction_state.fail = failed && new_disjunction_state.fail;
            else
                new_disjunction_state.fail = failed || new_disjunction_state.fail;

            state.string_position = new_disjunction_state.initial_position;
            state.string_position_in_code_units = new_disjunction_state.initial_code_unit_position;
            inverse_matched = false;
        }
    }

    if (!has_single_argument) {
        auto& new_disjunction_state = current_disjunction_state();
        if (new_disjunction_state.active) {
            if (!new_disjunction_state.fail) {
                state.string_position = new_disjunction_state.last_accepted_position.value_or(new_disjunction_state.initial_position);
                state.string_position_in_code_units = new_disjunction_state.last_accepted_code_unit_position.value_or(new_disjunction_state.initial_code_unit_position);
            }
        }
    }

    if (current_inversion_state() && !inverse_matched)
        advance_string_position(state, input.view);

    if ((!had_zero_length_match && string_position == state.string_position) || state.string_position > input.view.length())
        return ExecutionResult::Failed_ExecuteLowPrioForks;

    return ExecutionResult::Continue;
}

ALWAYS_INLINE void OpCode_Compare::compare_char(MatchInput const& input, MatchState& state, u32 ch1, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    // FIXME: Figure out how to do this if unicode() without performing a substring split first.
    auto input_view = input.view.unicode()
        ? input.view.substring_view(state.string_position, 1)[0]
        : input.view.code_unit_at(state.string_position_in_code_units);

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

ALWAYS_INLINE bool OpCode_Compare::compare_string(MatchInput const& input, MatchState& state, RegexStringView str, bool& had_zero_length_match)
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
        compare_char(input, state, str[0], false, inverse_matched);
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

ALWAYS_INLINE void OpCode_Compare::compare_character_class(MatchInput const& input, MatchState& state, CharClass character_class, u32 ch, bool inverse, bool& inverse_matched)
{
    if (matches_character_class(character_class, ch, input.regex_options & AllFlags::Insensitive)) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, ch);
    }
}

bool OpCode_Compare::matches_character_class(CharClass character_class, u32 ch, bool insensitive)
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

ALWAYS_INLINE void OpCode_Compare::compare_character_range(MatchInput const& input, MatchState& state, u32 from, u32 to, u32 ch, bool inverse, bool& inverse_matched)
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

ALWAYS_INLINE void OpCode_Compare::compare_property(MatchInput const& input, MatchState& state, Unicode::Property property, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view[state.string_position_in_code_units];
    bool equal = Unicode::code_point_has_property(code_point, property);

    if (equal) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, code_point);
    }
}

ALWAYS_INLINE void OpCode_Compare::compare_general_category(MatchInput const& input, MatchState& state, Unicode::GeneralCategory general_category, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view[state.string_position_in_code_units];
    bool equal = Unicode::code_point_has_general_category(code_point, general_category);

    if (equal) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, code_point);
    }
}

ALWAYS_INLINE void OpCode_Compare::compare_script(MatchInput const& input, MatchState& state, Unicode::Script script, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view[state.string_position_in_code_units];
    bool equal = Unicode::code_point_has_script(code_point, script);

    if (equal) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, code_point);
    }
}

ALWAYS_INLINE void OpCode_Compare::compare_script_extension(MatchInput const& input, MatchState& state, Unicode::Script script, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view[state.string_position_in_code_units];
    bool equal = Unicode::code_point_has_script_extension(code_point, script);

    if (equal) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, code_point);
    }
}

ByteString OpCode_Compare::arguments_string() const
{
    return ByteString::formatted("argc={}, args={} ", arguments_count(), arguments_size());
}

Vector<CompareTypeAndValuePair> OpCode_Compare::flat_compares() const
{
    Vector<CompareTypeAndValuePair> result;

    size_t offset { state().instruction_position + 3 };

    for (size_t i = 0; i < arguments_count(); ++i) {
        auto compare_type = (CharacterCompareType)m_bytecode->at(offset++);

        if (compare_type == CharacterCompareType::Char) {
            auto ch = m_bytecode->at(offset++);
            result.append({ compare_type, ch });
        } else if (compare_type == CharacterCompareType::Reference) {
            auto ref = m_bytecode->at(offset++);
            result.append({ compare_type, ref });
        } else if (compare_type == CharacterCompareType::String) {
            auto& length = m_bytecode->at(offset++);
            for (size_t k = 0; k < length; ++k)
                result.append({ CharacterCompareType::Char, m_bytecode->at(offset + k) });
            offset += length;
        } else if (compare_type == CharacterCompareType::CharClass) {
            auto character_class = m_bytecode->at(offset++);
            result.append({ compare_type, character_class });
        } else if (compare_type == CharacterCompareType::CharRange) {
            auto value = m_bytecode->at(offset++);
            result.append({ compare_type, value });
        } else if (compare_type == CharacterCompareType::LookupTable) {
            auto count = m_bytecode->at(offset++);
            for (size_t i = 0; i < count; ++i)
                result.append({ CharacterCompareType::CharRange, m_bytecode->at(offset++) });
        } else if (compare_type == CharacterCompareType::GeneralCategory
            || compare_type == CharacterCompareType::Property
            || compare_type == CharacterCompareType::Script
            || compare_type == CharacterCompareType::ScriptExtension) {
            auto value = m_bytecode->at(offset++);
            result.append({ compare_type, value });
        } else {
            result.append({ compare_type, 0 });
        }
    }
    return result;
}

Vector<ByteString> OpCode_Compare::variable_arguments_to_byte_string(Optional<MatchInput const&> input) const
{
    Vector<ByteString> result;

    size_t offset { state().instruction_position + 3 };
    RegexStringView const& view = ((input.has_value()) ? input.value().view : StringView {});

    for (size_t i = 0; i < arguments_count(); ++i) {
        auto compare_type = (CharacterCompareType)m_bytecode->at(offset++);
        result.empend(ByteString::formatted("type={} [{}]", (size_t)compare_type, character_compare_type_name(compare_type)));

        auto string_start_offset = state().string_position_before_match;

        if (compare_type == CharacterCompareType::Char) {
            auto ch = m_bytecode->at(offset++);
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
            auto ref = m_bytecode->at(offset++);
            result.empend(ByteString::formatted(" number={}", ref));
            if (input.has_value()) {
                if (state().capture_group_matches.size() > input->match_index) {
                    auto& match = state().capture_group_matches[input->match_index];
                    if (match.size() > ref) {
                        auto& group = match[ref];
                        result.empend(ByteString::formatted(" left={}", group.left_column));
                        result.empend(ByteString::formatted(" right={}", group.left_column + group.view.length_in_code_units()));
                        result.empend(ByteString::formatted(" contents='{}'", group.view));
                    } else {
                        result.empend(ByteString::formatted(" (invalid ref, max={})", match.size() - 1));
                    }
                } else {
                    result.empend(ByteString::formatted(" (invalid index {}, max={})", input->match_index, state().capture_group_matches.size() - 1));
                }
            }
        } else if (compare_type == CharacterCompareType::String) {
            auto& length = m_bytecode->at(offset++);
            StringBuilder str_builder;
            for (size_t i = 0; i < length; ++i)
                str_builder.append(m_bytecode->at(offset++));
            result.empend(ByteString::formatted(" value=\"{}\"", str_builder.string_view().substring_view(0, length)));
            if (!view.is_null() && view.length() > state().string_position)
                result.empend(ByteString::formatted(
                    " compare against: \"{}\"",
                    input.value().view.substring_view(string_start_offset, string_start_offset + length > view.length() ? 0 : length).to_byte_string()));
        } else if (compare_type == CharacterCompareType::CharClass) {
            auto character_class = (CharClass)m_bytecode->at(offset++);
            result.empend(ByteString::formatted(" ch_class={} [{}]", (size_t)character_class, character_class_name(character_class)));
            if (!view.is_null() && view.length() > state().string_position)
                result.empend(ByteString::formatted(
                    " compare against: '{}'",
                    input.value().view.substring_view(string_start_offset, state().string_position > view.length() ? 0 : 1).to_byte_string()));
        } else if (compare_type == CharacterCompareType::CharRange) {
            auto value = (CharRange)m_bytecode->at(offset++);
            result.empend(ByteString::formatted(" ch_range={:x}-{:x}", value.from, value.to));
            if (!view.is_null() && view.length() > state().string_position)
                result.empend(ByteString::formatted(
                    " compare against: '{}'",
                    input.value().view.substring_view(string_start_offset, state().string_position > view.length() ? 0 : 1).to_byte_string()));
        } else if (compare_type == CharacterCompareType::LookupTable) {
            auto count = m_bytecode->at(offset++);
            for (size_t j = 0; j < count; ++j) {
                auto range = (CharRange)m_bytecode->at(offset++);
                result.append(ByteString::formatted(" {:x}-{:x}", range.from, range.to));
            }
            if (!view.is_null() && view.length() > state().string_position)
                result.empend(ByteString::formatted(
                    " compare against: '{}'",
                    input.value().view.substring_view(string_start_offset, state().string_position > view.length() ? 0 : 1).to_byte_string()));
        } else if (compare_type == CharacterCompareType::GeneralCategory
            || compare_type == CharacterCompareType::Property
            || compare_type == CharacterCompareType::Script
            || compare_type == CharacterCompareType::ScriptExtension) {

            auto value = m_bytecode->at(offset++);
            result.empend(ByteString::formatted(" value={}", value));
        }
    }
    return result;
}

ALWAYS_INLINE ExecutionResult OpCode_Repeat::execute(MatchInput const&, MatchState& state) const
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

ALWAYS_INLINE ExecutionResult OpCode_ResetRepeat::execute(MatchInput const&, MatchState& state) const
{
    if (id() >= state.repetition_marks.size())
        state.repetition_marks.resize(id() + 1);

    state.repetition_marks.mutable_at(id()) = 0;
    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_Checkpoint::execute(MatchInput const&, MatchState& state) const
{
    auto id = this->id();
    if (id >= state.checkpoints.size())
        state.checkpoints.resize(id + 1);

    state.checkpoints[id] = state.string_position + 1;
    return ExecutionResult::Continue;
}

ALWAYS_INLINE ExecutionResult OpCode_JumpNonEmpty::execute(MatchInput const& input, MatchState& state) const
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

    if (state.string_position < input.view.length()) {
        return ExecutionResult::Failed_ExecuteLowPrioForks;
    }

    return ExecutionResult::Continue;
}

}
