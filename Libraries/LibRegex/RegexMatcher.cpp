/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/BumpAllocator.h>
#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/StringBuilder.h>
#include <LibRegex/RegexMatcher.h>
#include <LibRegex/RegexParser.h>
#include <LibUnicode/CharacterTypes.h>

#if REGEX_DEBUG
#    include <LibRegex/RegexDebug.h>
#endif

namespace regex {

#if REGEX_DEBUG
static RegexDebug<ByteCode> s_regex_dbg(stderr);
#endif

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
    struct BytecodeSizeVisitor {
        size_t operator()(ByteCode const& bc) const { return bc.size() * sizeof(ByteCodeValueType); }
        size_t operator()(FlatByteCode const& bc) const { return bc.size(); }
    };
    auto bytecode_size = result.bytecode.visit(BytecodeSizeVisitor {});
    if (bytecode_size > MaxRegexCachedBytecodeSize)
        return;

    while (bytecode_size + s_cached_bytecode_size<Parser> > MaxRegexCachedBytecodeSize)
        s_cached_bytecode_size<Parser> -= s_parser_cache<Parser>.take_first().bytecode.visit(BytecodeSizeVisitor {});

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

struct SufficientlyUniformValueTraits : DefaultTraits<u64> {
    static constexpr unsigned hash(u64 value)
    {
        return (value >> 32) ^ value;
    }
};

// U+2028 LINE SEPARATOR
constexpr static u32 const LineSeparator { 0x2028 };
// U+2029 PARAGRAPH SEPARATOR
constexpr static u32 const ParagraphSeparator { 0x2029 };

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

static void reverse_string_position(MatchState& state, RegexStringView view, size_t amount)
{
    VERIFY(state.string_position >= amount);
    state.string_position -= amount;
    if (view.unicode())
        state.string_position_in_code_units = view.code_unit_offset_of(state.string_position);
    else
        state.string_position_in_code_units -= amount;
}

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

    FlatByteCode const& bc = m_pattern->parser_result.bytecode.template get<FlatByteCode>();
    auto const* bytecode = bc.data();
    auto const bytecode_size = bc.size();

    BumpAllocatedLinkedList<MatchState> fork_stack;
    HashTable<u64, SufficientlyUniformValueTraits> seen_state_hashes;

    if (state.repetition_marks.size() < bc.repetition_count())
        state.repetition_marks.resize(bc.repetition_count());
    if (state.checkpoints.size() < bc.checkpoint_count())
        state.checkpoints.resize(bc.checkpoint_count());

    auto do_backtrack = [&](ExecuteResult no_match_result) -> Optional<ExecuteResult> {
        while (!fork_stack.is_empty()) {
            state = fork_stack.take_last();
            if (auto hash = state.u64_hash(); seen_state_hashes.set(hash) != HashSetResult::InsertedNewEntry)
                continue;
            return {};
        }
        return no_match_result;
    };

    auto handle_fork = [&](bool is_replace, bool is_prio_low, size_t insn_size) {
        auto fork_ip = state.instruction_position;
        auto continue_ip = state.instruction_position + insn_size;
        auto fork_target = state.fork_at_position;
        auto resume_ip = is_prio_low ? fork_target : continue_ip;

        bool found = false;
        if (is_replace && input.fork_to_replace.has_value()) {
            for (auto it = fork_stack.reverse_begin(); it != fork_stack.reverse_end(); ++it) {
                if (it->initiating_fork.has_value() && *it->initiating_fork == *input.fork_to_replace) {
                    (*it) = state;
                    it->instruction_position = resume_ip;
                    it->initiating_fork = *input.fork_to_replace;
                    found = true;
                    break;
                }
            }
            input.fork_to_replace.clear();
        }

        if (!found) {
            fork_stack.append(state);
            fork_stack.last().instruction_position = resume_ip;
            fork_stack.last().initiating_fork = Optional<size_t> { fork_ip };
        }

        if (is_prio_low) {
            state.instruction_position = continue_ip;
            state.string_position_before_rseek = NumericLimits<size_t>::max();
            state.string_position_in_code_units_before_rseek = NumericLimits<size_t>::max();
        } else {
            state.instruction_position = fork_target;
        }
    };

