/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/BumpAllocator.h>
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/StringBuilder.h>
#include <LibRegex/RegexMatcher.h>
#include <LibRegex/RegexParser.h>
#include <LibUnicode/CharacterTypes.h>

#if REGEX_DEBUG
#    include <LibRegex/RegexDebug.h>
#endif

// U+2028 LINE SEPARATOR
constexpr static u32 const LineSeparator { 0x2028 };
// U+2029 PARAGRAPH SEPARATOR
constexpr static u32 const ParagraphSeparator { 0x2029 };

namespace regex {

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

static ALWAYS_INLINE void compare_char(MatchInput const& input, MatchState& state, u32 ch1, bool inverse, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    // FIXME: Figure out how to do this if unicode() without performing a substring split first.
    auto input_view = input.view.unicode()
        ? input.view.substring_view(state.string_position, 1).code_point_at(0)
        : input.view.unicode_aware_code_point_at(state.string_position_in_code_units);

    bool equal;
    if (state.current_options & AllFlags::Insensitive) {
        equal = Unicode::canonicalize(input_view, input.view.unicode()) == Unicode::canonicalize(ch1, input.view.unicode());
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

static ALWAYS_INLINE bool compare_string(MatchInput const& input, MatchState& state, RegexStringView str, bool& had_zero_length_match)
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
    if (state.current_options & AllFlags::Insensitive)
        equals = subject.equals_ignoring_case(str, input.view.unicode());
    else
        equals = subject.equals(str);

    if (equals)
        advance_string_position(state, input.view, str);

    return equals;
}

static ALWAYS_INLINE void compare_character_class(MatchInput const& input, MatchState& state, CharClass character_class, u32 ch, bool inverse, bool& inverse_matched)
{
    if (matches_character_class(character_class, ch, state.current_options & AllFlags::Insensitive, input.view.unicode())) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, ch);
    }
}

static ALWAYS_INLINE void compare_character_range(MatchInput const& input, MatchState& state, u32 from, u32 to, u32 ch, bool inverse, bool& inverse_matched)
{
    bool matched = false;
    if (state.current_options & AllFlags::Insensitive) {
        matched = Unicode::code_point_matches_range_ignoring_case(ch, from, to, input.view.unicode());
    } else {
        matched = (ch >= from && ch <= to);
    }

    if (matched) {
        if (inverse)
            inverse_matched = true;
        else
            advance_string_position(state, input.view, ch);
    }
}

