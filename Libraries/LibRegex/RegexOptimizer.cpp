/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Enumerate.h>
#include <AK/Function.h>
#include <AK/GenericShorthands.h>
#include <AK/Queue.h>
#include <AK/QuickSort.h>
#include <AK/RedBlackTree.h>
#include <AK/Stack.h>
#include <AK/TemporaryChange.h>
#include <AK/Trie.h>
#include <AK/Vector.h>
#include <LibRegex/Regex.h>
#include <LibRegex/RegexBytecodeStreamOptimizer.h>
#include <LibRegex/RegexDebug.h>
#include <LibUnicode/CharacterTypes.h>
#if REGEX_DEBUG
#    include <AK/ScopeGuard.h>
#    include <AK/ScopeLogger.h>
#endif

namespace regex {

using Detail::Block;

struct BytecodeRewriter {
    struct Instruction {
        size_t old_ip;
        size_t size;
        OpCodeId id;
        bool skip;
    };
    Vector<Instruction> instructions;
    HashMap<size_t, size_t> new_ip_mapping;
    StringView target_pattern;

    BytecodeRewriter(ByteCode const& bytecode, StringView pattern = {})
        : target_pattern(pattern)
    {
        auto flat = bytecode.flat_data();
        auto state = MatchState::only_for_enumeration();

        for (size_t old_ip = 0; old_ip < flat.size();) {
            state.instruction_position = old_ip;
            auto& op = bytecode.get_opcode(state);
            auto sz = op.size();

            instructions.append({ old_ip, sz, op.opcode_id(), false });
            old_ip += sz;
        }
    }

    void mark_range_for_skip(size_t start, size_t end)
    {
        for (auto& instr : instructions) {
            if (instr.old_ip >= start && instr.old_ip < end)
                instr.skip = true;
        }
    }

    void build_ip_mapping(ByteCode const& bytecode, Span<ByteCode const> replacements)
    {
        new_ip_mapping.ensure_capacity(instructions.size() + 1);
        size_t current_new_ip = 0;
        auto replacements_it = replacements.begin();

        for (auto& instr : instructions) {
            new_ip_mapping.set(instr.old_ip, current_new_ip);
            auto& replacement = *replacements_it;
            ++replacements_it;

            if (!instr.skip)
                current_new_ip += instr.size;
            else
                current_new_ip += replacement.size();
        }

        new_ip_mapping.set(bytecode.size(), current_new_ip);
    }

    template<typename Range>
    requires(requires(Range r) { r.start_ip; r.end_ip; })
    void build_ip_mapping(ByteCode const& bytecode, Span<Range const> replacement_ranges, Span<ByteCode const> replacements)
    {
        new_ip_mapping.ensure_capacity(instructions.size() + 1);
        size_t current_new_ip = 0;
        auto instruction_it = instructions.begin();

        for (auto i = 0uz; i < replacements.size(); ++i) {
            auto& range = replacement_ranges[i];
            auto& replacement = replacements[i];

            while (instruction_it != instructions.end()) {
                auto& instr = *instruction_it;
                if (instr.old_ip >= range.start_ip) {
                    ASSERT(instr.old_ip < range.end_ip);
                    new_ip_mapping.set(instr.old_ip, current_new_ip);
                    break;
                }
                new_ip_mapping.set(instr.old_ip, current_new_ip);
                current_new_ip += instr.size;
                ++instruction_it;
            }
            current_new_ip += replacement.size();

            // Skip instructions in the replacement range
            while (instruction_it != instructions.end()) {
                auto& instr = *instruction_it;
                if (instr.old_ip >= range.end_ip)
                    break;
                ++instruction_it;
            }
        }
        // Map any remaining instructions
        for (; instruction_it != instructions.end(); ++instruction_it) {
            auto& instr = *instruction_it;
            new_ip_mapping.set(instr.old_ip, current_new_ip);
            current_new_ip += instr.size;
        }
        new_ip_mapping.set(bytecode.size(), current_new_ip);
    }

    template<typename Range>
    requires(requires(Range r) { r.start_ip; r.end_ip; })
    ByteCode rebuild(ByteCode const& bytecode, Span<Range> replacement_ranges, Span<ByteCode const> replacements)
    {
        // Assumes that replacement_ranges and replacements are the same size
        // As well as in order
        VERIFY(replacement_ranges.size() == replacements.size());
        auto flat = bytecode.flat_data();
        ByteCode result;
        result.merge_string_tables_from({ &bytecode, 1 });

        size_t total_new_size = bytecode.size();
        // FIXME: Get a zip(it...) helper
        for (auto i = 0uz; i < replacement_ranges.size(); ++i) {
            mark_range_for_skip(replacement_ranges[i].start_ip, replacement_ranges[i].end_ip);
            total_new_size -= (replacement_ranges[i].end_ip - replacement_ranges[i].start_ip);
            total_new_size += replacements[i].size();
        }
        build_ip_mapping<Range>(bytecode, replacement_ranges, replacements);
        result.ensure_capacity(total_new_size);

        // FIXME: Use a zip(...) helper
        auto instructions_it = instructions.begin();
        for (auto i = 0uz; i < replacement_ranges.size(); ++i) {
            auto& range = replacement_ranges[i];
            auto& replacement = replacements[i];

            // Append and adjust all instructions before the replacement range
            for (; instructions_it != instructions.end(); ++instructions_it) {
                auto& instr = *instructions_it;
                if (instr.old_ip >= range.start_ip) {
                    ASSERT(instr.old_ip < range.end_ip);
                    ++instructions_it;
                    break;
                }
                VERIFY(instr.skip == false);
                auto slice = Vector<ByteCodeValueType> { flat.slice(instr.old_ip, instr.size) };
                adjust_jump_in_slice(bytecode, slice, instr);
                result.append(move(slice));
            }
            // Finally insert the replacement
            result.extend(replacement);

            // Skip instructions in the replacement range
            while (instructions_it != instructions.end()) {
                auto& instr = *instructions_it;
                if (instr.old_ip >= range.end_ip)
                    break;
                ++instructions_it;
            }
        }
        // Append any remaining instructions
        for (; instructions_it != instructions.end(); ++instructions_it) {
            auto& instr = *instructions_it;
            auto slice = Vector<ByteCodeValueType> { flat.slice(instr.old_ip, instr.size) };
            adjust_jump_in_slice(bytecode, slice, instr);
            result.append(move(slice));
            VERIFY(instr.skip == false);
        }

        result.flatten();
        return result;
    }

