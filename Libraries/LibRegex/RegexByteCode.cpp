/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RegexByteCode.h"

#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <LibUnicode/CharacterTypes.h>

namespace regex {

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

static bool is_word_character(u32 code_point, bool case_insensitive, bool unicode_mode)
{
    if (is_ascii_alphanumeric(code_point) || code_point == '_')
        return true;

    if (case_insensitive && unicode_mode) {
        auto canonical = Unicode::canonicalize(code_point, unicode_mode);
        if (is_ascii_alphanumeric(canonical) || canonical == '_')
            return true;
    }

    return false;
}

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

bool matches_character_class(CharClass character_class, u32 ch, bool insensitive, bool unicode_mode)
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
        return is_word_character(ch, insensitive, unicode_mode);
    case CharClass::Xdigit:
        return is_ascii_hex_digit(ch);
    }

    VERIFY_NOT_REACHED();
}

ByteString opcode_arguments_string(OpCodeId id, ByteCodeValueType const* data, size_t ip, MatchState const& state, ByteCodeBase const& bytecode)
{
    // argument(N) = data[ip + 1 + N]
    auto arg = [&](size_t n) -> ByteCodeValueType { return data[ip + 1 + n]; };
    auto sz = opcode_size(id, data, ip);

    switch (id) {
    case OpCodeId::SaveModifiers:
        return ByteString::formatted("new_modifiers={:#x}", arg(0));
    case OpCodeId::RestoreModifiers:
    case OpCodeId::Exit:
    case OpCodeId::FailForks:
    case OpCodeId::PopSaved:
    case OpCodeId::Save:
    case OpCodeId::Restore:
    case OpCodeId::CheckBegin:
    case OpCodeId::CheckEnd:
        return ByteString::empty();
    case OpCodeId::GoBack:
        return ByteString::formatted("count={}", arg(0));
    case OpCodeId::SetStepBack:
        return ByteString::formatted("step={}", static_cast<i64>(arg(0)));
    case OpCodeId::IncStepBack:
        return ByteString::formatted("inc step back");
    case OpCodeId::CheckStepBack:
        return ByteString::formatted("check step back");
    case OpCodeId::CheckSavedPosition:
        return ByteString::formatted("check saved back");
    case OpCodeId::Jump:
        return ByteString::formatted("offset={} [&{}]", static_cast<ssize_t>(arg(0)), ip + sz + static_cast<ssize_t>(arg(0)));
    case OpCodeId::ForkJump:
        return ByteString::formatted("offset={} [&{}], sp: {}", static_cast<ssize_t>(arg(0)), ip + sz + static_cast<ssize_t>(arg(0)), state.string_position);
    case OpCodeId::ForkReplaceJump:
        return ByteString::formatted("offset={} [&{}], sp: {}", static_cast<ssize_t>(arg(0)), ip + sz + static_cast<ssize_t>(arg(0)), state.string_position);
    case OpCodeId::ForkStay:
        return ByteString::formatted("offset={} [&{}], sp: {}", static_cast<ssize_t>(arg(0)), ip + sz + static_cast<ssize_t>(arg(0)), state.string_position);
    case OpCodeId::ForkReplaceStay:
        return ByteString::formatted("offset={} [&{}], sp: {}", static_cast<ssize_t>(arg(0)), ip + sz + static_cast<ssize_t>(arg(0)), state.string_position);
    case OpCodeId::CheckBoundary:
        return ByteString::formatted("kind={} ({})", static_cast<unsigned long>(arg(0)), boundary_check_type_name(static_cast<BoundaryCheckType>(arg(0))));
    case OpCodeId::ClearCaptureGroup:
    case OpCodeId::SaveLeftCaptureGroup:
    case OpCodeId::SaveRightCaptureGroup:
    case OpCodeId::Checkpoint:
        return ByteString::formatted("id={}", arg(0));
    case OpCodeId::FailIfEmpty:
        return ByteString::formatted("checkpoint={}", arg(0));
    case OpCodeId::SaveRightNamedCaptureGroup:
        return ByteString::formatted("name_id={}, id={}", arg(0), arg(1));
    case OpCodeId::RSeekTo: {
        auto ch = arg(0);
        if (ch <= 0x7f)
            return ByteString::formatted("before '{}'", ch);
        return ByteString::formatted("before u+{:04x}", arg(0));
    }
    case OpCodeId::Compare:
        return ByteString::formatted("argc={}, args={} ", arg(0), arg(1));
    case OpCodeId::CompareSimple: {
        StringBuilder builder;
        auto type = static_cast<CharacterCompareType>(arg(1));
        builder.append(character_compare_type_name(type));
        switch (type) {
        case CharacterCompareType::Char: {
            auto ch = arg(2);
            if (is_ascii_printable(ch))
                builder.append(ByteString::formatted(" '{:c}'", static_cast<char>(ch)));
            else
                builder.append(ByteString::formatted(" 0x{:x}", ch));
            break;
        }
        case CharacterCompareType::String: {
            auto string_index = arg(2);
            auto string = bytecode.get_u16_string(string_index);
            builder.appendff(" \"{}\"", string);
            break;
        }
        case CharacterCompareType::CharClass: {
            auto character_class = static_cast<CharClass>(arg(2));
            builder.appendff(" {}", character_class_name(character_class));
            break;
        }
        case CharacterCompareType::Reference: {
            auto ref = arg(2);
            builder.appendff(" number={}", ref);
            break;
        }
        case CharacterCompareType::NamedReference: {
            auto ref = arg(2);
            builder.appendff(" named_number={}", ref);
            break;
        }
        case CharacterCompareType::GeneralCategory:
        case CharacterCompareType::Property:
        case CharacterCompareType::Script:
        case CharacterCompareType::ScriptExtension:
        case CharacterCompareType::StringSet: {
            builder.appendff(" value={}", arg(2));
            break;
        }
        case CharacterCompareType::LookupTable: {
            auto count_sensitive = arg(2);
            auto count_insensitive = arg(3);
            for (size_t j = 0; j < count_sensitive; ++j) {
                auto range = static_cast<CharRange>(arg(4 + j));
                builder.appendff(" {:x}-{:x}", range.from, range.to);
            }
            if (count_insensitive > 0) {
                builder.append(" [insensitive ranges:"sv);
                for (size_t j = 0; j < count_insensitive; ++j) {
                    auto range = static_cast<CharRange>(arg(4 + count_sensitive + j));
                    builder.appendff("  {:x}-{:x}", range.from, range.to);
                }
                builder.append(" ]"sv);
            }
            break;
        }
        case CharacterCompareType::CharRange: {
            auto value = arg(2);
            auto range = static_cast<CharRange>(value);
            builder.appendff(" {:x}-{:x}", range.from, range.to);
            break;
        }
        default:
            break;
        }
        return builder.to_byte_string();
    }
    case OpCodeId::Repeat: {
        auto repeat_id = arg(2);
        auto reps = repeat_id < state.repetition_marks.size() ? state.repetition_marks.at(repeat_id) : 0;
        return ByteString::formatted("offset={} [&{}] count={} id={} rep={}, sp: {}",
            static_cast<ssize_t>(arg(0)),
            ip - arg(0),
            arg(1) + 1,
            repeat_id,
            reps + 1,
            state.string_position);
    }
    case OpCodeId::ResetRepeat: {
        auto repeat_id = arg(0);
        auto reps = repeat_id < state.repetition_marks.size() ? state.repetition_marks.at(repeat_id) : 0;
        return ByteString::formatted("id={} rep={}", repeat_id, reps + 1);
    }
    case OpCodeId::JumpNonEmpty:
        return ByteString::formatted("{} offset={} [&{}], cp={}",
            opcode_id_name(static_cast<OpCodeId>(arg(2))),
            static_cast<ssize_t>(arg(0)), ip + sz + static_cast<ssize_t>(arg(0)),
            arg(1));
    case OpCodeId::ForkIf:
        return ByteString::formatted("{} {} offset={} [&{}]",
            opcode_id_name(static_cast<OpCodeId>(arg(1))),
            fork_if_condition_name(static_cast<ForkIfCondition>(arg(2))),
            static_cast<ssize_t>(arg(0)), ip + sz + static_cast<ssize_t>(arg(0)));
    }
    VERIFY_NOT_REACHED();
}

