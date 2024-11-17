/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Function.h>
#include <AK/Queue.h>
#include <AK/QuickSort.h>
#include <AK/RedBlackTree.h>
#include <AK/Stack.h>
#include <AK/Trie.h>
#include <LibRegex/Regex.h>
#include <LibRegex/RegexBytecodeStreamOptimizer.h>
#include <LibUnicode/CharacterTypes.h>
#if REGEX_DEBUG
#    include <AK/ScopeGuard.h>
#    include <AK/ScopeLogger.h>
#endif

namespace regex {

using Detail::Block;

template<typename Parser>
void Regex<Parser>::run_optimization_passes()
{
    parser_result.bytecode.flatten();

    auto blocks = split_basic_blocks(parser_result.bytecode);
    if (attempt_rewrite_entire_match_as_substring_search(blocks))
        return;

    // Rewrite fork loops as atomic groups
    // e.g. a*b -> (ATOMIC a*)b
    attempt_rewrite_loops_as_atomic_groups(blocks);

    // FIXME: "There are a few more conditions this can be true in (e.g. within an arbitrarily nested capture group)"
    MatchState state;
    auto& opcode = parser_result.bytecode.get_opcode(state);
    if (opcode.opcode_id() == OpCodeId::CheckBegin)
        parser_result.optimization_data.only_start_of_line = true;

    parser_result.bytecode.flatten();
}

template<typename Parser>
typename Regex<Parser>::BasicBlockList Regex<Parser>::split_basic_blocks(ByteCode const& bytecode)
{
    BasicBlockList block_boundaries;
    size_t end_of_last_block = 0;

    auto bytecode_size = bytecode.size();

    MatchState state;
    state.instruction_position = 0;
    auto check_jump = [&]<typename T>(OpCode const& opcode) {
        auto& op = static_cast<T const&>(opcode);
        ssize_t jump_offset = op.size() + op.offset();
        if (jump_offset >= 0) {
            block_boundaries.append({ end_of_last_block, state.instruction_position });
            end_of_last_block = state.instruction_position + opcode.size();
        } else {
            // This op jumps back, see if that's within this "block".
            if (jump_offset + state.instruction_position > end_of_last_block) {
                // Split the block!
                block_boundaries.append({ end_of_last_block, jump_offset + state.instruction_position });
                block_boundaries.append({ jump_offset + state.instruction_position, state.instruction_position });
                end_of_last_block = state.instruction_position + opcode.size();
            } else {
                // Nope, it's just a jump to another block
                block_boundaries.append({ end_of_last_block, state.instruction_position });
                end_of_last_block = state.instruction_position + opcode.size();
            }
        }
    };
    for (;;) {
        auto& opcode = bytecode.get_opcode(state);

        switch (opcode.opcode_id()) {
        case OpCodeId::Jump:
            check_jump.template operator()<OpCode_Jump>(opcode);
            break;
        case OpCodeId::JumpNonEmpty:
            check_jump.template operator()<OpCode_JumpNonEmpty>(opcode);
            break;
        case OpCodeId::ForkJump:
            check_jump.template operator()<OpCode_ForkJump>(opcode);
            break;
        case OpCodeId::ForkStay:
            check_jump.template operator()<OpCode_ForkStay>(opcode);
            break;
        case OpCodeId::FailForks:
            block_boundaries.append({ end_of_last_block, state.instruction_position });
            end_of_last_block = state.instruction_position + opcode.size();
            break;
        case OpCodeId::Repeat: {
            // Repeat produces two blocks, one containing its repeated expr, and one after that.
            auto repeat_start = state.instruction_position - static_cast<OpCode_Repeat const&>(opcode).offset();
            if (repeat_start > end_of_last_block)
                block_boundaries.append({ end_of_last_block, repeat_start });
            block_boundaries.append({ repeat_start, state.instruction_position });
            end_of_last_block = state.instruction_position + opcode.size();
            break;
        }
        default:
            break;
        }

        auto next_ip = state.instruction_position + opcode.size();
        if (next_ip < bytecode_size)
            state.instruction_position = next_ip;
        else
            break;
    }

    if (end_of_last_block < bytecode_size)
        block_boundaries.append({ end_of_last_block, bytecode_size });

    quick_sort(block_boundaries, [](auto& a, auto& b) { return a.start < b.start; });

    return block_boundaries;
}

static bool has_overlap(Vector<CompareTypeAndValuePair> const& lhs, Vector<CompareTypeAndValuePair> const& rhs)
{

    // We have to fully interpret the two sequences to determine if they overlap (that is, keep track of inversion state and what ranges they cover).
    bool inverse { false };
    bool temporary_inverse { false };
    bool reset_temporary_inverse { false };

    auto current_lhs_inversion_state = [&]() -> bool { return temporary_inverse ^ inverse; };

    RedBlackTree<u32, u32> lhs_ranges;
    RedBlackTree<u32, u32> lhs_negated_ranges;
    HashTable<CharClass> lhs_char_classes;
    HashTable<CharClass> lhs_negated_char_classes;

    auto has_any_unicode_property = false;
    HashTable<Unicode::GeneralCategory> lhs_unicode_general_categories;
    HashTable<Unicode::Property> lhs_unicode_properties;
    HashTable<Unicode::Script> lhs_unicode_scripts;
    HashTable<Unicode::Script> lhs_unicode_script_extensions;
    HashTable<Unicode::GeneralCategory> lhs_negated_unicode_general_categories;
    HashTable<Unicode::Property> lhs_negated_unicode_properties;
    HashTable<Unicode::Script> lhs_negated_unicode_scripts;
    HashTable<Unicode::Script> lhs_negated_unicode_script_extensions;

    auto any_unicode_property_matches = [&](u32 code_point) {
        if (any_of(lhs_negated_unicode_general_categories, [code_point](auto category) { return Unicode::code_point_has_general_category(code_point, category); }))
            return false;
        if (any_of(lhs_negated_unicode_properties, [code_point](auto property) { return Unicode::code_point_has_property(code_point, property); }))
            return false;
        if (any_of(lhs_negated_unicode_scripts, [code_point](auto script) { return Unicode::code_point_has_script(code_point, script); }))
            return false;
        if (any_of(lhs_negated_unicode_script_extensions, [code_point](auto script) { return Unicode::code_point_has_script_extension(code_point, script); }))
            return false;

        if (any_of(lhs_unicode_general_categories, [code_point](auto category) { return Unicode::code_point_has_general_category(code_point, category); }))
            return true;
        if (any_of(lhs_unicode_properties, [code_point](auto property) { return Unicode::code_point_has_property(code_point, property); }))
            return true;
        if (any_of(lhs_unicode_scripts, [code_point](auto script) { return Unicode::code_point_has_script(code_point, script); }))
            return true;
        if (any_of(lhs_unicode_script_extensions, [code_point](auto script) { return Unicode::code_point_has_script_extension(code_point, script); }))
            return true;
        return false;
    };

    auto range_contains = [&]<typename T>(T& value) -> bool {
        u32 start;
        u32 end;

        if constexpr (IsSame<T, CharRange>) {
            start = value.from;
            end = value.to;
        } else {
            start = value;
            end = value;
        }

        if (has_any_unicode_property) {
            // We have some properties, and a range is present
            // Instead of checking every single code point in the range, assume it's a match.
            return start != end || any_unicode_property_matches(start);
        }

        auto* max = lhs_ranges.find_smallest_not_below(start);
        return max && *max <= end;
    };

    auto char_class_contains = [&](CharClass const& value) -> bool {
        if (lhs_char_classes.contains(value))
            return true;

        if (lhs_negated_char_classes.contains(value))
            return false;

        if (lhs_ranges.is_empty())
            return false;

        for (auto it = lhs_ranges.begin(); it != lhs_ranges.end(); ++it) {
            auto start = it.key();
            auto end = *it;
            for (u32 ch = start; ch <= end; ++ch) {
                if (OpCode_Compare::matches_character_class(value, ch, false))
                    return true;
            }
        }

        return false;
    };

    for (auto const& pair : lhs) {
        if (reset_temporary_inverse) {
            reset_temporary_inverse = false;
            temporary_inverse = false;
        } else {
            reset_temporary_inverse = true;
        }

        switch (pair.type) {
        case CharacterCompareType::Inverse:
            inverse = !inverse;
            break;
        case CharacterCompareType::TemporaryInverse:
            temporary_inverse = true;
            reset_temporary_inverse = false;
            break;
        case CharacterCompareType::AnyChar:
            // Special case: if not inverted, AnyChar is always in the range.
            if (!current_lhs_inversion_state())
                return true;
            break;
        case CharacterCompareType::Char:
            if (!current_lhs_inversion_state())
                lhs_ranges.insert(pair.value, pair.value);
            else
                lhs_negated_ranges.insert(pair.value, pair.value);
            break;
        case CharacterCompareType::String:
            // FIXME: We just need to look at the last character of this string, but we only have the first character here.
            //        Just bail out to avoid false positives.
            return true;
        case CharacterCompareType::CharClass:
            if (!current_lhs_inversion_state())
                lhs_char_classes.set(static_cast<CharClass>(pair.value));
            else
                lhs_negated_char_classes.set(static_cast<CharClass>(pair.value));
            break;
        case CharacterCompareType::CharRange: {
            auto range = CharRange(pair.value);
            if (!current_lhs_inversion_state())
                lhs_ranges.insert(range.from, range.to);
            else
                lhs_negated_ranges.insert(range.from, range.to);
            break;
        }
        case CharacterCompareType::LookupTable:
            // We've transformed this into a series of ranges in flat_compares(), so bail out if we see it.
            return true;
        case CharacterCompareType::Reference:
            // We've handled this before coming here.
            break;
        case CharacterCompareType::Property:
            has_any_unicode_property = true;
            if (!current_lhs_inversion_state())
                lhs_unicode_properties.set(static_cast<Unicode::Property>(pair.value));
            else
                lhs_negated_unicode_properties.set(static_cast<Unicode::Property>(pair.value));
            break;
        case CharacterCompareType::GeneralCategory:
            has_any_unicode_property = true;
            if (!current_lhs_inversion_state())
                lhs_unicode_general_categories.set(static_cast<Unicode::GeneralCategory>(pair.value));
            else
                lhs_negated_unicode_general_categories.set(static_cast<Unicode::GeneralCategory>(pair.value));
            break;
        case CharacterCompareType::Script:
            has_any_unicode_property = true;
            if (!current_lhs_inversion_state())
                lhs_unicode_scripts.set(static_cast<Unicode::Script>(pair.value));
            else
                lhs_negated_unicode_scripts.set(static_cast<Unicode::Script>(pair.value));
            break;
        case CharacterCompareType::ScriptExtension:
            has_any_unicode_property = true;
            if (!current_lhs_inversion_state())
                lhs_unicode_script_extensions.set(static_cast<Unicode::Script>(pair.value));
            else
                lhs_negated_unicode_script_extensions.set(static_cast<Unicode::Script>(pair.value));
            break;
        case CharacterCompareType::Or:
        case CharacterCompareType::EndAndOr:
            // These are the default behaviour for [...], so we don't need to do anything (unless we add support for 'And' below).
            break;
        case CharacterCompareType::And:
            // FIXME: These are too difficult to handle, so bail out.
            return true;
        case CharacterCompareType::Undefined:
        case CharacterCompareType::RangeExpressionDummy:
            // These do not occur in valid bytecode.
            VERIFY_NOT_REACHED();
        }
    }

    if constexpr (REGEX_DEBUG) {
        dbgln("lhs ranges:");
        for (auto it = lhs_ranges.begin(); it != lhs_ranges.end(); ++it)
            dbgln("  {}..{}", it.key(), *it);
        dbgln("lhs negated ranges:");
        for (auto it = lhs_negated_ranges.begin(); it != lhs_negated_ranges.end(); ++it)
            dbgln("  {}..{}", it.key(), *it);
    }

    temporary_inverse = false;
    reset_temporary_inverse = false;
    inverse = false;
    auto in_or = false; // We're in an OR block, so we should wait for the EndAndOr to decide if we would match.
    auto matched_in_or = false;
    auto inverse_matched_in_or = false;

    for (auto const& pair : rhs) {
        if (reset_temporary_inverse) {
            reset_temporary_inverse = false;
            temporary_inverse = false;
        } else {
            reset_temporary_inverse = true;
        }

        if constexpr (REGEX_DEBUG) {
            dbgln("check {} ({}) [inverted? {}] against {{", character_compare_type_name(pair.type), pair.value, current_lhs_inversion_state());
            for (auto it = lhs_ranges.begin(); it != lhs_ranges.end(); ++it)
                dbgln("  {}..{}", it.key(), *it);
            for (auto it = lhs_negated_ranges.begin(); it != lhs_negated_ranges.end(); ++it)
                dbgln("  ^[{}..{}]", it.key(), *it);
            for (auto& char_class : lhs_char_classes)
                dbgln("  {}", character_class_name(char_class));
            for (auto& char_class : lhs_negated_char_classes)
                dbgln("  ^{}", character_class_name(char_class));
            dbgln("}}, in or: {}, matched in or: {}, inverse matched in or: {}", in_or, matched_in_or, inverse_matched_in_or);
        }

        switch (pair.type) {
        case CharacterCompareType::Inverse:
            inverse = !inverse;
            break;
        case CharacterCompareType::TemporaryInverse:
            temporary_inverse = true;
            reset_temporary_inverse = false;
            break;
        case CharacterCompareType::AnyChar:
            // Special case: if not inverted, AnyChar is always in the range.
            if (!in_or && !current_lhs_inversion_state())
                return true;
            if (in_or) {
                matched_in_or = true;
                inverse_matched_in_or = false;
            }
            break;
        case CharacterCompareType::Char: {
            auto matched = range_contains(pair.value);
            if (!in_or && (current_lhs_inversion_state() ^ matched))
                return true;
            if (in_or) {
                matched_in_or |= matched;
                inverse_matched_in_or |= !matched;
            }
            break;
        }
        case CharacterCompareType::String:
            // FIXME: We just need to look at the last character of this string, but we only have the first character here.
            //        Just bail out to avoid false positives.
            return true;
        case CharacterCompareType::CharClass: {
            auto contains = char_class_contains(static_cast<CharClass>(pair.value));
            if (!in_or && (current_lhs_inversion_state() ^ contains))
                return true;
            if (in_or) {
                matched_in_or |= contains;
                inverse_matched_in_or |= !contains;
            }
            break;
        }
        case CharacterCompareType::CharRange: {
            auto range = CharRange(pair.value);
            auto contains = range_contains(range);
            if (!in_or && (contains ^ current_lhs_inversion_state()))
                return true;

            if (in_or) {
                matched_in_or |= contains;
                inverse_matched_in_or |= !contains;
            }

            break;
        }
        case CharacterCompareType::LookupTable:
            // We've transformed this into a series of ranges in flat_compares(), so bail out if we see it.
            return true;
        case CharacterCompareType::Reference:
            // We've handled this before coming here.
            break;
        case CharacterCompareType::Property:
            // The only reasonable scenario where we can check these properties without spending too much time is if:
            //  - the ranges are empty
            //  - the char classes are empty
            //  - the unicode properties are empty or contain only this property
            if (!lhs_ranges.is_empty() || !lhs_negated_ranges.is_empty() || !lhs_char_classes.is_empty() || !lhs_negated_char_classes.is_empty())
                return true;
            if (has_any_unicode_property && !lhs_unicode_properties.is_empty() && !lhs_negated_unicode_properties.is_empty()) {
                auto contains = lhs_unicode_properties.contains(static_cast<Unicode::Property>(pair.value));
                if (!in_or && (current_lhs_inversion_state() ^ contains))
                    return true;

                auto inverse_contains = lhs_negated_unicode_properties.contains(static_cast<Unicode::Property>(pair.value));
                if (!in_or && !(current_lhs_inversion_state() ^ inverse_contains))
                    return true;

                if (in_or) {
                    matched_in_or |= contains;
                    inverse_matched_in_or |= inverse_contains;
                }
            }
            break;
        case CharacterCompareType::GeneralCategory:
            if (!lhs_ranges.is_empty() || !lhs_negated_ranges.is_empty() || !lhs_char_classes.is_empty() || !lhs_negated_char_classes.is_empty())
                return true;
            if (has_any_unicode_property && !lhs_unicode_general_categories.is_empty() && !lhs_negated_unicode_general_categories.is_empty()) {
                auto contains = lhs_unicode_general_categories.contains(static_cast<Unicode::GeneralCategory>(pair.value));
                if (!in_or && (current_lhs_inversion_state() ^ contains))
                    return true;
                auto inverse_contains = lhs_negated_unicode_general_categories.contains(static_cast<Unicode::GeneralCategory>(pair.value));
                if (!in_or && !(current_lhs_inversion_state() ^ inverse_contains))
                    return true;
                if (in_or) {
                    matched_in_or |= contains;
                    inverse_matched_in_or |= inverse_contains;
                }
            }
            break;
        case CharacterCompareType::Script:
            if (!lhs_ranges.is_empty() || !lhs_negated_ranges.is_empty() || !lhs_char_classes.is_empty() || !lhs_negated_char_classes.is_empty())
                return true;
            if (has_any_unicode_property && !lhs_unicode_scripts.is_empty() && !lhs_negated_unicode_scripts.is_empty()) {
                auto contains = lhs_unicode_scripts.contains(static_cast<Unicode::Script>(pair.value));
                if (!in_or && (current_lhs_inversion_state() ^ contains))
                    return true;
                auto inverse_contains = lhs_negated_unicode_scripts.contains(static_cast<Unicode::Script>(pair.value));
                if (!in_or && !(current_lhs_inversion_state() ^ inverse_contains))
                    return true;
                if (in_or) {
                    matched_in_or |= contains;
                    inverse_matched_in_or |= inverse_contains;
                }
            }
            break;
        case CharacterCompareType::ScriptExtension:
            if (!lhs_ranges.is_empty() || !lhs_negated_ranges.is_empty() || !lhs_char_classes.is_empty() || !lhs_negated_char_classes.is_empty())
                return true;
            if (has_any_unicode_property && !lhs_unicode_script_extensions.is_empty() && !lhs_negated_unicode_script_extensions.is_empty()) {
                auto contains = lhs_unicode_script_extensions.contains(static_cast<Unicode::Script>(pair.value));
                if (!in_or && (current_lhs_inversion_state() ^ contains))
                    return true;
                auto inverse_contains = lhs_negated_unicode_script_extensions.contains(static_cast<Unicode::Script>(pair.value));
                if (!in_or && !(current_lhs_inversion_state() ^ inverse_contains))
                    return true;
                if (in_or) {
                    matched_in_or |= contains;
                    inverse_matched_in_or |= inverse_contains;
                }
            }
            break;
        case CharacterCompareType::Or:
            in_or = true;
            break;
        case CharacterCompareType::EndAndOr:
            // FIXME: Handle And when we support it below.
            VERIFY(in_or);
            in_or = false;
            if (current_lhs_inversion_state()) {
                if (!inverse_matched_in_or)
                    return true;
            } else {
                if (matched_in_or)
                    return true;
            }

            break;
        case CharacterCompareType::And:
            // FIXME: These are too difficult to handle, so bail out.
            return true;
        case CharacterCompareType::Undefined:
        case CharacterCompareType::RangeExpressionDummy:
            // These do not occur in valid bytecode.
            VERIFY_NOT_REACHED();
        }
    }

    return false;
}

enum class AtomicRewritePreconditionResult {
    SatisfiedWithProperHeader,
    SatisfiedWithEmptyHeader,
    NotSatisfied,
};
static AtomicRewritePreconditionResult block_satisfies_atomic_rewrite_precondition(ByteCode const& bytecode, Block const& repeated_block, Block const& following_block)
{
    Vector<Vector<CompareTypeAndValuePair>> repeated_values;
    MatchState state;
    auto has_seen_actionable_opcode = false;
    for (state.instruction_position = repeated_block.start; state.instruction_position < repeated_block.end;) {
        auto& opcode = bytecode.get_opcode(state);
        switch (opcode.opcode_id()) {
        case OpCodeId::Compare: {
            has_seen_actionable_opcode = true;
            auto compares = static_cast<OpCode_Compare const&>(opcode).flat_compares();
            if (repeated_values.is_empty() && any_of(compares, [](auto& compare) { return compare.type == CharacterCompareType::AnyChar; }))
                return AtomicRewritePreconditionResult::NotSatisfied;
            repeated_values.append(move(compares));
            break;
        }
        case OpCodeId::CheckBegin:
        case OpCodeId::CheckEnd:
            has_seen_actionable_opcode = true;
            if (repeated_values.is_empty())
                return AtomicRewritePreconditionResult::SatisfiedWithProperHeader;
            break;
        case OpCodeId::CheckBoundary:
            // FIXME: What should we do with these? for now, let's fail.
            return AtomicRewritePreconditionResult::NotSatisfied;
        case OpCodeId::Restore:
        case OpCodeId::GoBack:
            return AtomicRewritePreconditionResult::NotSatisfied;
        case OpCodeId::ForkJump:
        case OpCodeId::ForkReplaceJump:
        case OpCodeId::JumpNonEmpty:
            // We could attempt to recursively resolve the follow set, but pretending that this just goes nowhere is faster.
            if (!has_seen_actionable_opcode)
                return AtomicRewritePreconditionResult::NotSatisfied;
            break;
        default:
            break;
        }

        state.instruction_position += opcode.size();
    }
    dbgln_if(REGEX_DEBUG, "Found {} entries in reference", repeated_values.size());

    bool following_block_has_at_least_one_compare = false;
    // Find the first compare in the following block, it must NOT match any of the values in `repeated_values'.
    auto final_instruction = following_block.start;
    for (state.instruction_position = following_block.start; state.instruction_position < following_block.end;) {
        final_instruction = state.instruction_position;
        auto& opcode = bytecode.get_opcode(state);
        switch (opcode.opcode_id()) {
        case OpCodeId::Compare: {
            following_block_has_at_least_one_compare = true;
            // We found a compare, let's see what it has.
            auto compares = static_cast<OpCode_Compare const&>(opcode).flat_compares();
            if (compares.is_empty())
                break;

            if (any_of(compares, [&](auto& compare) {
                    return compare.type == CharacterCompareType::AnyChar || compare.type == CharacterCompareType::Reference;
                }))
                return AtomicRewritePreconditionResult::NotSatisfied;

            if (any_of(repeated_values, [&](auto& repeated_value) { return has_overlap(compares, repeated_value); }))
                return AtomicRewritePreconditionResult::NotSatisfied;

            return AtomicRewritePreconditionResult::SatisfiedWithProperHeader;
        }
        case OpCodeId::CheckBegin:
        case OpCodeId::CheckEnd:
            return AtomicRewritePreconditionResult::SatisfiedWithProperHeader; // Nothing can match the end!
        case OpCodeId::CheckBoundary:
            // FIXME: What should we do with these? For now, consider them a failure.
            return AtomicRewritePreconditionResult::NotSatisfied;
        case OpCodeId::ForkJump:
        case OpCodeId::ForkReplaceJump:
        case OpCodeId::JumpNonEmpty:
            // See note in the previous switch, same cases.
            if (!following_block_has_at_least_one_compare)
                return AtomicRewritePreconditionResult::NotSatisfied;
            break;
        default:
            break;
        }

        state.instruction_position += opcode.size();
    }

    // If the following block falls through, we can't rewrite it.
    state.instruction_position = final_instruction;
    switch (bytecode.get_opcode(state).opcode_id()) {
    case OpCodeId::Jump:
    case OpCodeId::JumpNonEmpty:
    case OpCodeId::ForkJump:
    case OpCodeId::ForkReplaceJump:
        break;
    default:
        return AtomicRewritePreconditionResult::NotSatisfied;
    }

    if (following_block_has_at_least_one_compare)
        return AtomicRewritePreconditionResult::SatisfiedWithProperHeader;
    return AtomicRewritePreconditionResult::SatisfiedWithEmptyHeader;
}

template<typename Parser>
bool Regex<Parser>::attempt_rewrite_entire_match_as_substring_search(BasicBlockList const& basic_blocks)
{
    // If there's no jumps, we can probably rewrite this as a substring search (Compare { string = str }).
    if (basic_blocks.size() > 1)
        return false;

    if (basic_blocks.is_empty()) {
        parser_result.optimization_data.pure_substring_search = ""sv;
        return true; // Empty regex, sure.
    }

    auto& bytecode = parser_result.bytecode;

    auto is_unicode = parser_result.options.has_flag_set(AllFlags::Unicode);

    // We have a single basic block, let's see if it's a series of character or string compares.
    StringBuilder final_string;
    MatchState state;
    while (state.instruction_position < bytecode.size()) {
        auto& opcode = bytecode.get_opcode(state);
        switch (opcode.opcode_id()) {
        case OpCodeId::Compare: {
            auto& compare = static_cast<OpCode_Compare const&>(opcode);
            for (auto& flat_compare : compare.flat_compares()) {
                if (flat_compare.type != CharacterCompareType::Char)
                    return false;

                if (is_unicode || flat_compare.value <= 0x7f)
                    final_string.append_code_point(flat_compare.value);
                else
                    final_string.append(bit_cast<char>(static_cast<u8>(flat_compare.value)));
            }
            break;
        }
        default:
            return false;
        }
        state.instruction_position += opcode.size();
    }

    parser_result.optimization_data.pure_substring_search = final_string.to_byte_string();
    return true;
}

template<typename Parser>
void Regex<Parser>::attempt_rewrite_loops_as_atomic_groups(BasicBlockList const& basic_blocks)
{
    auto& bytecode = parser_result.bytecode;
    if constexpr (REGEX_DEBUG) {
        RegexDebug dbg;
        dbg.print_bytecode(*this);
        for (auto const& block : basic_blocks)
            dbgln("block from {} to {}", block.start, block.end);
    }

    // A pattern such as:
    //     bb0       |  RE0
    //               |  ForkX bb0
    //     -------------------------
    //     bb1       |  RE1
    // can be rewritten as:
    //     -------------------------
    //     bb0       | RE0
    //               | ForkReplaceX bb0
    //     -------------------------
    //     bb1       | RE1
    // provided that first(RE1) not-in end(RE0), which is to say
    // that RE1 cannot start with whatever RE0 has matched (ever).
    //
    // Alternatively, a second form of this pattern can also occur:
    //     bb0 | *
    //         | ForkX bb2
    //     ------------------------
    //     bb1 | RE0
    //         | Jump bb0
    //     ------------------------
    //     bb2 | RE1
    // which can be transformed (with the same preconditions) to:
    //     bb0 | *
    //         | ForkReplaceX bb2
    //     ------------------------
    //     bb1 | RE0
    //         | Jump bb0
    //     ------------------------
    //     bb2 | RE1

    enum class AlternateForm {
        DirectLoopWithoutHeader,               // loop without proper header, a block forking to itself. i.e. the first form.
        DirectLoopWithoutHeaderAndEmptyFollow, // loop without proper header, a block forking to itself. i.e. the first form but with RE1 being empty.
        DirectLoopWithHeader,                  // loop with proper header, i.e. the second form.
    };
    struct CandidateBlock {
        Block forking_block;
        Optional<Block> new_target_block;
        AlternateForm form;
    };
    Vector<CandidateBlock> candidate_blocks;

    auto is_an_eligible_jump = [](OpCode const& opcode, size_t ip, size_t block_start, AlternateForm alternate_form) {
        switch (opcode.opcode_id()) {
        case OpCodeId::JumpNonEmpty: {
            auto const& op = static_cast<OpCode_JumpNonEmpty const&>(opcode);
            auto form = op.form();
            if (form != OpCodeId::Jump && alternate_form == AlternateForm::DirectLoopWithHeader)
                return false;
            if (form != OpCodeId::ForkJump && form != OpCodeId::ForkStay && alternate_form == AlternateForm::DirectLoopWithoutHeader)
                return false;
            return op.offset() + ip + opcode.size() == block_start;
        }
        case OpCodeId::ForkJump:
            if (alternate_form == AlternateForm::DirectLoopWithHeader)
                return false;
            return static_cast<OpCode_ForkJump const&>(opcode).offset() + ip + opcode.size() == block_start;
        case OpCodeId::ForkStay:
            if (alternate_form == AlternateForm::DirectLoopWithHeader)
                return false;
            return static_cast<OpCode_ForkStay const&>(opcode).offset() + ip + opcode.size() == block_start;
        case OpCodeId::Jump:
            // Infinite loop does *not* produce forks.
            if (alternate_form == AlternateForm::DirectLoopWithoutHeader)
                return false;
            if (alternate_form == AlternateForm::DirectLoopWithHeader)
                return static_cast<OpCode_Jump const&>(opcode).offset() + ip + opcode.size() == block_start;
            VERIFY_NOT_REACHED();
        default:
            return false;
        }
    };
    for (size_t i = 0; i < basic_blocks.size(); ++i) {
        auto forking_block = basic_blocks[i];
        Optional<Block> fork_fallback_block;
        if (i + 1 < basic_blocks.size())
            fork_fallback_block = basic_blocks[i + 1];
        MatchState state;
        // Check if the last instruction in this block is a jump to the block itself:
        {
            state.instruction_position = forking_block.end;
            auto& opcode = bytecode.get_opcode(state);
            if (is_an_eligible_jump(opcode, state.instruction_position, forking_block.start, AlternateForm::DirectLoopWithoutHeader)) {
                // We've found RE0 (and RE1 is just the following block, if any), let's see if the precondition applies.
                // if RE1 is empty, there's no first(RE1), so this is an automatic pass.
                if (!fork_fallback_block.has_value()
                    || (fork_fallback_block->end == fork_fallback_block->start && block_satisfies_atomic_rewrite_precondition(bytecode, forking_block, *fork_fallback_block) != AtomicRewritePreconditionResult::NotSatisfied)) {
                    candidate_blocks.append({ forking_block, fork_fallback_block, AlternateForm::DirectLoopWithoutHeader });
                    break;
                }

                auto precondition = block_satisfies_atomic_rewrite_precondition(bytecode, forking_block, *fork_fallback_block);
                if (precondition == AtomicRewritePreconditionResult::SatisfiedWithProperHeader) {
                    candidate_blocks.append({ forking_block, fork_fallback_block, AlternateForm::DirectLoopWithoutHeader });
                    break;
                }
                if (precondition == AtomicRewritePreconditionResult::SatisfiedWithEmptyHeader) {
                    candidate_blocks.append({ forking_block, fork_fallback_block, AlternateForm::DirectLoopWithoutHeaderAndEmptyFollow });
                    break;
                }
            }
        }
        // Check if the last instruction in the last block is a direct jump to this block
        if (fork_fallback_block.has_value()) {
            state.instruction_position = fork_fallback_block->end;
            auto& opcode = bytecode.get_opcode(state);
            if (is_an_eligible_jump(opcode, state.instruction_position, forking_block.start, AlternateForm::DirectLoopWithHeader)) {
                // We've found bb1 and bb0, let's just make sure that bb0 forks to bb2.
                state.instruction_position = forking_block.end;
                auto& opcode = bytecode.get_opcode(state);
                if (opcode.opcode_id() == OpCodeId::ForkJump || opcode.opcode_id() == OpCodeId::ForkStay) {
                    Optional<Block> block_following_fork_fallback;
                    if (i + 2 < basic_blocks.size())
                        block_following_fork_fallback = basic_blocks[i + 2];
                    if (!block_following_fork_fallback.has_value()
                        || block_satisfies_atomic_rewrite_precondition(bytecode, *fork_fallback_block, *block_following_fork_fallback) != AtomicRewritePreconditionResult::NotSatisfied) {
                        candidate_blocks.append({ forking_block, {}, AlternateForm::DirectLoopWithHeader });
                        break;
                    }
                }
            }
            // We've found a slightly degenerate case, where the next block jumps back to the _jump_ instruction in the forking block.
            // This is a direct loop without a proper header that is posing as a loop with a header.
            if (is_an_eligible_jump(opcode, state.instruction_position, forking_block.end, AlternateForm::DirectLoopWithHeader)) {
                // We've found bb1 and bb0, let's just make sure that bb0 forks to bb2.
                state.instruction_position = forking_block.end;
                auto& opcode = bytecode.get_opcode(state);
                if (opcode.opcode_id() == OpCodeId::ForkJump || opcode.opcode_id() == OpCodeId::ForkStay) {
                    Optional<Block> block_following_fork_fallback;
                    if (i + 2 < basic_blocks.size())
                        block_following_fork_fallback = basic_blocks[i + 2];
                    if (!block_following_fork_fallback.has_value()
                        || block_satisfies_atomic_rewrite_precondition(bytecode, *fork_fallback_block, *block_following_fork_fallback) != AtomicRewritePreconditionResult::NotSatisfied) {
                        candidate_blocks.append({ forking_block, {}, AlternateForm::DirectLoopWithoutHeader });
                        break;
                    }
                }
            }
        }
    }

    dbgln_if(REGEX_DEBUG, "Found {} candidate blocks", candidate_blocks.size());
    if (candidate_blocks.is_empty()) {
        dbgln_if(REGEX_DEBUG, "Failed to find anything for {}", pattern_value);
        return;
    }

    RedBlackTree<size_t, size_t> needed_patches;

    // Reverse the blocks, so we can patch the bytecode without messing with the latter patches.
    quick_sort(candidate_blocks, [](auto& a, auto& b) { return b.forking_block.start > a.forking_block.start; });
    for (auto& candidate : candidate_blocks) {
        // Note that both forms share a ForkReplace patch in forking_block.
        // Patch the ForkX in forking_block to be a ForkReplaceX instead.
        auto& opcode_id = bytecode[candidate.forking_block.end];
        if (opcode_id == (ByteCodeValueType)OpCodeId::ForkStay) {
            opcode_id = (ByteCodeValueType)OpCodeId::ForkReplaceStay;
        } else if (opcode_id == (ByteCodeValueType)OpCodeId::ForkJump) {
            opcode_id = (ByteCodeValueType)OpCodeId::ForkReplaceJump;
        } else if (opcode_id == (ByteCodeValueType)OpCodeId::JumpNonEmpty) {
            auto& jump_opcode_id = bytecode[candidate.forking_block.end + 3];
            if (jump_opcode_id == (ByteCodeValueType)OpCodeId::ForkStay)
                jump_opcode_id = (ByteCodeValueType)OpCodeId::ForkReplaceStay;
            else if (jump_opcode_id == (ByteCodeValueType)OpCodeId::ForkJump)
                jump_opcode_id = (ByteCodeValueType)OpCodeId::ForkReplaceJump;
            else
                VERIFY_NOT_REACHED();
        } else {
            VERIFY_NOT_REACHED();
        }
    }

    if (!needed_patches.is_empty()) {
        MatchState state;
        auto bytecode_size = bytecode.size();
        state.instruction_position = 0;
        struct Patch {
            ssize_t value;
            size_t offset;
            bool should_negate { false };
        };
        for (;;) {
            if (state.instruction_position >= bytecode_size)
                break;

            auto& opcode = bytecode.get_opcode(state);
            Stack<Patch, 2> patch_points;

            switch (opcode.opcode_id()) {
            case OpCodeId::Jump:
                patch_points.push({ static_cast<OpCode_Jump const&>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::JumpNonEmpty:
                patch_points.push({ static_cast<OpCode_JumpNonEmpty const&>(opcode).offset(), state.instruction_position + 1 });
                patch_points.push({ static_cast<OpCode_JumpNonEmpty const&>(opcode).checkpoint(), state.instruction_position + 2 });
                break;
            case OpCodeId::ForkJump:
                patch_points.push({ static_cast<OpCode_ForkJump const&>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::ForkStay:
                patch_points.push({ static_cast<OpCode_ForkStay const&>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::Repeat:
                patch_points.push({ -(ssize_t) static_cast<OpCode_Repeat const&>(opcode).offset(), state.instruction_position + 1, true });
                break;
            default:
                break;
            }

            while (!patch_points.is_empty()) {
                auto& patch_point = patch_points.top();
                auto target_offset = patch_point.value + state.instruction_position + opcode.size();

                constexpr auto do_patch = [](auto& patch_it, auto& patch_point, auto& target_offset, auto& bytecode, auto ip) {
                    if (patch_it.key() == ip)
                        return;

                    if (patch_point.value < 0 && target_offset <= patch_it.key() && ip > patch_it.key())
                        bytecode[patch_point.offset] += (patch_point.should_negate ? 1 : -1) * (*patch_it);
                    else if (patch_point.value > 0 && target_offset >= patch_it.key() && ip < patch_it.key())
                        bytecode[patch_point.offset] += (patch_point.should_negate ? -1 : 1) * (*patch_it);
                };

                if (auto patch_it = needed_patches.find_largest_not_above_iterator(target_offset); !patch_it.is_end())
                    do_patch(patch_it, patch_point, target_offset, bytecode, state.instruction_position);
                else if (auto patch_it = needed_patches.find_largest_not_above_iterator(state.instruction_position); !patch_it.is_end())
                    do_patch(patch_it, patch_point, target_offset, bytecode, state.instruction_position);

                patch_points.pop();
            }

            state.instruction_position += opcode.size();
        }
    }

    if constexpr (REGEX_DEBUG) {
        warnln("Transformed to:");
        RegexDebug dbg;
        dbg.print_bytecode(*this);
    }
}

void Optimizer::append_alternation(ByteCode& target, ByteCode&& left, ByteCode&& right)
{
    Array<ByteCode, 2> alternatives;
    alternatives[0] = move(left);
    alternatives[1] = move(right);

    append_alternation(target, alternatives);
}

template<typename K, typename V, typename KTraits>
using OrderedHashMapForTrie = OrderedHashMap<K, V, KTraits>;

void Optimizer::append_alternation(ByteCode& target, Span<ByteCode> alternatives)
{
    if (alternatives.size() == 0)
        return;

    if (alternatives.size() == 1)
        return target.extend(move(alternatives[0]));

    if (all_of(alternatives, [](auto& x) { return x.is_empty(); }))
        return;

    for (auto& entry : alternatives)
        entry.flatten();

#if REGEX_DEBUG
    ScopeLogger<true> log;
    warnln("Alternations:");
    RegexDebug dbg;
    for (auto& entry : alternatives) {
        warnln("----------");
        dbg.print_bytecode(entry);
    }
    ScopeGuard print_at_end {
        [&] {
            warnln("======================");
            RegexDebug dbg;
            dbg.print_bytecode(target);
        }
    };
#endif

    // First, find incoming jump edges.
    // We need them for two reasons:
    // - We need to distinguish between insn-A-jumped-to-by-insn-B and insn-A-jumped-to-by-insn-C (as otherwise we'd break trie invariants)
    // - We need to know which jumps to patch when we're done

    struct JumpEdge {
        Span<ByteCodeValueType const> jump_insn;
    };
    Vector<HashMap<size_t, Vector<JumpEdge>>> incoming_jump_edges_for_each_alternative;
    incoming_jump_edges_for_each_alternative.resize(alternatives.size());

    auto has_any_backwards_jump = false;

    MatchState state;

    for (size_t i = 0; i < alternatives.size(); ++i) {
        auto& alternative = alternatives[i];
        // Add a jump to the "end" of the block; this is implicit in the bytecode, but we need it to be explicit in the trie.
        // Jump{offset=0}
        alternative.append(static_cast<ByteCodeValueType>(OpCodeId::Jump));
        alternative.append(0);

        auto& incoming_jump_edges = incoming_jump_edges_for_each_alternative[i];

        auto alternative_bytes = alternative.spans<1>().singular_span();
        for (state.instruction_position = 0; state.instruction_position < alternative.size();) {
            auto& opcode = alternative.get_opcode(state);
            auto opcode_bytes = alternative_bytes.slice(state.instruction_position, opcode.size());

            switch (opcode.opcode_id()) {
            case OpCodeId::Jump:
                incoming_jump_edges.ensure(static_cast<OpCode_Jump const&>(opcode).offset() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= static_cast<OpCode_Jump const&>(opcode).offset() < 0;
                break;
            case OpCodeId::JumpNonEmpty:
                incoming_jump_edges.ensure(static_cast<OpCode_JumpNonEmpty const&>(opcode).offset() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= static_cast<OpCode_JumpNonEmpty const&>(opcode).offset() < 0;
                break;
            case OpCodeId::ForkJump:
                incoming_jump_edges.ensure(static_cast<OpCode_ForkJump const&>(opcode).offset() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= static_cast<OpCode_ForkJump const&>(opcode).offset() < 0;
                break;
            case OpCodeId::ForkStay:
                incoming_jump_edges.ensure(static_cast<OpCode_ForkStay const&>(opcode).offset() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= static_cast<OpCode_ForkStay const&>(opcode).offset() < 0;
                break;
            case OpCodeId::ForkReplaceJump:
                incoming_jump_edges.ensure(static_cast<OpCode_ForkReplaceJump const&>(opcode).offset() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= static_cast<OpCode_ForkReplaceJump const&>(opcode).offset() < 0;
                break;
            case OpCodeId::ForkReplaceStay:
                incoming_jump_edges.ensure(static_cast<OpCode_ForkReplaceStay const&>(opcode).offset() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= static_cast<OpCode_ForkReplaceStay const&>(opcode).offset() < 0;
                break;
            case OpCodeId::Repeat:
                incoming_jump_edges.ensure(state.instruction_position - static_cast<OpCode_Repeat const&>(opcode).offset()).append({ opcode_bytes });
                has_any_backwards_jump = true;
                break;
            default:
                break;
            }
            state.instruction_position += opcode.size();
        }
    }

    struct QualifiedIP {
        size_t alternative_index;
        size_t instruction_position;
    };
    using Tree = Trie<DisjointSpans<ByteCodeValueType const>, Vector<QualifiedIP>, Traits<DisjointSpans<ByteCodeValueType const>>, void, OrderedHashMapForTrie>;
    Tree trie { {} }; // Root node is empty, key{ instruction_bytes, dependent_instruction_bytes... } -> IP

    size_t common_hits = 0;
    size_t total_nodes = 0;
    size_t total_bytecode_entries_in_tree = 0;
    for (size_t i = 0; i < alternatives.size(); ++i) {
        auto& alternative = alternatives[i];
        auto& incoming_jump_edges = incoming_jump_edges_for_each_alternative[i];

        auto* active_node = &trie;
        auto alternative_span = alternative.spans<1>().singular_span();
        for (state.instruction_position = 0; state.instruction_position < alternative_span.size();) {
            total_nodes += 1;
            auto& opcode = alternative.get_opcode(state);
            auto opcode_bytes = alternative_span.slice(state.instruction_position, opcode.size());
            Vector<Span<ByteCodeValueType const>> node_key_bytes;
            node_key_bytes.append(opcode_bytes);

            if (auto edges = incoming_jump_edges.get(state.instruction_position); edges.has_value()) {
                for (auto& edge : *edges)
                    node_key_bytes.append(edge.jump_insn);
            }

            active_node = static_cast<decltype(active_node)>(MUST(active_node->ensure_child(DisjointSpans<ByteCodeValueType const> { move(node_key_bytes) })));

            if (active_node->has_metadata()) {
                active_node->metadata_value().append({ i, state.instruction_position });
                common_hits += 1;
            } else {
                active_node->set_metadata(Vector<QualifiedIP> { QualifiedIP { i, state.instruction_position } });
                total_bytecode_entries_in_tree += opcode.size();
            }
            state.instruction_position += opcode.size();
        }
    }

    if constexpr (REGEX_DEBUG) {
        Function<void(decltype(trie)&, size_t)> print_tree = [&](decltype(trie)& node, size_t indent = 0) mutable {
            ByteString name = "(no ip)";
            ByteString insn;
            if (node.has_metadata()) {
                name = ByteString::formatted(
                    "{}@{} ({} node{})",
                    node.metadata_value().first().instruction_position,
                    node.metadata_value().first().alternative_index,
                    node.metadata_value().size(),
                    node.metadata_value().size() == 1 ? "" : "s");

                MatchState state;
                state.instruction_position = node.metadata_value().first().instruction_position;
                auto& opcode = alternatives[node.metadata_value().first().alternative_index].get_opcode(state);
                insn = ByteString::formatted("{} {}", opcode.to_byte_string(), opcode.arguments_string());
            }
            dbgln("{:->{}}| {} -- {}", "", indent * 2, name, insn);
            for (auto& child : node.children())
                print_tree(static_cast<decltype(trie)&>(*child.value), indent + 1);
        };

        print_tree(trie, 0);
    }

    // This is really only worth it if we don't blow up the size by the 2-extra-instruction-per-node scheme, similarly, if no nodes are shared, we're better off not using a tree.
    auto tree_cost = (total_nodes - common_hits) * 2;
    auto chain_cost = total_nodes + alternatives.size() * 2;
    dbgln_if(REGEX_DEBUG, "Total nodes: {}, common hits: {} (tree cost = {}, chain cost = {})", total_nodes, common_hits, tree_cost, chain_cost);

    if (common_hits == 0 || tree_cost > chain_cost) {
        // It's better to lay these out as a normal sequence of instructions.
        auto patch_start = target.size();
        for (size_t i = 1; i < alternatives.size(); ++i) {
            target.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));
            target.empend(0u); // To be filled later.
        }

        size_t size_to_jump = 0;
        bool seen_one_empty = false;
        for (size_t i = alternatives.size(); i > 0; --i) {
            auto& entry = alternatives[i - 1];
            if (entry.is_empty()) {
                if (seen_one_empty)
                    continue;
                seen_one_empty = true;
            }

            auto is_first = i == 1;
            auto instruction_size = entry.size() + (is_first ? 0 : 2); // Jump; -> +2
            size_to_jump += instruction_size;

            if (!is_first)
                target[patch_start + (i - 2) * 2 + 1] = size_to_jump + (alternatives.size() - i) * 2;

            dbgln_if(REGEX_DEBUG, "{} size = {}, cum={}", i - 1, instruction_size, size_to_jump);
        }

        seen_one_empty = false;
        for (size_t i = alternatives.size(); i > 0; --i) {
            auto& chunk = alternatives[i - 1];
            if (chunk.is_empty()) {
                if (seen_one_empty)
                    continue;
                seen_one_empty = true;
            }

            ByteCode* previous_chunk = nullptr;
            size_t j = i - 1;
            auto seen_one_empty_before = chunk.is_empty();
            while (j >= 1) {
                --j;
                auto& candidate_chunk = alternatives[j];
                if (candidate_chunk.is_empty()) {
                    if (seen_one_empty_before)
                        continue;
                }
                previous_chunk = &candidate_chunk;
                break;
            }

            size_to_jump -= chunk.size() + (previous_chunk ? 2 : 0);

            target.extend(move(chunk));
            target.empend(static_cast<ByteCodeValueType>(OpCodeId::Jump));
            target.empend(size_to_jump); // Jump to the _END label
        }
    } else {
        target.ensure_capacity(total_bytecode_entries_in_tree + common_hits * 6);

        auto node_is = [](Tree const* node, QualifiedIP ip) {
            if (!node->has_metadata())
                return false;
            for (auto& node_ip : node->metadata_value()) {
                if (node_ip.alternative_index == ip.alternative_index && node_ip.instruction_position == ip.instruction_position)
                    return true;
            }
            return false;
        };

        struct Patch {
            QualifiedIP source_ip;
            size_t target_ip;
            bool done { false };
        };
        Vector<Patch> patch_locations;
        patch_locations.ensure_capacity(total_nodes);

        auto add_patch_point = [&](Tree const* node, size_t target_ip) {
            if (!node->has_metadata())
                return;
            auto& node_ip = node->metadata_value().first();
            patch_locations.append({ node_ip, target_ip });
        };

        Queue<Tree*> nodes_to_visit;
        nodes_to_visit.enqueue(&trie);

        HashMap<size_t, NonnullOwnPtr<RedBlackTree<u64, u64>>> instruction_positions;
        if (has_any_backwards_jump)
            MUST(instruction_positions.try_ensure_capacity(alternatives.size()));

        auto ip_mapping_for_alternative = [&](size_t i) -> RedBlackTree<u64, u64>& {
            return *instruction_positions.ensure(i, [] {
                return make<RedBlackTree<u64, u64>>();
            });
        };

        // each node:
        //   node.re
        //   forkjump child1
        //   forkjump child2
        //   ...
        while (!nodes_to_visit.is_empty()) {
            auto const* node = nodes_to_visit.dequeue();
            for (auto& patch : patch_locations) {
                if (!patch.done && node_is(node, patch.source_ip)) {
                    auto value = static_cast<ByteCodeValueType>(target.size() - patch.target_ip - 1);
                    if (value == 0)
                        target[patch.target_ip - 1] = static_cast<ByteCodeValueType>(OpCodeId::Jump);
                    target[patch.target_ip] = value;
                    patch.done = true;
                }
            }

            if (!node->value().individual_spans().is_empty()) {
                auto insn_bytes = node->value().individual_spans().first();

                target.ensure_capacity(target.size() + insn_bytes.size());
                state.instruction_position = target.size();
                target.append(insn_bytes);

                if (has_any_backwards_jump) {
                    for (auto& ip : node->metadata_value())
                        ip_mapping_for_alternative(ip.alternative_index).insert(ip.instruction_position, state.instruction_position);
                }

                auto& opcode = target.get_opcode(state);

                ssize_t jump_offset;
                auto is_jump = true;
                auto patch_location = state.instruction_position + 1;

                switch (opcode.opcode_id()) {
                case OpCodeId::Jump:
                    jump_offset = static_cast<OpCode_Jump const&>(opcode).offset();
                    break;
                case OpCodeId::JumpNonEmpty:
                    jump_offset = static_cast<OpCode_JumpNonEmpty const&>(opcode).offset();
                    break;
                case OpCodeId::ForkJump:
                    jump_offset = static_cast<OpCode_ForkJump const&>(opcode).offset();
                    break;
                case OpCodeId::ForkStay:
                    jump_offset = static_cast<OpCode_ForkStay const&>(opcode).offset();
                    break;
                case OpCodeId::ForkReplaceJump:
                    jump_offset = static_cast<OpCode_ForkReplaceJump const&>(opcode).offset();
                    break;
                case OpCodeId::ForkReplaceStay:
                    jump_offset = static_cast<OpCode_ForkReplaceStay const&>(opcode).offset();
                    break;
                case OpCodeId::Repeat:
                    jump_offset = static_cast<ssize_t>(0) - static_cast<ssize_t>(static_cast<OpCode_Repeat const&>(opcode).offset()) - static_cast<ssize_t>(opcode.size());
                    break;
                default:
                    is_jump = false;
                    break;
                }

                if (is_jump) {
                    VERIFY(node->has_metadata());
                    QualifiedIP ip = node->metadata_value().first();
                    auto intended_jump_ip = ip.instruction_position + jump_offset + opcode.size();
                    if (jump_offset < 0) {
                        VERIFY(has_any_backwards_jump);
                        // We should've already seen this instruction, so we can just patch it in.
                        auto& ip_mapping = ip_mapping_for_alternative(ip.alternative_index);
                        auto target_ip = ip_mapping.find(intended_jump_ip);
                        if (!target_ip) {
                            RegexDebug dbg;
                            size_t x = 0;
                            for (auto& entry : alternatives) {
                                warnln("----------- {} ----------", x++);
                                dbg.print_bytecode(entry);
                            }

                            dbgln("Regex Tree / Unknown backwards jump: {}@{} -> {}",
                                ip.instruction_position,
                                ip.alternative_index,
                                intended_jump_ip);
                            VERIFY_NOT_REACHED();
                        }
                        target[patch_location] = static_cast<ByteCodeValueType>(*target_ip - patch_location - 1);
                    } else {
                        patch_locations.append({ QualifiedIP { ip.alternative_index, intended_jump_ip }, patch_location });
                    }
                }
            }

            for (auto const& child : node->children()) {
                auto* child_node = static_cast<Tree*>(child.value.ptr());
                target.append(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));
                add_patch_point(child_node, target.size());
                target.append(static_cast<ByteCodeValueType>(0));
                nodes_to_visit.enqueue(child_node);
            }
        }

        for (auto& patch : patch_locations) {
            if (patch.done)
                continue;

            auto& alternative = alternatives[patch.source_ip.alternative_index];
            if (patch.source_ip.instruction_position >= alternative.size()) {
                // This just wants to jump to the end of the alternative, which is fine.
                // Patch it to jump to the end of the target instead.
                target[patch.target_ip] = static_cast<ByteCodeValueType>(target.size() - patch.target_ip - 1);
                continue;
            }

            dbgln("Regex Tree / Unpatched jump: {}@{} -> {}@{}",
                patch.source_ip.instruction_position,
                patch.source_ip.alternative_index,
                patch.target_ip,
                target[patch.target_ip]);
            VERIFY_NOT_REACHED();
        }
    }
}

enum class LookupTableInsertionOutcome {
    Successful,
    ReplaceWithAnyChar,
    TemporaryInversionNeeded,
    PermanentInversionNeeded,
    FlushOnInsertion,
    FinishFlushOnInsertion,
    CannotPlaceInTable,
};
static LookupTableInsertionOutcome insert_into_lookup_table(RedBlackTree<ByteCodeValueType, CharRange>& table, CompareTypeAndValuePair pair)
{
    switch (pair.type) {
    case CharacterCompareType::Inverse:
        return LookupTableInsertionOutcome::PermanentInversionNeeded;
    case CharacterCompareType::TemporaryInverse:
        return LookupTableInsertionOutcome::TemporaryInversionNeeded;
    case CharacterCompareType::AnyChar:
        return LookupTableInsertionOutcome::ReplaceWithAnyChar;
    case CharacterCompareType::CharClass:
        return LookupTableInsertionOutcome::CannotPlaceInTable;
    case CharacterCompareType::Char:
        table.insert(pair.value, { (u32)pair.value, (u32)pair.value });
        break;
    case CharacterCompareType::CharRange: {
        CharRange range { pair.value };
        table.insert(range.from, range);
        break;
    }
    case CharacterCompareType::EndAndOr:
        return LookupTableInsertionOutcome::FinishFlushOnInsertion;
    case CharacterCompareType::And:
        return LookupTableInsertionOutcome::FlushOnInsertion;
    case CharacterCompareType::Reference:
    case CharacterCompareType::Property:
    case CharacterCompareType::GeneralCategory:
    case CharacterCompareType::Script:
    case CharacterCompareType::ScriptExtension:
    case CharacterCompareType::Or:
        return LookupTableInsertionOutcome::CannotPlaceInTable;
    case CharacterCompareType::Undefined:
    case CharacterCompareType::RangeExpressionDummy:
    case CharacterCompareType::String:
    case CharacterCompareType::LookupTable:
        VERIFY_NOT_REACHED();
    }

    return LookupTableInsertionOutcome::Successful;
}

void Optimizer::append_character_class(ByteCode& target, Vector<CompareTypeAndValuePair>&& pairs)
{
    ByteCode arguments;
    size_t argument_count = 0;

    if (pairs.size() <= 1) {
        for (auto& pair : pairs) {
            arguments.append(to_underlying(pair.type));
            if (pair.type != CharacterCompareType::AnyChar
                && pair.type != CharacterCompareType::TemporaryInverse
                && pair.type != CharacterCompareType::Inverse
                && pair.type != CharacterCompareType::And
                && pair.type != CharacterCompareType::Or
                && pair.type != CharacterCompareType::EndAndOr)
                arguments.append(pair.value);
            ++argument_count;
        }
    } else {
        RedBlackTree<ByteCodeValueType, CharRange> table;
        RedBlackTree<ByteCodeValueType, CharRange> inverted_table;
        auto* current_table = &table;
        auto* current_inverted_table = &inverted_table;
        bool invert_for_next_iteration = false;
        bool is_currently_inverted = false;

        auto flush_tables = [&] {
            auto append_table = [&](auto& table) {
                ++argument_count;
                arguments.append(to_underlying(CharacterCompareType::LookupTable));
                auto size_index = arguments.size();
                arguments.append(0);
                Optional<CharRange> active_range;
                size_t range_count = 0;
                for (auto& range : table) {
                    if (!active_range.has_value()) {
                        active_range = range;
                        continue;
                    }

                    if (range.from <= active_range->to + 1 && range.to + 1 >= active_range->from) {
                        active_range = CharRange { min(range.from, active_range->from), max(range.to, active_range->to) };
                    } else {
                        ++range_count;
                        arguments.append(active_range.release_value());
                        active_range = range;
                    }
                }
                if (active_range.has_value()) {
                    ++range_count;
                    arguments.append(active_range.release_value());
                }
                arguments[size_index] = range_count;
            };

            auto contains_regular_table = !table.is_empty();
            auto contains_inverted_table = !inverted_table.is_empty();
            if (contains_regular_table)
                append_table(table);

            if (contains_inverted_table) {
                ++argument_count;
                arguments.append(to_underlying(CharacterCompareType::TemporaryInverse));
                append_table(inverted_table);
            }

            table.clear();
            inverted_table.clear();
        };

        auto flush_on_every_insertion = false;
        for (auto& value : pairs) {
            auto should_invert_after_this_iteration = invert_for_next_iteration;
            invert_for_next_iteration = false;

            auto insertion_result = insert_into_lookup_table(*current_table, value);
            switch (insertion_result) {
            case LookupTableInsertionOutcome::Successful:
                if (flush_on_every_insertion)
                    flush_tables();
                break;
            case LookupTableInsertionOutcome::ReplaceWithAnyChar: {
                table.clear();
                inverted_table.clear();
                arguments.append(to_underlying(CharacterCompareType::AnyChar));
                ++argument_count;
                break;
            }
            case LookupTableInsertionOutcome::TemporaryInversionNeeded:
                swap(current_table, current_inverted_table);
                invert_for_next_iteration = true;
                is_currently_inverted = !is_currently_inverted;
                break;
            case LookupTableInsertionOutcome::PermanentInversionNeeded:
                flush_tables();
                arguments.append(to_underlying(CharacterCompareType::Inverse));
                ++argument_count;
                break;
            case LookupTableInsertionOutcome::FlushOnInsertion:
            case LookupTableInsertionOutcome::FinishFlushOnInsertion:
                flush_tables();
                flush_on_every_insertion = insertion_result == LookupTableInsertionOutcome::FlushOnInsertion;
                [[fallthrough]];
            case LookupTableInsertionOutcome::CannotPlaceInTable:
                if (is_currently_inverted) {
                    arguments.append(to_underlying(CharacterCompareType::TemporaryInverse));
                    ++argument_count;
                }
                arguments.append(to_underlying(value.type));

                if (value.type != CharacterCompareType::AnyChar
                    && value.type != CharacterCompareType::TemporaryInverse
                    && value.type != CharacterCompareType::Inverse
                    && value.type != CharacterCompareType::And
                    && value.type != CharacterCompareType::Or
                    && value.type != CharacterCompareType::EndAndOr)
                    arguments.append(value.value);
                ++argument_count;
                break;
            }

            if (should_invert_after_this_iteration) {
                swap(current_table, current_inverted_table);
                is_currently_inverted = !is_currently_inverted;
            }
        }

        flush_tables();
    }

    target.empend(static_cast<ByteCodeValueType>(OpCodeId::Compare));
    target.empend(argument_count);   // number of arguments
    target.empend(arguments.size()); // size of arguments
    target.extend(move(arguments));
}

template void Regex<PosixBasicParser>::run_optimization_passes();
template void Regex<PosixExtendedParser>::run_optimization_passes();
template void Regex<ECMA262Parser>::run_optimization_passes();
}