static ALWAYS_INLINE void compare_property(MatchInput const& input, MatchState& state, Unicode::Property property, bool inverse, bool is_double_negation, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view.code_point_at(state.string_position_in_code_units);
    bool case_insensitive = (state.current_options & AllFlags::Insensitive) && input.view.unicode();
    bool is_unicode_sets_mode = state.current_options.has_flag_set(AllFlags::UnicodeSets);

    // In /u mode, case folding happens after complementing: \P{x} matches caseFold(allChars - charsWithX).
    // This means a code point matches the inverted property if ANY of its case variants lacks the property.
    // In /v mode, case folding happens before complementing: \P{x} matches caseFold(allChars) - caseFold(charsWithX),
    // so we can just use normal case-insensitive matching and invert the result.
    if ((inverse || is_double_negation) && case_insensitive && !is_unicode_sets_mode) {
        bool any_variant_lacks_property = false;
        Unicode::for_each_case_folded_code_point(code_point, [&](u32 variant) {
            if (!Unicode::code_point_has_property(variant, property)) {
                any_variant_lacks_property = true;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });

        if (is_double_negation) {
            if (any_variant_lacks_property)
                return;
            advance_string_position(state, input.view, code_point);
        } else if (!any_variant_lacks_property) {
            inverse_matched = true;
            return;
        }
    } else {
        auto case_sensitivity = case_insensitive && (is_unicode_sets_mode || !inverse) ? CaseSensitivity::CaseInsensitive : CaseSensitivity::CaseSensitive;
        if (Unicode::code_point_has_property(code_point, property, case_sensitivity)) {
            if (inverse)
                inverse_matched = true;
            else
                advance_string_position(state, input.view, code_point);
        }
    }
}

static ALWAYS_INLINE void compare_general_category(MatchInput const& input, MatchState& state, Unicode::GeneralCategory general_category, bool inverse, bool is_double_negation, bool& inverse_matched)
{
    if (state.string_position == input.view.length())
        return;

    u32 code_point = input.view.code_point_at(state.string_position_in_code_units);
    bool case_insensitive = (state.current_options & AllFlags::Insensitive) && input.view.unicode();
    bool is_unicode_sets_mode = state.current_options.has_flag_set(AllFlags::UnicodeSets);

    // See comment in compare_property for /u vs /v mode case folding semantics.
    if ((inverse || is_double_negation) && case_insensitive && !is_unicode_sets_mode) {
        bool any_variant_lacks_category = false;
        Unicode::for_each_case_folded_code_point(code_point, [&](u32 variant) {
            if (!Unicode::code_point_has_general_category(variant, general_category)) {
                any_variant_lacks_category = true;
                return IterationDecision::Break;
            }
            return IterationDecision::Continue;
        });

        if (is_double_negation) {
            if (any_variant_lacks_category)
                return;
            advance_string_position(state, input.view, code_point);
        } else if (!any_variant_lacks_category) {
            inverse_matched = true;
            return;
        }
    } else {
        auto case_sensitivity = case_insensitive && (is_unicode_sets_mode || !inverse) ? CaseSensitivity::CaseInsensitive : CaseSensitivity::CaseSensitive;
        if (Unicode::code_point_has_general_category(code_point, general_category, case_sensitivity)) {
            if (inverse)
                inverse_matched = true;
            else
                advance_string_position(state, input.view, code_point);
        }
    }
}

static ALWAYS_INLINE void compare_script(MatchInput const& input, MatchState& state, Unicode::Script script, bool inverse, bool& inverse_matched)
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

static ALWAYS_INLINE void compare_script_extension(MatchInput const& input, MatchState& state, Unicode::Script script, bool inverse, bool& inverse_matched)
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

#if REGEX_DEBUG
static RegexDebug<FlatByteCode> s_regex_dbg(stderr);
#endif

template<typename ByteCode, bool IsSimple>
ALWAYS_INLINE ExecutionResult compare_execute(ByteCode const& bc, MatchInput const& input, MatchState& state)
{
    auto const argument_count = IsSimple ? 1 : bc.at(state.instruction_position + 1);
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

    auto const* bytecode_data = bc.flat_data().data();

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
                || (state.current_options.has_flag_set(AllFlags::Internal_ECMA262DotSemantics)
                        ? (input_view == '\r' || input_view == LineSeparator || input_view == ParagraphSeparator)
                        : false);

            if (!is_equivalent_to_newline || (state.current_options.has_flag_set(AllFlags::SingleLine) && state.current_options.has_flag_set(AllFlags::Internal_ConsiderNewline))) {
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
            auto string = bc.get_u16_string(string_index);

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
            auto sensitive_range_data = bc.flat_data().slice(offset, count_sensitive);
            offset += count_sensitive;
            auto insensitive_range_data = bc.flat_data().slice(offset, count_insensitive);
            offset += count_insensitive;

            bool const insensitive = state.current_options & AllFlags::Insensitive;
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

            auto reference_name_index = bc.get_group_name_index(reference_number);

            if (reference_name_index.has_value()) {
                auto target_name_string = bc.get_string(reference_name_index.value());

                for (size_t i = 0; i < groups.size(); ++i) {
                    if (groups[i].view.is_null())
                        continue;

                    auto group_name_index = bc.get_group_name_index(i);

                    if (group_name_index.has_value()) {
                        auto group_name_string = bc.get_string(group_name_index.value());

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
            compare_property(input, state, property, current_inversion_state(), temporary_inverse && inverse, inverse_matched);
            break;
        }
        case CharacterCompareType::GeneralCategory: {
            auto general_category = static_cast<Unicode::GeneralCategory>(bytecode_data[offset++]);
            compare_general_category(input, state, general_category, current_inversion_state(), temporary_inverse && inverse, inverse_matched);
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

                    if (state.current_options & AllFlags::Insensitive) {
                        bool found_child = false;
                        for (auto const& [key, child] : current->children()) {
                            if (Unicode::canonicalize(key, input.view.unicode()) == Unicode::canonicalize(value, input.view.unicode())) {
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
                auto const& trie = bc.string_set_table().get_u8_trie(string_set_index);
                StringView view;
                find_longest_match(view, trie);
            } else {
                auto const& view = input.view.u16_view();
                auto const& trie = bc.string_set_table().get_u16_trie(string_set_index);
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

struct InstructionResult {
    ExecutionResult result;
    size_t size;
};

// Switch-based interpreter: executes a single bytecode instruction.
// Returns the ExecutionResult and the instruction size.
static ALWAYS_INLINE InstructionResult execute_instruction(
    OpCodeId id,
    ByteCodeValueType const* data,
    size_t data_size,
    FlatByteCode const& bytecode,
    MatchInput const& input,
    MatchState& state)
{
    auto ip = state.instruction_position;
    auto current_ip_size = opcode_size(id, data, ip);

    switch (id) {
    case OpCodeId::Exit: {
        if (state.string_position > input.view.length() || state.instruction_position >= data_size)
            return { ExecutionResult::Succeeded, current_ip_size };
        return { ExecutionResult::Failed, current_ip_size };
    }
    case OpCodeId::Save: {
        save_string_position(input, state);
        state.forks_since_last_save = 0;
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::Restore: {
        if (!restore_string_position(input, state))
            return { ExecutionResult::Failed, current_ip_size };
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::GoBack: {
        auto count = data[ip + OpArgs::GoBack::count];
        if (count > state.string_position)
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        reverse_string_position(state, input.view, count);
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::SetStepBack: {
        state.step_backs.append(static_cast<i64>(data[ip + OpArgs::SetStepBack::step]));
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::IncStepBack: {
        if (state.step_backs.is_empty())
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        size_t last_step_back = static_cast<size_t>(++state.step_backs.last());
        if (last_step_back > state.string_position)
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        reverse_string_position(state, input.view, last_step_back);
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::CheckStepBack: {
        if (state.step_backs.is_empty())
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        if (input.saved_positions.is_empty())
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        if (static_cast<size_t>(state.step_backs.last()) > input.saved_positions.last())
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        state.string_position = input.saved_positions.last();
        state.string_position_in_code_units = input.saved_code_unit_positions.last();
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::CheckSavedPosition: {
        if (input.saved_positions.is_empty())
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        if (state.string_position != input.saved_positions.last())
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        state.step_backs.take_last();
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::FailForks: {
        input.fail_counter += state.forks_since_last_save;
        return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
    }
    case OpCodeId::PopSaved: {
        if (input.saved_positions.is_empty() || input.saved_code_unit_positions.is_empty())
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        input.saved_positions.take_last();
        input.saved_code_unit_positions.take_last();
        return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
    }
    case OpCodeId::Jump: {
        state.instruction_position += static_cast<ssize_t>(data[ip + OpArgs::Jump::offset]);
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::ForkJump: {
        auto offset = static_cast<ssize_t>(data[ip + OpArgs::Jump::offset]);
        state.fork_at_position = ip + current_ip_size + offset;
        state.forks_since_last_save++;
        return { ExecutionResult::Fork_PrioHigh, current_ip_size };
    }
    case OpCodeId::ForkReplaceJump: {
        auto offset = static_cast<ssize_t>(data[ip + OpArgs::Jump::offset]);
        state.fork_at_position = ip + current_ip_size + offset;
        input.fork_to_replace = ip;
        state.forks_since_last_save++;
        return { ExecutionResult::Fork_PrioHigh, current_ip_size };
    }
    case OpCodeId::ForkStay: {
        auto offset = static_cast<ssize_t>(data[ip + OpArgs::Jump::offset]);
        state.fork_at_position = ip + current_ip_size + offset;
        state.forks_since_last_save++;
        return { ExecutionResult::Fork_PrioLow, current_ip_size };
    }
    case OpCodeId::ForkReplaceStay: {
        auto offset = static_cast<ssize_t>(data[ip + OpArgs::Jump::offset]);
        state.fork_at_position = ip + current_ip_size + offset;
        input.fork_to_replace = ip;
        return { ExecutionResult::Fork_PrioLow, current_ip_size };
    }
    case OpCodeId::ForkIf: {
        auto offset = static_cast<ssize_t>(data[ip + OpArgs::ForkIf::offset]);
        auto form = static_cast<OpCodeId>(data[ip + OpArgs::ForkIf::form]);
        auto condition = static_cast<ForkIfCondition>(data[ip + OpArgs::ForkIf::condition]);

        auto next_step = [&](bool do_fork) -> ExecutionResult {
            switch (form) {
            case OpCodeId::ForkJump:
                if (do_fork) {
                    state.fork_at_position = ip + current_ip_size + offset;
                    state.forks_since_last_save++;
                    return ExecutionResult::Fork_PrioHigh;
                }
                return ExecutionResult::Continue;
            case OpCodeId::ForkReplaceJump:
                if (do_fork) {
                    state.fork_at_position = ip + current_ip_size + offset;
                    input.fork_to_replace = ip;
                    state.forks_since_last_save++;
                    return ExecutionResult::Fork_PrioHigh;
                }
                return ExecutionResult::Continue;
            case OpCodeId::ForkStay:
                if (do_fork) {
                    state.fork_at_position = ip + current_ip_size + offset;
                    state.forks_since_last_save++;
                    return ExecutionResult::Fork_PrioLow;
                }
                state.instruction_position += offset;
                return ExecutionResult::Continue;
            case OpCodeId::ForkReplaceStay:
                if (do_fork) {
                    state.fork_at_position = ip + current_ip_size + offset;
                    input.fork_to_replace = ip;
                    return ExecutionResult::Fork_PrioLow;
                }
                state.instruction_position += offset;
                return ExecutionResult::Continue;
            default:
                VERIFY_NOT_REACHED();
            }
        };

        switch (condition) {
        case ForkIfCondition::AtStartOfLine:
            return { next_step(!input.in_the_middle_of_a_line), current_ip_size };
        case ForkIfCondition::Invalid:
        default:
            VERIFY_NOT_REACHED();
        }
    }
    case OpCodeId::CheckBegin: {
        auto is_at_line_boundary = [&] {
            if (state.string_position == 0)
                return true;
            if (state.current_options.has_flag_set(AllFlags::Multiline) && state.current_options.has_flag_set(AllFlags::Internal_ConsiderNewline)) {
                auto input_view = input.view.substring_view(state.string_position - 1, 1).code_point_at(0);
                return input_view == '\r' || input_view == '\n' || input_view == LineSeparator || input_view == ParagraphSeparator;
            }
            return false;
        }();
        if (is_at_line_boundary && (state.current_options & AllFlags::MatchNotBeginOfLine))
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        if ((is_at_line_boundary && !(state.current_options & AllFlags::MatchNotBeginOfLine))
            || (!is_at_line_boundary && (state.current_options & AllFlags::MatchNotBeginOfLine))
            || (is_at_line_boundary && (state.current_options & AllFlags::Global)))
            return { ExecutionResult::Continue, current_ip_size };
        return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
    }
    case OpCodeId::CheckEnd: {
        auto is_at_line_boundary = [&] {
            if (state.string_position == input.view.length())
                return true;
            if (state.current_options.has_flag_set(AllFlags::Multiline) && state.current_options.has_flag_set(AllFlags::Internal_ConsiderNewline)) {
                auto input_view = input.view.substring_view(state.string_position, 1).code_point_at(0);
                return input_view == '\r' || input_view == '\n' || input_view == LineSeparator || input_view == ParagraphSeparator;
            }
            return false;
        }();
        if (is_at_line_boundary && (state.current_options & AllFlags::MatchNotEndOfLine))
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        if ((is_at_line_boundary && !(state.current_options & AllFlags::MatchNotEndOfLine))
            || (!is_at_line_boundary && (state.current_options & AllFlags::MatchNotEndOfLine || state.current_options & AllFlags::MatchNotBeginOfLine)))
            return { ExecutionResult::Continue, current_ip_size };
        return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
    }
    case OpCodeId::CheckBoundary: {
        auto boundary_type = static_cast<BoundaryCheckType>(data[ip + OpArgs::CheckBoundary::type]);
        auto isword = [&](auto ch) {
            return is_word_character(ch, state.current_options & AllFlags::Insensitive, input.view.unicode());
        };
        auto is_word_boundary = [&] {
            if (state.string_position == input.view.length())
                return (state.string_position > 0 && isword(input.view.code_point_at(state.string_position_in_code_units - 1)));
            if (state.string_position == 0)
                return (isword(input.view.code_point_at(0)));
            return !!(isword(input.view.code_point_at(state.string_position_in_code_units)) ^ isword(input.view.code_point_at(state.string_position_in_code_units - 1)));
        };
        switch (boundary_type) {
        case BoundaryCheckType::Word:
            if (is_word_boundary())
                return { ExecutionResult::Continue, current_ip_size };
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        case BoundaryCheckType::NonWord:
            if (!is_word_boundary())
                return { ExecutionResult::Continue, current_ip_size };
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        }
        VERIFY_NOT_REACHED();
    }
    case OpCodeId::ClearCaptureGroup: {
        auto group_id = data[ip + OpArgs::ClearCaptureGroup::id];
        if (input.match_index < state.capture_group_matches_size()) {
            auto group = state.mutable_capture_group_matches(input.match_index);
            group[group_id - 1].reset();
        }
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::FailIfEmpty: {
        auto checkpoint_id = data[ip + OpArgs::FailIfEmpty::checkpoint];
        u64 current_position = state.string_position + 1;
        auto checkpoint_position = state.checkpoints.get(checkpoint_id).value_or(current_position);
        if (checkpoint_position == current_position)
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::SaveLeftCaptureGroup: {
        auto group_id = data[ip + OpArgs::SaveLeftCaptureGroup::id];
        if (input.match_index >= state.capture_group_matches_size()) {
            state.flat_capture_group_matches.ensure_capacity((input.match_index + 1) * state.capture_group_count);
            for (size_t i = state.capture_group_matches_size(); i <= input.match_index; ++i)
                for (size_t j = 0; j < state.capture_group_count; ++j)
                    state.flat_capture_group_matches.append({});
        }
        state.mutable_capture_group_matches(input.match_index).at(group_id - 1).left_column = state.string_position;
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::SaveRightCaptureGroup: {
        auto group_id = data[ip + OpArgs::SaveRightCaptureGroup::id];
        auto& match = state.capture_group_matches(input.match_index).at(group_id - 1);
        auto start_position = match.left_column;
        if (state.string_position < start_position) {
            dbgln("Right capture group {} is before left capture group {}!", state.string_position, start_position);
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        }
        auto length = state.string_position - start_position;
        if (start_position < match.column && state.step_backs.is_empty())
            return { ExecutionResult::Continue, current_ip_size };
        VERIFY(start_position + length <= input.view.length_in_code_units());
        auto captured_text = input.view.substring_view(start_position, length);
        auto& existing_capture = state.mutable_capture_group_matches(input.match_index).at(group_id - 1);
        if (length == 0 && !existing_capture.view.is_null() && existing_capture.view.length() > 0) {
            auto existing_end_position = existing_capture.global_offset - input.global_offset + existing_capture.view.length();
            if (existing_end_position == state.string_position)
                return { ExecutionResult::Continue, current_ip_size };
        }
        state.mutable_capture_group_matches(input.match_index).at(group_id - 1) = { captured_text, input.line, start_position, input.global_offset + start_position };
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::SaveRightNamedCaptureGroup: {
        auto name_index = data[ip + OpArgs::SaveRightNamedCaptureGroup::name_index];
        auto group_id = data[ip + OpArgs::SaveRightNamedCaptureGroup::id];
        auto& match = state.capture_group_matches(input.match_index).at(group_id - 1);
        auto start_position = match.left_column;
        if (state.string_position < start_position)
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        auto length = state.string_position - start_position;
        if (start_position < match.column)
            return { ExecutionResult::Continue, current_ip_size };
        VERIFY(start_position + length <= input.view.length_in_code_units());
        auto view = input.view.substring_view(start_position, length);
        auto& existing_capture = state.mutable_capture_group_matches(input.match_index).at(group_id - 1);
        if (length == 0 && !existing_capture.view.is_null() && existing_capture.view.length() > 0) {
            auto existing_end_position = existing_capture.global_offset - input.global_offset + existing_capture.view.length();
            if (existing_end_position == state.string_position)
                return { ExecutionResult::Continue, current_ip_size };
        }
        state.mutable_capture_group_matches(input.match_index).at(group_id - 1) = { view, name_index, input.line, start_position, input.global_offset + start_position };
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::RSeekTo: {
        auto ch = data[ip + OpArgs::RSeekTo::ch];

        size_t search_from;
        size_t search_from_in_code_units;
        auto line_limited = false;

        if (state.string_position_before_rseek == NumericLimits<size_t>::max()) {
            state.string_position_before_rseek = state.string_position;
            state.string_position_in_code_units_before_rseek = state.string_position_in_code_units;

            if (!input.regex_options.has_flag_set(AllFlags::SingleLine)) {
                auto end_of_line = input.view.find_end_of_line(state.string_position, state.string_position_in_code_units);
                search_from = end_of_line.code_point_index + 1;
                search_from_in_code_units = end_of_line.code_unit_index + 1;
                line_limited = true;
            } else {
                search_from = NumericLimits<size_t>::max();
                search_from_in_code_units = NumericLimits<size_t>::max();
            }
        } else {
            search_from = state.string_position;
            search_from_in_code_units = state.string_position_in_code_units;
        }

        auto next = input.view.find_index_of_previous(ch, search_from, search_from_in_code_units);
        if (!next.has_value() || next->code_unit_index < state.string_position_in_code_units_before_rseek) {
            if (line_limited)
                return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
            return { ExecutionResult::Failed_ExecuteLowPrioForksButNoFurtherPossibleMatches, current_ip_size };
        }
        state.string_position = next->code_point_index;
        state.string_position_in_code_units = next->code_unit_index;
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::SaveModifiers: {
        auto current_flags = to_underlying(state.current_options.value());
        state.modifier_stack.append(current_flags);
        state.current_options = AllOptions { static_cast<AllFlags>(data[ip + OpArgs::SaveModifiers::new_modifiers]) };
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::RestoreModifiers: {
        if (state.modifier_stack.is_empty())
            return { ExecutionResult::Failed, current_ip_size };
        auto previous_modifiers = state.modifier_stack.take_last();
        state.current_options = AllOptions { static_cast<AllFlags>(previous_modifiers) };
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::Repeat: {
        auto repeat_offset = data[ip + OpArgs::Repeat::offset];
        auto repeat_count = data[ip + OpArgs::Repeat::count];
        auto repeat_id = data[ip + OpArgs::Repeat::id];
        VERIFY(repeat_count > 0);
        if (repeat_id >= state.repetition_marks.size())
            state.repetition_marks.resize(repeat_id + 1);
        auto& repetition_mark = state.repetition_marks.mutable_at(repeat_id);
        if (repetition_mark == repeat_count - 1) {
            repetition_mark = 0;
        } else {
            state.instruction_position -= repeat_offset + current_ip_size;
            ++repetition_mark;
        }
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::ResetRepeat: {
        auto repeat_id = data[ip + OpArgs::ResetRepeat::id];
        if (repeat_id >= state.repetition_marks.size())
            state.repetition_marks.resize(repeat_id + 1);
        state.repetition_marks.mutable_at(repeat_id) = 0;
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::Checkpoint: {
        auto checkpoint_id = data[ip + OpArgs::Checkpoint::id];
        if (checkpoint_id >= state.checkpoints.size())
            state.checkpoints.resize(checkpoint_id + 1);
        state.checkpoints[checkpoint_id] = state.string_position + 1;
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::JumpNonEmpty: {
        auto offset = static_cast<ssize_t>(data[ip + OpArgs::JumpNonEmpty::offset]);
        auto checkpoint_id = data[ip + OpArgs::JumpNonEmpty::checkpoint];
        auto form = static_cast<OpCodeId>(data[ip + OpArgs::JumpNonEmpty::form]);

        u64 current_position = state.string_position;
        auto checkpoint_position = state.checkpoints.get(checkpoint_id).value_or(0);

        if (checkpoint_position != 0 && checkpoint_position != current_position + 1) {
            if (form == OpCodeId::Jump) {
                state.instruction_position += offset;
                return { ExecutionResult::Continue, current_ip_size };
            }
            state.fork_at_position = ip + current_ip_size + offset;
            if (form == OpCodeId::ForkJump) {
                state.forks_since_last_save++;
                return { ExecutionResult::Fork_PrioHigh, current_ip_size };
            }
            if (form == OpCodeId::ForkStay) {
                state.forks_since_last_save++;
                return { ExecutionResult::Fork_PrioLow, current_ip_size };
            }
            if (form == OpCodeId::ForkReplaceStay) {
                input.fork_to_replace = ip;
                return { ExecutionResult::Fork_PrioLow, current_ip_size };
            }
            if (form == OpCodeId::ForkReplaceJump) {
                input.fork_to_replace = ip;
                return { ExecutionResult::Fork_PrioHigh, current_ip_size };
            }
        }
        if (form == OpCodeId::Jump && state.string_position < input.view.length())
            return { ExecutionResult::Failed_ExecuteLowPrioForks, current_ip_size };
        return { ExecutionResult::Continue, current_ip_size };
    }
    case OpCodeId::Compare:
        return { compare_execute<FlatByteCode, false>(bytecode, input, state), current_ip_size };
    case OpCodeId::CompareSimple:
        return { compare_execute<FlatByteCode, true>(bytecode, input, state), current_ip_size };
    }
    VERIFY_NOT_REACHED();
}

template<class Parser>
regex::Parser::Result Regex<Parser>::parse_pattern(StringView pattern, typename ParserTraits<Parser>::OptionsType regex_options)
{
    regex::Lexer lexer(pattern);

    Parser parser(lexer, regex_options);
    return parser.parse();
}

template<typename Parser>
struct CacheKey {
    ByteString pattern;
    typename ParserTraits<Parser>::OptionsType options;

    bool operator==(CacheKey const& other) const
    {
        return pattern == other.pattern && options.value() == other.options.value();
    }
};
template<class Parser>
static OrderedHashMap<CacheKey<Parser>, regex::Parser::Result> s_parser_cache;

template<class Parser>
static size_t s_cached_bytecode_size = 0;

static constexpr auto MaxRegexCachedBytecodeSize = 1 * MiB;

template<class Parser>
static void cache_parse_result(regex::Parser::Result const& result, CacheKey<Parser> const& key)
{
    auto bytecode_size = result.bytecode.visit([](auto& bytecode) { return bytecode.size() * sizeof(ByteCodeValueType); });
    if (bytecode_size > MaxRegexCachedBytecodeSize)
        return;

    while (bytecode_size + s_cached_bytecode_size<Parser> > MaxRegexCachedBytecodeSize)
        s_cached_bytecode_size<Parser> -= s_parser_cache<Parser>.take_first().bytecode.visit([](auto& bytecode) { return bytecode.size() * sizeof(ByteCodeValueType); });

    s_parser_cache<Parser>.set(key, result);
    s_cached_bytecode_size<Parser> += bytecode_size;
}

template<class Parser>
Regex<Parser>::Regex(ByteString pattern, typename ParserTraits<Parser>::OptionsType regex_options)
    : pattern_value(move(pattern))
    , parser_result(ByteCode {})
{
    if (auto cache_entry = s_parser_cache<Parser>.get({ pattern_value, regex_options }); cache_entry.has_value()) {
        parser_result = cache_entry.value();
    } else {
        regex::Lexer lexer(pattern_value);

        Parser parser(lexer, regex_options);
        parser_result = parser.parse();
        parser_result.bytecode.template get<ByteCode>().flatten();

        run_optimization_passes();

        if (parser_result.error == regex::Error::NoError)
            cache_parse_result<Parser>(parser_result, { pattern_value, regex_options });
    }

    if (parser_result.error == regex::Error::NoError)
        matcher = make<Matcher<Parser>>(this, static_cast<decltype(regex_options.value())>(parser_result.options.value()));
}

template<class Parser>
Regex<Parser>::Regex(regex::Parser::Result parse_result, ByteString pattern, typename ParserTraits<Parser>::OptionsType regex_options)
    : pattern_value(move(pattern))
    , parser_result(move(parse_result))
{
    parser_result.bytecode.template get<ByteCode>().flatten();
    run_optimization_passes();
    if (parser_result.error == regex::Error::NoError)
        matcher = make<Matcher<Parser>>(this, regex_options | static_cast<decltype(regex_options.value())>(parser_result.options.value()));
}

template<class Parser>
Regex<Parser>::Regex(Regex const& other)
    : pattern_value(other.pattern_value)
    , parser_result(other.parser_result)
{
    if (other.matcher)
        matcher = make<Matcher<Parser>>(this, other.matcher->options());
}

template<class Parser>
Regex<Parser>::Regex(Regex&& regex)
    : pattern_value(move(regex.pattern_value))
    , parser_result(move(regex.parser_result))
    , matcher(move(regex.matcher))
    , start_offset(regex.start_offset)
{
    if (matcher)
        matcher->reset_pattern({}, this);
}

template<class Parser>
Regex<Parser>& Regex<Parser>::operator=(Regex&& regex)
{
    pattern_value = move(regex.pattern_value);
    parser_result = move(regex.parser_result);
    matcher = move(regex.matcher);
    if (matcher)
        matcher->reset_pattern({}, this);
    start_offset = regex.start_offset;
    return *this;
}

template<class Parser>
typename ParserTraits<Parser>::OptionsType Regex<Parser>::options() const
{
    if (!matcher || parser_result.error != Error::NoError)
        return {};

    return matcher->options();
}

template<class Parser>
ByteString Regex<Parser>::error_string(Optional<ByteString> message) const
{
    StringBuilder eb;
    eb.append("Error during parsing of regular expression:\n"sv);
    eb.appendff("    {}\n    ", pattern_value);
    for (size_t i = 0; i < parser_result.error_token.position(); ++i)
        eb.append(' ');

    eb.appendff("^---- {}", message.value_or(get_error_string(parser_result.error)));
    return eb.to_byte_string();
}

template<typename Parser>
RegexResult Matcher<Parser>::match(RegexStringView view, Optional<typename ParserTraits<Parser>::OptionsType> regex_options) const
{
    AllOptions options = m_regex_options | regex_options.value_or({}).value();

    if constexpr (!IsSame<Parser, ECMA262>) {
        if (options.has_flag_set(AllFlags::Multiline))
            return match(view.lines(), regex_options); // FIXME: how do we know, which line ending a line has (1char or 2char)? This is needed to get the correct match offsets from start of string...
    }

    Vector<RegexStringView> views;
    views.append(view);
    return match(views, regex_options);
}

template<typename Parser>
RegexResult Matcher<Parser>::match(Vector<RegexStringView> const& views, Optional<typename ParserTraits<Parser>::OptionsType> regex_options) const
{
    // If the pattern *itself* isn't stateful, reset any changes to start_offset.
    if (!((AllFlags)m_regex_options.value() & AllFlags::Internal_Stateful))
        m_pattern->start_offset = 0;

    size_t match_count { 0 };

    MatchInput input;
    size_t operations = 0;

    input.pattern = m_pattern->pattern_value;

    input.regex_options = m_regex_options | regex_options.value_or({}).value();
    input.start_offset = m_pattern->start_offset;
    MatchState state(m_pattern->parser_result.capture_groups_count, input.regex_options);
    size_t lines_to_skip = 0;

    bool unicode = input.regex_options.has_flag_set(AllFlags::Unicode) || input.regex_options.has_flag_set(AllFlags::UnicodeSets);
    for (auto const& view : views)
        const_cast<RegexStringView&>(view).set_unicode(unicode);

    if constexpr (REGEX_DEBUG) {
        if (input.regex_options.has_flag_set(AllFlags::Internal_Stateful)) {
            if (views.size() > 1 && input.start_offset > views.first().length()) {
                dbgln("Started with start={}, goff={}, skip={}", input.start_offset, input.global_offset, lines_to_skip);
                for (auto const& view : views) {
                    if (input.start_offset < view.length() + 1)
                        break;
                    ++lines_to_skip;
                    input.start_offset -= view.length() + 1;
                    input.global_offset += view.length() + 1;
                }
                dbgln("Ended with start={}, goff={}, skip={}", input.start_offset, input.global_offset, lines_to_skip);
            }
        }
    }

    auto append_match = [](auto& input, auto& state, auto& start_position) {
        if (state.matches.size() == input.match_index)
            state.matches.empend();

        VERIFY(start_position + state.string_position - start_position <= input.view.length());
        state.matches.mutable_at(input.match_index) = { input.view.substring_view(start_position, state.string_position - start_position), input.line, start_position, input.global_offset + start_position };
    };

#if REGEX_DEBUG
    s_regex_dbg.print_header();
#endif

    bool continue_search = input.regex_options.has_flag_set(AllFlags::Global) || input.regex_options.has_flag_set(AllFlags::Multiline);
    if (input.regex_options.has_flag_set(AllFlags::Sticky))
        continue_search = false;

    auto single_match_only = input.regex_options.has_flag_set(AllFlags::SingleMatch);
    auto only_start_of_line = m_pattern->parser_result.optimization_data.only_start_of_line && !input.regex_options.has_flag_set(AllFlags::Multiline);

    auto compare_range = [insensitive = input.regex_options & AllFlags::Insensitive](auto needle, CharRange range) {
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
    };

    for (auto const& view : views) {
        input.in_the_middle_of_a_line = false;
        if (lines_to_skip != 0) {
            ++input.line;
            --lines_to_skip;
            continue;
        }
        input.view = view;
        dbgln_if(REGEX_DEBUG, "[match] Starting match with view ({}): _{}_", view.length(), view);

        auto view_length = view.length();
        size_t view_index = m_pattern->start_offset;
        state.string_position = view_index;
        if (view.unicode()) {
            if (view_index < view_length)
                state.string_position_in_code_units = view.code_unit_offset_of(view_index);
            else
                state.string_position_in_code_units = view.length_in_code_units();
        } else {
            state.string_position_in_code_units = view_index;
        }
        bool succeeded = false;

        if (view_index == view_length && m_pattern->parser_result.match_length_minimum == 0) {
            // Run the code until it tries to consume something.
            // This allows non-consuming code to run on empty strings, for instance
            // e.g. "Exit"
            size_t temp_operations = operations;

            input.column = match_count;
            input.match_index = match_count;

            state.instruction_position = 0;
            state.repetition_marks.clear();
            state.modifier_stack.clear();
            state.current_options = input.regex_options;

            auto result = execute(input, state, temp_operations);
            // This success is acceptable only if it doesn't read anything from the input (input length is 0).
            if (result == ExecuteResult::Matched && (state.string_position <= view_index)) {
                operations = temp_operations;
                if (!match_count) {
                    // Nothing was *actually* matched, so append an empty match.
                    append_match(input, state, view_index);
                    ++match_count;

                    // This prevents a regex pattern like ".*" from matching the empty string
                    // multiple times, once in this block and once in the following for loop.
                    if (view_index == 0 && view_length == 0)
                        ++view_index;
                }
            }
        }

        for (; view_index <= view_length; ++view_index, input.in_the_middle_of_a_line = true) {
            if (view_index == view_length) {
                if (input.regex_options.has_flag_set(AllFlags::Multiline))
                    break;
            }

            // FIXME: More performant would be to know the remaining minimum string
            //        length needed to match from the current position onwards within
            //        the vm. Add new OpCode for MinMatchLengthFromSp with the value of
            //        the remaining string length from the current path. The value though
            //        has to be filled in reverse. That implies a second run over bytecode
            //        after generation has finished.
            auto const match_length_minimum = m_pattern->parser_result.match_length_minimum;
            if (match_length_minimum && match_length_minimum > view_length - view_index)
                break;

            auto const insensitive = input.regex_options.has_flag_set(AllFlags::Insensitive);
            if (auto& starting_ranges = m_pattern->parser_result.optimization_data.starting_ranges; !starting_ranges.is_empty()) {
                auto ranges = insensitive ? m_pattern->parser_result.optimization_data.starting_ranges_insensitive.span() : starting_ranges.span();
                auto code_unit_index = input.view.unicode() ? input.view.code_unit_offset_of(view_index) : view_index;
                auto ch = input.view.unicode_aware_code_point_at(code_unit_index);
                if (insensitive)
                    ch = to_ascii_lowercase(ch);

                if (!binary_search(ranges, ch, nullptr, compare_range))
                    goto done_matching;
            }

            input.column = match_count;
            input.match_index = match_count;

            state.string_position = view_index;
            if (input.view.unicode()) {
                if (view_index < view_length)
                    state.string_position_in_code_units = input.view.code_unit_offset_of(view_index);
                else
                    state.string_position_in_code_units = input.view.length_in_code_units();
            } else {
                state.string_position_in_code_units = view_index;
            }
            state.instruction_position = 0;
            state.repetition_marks.clear();
            state.modifier_stack.clear();
            state.current_options = input.regex_options;
            state.string_position_before_rseek = NumericLimits<size_t>::max();
            state.string_position_in_code_units_before_rseek = NumericLimits<size_t>::max();

            if (auto const result = execute(input, state, operations); result == ExecuteResult::Matched) {
                succeeded = true;

                if (input.regex_options.has_flag_set(AllFlags::MatchNotEndOfLine) && state.string_position == input.view.length()) {
                    if (!continue_search)
                        break;
                    continue;
                }
                if (input.regex_options.has_flag_set(AllFlags::MatchNotBeginOfLine) && view_index == 0) {
                    if (!continue_search)
                        break;
                    continue;
                }

                dbgln_if(REGEX_DEBUG, "state.string_position={}, view_index={}", state.string_position, view_index);
                dbgln_if(REGEX_DEBUG, "[match] Found a match (length={}): '{}'", state.string_position - view_index, input.view.substring_view(view_index, state.string_position - view_index));

                ++match_count;

                if (continue_search) {
                    append_match(input, state, view_index);

                    bool has_zero_length = state.string_position == view_index;
                    view_index = state.string_position - (has_zero_length ? 0 : 1);
                    if (single_match_only)
                        break;
                    continue;
                }
                if (input.regex_options.has_flag_set(AllFlags::Internal_Stateful)) {
                    append_match(input, state, view_index);
                    break;
                }
                if (state.string_position < view_length) {
                    return { false, 0, {}, {}, {}, operations };
                }

                append_match(input, state, view_index);
                break;
            } else if (result == ExecuteResult::DidNotMatchAndNoFurtherPossibleMatchesInView) {
                break;
            }

        done_matching:
            if (!continue_search || only_start_of_line)
                break;
        }

        ++input.line;
        input.global_offset += view.length() + 1; // +1 includes the line break character

        if (input.regex_options.has_flag_set(AllFlags::Internal_Stateful))
            m_pattern->start_offset = state.string_position;

        if (succeeded && !continue_search)
            break;
    }

    auto flat_capture_group_matches = move(state.flat_capture_group_matches).release();
    if (flat_capture_group_matches.size() < state.capture_group_count * match_count) {
        flat_capture_group_matches.ensure_capacity(match_count * state.capture_group_count);
        for (size_t i = flat_capture_group_matches.size(); i < match_count * state.capture_group_count; ++i)
            flat_capture_group_matches.unchecked_empend();
    }

    Vector<Span<Match>> capture_group_matches;
    for (size_t i = 0; i < match_count; ++i) {
        auto span = flat_capture_group_matches.span().slice(state.capture_group_count * i, state.capture_group_count);
        capture_group_matches.append(span);
    }

    RegexResult result {
        match_count != 0,
        match_count,
        move(state.matches).release(),
        move(flat_capture_group_matches),
        move(capture_group_matches),
        operations,
        m_pattern->parser_result.capture_groups_count,
        m_pattern->parser_result.named_capture_groups_count,
    };

    if (match_count > 0)
        VERIFY(result.capture_group_matches.size() >= match_count);
    else
        result.capture_group_matches.clear_with_capacity();

    return result;
}

template<typename T>
class BumpAllocatedLinkedList {
public:
    BumpAllocatedLinkedList() = default;

    ALWAYS_INLINE void append(T value)
    {
        auto node_ptr = m_allocator.allocate(move(value));
        VERIFY(node_ptr);

        if (!m_first) {
            m_first = node_ptr;
            m_last = node_ptr;
            return;
        }

        node_ptr->previous = m_last;
        m_last->next = node_ptr;
        m_last = node_ptr;
    }

    ALWAYS_INLINE T take_last()
    {
        VERIFY(m_last);
        T value = move(m_last->value);
        if (m_last == m_first) {
            m_last = nullptr;
            m_first = nullptr;
        } else {
            m_last = m_last->previous;
            m_last->next = nullptr;
        }
        return value;
    }

    ALWAYS_INLINE T& last()
    {
        return m_last->value;
    }

    ALWAYS_INLINE bool is_empty() const
    {
        return m_first == nullptr;
    }

    auto reverse_begin() { return ReverseIterator(m_last); }
    auto reverse_end() { return ReverseIterator(); }

private:
    struct Node {
        T value;
        Node* next { nullptr };
        Node* previous { nullptr };
    };

    struct ReverseIterator {
        ReverseIterator() = default;
        explicit ReverseIterator(Node* node)
            : m_node(node)
        {
        }

        T* operator->() { return &m_node->value; }
        T& operator*() { return m_node->value; }
        bool operator==(ReverseIterator const& it) const { return m_node == it.m_node; }
        ReverseIterator& operator++()
        {
            if (m_node)
                m_node = m_node->previous;
            return *this;
        }

    private:
        Node* m_node;
    };

    UniformBumpAllocator<Node, true, 2 * MiB> m_allocator;
    Node* m_first { nullptr };
    Node* m_last { nullptr };
};

template<class Parser>
Matcher<Parser>::ExecuteResult Matcher<Parser>::execute(MatchInput const& input, MatchState& state, size_t& operations) const
{
    if (m_pattern->parser_result.optimization_data.pure_substring_search.has_value() && input.view.is_u16_view()) {
        // Yay, we can do a simple substring search!
        auto is_insensitive = input.regex_options.has_flag_set(AllFlags::Insensitive);
        auto is_unicode = input.view.unicode() || input.regex_options.has_flag_set(AllFlags::Unicode) || input.regex_options.has_flag_set(AllFlags::UnicodeSets);
        // Utf16View::equals_ignoring_case can't handle unicode case folding, so we can only use it for ASCII case insensitivity.
        if (!(is_insensitive && is_unicode)) {
            auto input_view = input.view.u16_view();
            Span<u16 const> needle = m_pattern->parser_result.optimization_data.pure_substring_search->span();
            Utf16View needle_view { bit_cast<char16_t const*>(needle.data()), needle.size() };

            if (is_unicode) {
                if (needle_view.length_in_code_points() + state.string_position > input_view.length_in_code_points())
                    return ExecuteResult::DidNotMatch;
            } else {
                if (needle_view.length_in_code_units() + state.string_position_in_code_units > input_view.length_in_code_units())
                    return ExecuteResult::DidNotMatch;
            }

            Utf16View haystack;
            if (is_unicode)
                haystack = input_view.unicode_substring_view(state.string_position, needle_view.length_in_code_points());
            else
                haystack = input_view.substring_view(state.string_position_in_code_units, needle_view.length_in_code_units());

            if (is_insensitive) {
                if (!Unicode::ranges_equal_ignoring_case(haystack, needle_view, input.view.unicode()))
                    return ExecuteResult::DidNotMatch;
            } else {
                if (haystack != needle_view)
                    return ExecuteResult::DidNotMatch;
            }

            if (input.view.unicode())
                state.string_position += haystack.length_in_code_points();
            else
                state.string_position += haystack.length_in_code_units();
            state.string_position_in_code_units += haystack.length_in_code_units();
            return ExecuteResult::Matched;
        }
    }

    BumpAllocatedLinkedList<MatchState> states_to_try_next;
    HashTable<u64, IdentityHashTraits<u64>> seen_state_hashes;

    auto& bytecode = m_pattern->parser_result.bytecode.template get<FlatByteCode>();
    auto const* data = bytecode.flat_data().data();
    auto const data_size = bytecode.size();

    for (;;) {
        auto const ip = state.instruction_position;
        OpCodeId id = (data_size <= ip)
            ? OpCodeId::Exit
            : static_cast<OpCodeId>(data[ip]);
        ++operations;

        ExecutionResult result;
        size_t current_opcode_size;
        if (input.fail_counter > 0) {
            --input.fail_counter;
            result = ExecutionResult::Failed_ExecuteLowPrioForks;
            current_opcode_size = opcode_size(id, data, ip);
        } else {
            auto insn_result = execute_instruction(id, data, data_size, bytecode, input, state);
            result = insn_result.result;
            current_opcode_size = insn_result.size;
        }

        state.instruction_position += current_opcode_size;

        switch (result) {
        case ExecutionResult::Fork_PrioLow: {
            bool found = false;
            if (input.fork_to_replace.has_value()) {
                for (auto it = states_to_try_next.reverse_begin(); it != states_to_try_next.reverse_end(); ++it) {
                    if (it->initiating_fork == input.fork_to_replace.value()) {
                        (*it) = state;
                        it->instruction_position = state.fork_at_position;
                        it->initiating_fork = *input.fork_to_replace;
                        found = true;
                        break;
                    }
                }
                input.fork_to_replace.clear();
            }
            if (!found) {
                states_to_try_next.append(state);
                states_to_try_next.last().initiating_fork = state.instruction_position - current_opcode_size;
                states_to_try_next.last().instruction_position = state.fork_at_position;
            }
            state.string_position_before_rseek = NumericLimits<size_t>::max();
            state.string_position_in_code_units_before_rseek = NumericLimits<size_t>::max();
            continue;
        }
        case ExecutionResult::Fork_PrioHigh: {
            bool found = false;
            if (input.fork_to_replace.has_value()) {
                for (auto it = states_to_try_next.reverse_begin(); it != states_to_try_next.reverse_end(); ++it) {
                    if (it->initiating_fork == input.fork_to_replace.value()) {
                        (*it) = state;
                        it->initiating_fork = *input.fork_to_replace;
                        found = true;
                        break;
                    }
                }
                input.fork_to_replace.clear();
            }
            if (!found) {
                states_to_try_next.append(state);
                states_to_try_next.last().initiating_fork = state.instruction_position - current_opcode_size;
                states_to_try_next.last().string_position_before_rseek = NumericLimits<size_t>::max();
                states_to_try_next.last().string_position_in_code_units_before_rseek = NumericLimits<size_t>::max();
            }
            state.instruction_position = state.fork_at_position;
            continue;
        }
        case ExecutionResult::Continue:
            continue;
        case ExecutionResult::Succeeded:
            return ExecuteResult::Matched;
        case ExecutionResult::Failed: {
            bool found = false;
            while (!states_to_try_next.is_empty()) {
                state = states_to_try_next.take_last();
                if (auto hash = state.u64_hash(); seen_state_hashes.set(hash) != HashSetResult::InsertedNewEntry) {
                    dbgln_if(REGEX_DEBUG, "Already seen state, skipping: {}", hash);
                    continue;
                }
                found = true;
                break;
            }
            if (found)
                continue;
            return ExecuteResult::DidNotMatch;
        }
        case ExecutionResult::Failed_ExecuteLowPrioForks: {
            bool found = false;
            while (!states_to_try_next.is_empty()) {
                state = states_to_try_next.take_last();
                if (auto hash = state.u64_hash(); seen_state_hashes.set(hash) != HashSetResult::InsertedNewEntry) {
                    dbgln_if(REGEX_DEBUG, "Already seen state, skipping: {}", hash);
                    continue;
                }
                found = true;
                break;
            }
            if (!found)
                return ExecuteResult::DidNotMatch;
            continue;
        }
        case ExecutionResult::Failed_ExecuteLowPrioForksButNoFurtherPossibleMatches: {
            bool found = false;
            while (!states_to_try_next.is_empty()) {
                state = states_to_try_next.take_last();
                if (auto hash = state.u64_hash(); seen_state_hashes.set(hash) != HashSetResult::InsertedNewEntry) {
                    dbgln_if(REGEX_DEBUG, "Already seen state, skipping: {}", hash);
                    continue;
                }
                found = true;
                break;
            }
            if (!found)
                return ExecuteResult::DidNotMatchAndNoFurtherPossibleMatchesInView;
            continue;
        }
        }
    }

    VERIFY_NOT_REACHED();
}

template class Matcher<PosixBasicParser>;
template class Regex<PosixBasicParser>;

template class Matcher<PosixExtendedParser>;
template class Regex<PosixExtendedParser>;

template class Matcher<ECMA262Parser>;
template class Regex<ECMA262Parser>;

}

template<typename Parser>
struct AK::Traits<regex::CacheKey<Parser>> : public AK::DefaultTraits<regex::CacheKey<Parser>> {
    static unsigned hash(regex::CacheKey<Parser> const& key)
    {
        return pair_int_hash(key.pattern.hash(), to_underlying(key.options.value()));
    }
};