Vector<ByteString> compare_variable_arguments_to_byte_string(ByteCodeValueType const* data, size_t ip, MatchState const& state, ByteCodeBase const& bytecode, Optional<MatchInput const&> input)
{
    Vector<ByteString> result;

    size_t offset = ip + 3;
    RegexStringView const& view = input.has_value() ? input.value().view : StringView {};

    auto argument_count = data[ip + 1]; // arguments_count for Compare

    for (size_t i = 0; i < argument_count; ++i) {
        auto compare_type = static_cast<CharacterCompareType>(data[offset++]);
        result.empend(ByteString::formatted("type={} [{}]", static_cast<size_t>(compare_type), character_compare_type_name(compare_type)));

        auto string_start_offset = state.string_position_before_match;

        if (compare_type == CharacterCompareType::Char) {
            auto ch = data[offset++];
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
            auto ref = data[offset++];
            result.empend(ByteString::formatted(" number={}", ref));
            if (input.has_value()) {
                if (state.capture_group_matches_size() > input->match_index) {
                    auto match = state.capture_group_matches(input->match_index);
                    if (match.size() > ref) {
                        auto& group = match[ref];
                        result.empend(ByteString::formatted(" left={}", group.left_column));
                        result.empend(ByteString::formatted(" right={}", group.left_column + group.view.length_in_code_units()));
                        result.empend(ByteString::formatted(" contents='{}'", group.view));
                    } else {
                        result.empend(ByteString::formatted(" (invalid ref, max={})", match.size() - 1));
                    }
                } else {
                    result.empend(ByteString::formatted(" (invalid index {}, max={})", input->match_index, state.capture_group_matches_size() - 1));
                }
            }
        } else if (compare_type == CharacterCompareType::NamedReference) {
            auto ref = data[offset++];
            result.empend(ByteString::formatted(" named_number={}", ref));
            if (input.has_value()) {
                if (state.capture_group_matches_size() > input->match_index) {
                    auto match = state.capture_group_matches(input->match_index);
                    if (match.size() > ref) {
                        auto& group = match[ref];
                        result.empend(ByteString::formatted(" left={}", group.left_column));
                        result.empend(ByteString::formatted(" right={}", group.left_column + group.view.length_in_code_units()));
                        result.empend(ByteString::formatted(" contents='{}'", group.view));
                    } else {
                        result.empend(ByteString::formatted(" (invalid ref {}, max={})", ref, match.size() - 1));
                    }
                } else {
                    result.empend(ByteString::formatted(" (invalid index {}, max={})", input->match_index, state.capture_group_matches_size() - 1));
                }
            }
        } else if (compare_type == CharacterCompareType::String) {
            auto str_id = data[offset++];
            auto string = bytecode.get_u16_string(str_id);
            result.empend(ByteString::formatted(" value=\"{}\"", string));
            if (!view.is_null() && view.length() > state.string_position)
                result.empend(ByteString::formatted(
                    " compare against: \"{}\"",
                    input.value().view.substring_view(string_start_offset, string_start_offset + string.length_in_code_units() > view.length() ? 0 : string.length_in_code_units()).to_byte_string()));
        } else if (compare_type == CharacterCompareType::CharClass) {
            auto character_class = static_cast<CharClass>(data[offset++]);
            result.empend(ByteString::formatted(" ch_class={} [{}]", static_cast<size_t>(character_class), character_class_name(character_class)));
            if (!view.is_null() && view.length() > state.string_position)
                result.empend(ByteString::formatted(
                    " compare against: '{}'",
                    input.value().view.substring_view(string_start_offset, state.string_position > view.length() ? 0 : 1).to_byte_string()));
        } else if (compare_type == CharacterCompareType::CharRange) {
            auto value = static_cast<CharRange>(data[offset++]);
            result.empend(ByteString::formatted(" ch_range={:x}-{:x}", value.from, value.to));
            if (!view.is_null() && view.length() > state.string_position)
                result.empend(ByteString::formatted(
                    " compare against: '{}'",
                    input.value().view.substring_view(string_start_offset, state.string_position > view.length() ? 0 : 1).to_byte_string()));
        } else if (compare_type == CharacterCompareType::LookupTable) {
            auto count_sensitive = data[offset++];
            auto count_insensitive = data[offset++];
            for (size_t j = 0; j < count_sensitive; ++j) {
                auto range = static_cast<CharRange>(data[offset++]);
                result.append(ByteString::formatted(" {:x}-{:x}", range.from, range.to));
            }
            if (count_insensitive > 0) {
                result.append(" [insensitive ranges:");
                for (size_t j = 0; j < count_insensitive; ++j) {
                    auto range = static_cast<CharRange>(data[offset++]);
                    result.append(ByteString::formatted("  {:x}-{:x}", range.from, range.to));
                }
                result.append(" ]");
            }

            if (!view.is_null() && view.length() > state.string_position)
                result.empend(ByteString::formatted(
                    " compare against: '{}'",
                    input.value().view.substring_view(string_start_offset, state.string_position > view.length() ? 0 : 1).to_byte_string()));
        } else if (compare_type == CharacterCompareType::GeneralCategory
            || compare_type == CharacterCompareType::Property
            || compare_type == CharacterCompareType::Script
            || compare_type == CharacterCompareType::ScriptExtension
            || compare_type == CharacterCompareType::StringSet) {
            auto value = data[offset++];
            result.empend(ByteString::formatted(" value={}", value));
        }
    }
    return result;
}

