/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BumpAllocator.h>
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/StringBuilder.h>
#include <LibRegex/RegexMatcher.h>
#include <LibRegex/RegexParser.h>

#if REGEX_DEBUG
#    include <LibRegex/RegexDebug.h>
#endif

namespace regex {

#if REGEX_DEBUG
static RegexDebug s_regex_dbg(stderr);
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
Regex<Parser>::Regex(ByteString pattern, typename ParserTraits<Parser>::OptionsType regex_options)
    : pattern_value(move(pattern))
{
    if (auto cache_entry = s_parser_cache<Parser>.get({ pattern_value, regex_options }); cache_entry.has_value()) {
        parser_result = cache_entry.value();
    } else {
        regex::Lexer lexer(pattern_value);

        Parser parser(lexer, regex_options);
        parser_result = parser.parse();

        run_optimization_passes();
        if (parser_result.error == regex::Error::NoError) {
            while (parser_result.bytecode.size() * sizeof(ByteCodeValueType) + s_cached_bytecode_size<Parser> > MaxRegexCachedBytecodeSize)
                s_cached_bytecode_size<Parser> -= s_parser_cache<Parser>.take_first().bytecode.size() * sizeof(ByteCodeValueType);
            s_parser_cache<Parser>.set({ pattern_value, regex_options }, parser_result);
            s_cached_bytecode_size<Parser> += parser_result.bytecode.size() * sizeof(ByteCodeValueType);
        }
    }

    if (parser_result.error == regex::Error::NoError)
        matcher = make<Matcher<Parser>>(this, static_cast<decltype(regex_options.value())>(parser_result.options.value()));
}

template<class Parser>
Regex<Parser>::Regex(regex::Parser::Result parse_result, ByteString pattern, typename ParserTraits<Parser>::OptionsType regex_options)
    : pattern_value(move(pattern))
    , parser_result(move(parse_result))
{
    run_optimization_passes();
    if (parser_result.error == regex::Error::NoError)
        matcher = make<Matcher<Parser>>(this, regex_options | static_cast<decltype(regex_options.value())>(parser_result.options.value()));
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
    MatchState state;
    size_t operations = 0;

    input.regex_options = m_regex_options | regex_options.value_or({}).value();
    input.start_offset = m_pattern->start_offset;
    size_t lines_to_skip = 0;

    bool unicode = input.regex_options.has_flag_set(AllFlags::Unicode) || input.regex_options.has_flag_set(AllFlags::UnicodeSets);
    for (auto const& view : views)
        const_cast<RegexStringView&>(view).set_unicode(unicode);

    if (input.regex_options.has_flag_set(AllFlags::Internal_Stateful)) {
        if (views.size() > 1 && input.start_offset > views.first().length()) {
            dbgln_if(REGEX_DEBUG, "Started with start={}, goff={}, skip={}", input.start_offset, input.global_offset, lines_to_skip);
            for (auto const& view : views) {
                if (input.start_offset < view.length() + 1)
                    break;
                ++lines_to_skip;
                input.start_offset -= view.length() + 1;
                input.global_offset += view.length() + 1;
            }
            dbgln_if(REGEX_DEBUG, "Ended with start={}, goff={}, skip={}", input.start_offset, input.global_offset, lines_to_skip);
        }
    }

    if (c_match_preallocation_count) {
        state.matches.ensure_capacity(c_match_preallocation_count);
        state.capture_group_matches.ensure_capacity(c_match_preallocation_count);
        auto& capture_groups_count = m_pattern->parser_result.capture_groups_count;

        for (size_t j = 0; j < c_match_preallocation_count; ++j) {
            state.matches.empend();
            state.capture_group_matches.empend();
            state.capture_group_matches.mutable_at(j).ensure_capacity(capture_groups_count);
            for (size_t k = 0; k < capture_groups_count; ++k)
                state.capture_group_matches.mutable_at(j).unchecked_append({});
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

    for (auto const& view : views) {
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
        state.string_position_in_code_units = view_index;
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

            auto success = execute(input, state, temp_operations);
            // This success is acceptable only if it doesn't read anything from the input (input length is 0).
            if (success && (state.string_position <= view_index)) {
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

        for (; view_index <= view_length; ++view_index) {
            if (view_index == view_length && input.regex_options.has_flag_set(AllFlags::Multiline))
                break;

            auto& match_length_minimum = m_pattern->parser_result.match_length_minimum;
            // FIXME: More performant would be to know the remaining minimum string
            //        length needed to match from the current position onwards within
            //        the vm. Add new OpCode for MinMatchLengthFromSp with the value of
            //        the remaining string length from the current path. The value though
            //        has to be filled in reverse. That implies a second run over bytecode
            //        after generation has finished.
            if (match_length_minimum && match_length_minimum > view_length - view_index)
                break;

            input.column = match_count;
            input.match_index = match_count;

            state.string_position = view_index;
            state.string_position_in_code_units = view_index;
            state.instruction_position = 0;
            state.repetition_marks.clear();

            auto success = execute(input, state, operations);
            if (success) {
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
            }

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

    RegexResult result {
        match_count != 0,
        match_count,
        move(state.matches).release(),
        move(state.capture_group_matches).release(),
        operations,
        m_pattern->parser_result.capture_groups_count,
        m_pattern->parser_result.named_capture_groups_count,
    };

    if (match_count) {
        // Make sure there are as many capture matches as there are actual matches.
        if (result.capture_group_matches.size() < match_count)
            result.capture_group_matches.resize(match_count);
        for (auto& matches : result.capture_group_matches)
            matches.resize(m_pattern->parser_result.capture_groups_count + 1);
        if (!input.regex_options.has_flag_set(AllFlags::SkipTrimEmptyMatches)) {
            for (auto& matches : result.capture_group_matches)
                matches.remove_all_matching([](auto& match) { return match.view.is_null(); });
        }
    } else {
        result.capture_group_matches.clear_with_capacity();
    }

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
bool Matcher<Parser>::execute(MatchInput const& input, MatchState& state, size_t& operations) const
{
    if (m_pattern->parser_result.optimization_data.pure_substring_search.has_value() && input.view.is_string_view()) {
        // Yay, we can do a simple substring search!
        auto& needle = m_pattern->parser_result.optimization_data.pure_substring_search.value();
        if (needle.length() + state.string_position > input.view.length())
            return false;

        auto haystack = input.view.string_view().substring_view(state.string_position);
        if (input.regex_options.has_flag_set(AllFlags::Insensitive)) {
            if (!haystack.substring_view(0, needle.length()).equals_ignoring_ascii_case(needle))
                return false;
        } else {
            if (!haystack.starts_with(needle))
                return false;
        }

        state.string_position += needle.length();
        state.string_position_in_code_units += needle.length();
        return true;
    }

    BumpAllocatedLinkedList<MatchState> states_to_try_next;
    HashTable<u64> seen_state_hashes;
#if REGEX_DEBUG
    size_t recursion_level = 0;
#endif

    auto& bytecode = m_pattern->parser_result.bytecode;

    for (;;) {
        auto& opcode = bytecode.get_opcode(state);
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

        state.instruction_position += opcode.size();

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
                states_to_try_next.last().initiating_fork = state.instruction_position - opcode.size();
                states_to_try_next.last().instruction_position = state.fork_at_position;
            }
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
                states_to_try_next.last().initiating_fork = state.instruction_position - opcode.size();
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
            return true;
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
            return false;
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
                return false;
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
        return pair_int_hash(key.pattern.hash(), int_hash(to_underlying(key.options.value())));
    }
};