    static void* const dispatch_table[] = {
#define __ENUMERATE_OPCODE(name) &&handle_##name,
        ENUMERATE_OPCODES
#undef __ENUMERATE_OPCODE
    };

    for (;;) {
    dispatch:
        if (state.instruction_position >= bytecode_size) [[unlikely]]
            return ExecuteResult::Matched;

        auto& insn = *reinterpret_cast<RegexInstruction const*>(bytecode + state.instruction_position);
        ++operations;

        if (input.fail_counter > 0) [[unlikely]] {
            --input.fail_counter;
            goto do_backtrack_low_prio;
        }

        goto* dispatch_table[static_cast<size_t>(insn.m_type)];

    handle_Compare: {
        auto& op = bc.instruction_at<Op_Compare>(state.instruction_position);
        switch (execute_compare<false>(input, state, op.m_arg_count, op.compare_data(), op.m_compare_size, bc)) {
        case ExecutionResult::Continue:
            state.instruction_position += op.total_size();
            goto dispatch;
        case ExecutionResult::Failed_ExecuteLowPrioForks:
            goto do_backtrack_low_prio;
        default:
            goto do_backtrack_fail;
        }
    }

    handle_CompareSimple: {
        auto& op = bc.instruction_at<Op_Compare>(state.instruction_position);
        auto const* data = op.compare_data();
        // If it's a single chat, just do it inline.
        if (static_cast<CharacterCompareType>(data[0]) == CharacterCompareType::Char) [[likely]] {
            if (state.string_position >= input.view.length()) [[unlikely]]
                goto do_backtrack_low_prio;
            auto expected = data[1];
            auto actual = input.view.unicode_aware_code_point_at(state.string_position_in_code_units);
            if (state.current_options & AllFlags::Insensitive) [[unlikely]] {
                if (Unicode::canonicalize(actual, input.view.unicode()) != Unicode::canonicalize(expected, input.view.unicode()))
                    goto do_backtrack_low_prio;
            } else {
                if (actual != expected)
                    goto do_backtrack_low_prio;
            }
            ++state.string_position;
            if (input.view.unicode())
                state.string_position_in_code_units += input.view.length_of_code_point(actual);
            else
                ++state.string_position_in_code_units;
            state.string_position_before_match = state.string_position - 1;
            state.instruction_position += op.total_size();
            goto dispatch;
        }
        // It's not just a char, so do the full (simple) compare.
        switch (execute_compare<true>(input, state, 1, data, op.m_compare_size, bc)) {
        case ExecutionResult::Continue:
            state.instruction_position += op.total_size();
            goto dispatch;
        case ExecutionResult::Failed_ExecuteLowPrioForks:
            goto do_backtrack_low_prio;
        default:
            goto do_backtrack_fail;
        }
    }

    handle_Jump: {
        auto& op = bc.instruction_at<Op_Jump>(state.instruction_position);
        state.instruction_position = op.m_target;
        goto dispatch;
    }

    handle_ForkJump: {
        auto& op = bc.instruction_at<Op_Jump>(state.instruction_position);
        state.fork_at_position = op.m_target;
        state.forks_since_last_save++;
        handle_fork(false, false, sizeof(Op_Jump));
        goto dispatch;
    }

    handle_ForkStay: {
        auto& op = bc.instruction_at<Op_Jump>(state.instruction_position);
        state.fork_at_position = op.m_target;
        state.forks_since_last_save++;
        handle_fork(false, true, sizeof(Op_Jump));
        goto dispatch;
    }

    handle_ForkReplaceJump: {
        auto& op = bc.instruction_at<Op_Jump>(state.instruction_position);
        state.fork_at_position = op.m_target;
        input.fork_to_replace = state.instruction_position;
        state.forks_since_last_save++;
        handle_fork(true, false, sizeof(Op_Jump));
        goto dispatch;
    }

    handle_ForkReplaceStay: {
        auto& op = bc.instruction_at<Op_Jump>(state.instruction_position);
        state.fork_at_position = op.m_target;
        input.fork_to_replace = state.instruction_position;
        handle_fork(true, true, sizeof(Op_Jump));
        goto dispatch;
    }

    handle_JumpNonEmpty: {
        auto& op = bc.instruction_at<Op_JumpNonEmpty>(state.instruction_position);
        u64 current_position = state.string_position;
        auto cp = op.m_checkpoint_id;
        auto checkpoint_position = cp < state.checkpoints.size() ? state.checkpoints.at(cp) : static_cast<u64>(0);

        if (checkpoint_position != 0 && checkpoint_position != current_position + 1) {
            auto form = static_cast<OpCodeId>(op.m_form);
            if (form == OpCodeId::Jump) {
                state.instruction_position = op.m_target;
                goto dispatch;
            }
            state.fork_at_position = op.m_target;
            auto is_replace = form == OpCodeId::ForkReplaceStay || form == OpCodeId::ForkReplaceJump;
            auto is_prio_low = form == OpCodeId::ForkStay || form == OpCodeId::ForkReplaceStay;
            if (is_replace)
                input.fork_to_replace = state.instruction_position;
            else
                state.forks_since_last_save++;
            handle_fork(is_replace, is_prio_low, sizeof(Op_JumpNonEmpty));
            goto dispatch;
        }

        if (static_cast<OpCodeId>(op.m_form) == OpCodeId::Jump && state.string_position < input.view.length())
            goto do_backtrack_low_prio;
        state.instruction_position += sizeof(Op_JumpNonEmpty);
        goto dispatch;
    }

    handle_ForkIf: {
        auto& op = bc.instruction_at<Op_ForkIf>(state.instruction_position);
        auto form = static_cast<OpCodeId>(op.m_form);
        auto condition = static_cast<ForkIfCondition>(op.m_condition);

        auto do_fork = false;
        switch (condition) {
        case ForkIfCondition::AtStartOfLine:
            do_fork = !input.in_the_middle_of_a_line;
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        if (do_fork) {
            state.fork_at_position = op.m_target;
            state.forks_since_last_save++;
            bool is_replace = (form == OpCodeId::ForkReplaceJump || form == OpCodeId::ForkReplaceStay);
            if (is_replace)
                input.fork_to_replace = state.instruction_position;
            bool is_prio_low = (form == OpCodeId::ForkStay || form == OpCodeId::ForkReplaceStay);
            handle_fork(is_replace, is_prio_low, sizeof(Op_ForkIf));
            goto dispatch;
        }

        // Not forking: for Stay forms, jump to target; for Jump forms, continue.
        if (form == OpCodeId::ForkStay || form == OpCodeId::ForkReplaceStay) {
            state.instruction_position = op.m_target;
            goto dispatch;
        }
        state.instruction_position += sizeof(Op_ForkIf);
        goto dispatch;
    }

    handle_FailForks: {
        input.fail_counter += state.forks_since_last_save;
        goto do_backtrack_low_prio;
    }

    handle_PopSaved: {
        if (input.saved_positions.is_empty() || input.saved_code_unit_positions.is_empty())
            goto do_backtrack_low_prio;
        input.saved_positions.take_last();
        input.saved_code_unit_positions.take_last();
        goto do_backtrack_low_prio;
    }

    handle_SaveLeftCaptureGroup: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        auto id = op.m_arg0;
        if (input.match_index >= state.capture_group_matches_size()) {
            state.flat_capture_group_matches.ensure_capacity((input.match_index + 1) * state.capture_group_count);
            for (size_t i = state.capture_group_matches_size(); i <= input.match_index; ++i)
                for (size_t j = 0; j < state.capture_group_count; ++j)
                    state.flat_capture_group_matches.append({});
        }
        state.mutable_capture_group_matches(input.match_index).at(id - 1).left_column = state.string_position;
        state.instruction_position += sizeof(Op_WithArg);
        goto dispatch;
    }

    handle_SaveRightCaptureGroup: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        auto id = op.m_arg0;
        auto& match = state.capture_group_matches(input.match_index).at(id - 1);
        auto start_position = match.left_column;
        if (state.string_position < start_position)
            goto do_backtrack_low_prio;
        auto length = state.string_position - start_position;
        if (start_position < match.column && state.step_backs.is_empty()) {
            state.instruction_position += sizeof(Op_WithArg);
            goto dispatch;
        }
        VERIFY(start_position + length <= input.view.length_in_code_units());
        auto captured_text = input.view.substring_view(start_position, length);
        auto& existing_capture = state.mutable_capture_group_matches(input.match_index).at(id - 1);
        if (length == 0 && !existing_capture.view.is_null() && existing_capture.view.length() > 0) {
            auto existing_end_position = existing_capture.global_offset - input.global_offset + existing_capture.view.length();
            if (existing_end_position == state.string_position) {
                state.instruction_position += sizeof(Op_WithArg);
                goto dispatch;
            }
        }
        state.mutable_capture_group_matches(input.match_index).at(id - 1) = { captured_text, input.line, start_position, input.global_offset + start_position };
        state.instruction_position += sizeof(Op_WithArg);
        goto dispatch;
    }