Vector<CompareTypeAndValuePair> flat_compares_at(ByteCodeValueType const* data, size_t ip, bool is_simple)
{
    Vector<CompareTypeAndValuePair> result;

    size_t offset = ip + (is_simple ? 2 : 3);
    auto argument_count = is_simple ? 1 : data[ip + OpArgs::Compare::arguments_count];

    for (size_t i = 0; i < argument_count; ++i) {
        auto compare_type = (CharacterCompareType)data[offset++];

        if (compare_type == CharacterCompareType::Char) {
            auto ch = data[offset++];
            result.append({ compare_type, ch });
        } else if (compare_type == CharacterCompareType::Reference) {
            auto ref = data[offset++];
            result.append({ compare_type, ref });
        } else if (compare_type == CharacterCompareType::NamedReference) {
            auto ref = data[offset++];
            result.append({ compare_type, ref });
        } else if (compare_type == CharacterCompareType::String) {
            auto string_index = data[offset++];
            result.append({ compare_type, string_index });
        } else if (compare_type == CharacterCompareType::CharClass) {
            auto character_class = data[offset++];
            result.append({ compare_type, character_class });
        } else if (compare_type == CharacterCompareType::CharRange) {
            auto value = data[offset++];
            result.append({ compare_type, value });
        } else if (compare_type == CharacterCompareType::LookupTable) {
            auto count_sensitive = data[offset++];
            auto count_insensitive = data[offset++];
            for (size_t j = 0; j < count_sensitive; ++j)
                result.append({ CharacterCompareType::CharRange, data[offset++] });
            offset += count_insensitive; // Skip insensitive ranges
        } else if (compare_type == CharacterCompareType::GeneralCategory
            || compare_type == CharacterCompareType::Property
            || compare_type == CharacterCompareType::Script
            || compare_type == CharacterCompareType::ScriptExtension
            || compare_type == CharacterCompareType::StringSet) {
            auto value = data[offset++];
            result.append({ compare_type, value });
        } else {
            result.append({ compare_type, 0 });
        }
    }
    return result;
}

}
