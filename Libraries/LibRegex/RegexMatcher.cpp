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

namespace regex {

#if REGEX_DEBUG
static RegexDebug<FlatByteCode> s_regex_dbg(stderr);
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
#if REGEX_DEBUG
    size_t recursion_level = 0;
#endif

    auto& bytecode = m_pattern->parser_result.bytecode.template get<FlatByteCode>();

    for (;;) {
        auto& opcode = bytecode.get_opcode(state);
        auto const opcode_size = opcode.size();
        ++operations;

#if REGEX_DEBUG
        s_regex_dbg.print_opcode("VM", opcode, state, recursion_level, false);
#endif

        ExecutionResult result;
        if (input.fail_counter > 0) {
            --input.fail_counter;
            result = ExecutionResult::Failed_ExecuteLowPrioForks;
        } else {
            result = opcode.execute(input, state);
        }

#if REGEX_DEBUG
        s_regex_dbg.print_result(opcode, bytecode, input, state, result);
#endif

        state.instruction_position += opcode_size;

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
                states_to_try_next.last().initiating_fork = state.instruction_position - opcode_size;
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
                states_to_try_next.last().initiating_fork = state.instruction_position - opcode_size;
                states_to_try_next.last().string_position_before_rseek = NumericLimits<size_t>::max();
                states_to_try_next.last().string_position_in_code_units_before_rseek = NumericLimits<size_t>::max();
            }
            state.instruction_position = state.fork_at_position;
#if REGEX_DEBUG
            ++recursion_level;
#endif
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
#if REGEX_DEBUG
            ++recursion_level;
#endif
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
#if REGEX_DEBUG
            ++recursion_level;
#endif
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
