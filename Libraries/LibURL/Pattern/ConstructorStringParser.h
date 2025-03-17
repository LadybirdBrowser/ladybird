/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Pattern/Init.h>
#include <LibURL/Pattern/PatternError.h>
#include <LibURL/Pattern/Tokenizer.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#constructor-string-parser
class ConstructorStringParser {
public:
    static PatternErrorOr<Init> parse(Utf8View const& input);

private:
    ConstructorStringParser(Utf8View const& input, Vector<Token> token_list);

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-state
    enum class State {
        Initial,
        Protocol,
        Authority,
        Username,
        Password,
        Hostname,
        Port,
        Pathname,
        Search,
        Hash,
        Done,
    };
    StringView state_to_string() const;

    void rewind();
    void rewind_and_set_state(State);

    bool next_is_authority_slashes() const;

    bool is_an_identity_terminator() const;
    bool is_a_port_prefix() const;
    bool is_a_pathname_start() const;
    bool is_a_password_prefix() const;
    bool is_a_search_prefix() const;
    bool is_a_hash_prefix() const;
    bool is_a_protocol_suffix() const;
    bool is_an_ipv6_open() const;
    bool is_an_ipv6_close() const;
    bool is_a_group_open() const;
    bool is_a_group_close() const;

    Token const& get_a_safe_token(u32 index) const;
    bool is_a_non_special_pattern_char(u32 index, char value) const;
    void change_state(State, u32 skip);
    String make_a_component_string() const;
    PatternErrorOr<void> compute_protocol_matches_a_special_scheme_flag();

    Optional<String> const& result_for_active_state() const;
    void set_result_for_active_state(Optional<String> value);

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-input
    // A constructor string parser has an associated input, a string, which must be set upon creation.
    Utf8View m_input;

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-token-list
    // A constructor string parser has an associated token list, a token list, which must be set upon creation.
    Vector<Token> m_token_list;

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-result
    // A constructor string parser has an associated result, a URLPatternInit, initially set to a new URLPatternInit.
    Init m_result;

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-component-start
    // A constructor string parser has an associated component start, a number, initially set to 0.
    u32 m_component_start { 0 };

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-token-index
    // A constructor string parser has an associated token index, a number, initially set to 0.
    u32 m_token_index { 0 };

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-token-increment
    // A constructor string parser has an associated token increment, a number, initially set to 1.
    u32 m_token_increment { 1 };

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-group-depth
    // A constructor string parser has an associated group depth, a number, initially set to 0.
    u32 m_group_depth { 0 };

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-hostname-ipv6-bracket-depth
    // A constructor string parser has an associated hostname IPv6 bracket depth, a number, initially set to 0.
    u32 m_hostname_ipv6_bracket_depth { 0 };

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-protocol-matches-a-special-scheme-flag
    // A constructor string parser has an associated protocol matches a special scheme flag, a boolean, initially set to false.
    bool m_protocol_matches_a_special_scheme { false };

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-state
    // A constructor string parser has an associated state, a string, initially set to "init".
    State m_state { State::Initial };
};

}