    ByteCode rebuild(ByteCode const& bytecode, Function<void(Instruction const&, ByteCode&)> insert_replacement = nullptr)
    {
        auto flat = bytecode.flat_data();
        ByteCode result;
        result.merge_string_tables_from({ &bytecode, 1 });

        Vector<ByteCode> replacements;
        replacements.resize_with_default_value(instructions.size(), ByteCode {});

        size_t total_new_size = 0;
        for (auto const& [i, instr] : enumerate(instructions)) {
            if (!instr.skip) {
                total_new_size += instr.size;
            } else if (insert_replacement) {
                ByteCode temp;
                insert_replacement(instr, temp);
                total_new_size += temp.size();
                replacements[i] = move(temp);
            }
        }
        build_ip_mapping(bytecode, replacements);
        result.ensure_capacity(total_new_size);

        auto replacements_it = replacements.begin();
        for (auto& instr : instructions) {
            auto& replacement = *replacements_it;
            ++replacements_it;

            if (instr.skip) {
                result.extend(move(replacement));
                continue;
            }

            auto slice = Vector<ByteCodeValueType> { flat.slice(instr.old_ip, instr.size) };
            adjust_jump_in_slice(bytecode, slice, instr);
            result.append(move(slice));
        }

        result.flatten();
        return result;
    }

private:
    void adjust_jump_in_slice(ByteCode const& bytecode, Vector<ByteCodeValueType>& slice, Instruction const& instr)
    {
        auto adjust = [&](size_t idx, bool is_repeat) {
            auto old_offset = slice[idx];
            auto target_old = is_repeat
                ? instr.old_ip - old_offset
                : instr.old_ip + instr.size + old_offset;

            if (!new_ip_mapping.contains(target_old)) {
                dbgln("In pattern /{}/", target_pattern);
                dbgln("Target {} not found in new_ip mapping (in {})", target_old, instr.old_ip);
                RegexDebug<ByteCode> dbg(stderr);
                dbg.print_bytecode(bytecode);
                VERIFY_NOT_REACHED();
            }

            size_t target_new = *new_ip_mapping.get(target_old);
            size_t source_new = *new_ip_mapping.get(instr.old_ip);
            auto new_offset = is_repeat
                ? source_new - target_new
                : target_new - source_new - instr.size;

            slice[idx] = static_cast<ByteCodeValueType>(new_offset);
        };

        switch (instr.id) {
        case OpCodeId::Jump:
        case OpCodeId::ForkJump:
        case OpCodeId::ForkStay:
        case OpCodeId::ForkReplaceJump:
        case OpCodeId::ForkReplaceStay:
        case OpCodeId::JumpNonEmpty:
        case OpCodeId::ForkIf:
            adjust(1, false);
            break;
        case OpCodeId::Repeat:
            adjust(1, true);
            break;
        default:
            break;
        }
    }
};

template<typename Parser>
static typename Regex<Parser>::BasicBlockList split_basic_blocks_for_atomic_groups(ByteCode const& bytecode)
{
    typename Regex<Parser>::BasicBlockList block_boundaries;
    size_t end_of_last_block = 0;

    auto bytecode_size = bytecode.size();

    auto state = MatchState::only_for_enumeration();
    state.instruction_position = 0;
    auto check_jump = [&]<template<typename> class T>(auto const& opcode) {
        auto& op = static_cast<T<ByteCode> const&>(opcode);
        ssize_t jump_offset = op.size() + op.offset();
        if (jump_offset >= 0) {
            block_boundaries.append({ end_of_last_block, state.instruction_position, "Jump ahead"sv });
            end_of_last_block = state.instruction_position + opcode.size();
        } else {
            // This op jumps back, see if that's within this "block".
            if (jump_offset + state.instruction_position > end_of_last_block) {
                // Split the block!
                block_boundaries.append({ end_of_last_block, jump_offset + state.instruction_position, "Jump back 1"sv });
                block_boundaries.append({ jump_offset + state.instruction_position, state.instruction_position, "Jump back 2"sv });
                end_of_last_block = state.instruction_position + opcode.size();
            } else {
                // Nope, it's just a jump to another block
                block_boundaries.append({ end_of_last_block, state.instruction_position, "Jump"sv });
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
        case OpCodeId::ForkIf:
            check_jump.template operator()<OpCode_ForkIf>(opcode);
            break;
        case OpCodeId::FailForks:
            block_boundaries.append({ end_of_last_block, state.instruction_position, "FailForks"sv });
            end_of_last_block = state.instruction_position + opcode.size();
            break;
        case OpCodeId::Repeat: {
            // Repeat produces two blocks, one containing its repeated expr, and one after that.
            auto& repeat = static_cast<OpCode_Repeat<ByteCode> const&>(opcode);
            auto repeat_start = state.instruction_position - repeat.offset();
            if (repeat_start > end_of_last_block)
                block_boundaries.append({ end_of_last_block, repeat_start, "Repeat"sv });
            block_boundaries.append({ repeat_start, state.instruction_position, "Repeat after"sv });
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
        block_boundaries.append({ end_of_last_block, bytecode_size, "End"sv });

    quick_sort(block_boundaries, [](auto& a, auto& b) { return a.start < b.start; });

    return block_boundaries;
}

template<typename Parser>
void Regex<Parser>::run_optimization_passes()
{
    ScopeGuard switch_to_flat = [&] {
        parser_result.bytecode = FlatByteCode::from(move(parser_result.bytecode.template get<ByteCode>()));
    };
    rewrite_with_useless_jumps_removed();

    auto blocks = split_basic_blocks(parser_result.bytecode.get<ByteCode>());
    if (attempt_rewrite_entire_match_as_substring_search(blocks)) {
        return;
    }

    // Rewrite fork loops as atomic groups
    // e.g. a*b -> (ATOMIC a*)b
    blocks = split_basic_blocks_for_atomic_groups<Parser>(parser_result.bytecode.get<ByteCode>());
    attempt_rewrite_loops_as_atomic_groups(blocks);

    // Join adjacent compares that only match single characters into a single compare that matches a string.
    blocks = split_basic_blocks(parser_result.bytecode.get<ByteCode>());
    attempt_rewrite_adjacent_compares_as_string_compare(blocks);

    // Rewrite /.*x/ as a seek to x
    blocks = split_basic_blocks(parser_result.bytecode.get<ByteCode>());
    attempt_rewrite_dot_star_sequences_as_seek(blocks);

    // Simplify compares where possible
    blocks = split_basic_blocks(parser_result.bytecode.get<ByteCode>());
    rewrite_simple_compares(blocks);

    fill_optimization_data(split_basic_blocks(parser_result.bytecode.template get<ByteCode>()));
}

struct StaticallyInterpretedCompares {
    RedBlackTree<u32, u32> ranges;
    RedBlackTree<u32, u32> negated_ranges;
    HashTable<CharClass> char_classes;
    HashTable<CharClass> negated_char_classes;

    bool has_any_unicode_property = false;
    HashTable<Unicode::GeneralCategory> unicode_general_categories;
    HashTable<Unicode::Property> unicode_properties;
    HashTable<Unicode::Script> unicode_scripts;
    HashTable<Unicode::Script> unicode_script_extensions;
    HashTable<Unicode::GeneralCategory> negated_unicode_general_categories;
    HashTable<Unicode::Property> negated_unicode_properties;
    HashTable<Unicode::Script> negated_unicode_scripts;
    HashTable<Unicode::Script> negated_unicode_script_extensions;
};

static bool interpret_compares(Vector<CompareTypeAndValuePair> const& lhs, StaticallyInterpretedCompares& compares, ByteCodeBase const* bytecode = nullptr, bool as_follow = false)
{
    bool inverse { false };
    bool temporary_inverse { false };
    bool reset_temporary_inverse { false };

    auto current_lhs_inversion_state = [&]() -> bool { return temporary_inverse ^ inverse; };

    auto& lhs_ranges = compares.ranges;
    auto& lhs_negated_ranges = compares.negated_ranges;
    auto& lhs_char_classes = compares.char_classes;
    auto& lhs_negated_char_classes = compares.negated_char_classes;
    auto& has_any_unicode_property = compares.has_any_unicode_property;
    auto& lhs_unicode_general_categories = compares.unicode_general_categories;
    auto& lhs_unicode_properties = compares.unicode_properties;
    auto& lhs_unicode_scripts = compares.unicode_scripts;
    auto& lhs_unicode_script_extensions = compares.unicode_script_extensions;
    auto& lhs_negated_unicode_general_categories = compares.negated_unicode_general_categories;
    auto& lhs_negated_unicode_properties = compares.negated_unicode_properties;
    auto& lhs_negated_unicode_scripts = compares.negated_unicode_scripts;
    auto& lhs_negated_unicode_script_extensions = compares.negated_unicode_script_extensions;

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
                return false;
            break;
        case CharacterCompareType::Char:
            if (!current_lhs_inversion_state())
                lhs_ranges.insert(pair.value, pair.value);
            else
                lhs_negated_ranges.insert(pair.value, pair.value);
            break;
        case CharacterCompareType::String: {
            if (!as_follow)
                return false;
            auto string = bytecode->get_u16_string(pair.value);
            u32 ch = string.code_point_at(0);

            if (!current_lhs_inversion_state())
                lhs_ranges.insert(ch, ch);
            else
                lhs_negated_ranges.insert(ch, ch);
            break;
        }
        case CharacterCompareType::StringSet:
            return false;
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
            return false;
        case CharacterCompareType::Reference:
        case CharacterCompareType::NamedReference:
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
        case CharacterCompareType::Subtract:
            // FIXME: These are too difficult to handle, so bail out.
            return false;
        case CharacterCompareType::Undefined:
        case CharacterCompareType::RangeExpressionDummy:
            // These do not occur in valid bytecode.
            VERIFY_NOT_REACHED();
        }
    }

    return true;
}

template<class Parser>
void Regex<Parser>::fill_optimization_data(BasicBlockList const& blocks)
{
    if (blocks.is_empty())
        return;

    if constexpr (REGEX_DEBUG) {
        dbgln("Pulling out optimization data from bytecode:");
        RegexDebug<ByteCode> dbg;
        dbg.print_bytecode(*this);
        for (auto const& block : blocks)
            dbgln("block from {} to {} (comment: {})", block.start, block.end, block.comment);
    }

    ScopeGuard print = [&] {
        if constexpr (REGEX_DEBUG) {
            dbgln("Optimization data:");
            if (parser_result.optimization_data.starting_ranges.is_empty())
                dbgln("; - no starting ranges");
            for (auto const& range : parser_result.optimization_data.starting_ranges)
                dbgln("  - starting range: {}-{}", range.from, range.to);
            dbgln("; - only start of line: {}", parser_result.optimization_data.only_start_of_line);
        }
    };

    auto& bytecode = parser_result.bytecode.get<ByteCode>();

    auto state = MatchState::only_for_enumeration();
    auto block = blocks.first();
    for (state.instruction_position = block.start; state.instruction_position < block.end;) {
        auto& opcode = bytecode.get_opcode(state);
        switch (opcode.opcode_id()) {
        case OpCodeId::Compare: {
            auto& compare = to<OpCode_Compare>(opcode);
            if (compare.arguments_count() == 0)
                return; // This matches 'nothing', so there are no starting ranges that can satisfy it.
            auto flat_compares = compare.flat_compares();
            StaticallyInterpretedCompares compares;
            if (!interpret_compares(flat_compares, compares))
                return; // No idea, the bytecode is too complex.

            if (compares.has_any_unicode_property)
                return; // Faster to just run the bytecode.

            // FIXME: We should be able to handle these cases (jump ahead while...)
            if (!compares.char_classes.is_empty() || !compares.negated_char_classes.is_empty() || !compares.negated_ranges.is_empty())
                return;

            for (auto it = compares.ranges.begin(); it != compares.ranges.end(); ++it) {
                parser_result.optimization_data.starting_ranges.append({ it.key(), *it });
                for (auto range : Unicode::expand_range_case_insensitive(it.key(), *it))
                    parser_result.optimization_data.starting_ranges_insensitive.append({ range.from, range.to });
            }
            quick_sort(parser_result.optimization_data.starting_ranges_insensitive, [](CharRange a, CharRange b) { return a.from < b.from; });
            return;
        }
        case OpCodeId::CompareSimple: {
            auto& compare = to<OpCode_CompareSimple>(opcode);
            auto flat_compares = compare.flat_compares();
            StaticallyInterpretedCompares compares;
            if (!interpret_compares(flat_compares, compares))
                return; // No idea, the bytecode is too complex.

            if (compares.has_any_unicode_property)
                return; // Faster to just run the bytecode.

            // FIXME: We should be able to handle these cases (jump ahead while...)
            if (!compares.char_classes.is_empty() || !compares.negated_char_classes.is_empty() || !compares.negated_ranges.is_empty())
                return;

            for (auto it = compares.ranges.begin(); it != compares.ranges.end(); ++it) {
                parser_result.optimization_data.starting_ranges.append({ it.key(), *it });
                for (auto range : Unicode::expand_range_case_insensitive(it.key(), *it))
                    parser_result.optimization_data.starting_ranges_insensitive.append({ range.from, range.to });
            }
            quick_sort(parser_result.optimization_data.starting_ranges_insensitive, [](CharRange a, CharRange b) { return a.from < b.from; });
            return;
        }
        case OpCodeId::CheckBegin:
            parser_result.optimization_data.only_start_of_line = true;
            return;
        case OpCodeId::Checkpoint:
        case OpCodeId::Save:
        case OpCodeId::ClearCaptureGroup:
        case OpCodeId::SaveLeftCaptureGroup:
            // These do not 'match' anything, so look through them.
            state.instruction_position += opcode.size();
            continue;
        default:
            return;
        }
    }
}

template<typename Parser>
typename Regex<Parser>::BasicBlockList Regex<Parser>::split_basic_blocks(ByteCode const& bytecode)
{
    BasicBlockList block_boundaries;
    HashTable<size_t> block_starts;

    auto bytecode_size = bytecode.size();

    block_starts.set(0);

    auto state = MatchState::only_for_enumeration();
    state.instruction_position = 0;

    auto check_jump = [&]<template<typename> class T>(auto const& opcode) {
        auto& op = static_cast<T<ByteCode> const&>(opcode);
        ssize_t jump_offset = op.size() + op.offset();
        ssize_t target = state.instruction_position + jump_offset;

        block_starts.set(target);
        block_starts.set(state.instruction_position + opcode.size());
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
        case OpCodeId::ForkIf:
            check_jump.template operator()<OpCode_ForkIf>(opcode);
            break;
        case OpCodeId::FailForks:
            block_starts.set(state.instruction_position + opcode.size());
            break;
        case OpCodeId::Repeat: {
            auto& repeat = to<OpCode_Repeat>(opcode);
            auto repeat_start = state.instruction_position - repeat.offset();
            block_starts.set(repeat_start);
            block_starts.set(state.instruction_position + opcode.size());
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

    Vector<size_t> sorted_starts;
    for (auto start : block_starts)
        sorted_starts.append(start);
    quick_sort(sorted_starts);

    for (size_t i = 0; i < sorted_starts.size(); ++i) {
        size_t start = sorted_starts[i];
        size_t end;

        if (i + 1 < sorted_starts.size()) {
            size_t next_block_start = sorted_starts[i + 1];

            state.instruction_position = start;
            size_t last_ip = start;
            while (state.instruction_position < next_block_start) {
                last_ip = state.instruction_position;
                auto& opcode = bytecode.get_opcode(state);
                state.instruction_position += opcode.size();
            }
            end = last_ip;
        } else {
            state.instruction_position = start;
            size_t last_ip = start;
            while (state.instruction_position < bytecode_size) {
                last_ip = state.instruction_position;
                auto& opcode = bytecode.get_opcode(state);
                auto next_ip = state.instruction_position + opcode.size();
                if (next_ip >= bytecode_size)
                    break;
                state.instruction_position = next_ip;
            }
            end = last_ip;
        }

        block_boundaries.append({ start, end, "Block"sv });
    }

    return block_boundaries;
}

static bool has_overlap(Vector<CompareTypeAndValuePair> const& lhs, Vector<CompareTypeAndValuePair> const& rhs, bool insensitive = false, bool unicode_mode = false)
{
    // We have to fully interpret the two sequences to determine if they overlap (that is, keep track of inversion state and what ranges they cover).
    bool inverse { false };
    bool temporary_inverse { false };
    bool reset_temporary_inverse { false };

    auto current_lhs_inversion_state = [&]() -> bool { return temporary_inverse ^ inverse; };

    StaticallyInterpretedCompares compares;
    auto& lhs_ranges = compares.ranges;
    auto& lhs_negated_ranges = compares.negated_ranges;
    auto& lhs_char_classes = compares.char_classes;
    auto& lhs_negated_char_classes = compares.negated_char_classes;
    auto& has_any_unicode_property = compares.has_any_unicode_property;
    auto& lhs_unicode_general_categories = compares.unicode_general_categories;
    auto& lhs_unicode_properties = compares.unicode_properties;
    auto& lhs_unicode_scripts = compares.unicode_scripts;
    auto& lhs_unicode_script_extensions = compares.unicode_script_extensions;
    auto& lhs_negated_unicode_general_categories = compares.negated_unicode_general_categories;
    auto& lhs_negated_unicode_properties = compares.negated_unicode_properties;
    auto& lhs_negated_unicode_scripts = compares.negated_unicode_scripts;
    auto& lhs_negated_unicode_script_extensions = compares.negated_unicode_script_extensions;

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

        for (auto it = lhs_ranges.begin(); it != lhs_ranges.end(); ++it) {
            auto lhs_start = it.key();
            auto lhs_end = *it;
            if (lhs_start <= end && start <= lhs_end)
                return true;
        }

        if (insensitive) {
            auto expanded_ranges = Unicode::expand_range_case_insensitive(start, end);
            for (auto const& expanded : expanded_ranges) {
                for (auto it = lhs_ranges.begin(); it != lhs_ranges.end(); ++it) {
                    auto lhs_start = it.key();
                    auto lhs_end = *it;
                    if (lhs_start <= expanded.to && expanded.from <= lhs_end)
                        return true;
                }
            }
        }

        return false;
    };

    auto char_class_contains = [&](CharClass const& value) -> bool {
        if (lhs_char_classes.contains(value))
            return true;

        if (lhs_negated_char_classes.contains(value))
            return false;

        for (auto const& lhs_class : lhs_char_classes) {
            for (u32 ch = 0; ch < 128; ++ch) {
                if (OpCode_Compare<ByteCode>::matches_character_class(value, ch, insensitive, unicode_mode) && OpCode_Compare<ByteCode>::matches_character_class(lhs_class, ch, insensitive, unicode_mode))
                    return true;
            }
        }

        if (lhs_ranges.is_empty())
            return false;

        for (auto it = lhs_ranges.begin(); it != lhs_ranges.end(); ++it) {
            auto start = it.key();
            auto end = *it;
            for (u32 ch = start; ch <= end; ++ch) {
                if (OpCode_Compare<ByteCode>::matches_character_class(value, ch, insensitive, unicode_mode))
                    return true;
            }
        }

        return false;
    };

    if (!interpret_compares(lhs, compares))
        return true; // We can't interpret this, so we can't optimize it.

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
    struct DisjunctionState {
        bool in_or = false; // We're in an OR block, so we should wait for the EndAndOr to decide if we would match.
        bool matched_in_or = false;
        bool inverse_matched_in_or = false;
    };

    Vector<DisjunctionState, 2> disjunction_stack;
    disjunction_stack.empend();

    auto in_or = [&] -> bool& { return disjunction_stack.last().in_or; };
    auto matched_in_or = [&] -> bool& { return disjunction_stack.last().matched_in_or; };
    auto inverse_matched_in_or = [&] -> bool& { return disjunction_stack.last().inverse_matched_in_or; };

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
            dbgln("}}, in or: {}, matched in or: {}, inverse matched in or: {}", in_or(), matched_in_or(), inverse_matched_in_or());
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
            if (!in_or() && !current_lhs_inversion_state())
                return true;
            if (in_or()) {
                matched_in_or() = true;
                inverse_matched_in_or() = false;
            }
            break;
        case CharacterCompareType::Char: {
            auto matched = range_contains(pair.value);
            if (!matched) {
                for (auto const& char_class : lhs_char_classes) {
                    if (OpCode_Compare<ByteCode>::matches_character_class(char_class, pair.value, insensitive, unicode_mode)) {
                        matched = true;
                        break;
                    }
                }
            }
            if (!in_or() && (current_lhs_inversion_state() ^ matched))
                return true;
            if (in_or()) {
                matched_in_or() |= matched;
                inverse_matched_in_or() |= !matched;
            }
            break;
        }
        case CharacterCompareType::String:
            // FIXME: We just need to look at the last character of this string, but we only have the first character here.
            //        Just bail out to avoid false positives.
            return true;
        case CharacterCompareType::StringSet:
            return true;
        case CharacterCompareType::CharClass: {
            auto contains = char_class_contains(static_cast<CharClass>(pair.value));
            if (!in_or() && (current_lhs_inversion_state() ^ contains))
                return true;
            if (in_or()) {
                matched_in_or() |= contains;
                inverse_matched_in_or() |= !contains;
            }
            break;
        }
        case CharacterCompareType::CharRange: {
            auto range = CharRange(pair.value);
            auto contains = range_contains(range);
            if (!contains) {
                for (auto const& char_class : lhs_char_classes) {
                    for (u32 ch = range.from; ch <= range.to; ++ch) {
                        if (OpCode_Compare<ByteCode>::matches_character_class(char_class, ch, insensitive, unicode_mode)) {
                            contains = true;
                            break;
                        }
                    }
                    if (contains)
                        break;
                }
            }
            if (!in_or() && (contains ^ current_lhs_inversion_state()))
                return true;

            if (in_or()) {
                matched_in_or() |= contains;
                inverse_matched_in_or() |= !contains;
            }

            break;
        }
        case CharacterCompareType::LookupTable:
            // We've transformed this into a series of ranges in flat_compares(), so bail out if we see it.
            return true;
        case CharacterCompareType::Reference:
        case CharacterCompareType::NamedReference:
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
                if (!in_or() && (current_lhs_inversion_state() ^ contains))
                    return true;

                auto inverse_contains = lhs_negated_unicode_properties.contains(static_cast<Unicode::Property>(pair.value));
                if (!in_or() && !(current_lhs_inversion_state() ^ inverse_contains))
                    return true;

                if (in_or()) {
                    matched_in_or() |= contains;
                    inverse_matched_in_or() |= inverse_contains;
                }
            }
            break;
        case CharacterCompareType::GeneralCategory:
            if (!lhs_ranges.is_empty() || !lhs_negated_ranges.is_empty() || !lhs_char_classes.is_empty() || !lhs_negated_char_classes.is_empty())
                return true;
            if (has_any_unicode_property && !lhs_unicode_general_categories.is_empty() && !lhs_negated_unicode_general_categories.is_empty()) {
                auto contains = lhs_unicode_general_categories.contains(static_cast<Unicode::GeneralCategory>(pair.value));
                if (!in_or() && (current_lhs_inversion_state() ^ contains))
                    return true;
                auto inverse_contains = lhs_negated_unicode_general_categories.contains(static_cast<Unicode::GeneralCategory>(pair.value));
                if (!in_or() && !(current_lhs_inversion_state() ^ inverse_contains))
                    return true;
                if (in_or()) {
                    matched_in_or() |= contains;
                    inverse_matched_in_or() |= inverse_contains;
                }
            }
            break;
        case CharacterCompareType::Script:
            if (!lhs_ranges.is_empty() || !lhs_negated_ranges.is_empty() || !lhs_char_classes.is_empty() || !lhs_negated_char_classes.is_empty())
                return true;
            if (has_any_unicode_property && !lhs_unicode_scripts.is_empty() && !lhs_negated_unicode_scripts.is_empty()) {
                auto contains = lhs_unicode_scripts.contains(static_cast<Unicode::Script>(pair.value));
                if (!in_or() && (current_lhs_inversion_state() ^ contains))
                    return true;
                auto inverse_contains = lhs_negated_unicode_scripts.contains(static_cast<Unicode::Script>(pair.value));
                if (!in_or() && !(current_lhs_inversion_state() ^ inverse_contains))
                    return true;
                if (in_or()) {
                    matched_in_or() |= contains;
                    inverse_matched_in_or() |= inverse_contains;
                }
            }
            break;
        case CharacterCompareType::ScriptExtension:
            if (!lhs_ranges.is_empty() || !lhs_negated_ranges.is_empty() || !lhs_char_classes.is_empty() || !lhs_negated_char_classes.is_empty())
                return true;
            if (has_any_unicode_property && !lhs_unicode_script_extensions.is_empty() && !lhs_negated_unicode_script_extensions.is_empty()) {
                auto contains = lhs_unicode_script_extensions.contains(static_cast<Unicode::Script>(pair.value));
                if (!in_or() && (current_lhs_inversion_state() ^ contains))
                    return true;
                auto inverse_contains = lhs_negated_unicode_script_extensions.contains(static_cast<Unicode::Script>(pair.value));
                if (!in_or() && !(current_lhs_inversion_state() ^ inverse_contains))
                    return true;
                if (in_or()) {
                    matched_in_or() |= contains;
                    inverse_matched_in_or() |= inverse_contains;
                }
            }
            break;
        case CharacterCompareType::Or:
            disjunction_stack.empend(true);
            break;
        case CharacterCompareType::EndAndOr: {
            // FIXME: Handle And when we support it below.
            VERIFY(in_or());
            auto state = disjunction_stack.take_last();
            if (current_lhs_inversion_state()) {
                if (!state.inverse_matched_in_or)
                    return true;
            } else {
                if (state.matched_in_or)
                    return true;
            }

            break;
        }
        case CharacterCompareType::And:
        case CharacterCompareType::Subtract:
            // FIXME: These are too difficult to handle, so bail out.
            return true;
        case CharacterCompareType::Undefined:
        case CharacterCompareType::RangeExpressionDummy:
            // These do not occur in valid bytecode.
            VERIFY_NOT_REACHED();
        }
    }

    // We got to the end, just double-check that the inverse flag was not left on (which would match everything).
    return current_lhs_inversion_state();
}

static bool has_overlap(StaticallyInterpretedCompares const& lhs, StaticallyInterpretedCompares const& rhs, bool insensitive = false)
{
    if (lhs.has_any_unicode_property || rhs.has_any_unicode_property || !lhs.negated_ranges.is_empty() || !rhs.negated_ranges.is_empty() || !lhs.negated_char_classes.is_empty() || !rhs.negated_char_classes.is_empty())
        return true;

    for (auto it_lhs = lhs.ranges.begin(); it_lhs != lhs.ranges.end(); ++it_lhs) {
        auto lhs_start = it_lhs.key();
        auto lhs_end = *it_lhs;

        for (auto it_rhs = rhs.ranges.begin(); it_rhs != rhs.ranges.end(); ++it_rhs) {
            auto rhs_start = it_rhs.key();
            auto rhs_end = *it_rhs;

            // Check if ranges overlap
            if (lhs_start <= rhs_end && rhs_start <= lhs_end)
                return true;

            if (insensitive) {
                auto expanded_ranges = Unicode::expand_range_case_insensitive(rhs_start, rhs_end);
                for (auto const& expanded : expanded_ranges) {
                    if (lhs_start <= expanded.to && expanded.from <= lhs_end)
                        return true;
                }
            }
        }
    }

    for (auto& lhs_class : lhs.char_classes) {
        for (auto& rhs_class : rhs.char_classes) {
            if (lhs_class == rhs_class)
                return true;
        }
    }

    return false;
}
enum class AtomicRewritePreconditionResult {
    SatisfiedWithProperHeader,
    SatisfiedWithEmptyHeader,
    NotSatisfied,
};
static AtomicRewritePreconditionResult block_satisfies_atomic_rewrite_precondition(ByteCode const& bytecode, Block repeated_block, Block following_block, auto const& all_blocks, bool insensitive = false, bool unicode_mode = false)
{
    Vector<Vector<CompareTypeAndValuePair>> repeated_values;
    auto state = MatchState::only_for_enumeration();
    auto has_seen_actionable_opcode = false;
    for (state.instruction_position = repeated_block.start; state.instruction_position < repeated_block.end;) {
        auto& opcode = bytecode.get_opcode(state);
        switch (opcode.opcode_id()) {
        case OpCodeId::Compare: {
            has_seen_actionable_opcode = true;
            auto compares = to<OpCode_Compare>(opcode).flat_compares();
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
        case OpCodeId::ForkIf:
        case OpCodeId::JumpNonEmpty:
            // We could attempt to recursively resolve the follow set, but pretending that this just goes nowhere is faster.
            if (!has_seen_actionable_opcode)
                return AtomicRewritePreconditionResult::NotSatisfied;
            break;
        case OpCodeId::Jump: {
            // Just follow the jump, it's unconditional.
            auto& jump = to<OpCode_Jump>(opcode);
            auto jump_target = state.instruction_position + jump.offset() + jump.size();
            // Find the block that this jump leads to.
            auto next_block_it = find_if(all_blocks.begin(), all_blocks.end(), [jump_target](auto& block) { return block.start == jump_target; });
            if (next_block_it == all_blocks.end())
                return AtomicRewritePreconditionResult::NotSatisfied;
            repeated_block = *next_block_it;
            state.instruction_position = repeated_block.start;
            continue;
        }
        default:
            break;
        }

        state.instruction_position += opcode.size();
    }
    dbgln_if(REGEX_DEBUG, "Found {} entries in reference", repeated_values.size());

    auto accept_empty_follow = false;
    while (following_block.start == following_block.end && !accept_empty_follow) {
        dbgln_if(REGEX_DEBUG, "Following empty block {}", following_block.start);
        // If the following block has a single instruction, it must be some kind of jump.
        // Unless it's an unconditional jump, we can't rewrite it - so bail out.
        state.instruction_position = following_block.start;
        auto& opcode = bytecode.get_opcode(state);
        switch (opcode.opcode_id()) {
        case OpCodeId::Jump: {
            // Just follow the jump, it's unconditional.
            auto& jump = to<OpCode_Jump>(opcode);
            auto jump_target = state.instruction_position + jump.offset() + jump.size();
            if (jump_target < state.instruction_position) {
                dbgln_if(REGEX_DEBUG, "Jump to {} is backwards, I'm scared of loops", jump_target);
                return AtomicRewritePreconditionResult::NotSatisfied;
            }
            dbgln_if(REGEX_DEBUG, "Following jump to {}", jump_target);
            // Find the block that this jump leads to.
            auto next_block_it = find_if(all_blocks.begin(), all_blocks.end(), [jump_target](auto& block) { return block.start == jump_target; });
            if (next_block_it == all_blocks.end())
                return AtomicRewritePreconditionResult::NotSatisfied;
            following_block = *next_block_it;
            state.instruction_position = repeated_block.start;
            continue;
        }
        case OpCodeId::ForkJump:
        case OpCodeId::ForkIf:
        case OpCodeId::ForkReplaceJump:
        case OpCodeId::JumpNonEmpty:
            return AtomicRewritePreconditionResult::NotSatisfied;
        default:
            // No interesting effect here.
            dbgln_if(REGEX_DEBUG, "Empty follow had instruction {}", opcode.to_byte_string());
            accept_empty_follow = true;
            break;
        }
    }

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
            auto compares = to<OpCode_Compare>(opcode).flat_compares();
            if (compares.is_empty())
                break;

            if (any_of(compares, [&](auto& compare) {
                    return compare.type == CharacterCompareType::AnyChar || compare.type == CharacterCompareType::Reference || compare.type == CharacterCompareType::NamedReference;
                }))
                return AtomicRewritePreconditionResult::NotSatisfied;

            if (any_of(repeated_values, [&](auto& repeated_value) { return has_overlap(compares, repeated_value, insensitive, unicode_mode); }))
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
        case OpCodeId::ForkIf:
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
    case OpCodeId::ForkIf:
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
        parser_result.optimization_data.pure_substring_search.emplace();
        return true; // Empty regex, sure.
    }

    auto& bytecode = parser_result.bytecode.get<ByteCode>();

    // We have a single basic block, let's see if it's a series of character or string compares.
    Vector<u16> u16_units;
    auto state = MatchState::only_for_enumeration();
    while (state.instruction_position < bytecode.size()) {
        auto& opcode = bytecode.get_opcode(state);
        switch (opcode.opcode_id()) {
        case OpCodeId::Compare: {
            auto& compare = to<OpCode_Compare>(opcode);
            if (compare.arguments_count() == 0)
                return false; // This matches 'nothing', so we can't do a substring search.
            for (auto& flat_compare : compare.flat_compares()) {
                if (flat_compare.type != CharacterCompareType::Char)
                    return false;
                (void)AK::UnicodeUtils::code_point_to_utf16(flat_compare.value, [&](auto code_unit) { u16_units.append(code_unit); });
            }
            break;
        }
        default:
            return false;
        }
        state.instruction_position += opcode.size();
    }

    parser_result.optimization_data.pure_substring_search.emplace(move(u16_units));
    return true;
}

template<class Parser>
void Regex<Parser>::rewrite_with_useless_jumps_removed()
{
    auto& bytecode = parser_result.bytecode.get<ByteCode>();

    if constexpr (REGEX_DEBUG) {
        RegexDebug<ByteCode> dbg;
        dbg.print_bytecode(*this);
    }

    BytecodeRewriter rewriter(bytecode, pattern_value);

    auto state = MatchState::only_for_enumeration();
    for (auto& instr : rewriter.instructions) {
        state.instruction_position = instr.old_ip;
        auto& op = bytecode.get_opcode(state);

        bool is_useless = false;
        if (op.opcode_id() == OpCodeId::Jump) {
            is_useless = to<OpCode_Jump>(op).offset() == 0;
        } else if (op.opcode_id() == OpCodeId::JumpNonEmpty) {
            is_useless = to<OpCode_JumpNonEmpty>(op).offset() == 0;
        } else if (op.opcode_id() == OpCodeId::ForkJump || op.opcode_id() == OpCodeId::ForkReplaceJump) {
            is_useless = to<OpCode_ForkJump>(op).offset() == 0;
        } else if (op.opcode_id() == OpCodeId::ForkStay || op.opcode_id() == OpCodeId::ForkReplaceStay) {
            is_useless = to<OpCode_ForkStay>(op).offset() == 0;
        } else if (op.opcode_id() == OpCodeId::ForkIf) {
            is_useless = to<OpCode_ForkIf>(op).offset() == 0;
        }

        instr.skip = is_useless;
    }

    parser_result.bytecode = rewriter.rebuild(bytecode);
}

template<typename Parser>
void Regex<Parser>::attempt_rewrite_loops_as_atomic_groups(BasicBlockList const& basic_blocks)
{
    auto& bytecode = parser_result.bytecode.get<ByteCode>();
    bool const insensitive = parser_result.options.has_flag_set(AllFlags::Insensitive);
    bool const unicode_mode = parser_result.options.has_flag_set(AllFlags::Unicode);
    if constexpr (REGEX_DEBUG) {
        RegexDebug<ByteCode> dbg;
        dbg.print_bytecode(*this);
        for (auto const& block : basic_blocks)
            dbgln("block from {} to {} (comment: {})", block.start, block.end, block.comment);
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
    auto state = MatchState::only_for_enumeration();

    auto is_an_eligible_jump = [&state](auto& opcode, size_t ip, size_t block_start, AlternateForm alternate_form) {
        opcode.set_state(state);
        switch (opcode.opcode_id()) {
        case OpCodeId::JumpNonEmpty: {
            auto const& op = to<OpCode_JumpNonEmpty>(opcode);
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
            return to<OpCode_ForkJump>(opcode).offset() + ip + opcode.size() == block_start;
        case OpCodeId::ForkStay:
            if (alternate_form == AlternateForm::DirectLoopWithHeader)
                return false;
            return to<OpCode_ForkStay>(opcode).offset() + ip + opcode.size() == block_start;
        case OpCodeId::Jump:
            // Infinite loop does *not* produce forks.
            if (alternate_form == AlternateForm::DirectLoopWithoutHeader)
                return false;
            if (alternate_form == AlternateForm::DirectLoopWithHeader)
                return to<OpCode_Jump>(opcode).offset() + ip + opcode.size() == block_start;
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
        // Check if the last instruction in this block is a jump to the block itself:
        {
            state.instruction_position = forking_block.end;
            auto& opcode = bytecode.get_opcode(state);
            if (is_an_eligible_jump(opcode, state.instruction_position, forking_block.start, AlternateForm::DirectLoopWithoutHeader)) {
                // We've found RE0 (and RE1 is just the following block, if any), let's see if the precondition applies.
                // if RE1 is empty, there's no first(RE1), so this is an automatic pass.
                if (!fork_fallback_block.has_value()
                    || (fork_fallback_block->end == fork_fallback_block->start && block_satisfies_atomic_rewrite_precondition(bytecode, forking_block, *fork_fallback_block, basic_blocks, insensitive, unicode_mode) != AtomicRewritePreconditionResult::NotSatisfied)) {
                    candidate_blocks.append({ forking_block, fork_fallback_block, AlternateForm::DirectLoopWithoutHeader });
                    break;
                }

                auto precondition = block_satisfies_atomic_rewrite_precondition(bytecode, forking_block, *fork_fallback_block, basic_blocks, insensitive, unicode_mode);
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
                        || block_satisfies_atomic_rewrite_precondition(bytecode, *fork_fallback_block, *block_following_fork_fallback, basic_blocks, insensitive, unicode_mode) != AtomicRewritePreconditionResult::NotSatisfied) {
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
                        || block_satisfies_atomic_rewrite_precondition(bytecode, *fork_fallback_block, *block_following_fork_fallback, basic_blocks, insensitive, unicode_mode) != AtomicRewritePreconditionResult::NotSatisfied) {
                        candidate_blocks.append({ forking_block, {}, AlternateForm::DirectLoopWithoutHeader });
                        break;
                    }
                }
            }
        }
    }

    dbgln_if(REGEX_DEBUG, "Found {} candidate blocks", candidate_blocks.size());
    if constexpr (REGEX_DEBUG) {
        for (auto const& candidate : candidate_blocks) {
            dbgln("Candidate block from {} to {} (comment: {})", candidate.forking_block.start, candidate.forking_block.end, candidate.forking_block.comment);
            if (candidate.new_target_block.has_value())
                dbgln("  with target block from {} to {} (comment: {})", candidate.new_target_block->start, candidate.new_target_block->end, candidate.new_target_block->comment);
            switch (candidate.form) {
            case AlternateForm::DirectLoopWithoutHeader:
                dbgln("  form: DirectLoopWithoutHeader");
                break;
            case AlternateForm::DirectLoopWithoutHeaderAndEmptyFollow:
                dbgln("  form: DirectLoopWithoutHeaderAndEmptyFollow");
                break;
            case AlternateForm::DirectLoopWithHeader:
                dbgln("  form: DirectLoopWithHeader");
                break;
            default:
                dbgln("  form: Unknown");
                break;
            }
        }
    }
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
        auto state = MatchState::only_for_enumeration();
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
                patch_points.push({ to<OpCode_Jump>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::JumpNonEmpty:
                patch_points.push({ to<OpCode_JumpNonEmpty>(opcode).offset(), state.instruction_position + 1 });
                patch_points.push({ to<OpCode_JumpNonEmpty>(opcode).checkpoint(), state.instruction_position + 2 });
                break;
            case OpCodeId::ForkJump:
                patch_points.push({ to<OpCode_ForkJump>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::ForkStay:
                patch_points.push({ to<OpCode_ForkStay>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::ForkIf:
                patch_points.push({ to<OpCode_ForkIf>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::Repeat:
                patch_points.push({ -(ssize_t)to<OpCode_Repeat>(opcode).offset(), state.instruction_position + 1, true });
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
        RegexDebug<ByteCode> dbg;
        dbg.print_bytecode(*this);
    }
}

template<typename Parser>
void Regex<Parser>::attempt_rewrite_adjacent_compares_as_string_compare(BasicBlockList const& basic_blocks)
{
    auto& bytecode = parser_result.bytecode.get<ByteCode>();

    if (basic_blocks.is_empty())
        return;

    // Find sequences of single-character compares
    struct StringSequence {
        size_t start_ip;
        size_t end_ip;
        Vector<u32> characters;
    };
    Vector<StringSequence> sequences;

    for (auto const& block : basic_blocks) {
        auto state = MatchState::only_for_enumeration();
        Vector<u32> current_chars;
        size_t sequence_start = 0;
        bool in_sequence = false;

        for (state.instruction_position = block.start; state.instruction_position <= block.end;) {
            auto current_ip = state.instruction_position;
            auto& opcode = bytecode.get_opcode(state);

            bool is_single_char = false;
            u32 character = 0;

            if (opcode.opcode_id() == OpCodeId::Compare) {
                auto& compare = to<OpCode_Compare>(opcode);
                auto flat_compares = compare.flat_compares();

                if (flat_compares.size() == 1
                    && flat_compares[0].type == CharacterCompareType::Char) {
                    is_single_char = true;
                    character = flat_compares[0].value;
                }
            }

            if (is_single_char) {
                if (!in_sequence) {
                    sequence_start = current_ip;
                    current_chars.clear();
                    in_sequence = true;
                }
                current_chars.append(character);
            } else {
                if (in_sequence && current_chars.size() >= 2) {
                    sequences.append({ sequence_start, current_ip, move(current_chars) });
                    current_chars.clear();
                }
                in_sequence = false;
            }

            state.instruction_position += opcode.size();
        }

        if (in_sequence && current_chars.size() >= 2) {
            sequences.append({ sequence_start, state.instruction_position, move(current_chars) });
        }
    }

    if (sequences.is_empty())
        return;

    BytecodeRewriter rewriter(bytecode, pattern_value);
    Vector<ByteCode> replacements;
    replacements.ensure_capacity(sequences.size());
    for (auto const& seq : sequences) {
        StringBuilder string_builder(StringBuilder::Mode::UTF16);
        for (auto ch : seq.characters)
            string_builder.append_code_point(ch);
        ByteCode replacement;
        replacement.insert_bytecode_compare_string(string_builder.to_utf16_string());
        replacements.append(move(replacement));
    }

    parser_result.bytecode = rewriter.rebuild(bytecode, sequences.span(), replacements.span());
}

template<typename Parser>
void Regex<Parser>::attempt_rewrite_dot_star_sequences_as_seek(BasicBlockList const& basic_blocks)
{
    auto& bytecode = parser_result.bytecode.get<ByteCode>();

    if (basic_blocks.is_empty()) {
        dbgln_if(REGEX_DEBUG, "No basic blocks, skipping /.*/ rewrite");
        return;
    }

    // If a /.*/ sequence is followed by a compare C (with some non-matching ops {O} in between), we can rewrite:
    //     bbN: {O0}                     (optional non-matching ops before the pattern)
    //          ForkStay bbM
    //          Checkpoint p
    //          Compare AnyChar
    //          FailIfEmpty              (optional, noop for .*)
    //          JumpNonEmpty (back to ForkStay) p
    //     bbM: {O1}                     (optional non-matching ops)
    //          Compare C
    // as
    //     bbN: {O0}
    //     bbR: RSeekTo C
    //          ForkStay bbR
    //     bbM: {O1}
    //          Compare C
    //
    // Note: bbM is determined by the ForkStay's target, not necessarily the next sequential block
    // Note: The pattern may span across multiple basic blocks

    struct DotStarCandidate {
        size_t fork_ip;
        size_t checkpoint_ip;
        size_t compare_ip;
        size_t jump_ip;
        size_t following_block_start;
        u64 checkpoint_id;
        u32 seek_code_point;
    };
    OrderedHashMap<size_t, DotStarCandidate> candidates;

    auto state = MatchState::only_for_enumeration();

    for (size_t i = 0; i < basic_blocks.size(); ++i) {
        auto const& block = basic_blocks[i];

        state.instruction_position = block.start;

        if (state.instruction_position > block.end)
            continue;

        // Skip non-matching ops at the start of the block
        while (state.instruction_position <= block.end) {
            auto& op = bytecode.get_opcode(state);

            switch (op.opcode_id()) {
            case OpCodeId::Checkpoint:
            case OpCodeId::Save:
            case OpCodeId::SaveLeftCaptureGroup:
            case OpCodeId::SaveRightCaptureGroup:
            case OpCodeId::SaveRightNamedCaptureGroup:
            case OpCodeId::ClearCaptureGroup:
                state.instruction_position += op.size();
                continue;

            default:
                goto found_potential_fork;
            }
        }

        {
            if (state.instruction_position >= bytecode.size())
                continue;

            auto& op_at_boundary = bytecode.get_opcode(state);
            if (op_at_boundary.opcode_id() != OpCodeId::ForkStay)
                continue;
        }

    found_potential_fork:
        // (1) ForkStay bbM
        dbgln_if(REGEX_DEBUG, "Examining block {} from {} to {}", i, block.start, block.end);
        auto& first_op = bytecode.get_opcode(state);
        if (first_op.opcode_id() != OpCodeId::ForkStay) {
            dbgln_if(REGEX_DEBUG, "  did not find ForkStay at {}", state.instruction_position);
            continue;
        }

        auto fork_ip = state.instruction_position;
        auto& fork_op = to<OpCode_ForkStay>(first_op);

        // Find the actual following block by the fork target
        auto fork_target = fork_ip + fork_op.size() + fork_op.offset();

        size_t following_block_idx = 0;
        bool found_following_block = false;
        for (size_t j = 0; j < basic_blocks.size(); ++j) {
            if (basic_blocks[j].start == fork_target) {
                if (basic_blocks[j].start <= basic_blocks[j].end) {
                    following_block_idx = j;
                    found_following_block = true;
                    break;
                }
                continue;
            }
        }

        if (!found_following_block) {
            dbgln_if(REGEX_DEBUG, "  did not find non-empty following block for fork target {}", fork_target);
            continue;
        }

        auto const& following_block = basic_blocks[following_block_idx];
        dbgln_if(REGEX_DEBUG, "  Fork target {} is in block {} (from {} to {})", fork_target, following_block_idx, following_block.start, following_block.end);

        state.instruction_position += first_op.size();

        // (2) Checkpoint p
        auto& second_op = bytecode.get_opcode(state);
        if (second_op.opcode_id() != OpCodeId::Checkpoint) {
            dbgln_if(REGEX_DEBUG, "  did not find Checkpoint at {} (found opcode {})", state.instruction_position, (int)second_op.opcode_id());
            continue;
        }

        auto checkpoint_ip = state.instruction_position;
        auto checkpoint_id = to<OpCode_Checkpoint>(second_op).id();

        state.instruction_position += second_op.size();

        // (3) Compare AnyChar
        auto& third_op = bytecode.get_opcode(state);
        if (third_op.opcode_id() != OpCodeId::Compare) {
            dbgln_if(REGEX_DEBUG, "  did not find Compare at {} (found opcode {})", state.instruction_position, (int)third_op.opcode_id());
            continue;
        }

        auto compare_ip = state.instruction_position;
        auto& compare_op = to<OpCode_Compare>(third_op);
        auto flat_compares = compare_op.flat_compares();

        if (flat_compares.size() != 1 || flat_compares[0].type != CharacterCompareType::AnyChar) {
            dbgln_if(REGEX_DEBUG, "  Compare at {} is not AnyChar", state.instruction_position);
            continue;
        }

        state.instruction_position += third_op.size();

        // (3.5) Skip FailIfEmpty if present
        {
            auto& maybe_fail_op = bytecode.get_opcode(state);
            if (maybe_fail_op.opcode_id() == OpCodeId::FailIfEmpty)
                state.instruction_position += maybe_fail_op.size();
        }

        // (4) JumpNonEmpty back to ForkStay
        auto& fourth_op = bytecode.get_opcode(state);
        if (fourth_op.opcode_id() != OpCodeId::JumpNonEmpty) {
            dbgln_if(REGEX_DEBUG, "  did not find JumpNonEmpty at {} (found opcode {})", state.instruction_position, (int)fourth_op.opcode_id());
            continue;
        }

        auto jump_ip = state.instruction_position;
        auto& jump_op = to<OpCode_JumpNonEmpty>(fourth_op);

        if (jump_ip + jump_op.size() + jump_op.offset() != fork_ip) {
            dbgln_if(REGEX_DEBUG, "  JumpNonEmpty at {} does not jump back to ForkStay at {} (instead jumps to {})",
                state.instruction_position, fork_ip, jump_ip + jump_op.size() + jump_op.offset());
            continue;
        }
        if ((size_t)jump_op.checkpoint() != checkpoint_id) {
            dbgln_if(REGEX_DEBUG, "  JumpNonEmpty at {} does not reference Checkpoint id {} (instead references {})",
                state.instruction_position, checkpoint_id, jump_op.checkpoint());
            continue;
        }

        dbgln_if(REGEX_DEBUG, "  Found .* pattern from IP {} to {}", fork_ip, jump_ip + jump_op.size());

        // The following block must contain a Compare C, with only non-matching ops in between
        state.instruction_position = following_block.start;
        while (state.instruction_position <= following_block.end) {
            auto& op = bytecode.get_opcode(state);

            switch (op.opcode_id()) {
            case OpCodeId::Checkpoint:
            case OpCodeId::Save:
            case OpCodeId::SaveLeftCaptureGroup:
            case OpCodeId::SaveRightCaptureGroup:
            case OpCodeId::SaveRightNamedCaptureGroup:
            case OpCodeId::ClearCaptureGroup:
                state.instruction_position += op.size();
                continue;

            case OpCodeId::Compare: {
                auto& following_compare_op = to<OpCode_Compare>(op);
                auto following_compares = following_compare_op.flat_compares();

                StaticallyInterpretedCompares compares;
                if (!interpret_compares(following_compares, compares, &bytecode, true)) {
                    dbgln_if(REGEX_DEBUG, "  could not statically interpret compares at {} in following block", state.instruction_position);
                    goto next_block;
                }

                // Must be able to pull a single code point from this compare (i.e. a single 1-long range, no negations, no char classes, and no unicode properties)
                if (compares.ranges.size() != 1 || !compares.negated_ranges.is_empty()
                    || !compares.char_classes.is_empty() || !compares.negated_char_classes.is_empty()
                    || compares.has_any_unicode_property
                    || !compares.unicode_general_categories.is_empty()
                    || !compares.unicode_properties.is_empty()
                    || !compares.unicode_scripts.is_empty()
                    || !compares.unicode_script_extensions.is_empty()
                    || !compares.negated_unicode_general_categories.is_empty()
                    || !compares.negated_unicode_properties.is_empty()
                    || !compares.negated_unicode_scripts.is_empty()
                    || !compares.negated_unicode_script_extensions.is_empty()) {
                    dbgln_if(REGEX_DEBUG, "  compares at {} in following block are too complex to rewrite as SeekTo", state.instruction_position);
                    goto next_block;
                }

                auto it = compares.ranges.begin();
                if (it.key() != *it) {
                    // Not a single code point
                    dbgln_if(REGEX_DEBUG, "  compares at {} in following block are a range, not a single code point ({}..{})", state.instruction_position, it.key(), *it);
                    goto next_block;
                }

                auto seeked_code_point = it.key();

                candidates.set(fork_ip, { fork_ip, checkpoint_ip, compare_ip, jump_ip, following_block.start, checkpoint_id, seeked_code_point });

                dbgln_if(REGEX_DEBUG, "  Found sequence from {} to {} followed by Compare '{}' at {}, can rewrite as SeekTo",
                    fork_ip, jump_ip + 4, (char)seeked_code_point, state.instruction_position);
                goto next_block;
            }

            default:
                dbgln_if(REGEX_DEBUG, "  Hit non-matching, non-skippable opcode {} at {} in following block", (int)op.opcode_id(), state.instruction_position);
                goto next_block;
            }
        }

    next_block:
        continue;
    }

    dbgln_if(REGEX_DEBUG, "Found {} dot-star sequences to rewrite as SeekTo", candidates.size());

    if (candidates.is_empty())
        return;

    BytecodeRewriter rewriter(bytecode, pattern_value);

    struct Range {
        size_t start_ip;
        size_t end_ip;
    };

    Vector<Range> ranges_to_skip;
    Vector<ByteCode> replacements;
    ranges_to_skip.ensure_capacity(candidates.size());
    replacements.ensure_capacity(candidates.size());

    for (auto& [_, candidate] : candidates) {
        ranges_to_skip.empend(candidate.fork_ip, candidate.jump_ip + 4); // JumpNonEmpty = 4
        ByteCode replacement;
        replacement.empend(static_cast<ByteCodeValueType>(OpCodeId::RSeekTo));
        replacement.empend(candidate.seek_code_point);
        replacement.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkStay));
        replacement.empend(static_cast<ByteCodeValueType>(-4)); // Offset back to RSeekTo
        replacements.append(move(replacement));
    }

    parser_result.bytecode = rewriter.rebuild(bytecode, ranges_to_skip.span(), replacements.span());

    if constexpr (REGEX_DEBUG) {
        dbgln("After dot-star rewrite as SeekTo:");
        RegexDebug<ByteCode> dbg;
        dbg.print_bytecode(*this);
    }
}

template<typename Parser>
void Regex<Parser>::rewrite_simple_compares(BasicBlockList const& basic_blocks)
{
    // If a Compare opcode only has a single compare and that's a match opcode
    // we can rewrite it as a CompareSimple to avoid the overhead of handling multiple compares:
    //   Compare argc=1 args=S
    //       Char 'a'
    // -->
    //   CompareSimple args=S
    //       Char 'a'

    auto& bytecode = parser_result.bytecode.get<ByteCode>();

    if (basic_blocks.is_empty())
        return;

    struct SimpleCompareCandidate {
        size_t compare_ip;
        Vector<ByteCodeValueType> compare_data;
    };
    Vector<SimpleCompareCandidate> candidates;

    auto state = MatchState::only_for_enumeration();

    for (auto const& block : basic_blocks) {
        for (state.instruction_position = block.start; state.instruction_position <= block.end;) {
            auto current_ip = state.instruction_position;
            auto& opcode = bytecode.get_opcode(state);

            if (opcode.opcode_id() == OpCodeId::Compare) {
                auto& compare = to<OpCode_Compare>(opcode);
                auto flat_compares = compare.flat_compares();

                if (flat_compares.size() == 1 && !first_is_one_of(flat_compares[0].type, CharacterCompareType::And, CharacterCompareType::Or, CharacterCompareType::Inverse, CharacterCompareType::TemporaryInverse, CharacterCompareType::Subtract, CharacterCompareType::Undefined)) {
                    auto slice = bytecode.spans().slice(current_ip + 2, opcode.size() - 2);
                    Vector<ByteCodeValueType> data;
                    data.ensure_capacity(slice.size());
                    for (auto value : slice)
                        data.append(value);
                    candidates.append({ current_ip, move(data) }); // +2 to skip opcode id and argc
                }
            }

            state.instruction_position += opcode.size();
        }
    }

    if (candidates.is_empty())
        return;

    dbgln_if(REGEX_DEBUG, "Found {} simple compare candidates to rewrite", candidates.size());

    BytecodeRewriter rewriter(bytecode, pattern_value);

    for (auto& candidate : candidates) {
        auto& instr = *rewriter.instructions.find_if([&](auto& i) { return i.old_ip == candidate.compare_ip; });
        instr.skip = true;
    }

    size_t candidate_index = 0;
    auto insert_replacement = [&](auto const& instr, ByteCode& result) {
        while (candidate_index < candidates.size()) {
            auto& candidate = candidates[candidate_index];

            if (instr.old_ip == candidate.compare_ip) {
                result.empend(static_cast<ByteCodeValueType>(OpCodeId::CompareSimple));
                result.extend(move(candidate.compare_data));
                candidate_index++;
                return;
            }

            if (instr.old_ip < candidate.compare_ip)
                return;

            candidate_index++;
        }
    };

    parser_result.bytecode = rewriter.rebuild(bytecode, move(insert_replacement));

    if constexpr (REGEX_DEBUG) {
        dbgln("After simple compare rewrite:");
        RegexDebug<ByteCode> dbg;
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
    // Assume we have N alternatives A0..AN, each with M basic blocks bb0..bbM, each with I instructions 0..I (denoted Ai.bbj[k])
    // We can create the alternation is two ways:
    // - Lay them out sequentially, such that A0 is tried, then A1, then A2, etc.
    // - Generate a prefix tree for A*.bb*[*], and walk the tree at runtime.
    // For the first case, assuming we have two A0.bb0[0..2] and A1.bb0[0..2]:
    //   out.bb0:
    //     ForkStay out.bb1
    //     A0.bb0[*]
    //     Jump out.bb2
    //   out.bb1:
    //     A1.bb0[*]
    //   out.bb2:
    //     <end>
    // For the second case, assuming the following alternatives:
    //   A0.bb0:
    //     Compare 'a'
    //     Compare 'b'
    //     Compare 'd'
    //  A1.bb0:
    //     Compare 'a'
    //     Compare 'c'
    //     Compare 'd'
    // We can first generate a prefix tree (trie here), with each node denoted by [insn, insn*]:
    //  (root)
    //  |- [A0.bb0[0], A1.bb0[0]]
    //  |   |- [A0.bb0[1]]
    //  |   |   |- [A0.bb0[2]]
    //  |   |- [A1.bb0[1]]
    //  |   |   |- [A1.bb0[2]]
    // i.e. the first instruction of A0 and A1 are the same, so we can merge them into one node;
    // everything following that is different (A1.bb0[2] is not considered equivalent to A0.bb0[2] as they are jumped-to by different instructions,
    // in this case their previous instruction)
    // Then, each trie node N { insn, children } can be represented as:
    //   out for N:
    //     N.insn[*]
    //     ForkJump out for N.children[0]
    //     ForkJump out for N.children[1]
    //     ...
    // or if there's a single child, we can directly jump to it:
    //   out for N: // if N.children.size() == 1
    //     N.insn[*]
    //     Jump out for N.children[0]
    // For our example, this would yield:
    //   out for root:
    //     Jump out for [A0.bb0[0], A1.bb0[0]]
    //   out for [A0.bb0[0], A1.bb0[0]]:
    //     Compare 'a'
    //     ForkJump out for A0.bb0[1]
    //     ForkJump out for A1.bb0[1]
    //   out for A0.bb0[1]:
    //     Compare 'b'
    //     Jump out for A0.bb0[2]
    //   out for A1.bb0[1]:
    //     Compare 'c'
    //     Jump out for A1.bb0[2]
    //   out for A0.bb0[2]:
    //     Compare 'd'
    //   out for A1.bb0[2]:
    //     Compare 'd'
    if (alternatives.size() == 0)
        return;

    if (alternatives.size() == 1)
        return target.extend(move(alternatives[0]));

    target.merge_string_tables_from(alternatives);
    if (all_of(alternatives, [](auto& x) { return x.is_empty(); }))
        return;

    for (auto& entry : alternatives)
        entry.flatten();

#if REGEX_DEBUG
    ScopeLogger<true> log;
    warnln("Alternations:");
    RegexDebug<ByteCode> dbg;
    for (auto& entry : alternatives) {
        warnln("----------");
        dbg.print_bytecode(entry);
    }
    ScopeGuard print_at_end {
        [&] {
            warnln("======================");
            RegexDebug<ByteCode> dbg;
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

    auto state = MatchState::only_for_enumeration();

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
            case OpCodeId::Jump: {
                auto const& cast_opcode = to<OpCode_Jump>(opcode);
                incoming_jump_edges.ensure(cast_opcode.offset() + cast_opcode.size() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= cast_opcode.offset() < 0;
                break;
            }
            case OpCodeId::JumpNonEmpty: {
                auto const& cast_opcode = to<OpCode_JumpNonEmpty>(opcode);
                incoming_jump_edges.ensure(cast_opcode.offset() + cast_opcode.size() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= cast_opcode.offset() < 0;
                break;
            }
            case OpCodeId::ForkJump: {
                auto const& cast_opcode = to<OpCode_ForkJump>(opcode);
                incoming_jump_edges.ensure(cast_opcode.offset() + cast_opcode.size() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= cast_opcode.offset() < 0;
                break;
            }
            case OpCodeId::ForkStay: {
                auto const& cast_opcode = to<OpCode_ForkStay>(opcode);
                incoming_jump_edges.ensure(cast_opcode.offset() + cast_opcode.size() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= cast_opcode.offset() < 0;
                break;
            }
            case OpCodeId::ForkReplaceJump: {
                auto const& cast_opcode = to<OpCode_ForkReplaceJump>(opcode);
                incoming_jump_edges.ensure(cast_opcode.offset() + cast_opcode.size() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= cast_opcode.offset() < 0;
                break;
            }
            case OpCodeId::ForkReplaceStay: {
                auto const& cast_opcode = to<OpCode_ForkReplaceStay>(opcode);
                incoming_jump_edges.ensure(cast_opcode.offset() + cast_opcode.size() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= cast_opcode.offset() < 0;
                break;
            }
            case OpCodeId::ForkIf: {
                auto const& cast_opcode = to<OpCode_ForkIf>(opcode);
                incoming_jump_edges.ensure(cast_opcode.offset() + cast_opcode.size() + state.instruction_position).append({ opcode_bytes });
                has_any_backwards_jump |= cast_opcode.offset() < 0;
                break;
            }
            case OpCodeId::Repeat: {
                auto const& cast_opcode = to<OpCode_Repeat>(opcode);
                incoming_jump_edges.ensure(state.instruction_position - cast_opcode.offset()).append({ opcode_bytes });
                has_any_backwards_jump = true;
                break;
            }
            default:
                break;
            }
            state.instruction_position += opcode.size();
        }
    }

    struct QualifiedIP {
        size_t alternative_index;
        size_t instruction_position;

        bool operator==(QualifiedIP const& other) const = default;
    };
    struct NodeMetadataEntry {
        QualifiedIP ip;
        NonnullOwnPtr<StaticallyInterpretedCompares> first_compare_from_here;
    };
    using Tree = Trie<DisjointSpans<ByteCodeValueType const>, Vector<NodeMetadataEntry>, Traits<DisjointSpans<ByteCodeValueType const>>, void, OrderedHashMapForTrie>;
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

            active_node = static_cast<decltype(active_node)>(MUST(active_node->ensure_child(DisjointSpans<ByteCodeValueType const> { move(node_key_bytes) }, [] -> Vector<NodeMetadataEntry> { return {}; })));

            auto next_compare = [&alternative, &state](StaticallyInterpretedCompares& compares) {
                TemporaryChange state_change { state.instruction_position, state.instruction_position };

                auto* opcode = &alternative.get_opcode(state);
                auto opcode_id = opcode->opcode_id();
                while (opcode_id == OpCodeId::Checkpoint || opcode_id == OpCodeId::SaveLeftCaptureGroup
                    || opcode_id == OpCodeId::SaveRightCaptureGroup || opcode_id == OpCodeId::SaveRightNamedCaptureGroup
                    || opcode_id == OpCodeId::Save) {
                    state.instruction_position += opcode->size();
                    opcode = &alternative.get_opcode(state);
                    opcode_id = opcode->opcode_id();
                }

                // We found something functional, if it's a compare, we need to care.
                if (opcode_id != OpCodeId::Compare)
                    return;

                auto flat_compares = to<OpCode_Compare>(*opcode).flat_compares();
                interpret_compares(flat_compares, compares);
            };

            auto node_metadata = NodeMetadataEntry { { i, state.instruction_position }, make<StaticallyInterpretedCompares>() };
            auto& metadata = active_node->metadata_value();
            if (metadata.is_empty())
                total_bytecode_entries_in_tree += opcode.size();
            else
                common_hits++;
            metadata.append(move(node_metadata));
            next_compare(*active_node->metadata_value().last().first_compare_from_here);

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
                    node.metadata_value().first().ip.instruction_position,
                    node.metadata_value().first().ip.alternative_index,
                    node.metadata_value().size(),
                    node.metadata_value().size() == 1 ? "" : "s");

                auto state = MatchState::only_for_enumeration();
                state.instruction_position = node.metadata_value().first().ip.instruction_position;
                auto& opcode = alternatives[node.metadata_value().first().ip.alternative_index].get_opcode(state);
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
    auto chain_cost = total_bytecode_entries_in_tree + alternatives.size() * 2;
    dbgln_if(REGEX_DEBUG, "Total nodes: {}, common hits: {} (tree cost = {}, chain cost = {})", total_nodes, common_hits, tree_cost, chain_cost);

    // Make sure we're not breaking the order requirements (a should be tried before b in a|b)
    Queue<Tree::DetailTrie*> nodes_to_visit;
    nodes_to_visit.enqueue(&trie);
    while (!nodes_to_visit.is_empty()) {
        auto& node = *nodes_to_visit.dequeue();
        auto& children = node.children();
        for (auto& entry : children)
            nodes_to_visit.enqueue(entry.value.ptr());
        // If the children are not sorted right, we've got a problem.
        if (children.size() <= 1)
            continue;

        size_t max_index = 0;
        NodeMetadataEntry const* child_with_max_index = nullptr;
        for (auto& entry : children) {
            auto& child = *entry.value;
            if (child.has_metadata()) {
                for (auto& child_entry : child.metadata_value()) {
                    if (max_index > child_entry.ip.alternative_index) {
                        // We have a problem, an alternative later in the list is being tried before an earlier one.
                        // we can't use this trie...unless the first compare in this child is not the same as the one in the entry with max-index
                        // then there's no overlap and the order doesn't matter anyhow.
                        if (!has_overlap(*child_with_max_index->first_compare_from_here, *child_entry.first_compare_from_here)) {
                            // We can use this trie after all.
                            continue;
                        }
                        tree_cost = NumericLimits<size_t>::max();
                        goto exit_useless_loop;
                    }
                    max_index = child_entry.ip.alternative_index;
                    child_with_max_index = &child_entry;
                }
            }
        }
        continue;
    exit_useless_loop:
        break;
    }

    if (common_hits == 0 || tree_cost > chain_cost) {
        dbgln_if(REGEX_DEBUG, "Choosing sequential alternation layout over trie-based layout");
        // It's better to lay these out as a normal sequence of instructions.
        // We can avoid trying alternatives that we know cannot match in certain cases:
        // - If the alternative starts with an assertion, we can lift the assertion to the fork op itself (currently only ^).
        Vector<ForkIfCondition> fork_conditions;
        fork_conditions.resize_with_default_value(alternatives.size(), ForkIfCondition::Invalid);

        for (size_t i = 0; i < alternatives.size(); ++i) {
            auto& alternative = alternatives[i];
            auto state = MatchState::only_for_enumeration();
            state.instruction_position = 0;
            auto& first_opcode = alternative.get_opcode(state);
            if (first_opcode.opcode_id() == OpCodeId::CheckBegin)
                fork_conditions[i] = ForkIfCondition::AtStartOfLine;
        }

        Vector<size_t> jump_op_positions;
        Vector<size_t> jump_sizes;
        jump_op_positions.resize(alternatives.size() - 1);
        jump_sizes.resize(alternatives.size() - 1);

        for (size_t i = 1; i < alternatives.size(); ++i) {
            jump_op_positions[i - 1] = target.size();
            if (fork_conditions[i - 1] != ForkIfCondition::Invalid) {
                jump_sizes[i - 1] = 4;
                target.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkIf));
                target.empend(0u); // To be filled later.
                target.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));
                target.empend(static_cast<ByteCodeValueType>(fork_conditions[i - 1]));
            } else {
                jump_sizes[i - 1] = 2;
                target.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));
                target.empend(0u); // To be filled later.
            }
        }

        bool seen_one_empty = false;

        Vector<size_t> jump_to_end_patch_positions;
        jump_to_end_patch_positions.resize_with_default_value(alternatives.size(), NumericLimits<size_t>::max());

        for (size_t i = alternatives.size(); i > 0; --i) {
            auto& chunk = alternatives[i - 1];
            if (chunk.is_empty()) {
                if (seen_one_empty)
                    continue;
                seen_one_empty = true;
            }

            if (i < alternatives.size()) {
                auto this_block_start = target.size();
                auto position = jump_op_positions[i - 1];
                auto jump_size = jump_sizes[i - 1];
                target[position + 1] = static_cast<ByteCodeValueType>(this_block_start - position - jump_size);
            }

            target.extend(move(chunk));
            target.empend(static_cast<ByteCodeValueType>(OpCodeId::Jump));
            target.empend(0u); // Jump to the _END label
            jump_to_end_patch_positions[i - 1] = target.size() - 1;
        }

        auto end_position = target.size();
        for (size_t i = 0; i < alternatives.size(); ++i) {
            if (auto& position = jump_to_end_patch_positions[i]; position != NumericLimits<size_t>::max())
                target[position] = static_cast<ByteCodeValueType>(end_position - (position + 1));
        }
    } else {
        dbgln_if(REGEX_DEBUG, "Choosing trie-based alternation layout");
        target.ensure_capacity(total_bytecode_entries_in_tree + common_hits * 6);

        auto node_is = [](Tree const* node, QualifiedIP ip) {
            return node->metadata_value().span().first_matching([&](auto& entry) { return entry.ip == ip; }).has_value();
        };

        struct Patch {
            QualifiedIP source_ip;
            size_t target_ip;
            size_t size_delta { 0 };
            bool done { false };
        };
        Vector<Patch> patch_locations;
        patch_locations.ensure_capacity(total_nodes);

        HashMap<size_t, NonnullOwnPtr<RedBlackTree<u64, u64>>> instruction_positions;
        if (has_any_backwards_jump)
            MUST(instruction_positions.try_ensure_capacity(alternatives.size()));

        auto ip_mapping_for_alternative = [&](size_t i) -> RedBlackTree<u64, u64>& {
            return *instruction_positions.ensure(i, [] {
                return make<RedBlackTree<u64, u64>>();
            });
        };

        auto add_patch_point = [&](Tree const* node, size_t target_ip) {
            if (!node->has_metadata())
                return;

            patch_locations.append({ node->metadata_value().first().ip, target_ip });
        };

        Vector<Tree*> nodes_to_visit;
        nodes_to_visit.append(&trie);

        // each node:
        //   node.re
        //   forkjump child1
        //   forkjump child2
        //   ...
        while (!nodes_to_visit.is_empty()) {
            auto const* node = nodes_to_visit.take_last();
            for (auto& patch : patch_locations) {
                if (!patch.done && node_is(node, patch.source_ip)) {
                    auto value = static_cast<ByteCodeValueType>(target.size() - patch.target_ip - 1 - patch.size_delta);
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
                    for (auto& entry : node->metadata_value())
                        ip_mapping_for_alternative(entry.ip.alternative_index).insert(entry.ip.instruction_position, state.instruction_position);
                }

                auto& opcode = target.get_opcode(state);

                ssize_t jump_offset;
                auto is_jump = true;
                auto patch_location = state.instruction_position + 1;
                bool should_negate = false;
                size_t size_delta = opcode.size() - 2;

                switch (opcode.opcode_id()) {
                case OpCodeId::Jump:
                    jump_offset = to<OpCode_Jump>(opcode).offset();
                    break;
                case OpCodeId::JumpNonEmpty:
                    jump_offset = to<OpCode_JumpNonEmpty>(opcode).offset();
                    break;
                case OpCodeId::ForkJump:
                    jump_offset = to<OpCode_ForkJump>(opcode).offset();
                    break;
                case OpCodeId::ForkStay:
                    jump_offset = to<OpCode_ForkStay>(opcode).offset();
                    break;
                case OpCodeId::ForkReplaceJump:
                    jump_offset = to<OpCode_ForkReplaceJump>(opcode).offset();
                    break;
                case OpCodeId::ForkReplaceStay:
                    jump_offset = to<OpCode_ForkReplaceStay>(opcode).offset();
                    break;
                case OpCodeId::ForkIf:
                    jump_offset = to<OpCode_ForkIf>(opcode).offset();
                    break;
                case OpCodeId::Repeat:
                    jump_offset = static_cast<ssize_t>(0) - static_cast<ssize_t>(to<OpCode_Repeat>(opcode).offset()) - static_cast<ssize_t>(opcode.size());
                    should_negate = true;
                    break;
                default:
                    is_jump = false;
                    break;
                }

                if (is_jump) {
                    VERIFY(node->has_metadata());
                    if (node->metadata_value().size() > 1)
                        target[patch_location] = static_cast<ByteCodeValueType>(0); // Fall through instead.

                    auto only_one = node->metadata_value().size() == 1;
                    auto patch_size = opcode.size() - 1;
                    for (auto& entry : node->metadata_value()) {
                        auto& [alternative_index, instruction_position] = entry.ip;
                        if (!only_one) {
                            target.append(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));
                            patch_location = target.size();
                            should_negate = false;
                            patch_size = 1;
                            target.append(static_cast<ByteCodeValueType>(0));
                        }

                        auto intended_jump_ip = instruction_position + jump_offset + opcode.size();
                        if (jump_offset < 0) {
                            VERIFY(has_any_backwards_jump);
                            // We should've already seen this instruction, so we can just patch it in.
                            auto& ip_mapping = ip_mapping_for_alternative(alternative_index);
                            auto target_ip = ip_mapping.find(intended_jump_ip);
                            if (!target_ip) {
                                RegexDebug<ByteCode> dbg;
                                size_t x = 0;
                                for (auto& entry : alternatives) {
                                    warnln("----------- {} ----------", x++);
                                    dbg.print_bytecode(entry);
                                }

                                dbgln("Regex Tree / Unknown backwards jump: {}@{} -> {}",
                                    instruction_position,
                                    alternative_index,
                                    intended_jump_ip);
                                VERIFY_NOT_REACHED();
                            }
                            ssize_t target_value = *target_ip - patch_location - patch_size;
                            if (should_negate)
                                target_value = -target_value - opcode.size();
                            target[patch_location] = static_cast<ByteCodeValueType>(target_value);
                        } else {
                            patch_locations.append({ QualifiedIP { alternative_index, intended_jump_ip }, patch_location, size_delta });
                        }
                    }
                }
            }

            for (auto const& child : node->children()) {
                auto* child_node = static_cast<Tree*>(child.value.ptr());
                target.append(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));
                add_patch_point(child_node, target.size());
                target.append(static_cast<ByteCodeValueType>(0));
                nodes_to_visit.append(child_node);
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
    case CharacterCompareType::Subtract:
        return LookupTableInsertionOutcome::FlushOnInsertion;
    case CharacterCompareType::Reference:
    case CharacterCompareType::NamedReference:
    case CharacterCompareType::Property:
    case CharacterCompareType::GeneralCategory:
    case CharacterCompareType::Script:
    case CharacterCompareType::ScriptExtension:
    case CharacterCompareType::StringSet:
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
                && pair.type != CharacterCompareType::Subtract
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
            auto merge_overlapping_ranges = [](auto& source) {
                Optional<CharRange> active_range;
                Vector<ByteCodeValueType> result;
                for (auto& range : source) {
                    if (!active_range.has_value()) {
                        active_range = CharRange(range);
                        continue;
                    }
                    CharRange char_range(range);
                    if (char_range.from <= active_range->to + 1 && char_range.to + 1 >= active_range->from) {
                        active_range = CharRange { min(char_range.from, active_range->from), max(char_range.to, active_range->to) };
                    } else {
                        result.append(active_range.release_value());
                        active_range = char_range;
                    }
                }
                if (active_range.has_value())
                    result.append(active_range.release_value());
                return result;
            };

            auto append_table = [&](auto& table) {
                ++argument_count;
                arguments.append(to_underlying(CharacterCompareType::LookupTable));
                auto sensitive_size_index = arguments.size();
                auto insensitive_size_index = sensitive_size_index + 1;
                arguments.append(0);
                arguments.append(0);

                auto range_data = merge_overlapping_ranges(table);
                arguments.extend(range_data);
                arguments[sensitive_size_index] = range_data.size();

                Vector<ByteCodeValueType> insensitive_data;
                for (CharRange range : range_data) {
                    for (auto expanded : Unicode::expand_range_case_insensitive(range.from, range.to))
                        insensitive_data.append(CharRange { expanded.from, expanded.to });
                }
                quick_sort(insensitive_data, [](CharRange a, CharRange b) { return a.from < b.from; });

                auto merged_data = merge_overlapping_ranges(insensitive_data);
                arguments.extend(merged_data);
                arguments[insensitive_size_index] = merged_data.size();
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
                    && value.type != CharacterCompareType::Subtract
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