    handle_SaveRightNamedCaptureGroup: {
        auto& op = bc.instruction_at<Op_SaveRightNamedCapture>(state.instruction_position);
        auto id = op.m_group_id;
        auto name_index = op.m_name_index;
        auto& match = state.capture_group_matches(input.match_index).at(id - 1);
        auto start_position = match.left_column;
        if (state.string_position < start_position)
            goto do_backtrack_low_prio;
        auto length = state.string_position - start_position;
        if (start_position < match.column) {
            state.instruction_position += sizeof(Op_SaveRightNamedCapture);
            goto dispatch;
        }
        VERIFY(start_position + length <= input.view.length_in_code_units());
        auto view = input.view.substring_view(start_position, length);
        auto& existing_capture = state.mutable_capture_group_matches(input.match_index).at(id - 1);
        if (length == 0 && !existing_capture.view.is_null() && existing_capture.view.length() > 0) {
            auto existing_end_position = existing_capture.global_offset - input.global_offset + existing_capture.view.length();
            if (existing_end_position == state.string_position) {
                state.instruction_position += sizeof(Op_SaveRightNamedCapture);
                goto dispatch;
            }
        }
        state.mutable_capture_group_matches(input.match_index).at(id - 1) = { view, name_index, input.line, start_position, input.global_offset + start_position };
        state.instruction_position += sizeof(Op_SaveRightNamedCapture);
        goto dispatch;
    }

    handle_RSeekTo: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        auto ch = op.m_arg0;

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
                goto do_backtrack_low_prio;
            goto do_backtrack_no_further;
        }
        state.string_position = next->code_point_index;
        state.string_position_in_code_units = next->code_unit_index;
        state.instruction_position += sizeof(Op_WithArg);
        goto dispatch;
    }

    handle_CheckBegin: {
        auto is_at_line_boundary = [&] {
            if (state.string_position == 0)
                return true;
            if (state.current_options.has_flag_set(AllFlags::Multiline) && state.current_options.has_flag_set(AllFlags::Internal_ConsiderNewline)) {
                auto ch = input.view.substring_view(state.string_position - 1, 1).code_point_at(0);
                return ch == '\r' || ch == '\n' || ch == LineSeparator || ch == ParagraphSeparator;
            }
            return false;
        }();
        if (is_at_line_boundary && (state.current_options & AllFlags::MatchNotBeginOfLine))
            goto do_backtrack_low_prio;
        if ((is_at_line_boundary && !(state.current_options & AllFlags::MatchNotBeginOfLine))
            || (!is_at_line_boundary && (state.current_options & AllFlags::MatchNotBeginOfLine))
            || (is_at_line_boundary && (state.current_options & AllFlags::Global))) {
            state.instruction_position += sizeof(RegexInstruction);
            goto dispatch;
        }
        goto do_backtrack_low_prio;
    }

    handle_CheckEnd: {
        auto is_at_line_boundary = [&] {
            if (state.string_position == input.view.length())
                return true;
            if (state.current_options.has_flag_set(AllFlags::Multiline) && state.current_options.has_flag_set(AllFlags::Internal_ConsiderNewline)) {
                auto ch = input.view.substring_view(state.string_position, 1).code_point_at(0);
                return ch == '\r' || ch == '\n' || ch == LineSeparator || ch == ParagraphSeparator;
            }
            return false;
        }();
        if (is_at_line_boundary && (state.current_options & AllFlags::MatchNotEndOfLine))
            goto do_backtrack_low_prio;
        if ((is_at_line_boundary && !(state.current_options & AllFlags::MatchNotEndOfLine))
            || (!is_at_line_boundary && (state.current_options & AllFlags::MatchNotEndOfLine || state.current_options & AllFlags::MatchNotBeginOfLine))) {
            state.instruction_position += sizeof(RegexInstruction);
            goto dispatch;
        }
        goto do_backtrack_low_prio;
    }

    handle_CheckBoundary: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        auto type = static_cast<BoundaryCheckType>(op.m_arg0);
        bool const case_insensitive = state.current_options & AllFlags::Insensitive;
        bool const unicode = input.view.unicode();
        auto isword = [case_insensitive, unicode](u32 ch) {
            if (is_ascii_alphanumeric(ch) || ch == '_')
                return true;
            if (case_insensitive && unicode) {
                auto canonical = Unicode::canonicalize(ch, unicode);
                if (is_ascii_alphanumeric(canonical) || canonical == '_')
                    return true;
            }
            return false;
        };
        auto is_word_boundary = [&] {
            if (state.string_position == input.view.length())
                return state.string_position > 0 && isword(input.view.code_point_at(state.string_position_in_code_units - 1));
            if (state.string_position == 0)
                return isword(input.view.code_point_at(0));
            return !!(isword(input.view.code_point_at(state.string_position_in_code_units)) ^ isword(input.view.code_point_at(state.string_position_in_code_units - 1)));
        };
        bool boundary = is_word_boundary();
        if ((type == BoundaryCheckType::Word && boundary) || (type == BoundaryCheckType::NonWord && !boundary)) {
            state.instruction_position += sizeof(Op_WithArg);
            goto dispatch;
        }
        goto do_backtrack_low_prio;
    }

    handle_Save: {
        save_string_position(input, state);
        state.forks_since_last_save = 0;
        state.instruction_position += sizeof(RegexInstruction);
        goto dispatch;
    }

    handle_Restore: {
        if (!restore_string_position(input, state))
            goto do_backtrack_fail;
        state.instruction_position += sizeof(RegexInstruction);
        goto dispatch;
    }

    handle_GoBack: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        if (op.m_arg0 > state.string_position)
            goto do_backtrack_low_prio;
        reverse_string_position(state, input.view, op.m_arg0);
        state.instruction_position += sizeof(Op_WithArg);
        goto dispatch;
    }

    handle_SetStepBack: {
        auto& op = bc.instruction_at<Op_SetStepBack>(state.instruction_position);
        state.step_backs.append(op.m_step);
        state.instruction_position += sizeof(Op_SetStepBack);
        goto dispatch;
    }

    handle_IncStepBack: {
        if (state.step_backs.is_empty())
            goto do_backtrack_low_prio;
        size_t last_step_back = static_cast<size_t>(++state.step_backs[state.step_backs.size() - 1]);
        if (last_step_back > state.string_position)
            goto do_backtrack_low_prio;
        reverse_string_position(state, input.view, last_step_back);
        state.instruction_position += sizeof(RegexInstruction);
        goto dispatch;
    }

    handle_CheckStepBack: {
        if (state.step_backs.is_empty())
            goto do_backtrack_low_prio;
        if (input.saved_positions.is_empty())
            goto do_backtrack_low_prio;
        if (static_cast<size_t>(state.step_backs.last()) > input.saved_positions.last())
            goto do_backtrack_low_prio;
        state.string_position = input.saved_positions.last();
        state.string_position_in_code_units = input.saved_code_unit_positions.last();
        state.instruction_position += sizeof(RegexInstruction);
        goto dispatch;
    }

    handle_CheckSavedPosition: {
        if (input.saved_positions.is_empty())
            goto do_backtrack_low_prio;
        if (state.string_position != input.saved_positions.last())
            goto do_backtrack_low_prio;
        state.step_backs.take_last();
        state.instruction_position += sizeof(RegexInstruction);
        goto dispatch;
    }

    handle_ClearCaptureGroup: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        if (input.match_index < state.capture_group_matches_size()) {
            auto group = state.mutable_capture_group_matches(input.match_index);
            group[op.m_arg0 - 1].reset();
        }
        state.instruction_position += sizeof(Op_WithArg);
        goto dispatch;
    }

    handle_FailIfEmpty: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        u64 current_position = state.string_position + 1;
        auto cp = op.m_arg0;
        auto checkpoint_position = cp < state.checkpoints.size() ? state.checkpoints.at(cp) : current_position;
        if (checkpoint_position == current_position)
            goto do_backtrack_low_prio;
        state.instruction_position += sizeof(Op_WithArg);
        goto dispatch;
    }

    handle_Repeat: {
        auto& op = bc.instruction_at<Op_Repeat>(state.instruction_position);
        VERIFY(op.m_count > 0);
        if (op.m_id >= state.repetition_marks.size())
            state.repetition_marks.resize(op.m_id + 1);
        auto& rep = state.repetition_marks[op.m_id];
        if (rep == op.m_count - 1) {
            rep = 0;
            state.instruction_position += sizeof(Op_Repeat);
            goto dispatch;
        }
        state.instruction_position = op.m_target;
        ++rep;
        goto dispatch;
    }

    handle_ResetRepeat: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        auto id = op.m_arg0;
        if (id >= state.repetition_marks.size())
            state.repetition_marks.resize(id + 1);
        state.repetition_marks[id] = 0;
        state.instruction_position += sizeof(Op_WithArg);
        goto dispatch;
    }

    handle_Checkpoint: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        auto id = op.m_arg0;
        if (id >= state.checkpoints.size())
            state.checkpoints.resize(id + 1);
        state.checkpoints[id] = state.string_position + 1;
        state.instruction_position += sizeof(Op_WithArg);
        goto dispatch;
    }

    handle_SaveModifiers: {
        auto& op = bc.instruction_at<Op_WithArg>(state.instruction_position);
        auto current_flags = to_underlying(state.current_options.value());
        state.modifier_stack.append(current_flags);
        state.current_options = AllOptions { static_cast<AllFlags>(op.m_arg0) };
        state.instruction_position += sizeof(Op_WithArg);
        goto dispatch;
    }

    handle_RestoreModifiers: {
        if (state.modifier_stack.is_empty())
            goto do_backtrack_fail;
        auto previous_modifiers = state.modifier_stack.take_last();
        state.current_options = AllOptions { static_cast<AllFlags>(previous_modifiers) };
        state.instruction_position += sizeof(RegexInstruction);
        goto dispatch;
    }

    handle_Exit:
        return ExecuteResult::Matched;

    do_backtrack_fail: {
        if (auto r = do_backtrack(ExecuteResult::DidNotMatch); r.has_value())
            return *r;
        goto dispatch;
    }

    do_backtrack_low_prio: {
        if (auto r = do_backtrack(ExecuteResult::DidNotMatch); r.has_value())
            return *r;
        goto dispatch;
    }

    do_backtrack_no_further: {
        if (auto r = do_backtrack(ExecuteResult::DidNotMatchAndNoFurtherPossibleMatchesInView); r.has_value())
            return *r;
        goto dispatch;
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
        return pair_int_hash(key.pattern.hash(), int_hash(to_underlying(key.options.value())));
    }
};
