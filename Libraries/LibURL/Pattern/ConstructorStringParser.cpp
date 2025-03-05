/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/GenericShorthands.h>
#include <LibURL/Pattern/Canonicalization.h>
#include <LibURL/Pattern/Component.h>
#include <LibURL/Pattern/ConstructorStringParser.h>

namespace URL::Pattern {

StringView ConstructorStringParser::state_to_string() const
{
    switch (m_state) {
    case State::Initial:
        return "Initial"sv;
    case State::Protocol:
        return "Protocol"sv;
    case State::Authority:
        return "Authority"sv;
    case State::Username:
        return "Username"sv;
    case State::Password:
        return "Password"sv;
    case State::Hostname:
        return "Hostname"sv;
    case State::Port:
        return "Port"sv;
    case State::Pathname:
        return "Pathname"sv;
    case State::Search:
        return "Search"sv;
    case State::Hash:
        return "Hash"sv;
    case State::Done:
        return "Done"sv;
    }
    VERIFY_NOT_REACHED();
}

ConstructorStringParser::ConstructorStringParser(Utf8View const& input, Vector<Token> token_list)
    : m_input(input)
    , m_token_list(move(token_list))
{
}

// https://urlpattern.spec.whatwg.org/#parse-a-constructor-string
PatternErrorOr<Init> ConstructorStringParser::parse(Utf8View const& input)
{
    // 1. Let parser be a new constructor string parser whose input is input and token list is the result of running
    //    tokenize given input and "lenient".
    ConstructorStringParser parser { input, TRY(Tokenizer::tokenize(input, Tokenizer::Policy::Lenient)) };

    // 2. While parser’s token index is less than parser’s token list size:
    while (parser.m_token_index < parser.m_token_list.size()) {
        dbgln_if(URL_PATTERN_DEBUG, "{}\t| Token@{} (group depth {}) -> {}", parser.state_to_string(),
            parser.m_token_index, parser.m_group_depth, parser.m_token_list[parser.m_token_index].to_string());
        // 1. Set parser’s token increment to 1.
        parser.m_token_increment = 1;

        // NOTE: On every iteration of the parse loop the parser’s token index will be incremented by its token
        //       increment value. Typically this means incrementing by 1, but at certain times it is set to zero.
        //       The token increment is then always reset back to 1 at the top of the loop.

        // 2. If parser’s token list[parser’s token index]'s type is "end" then:
        if (parser.m_token_list[parser.m_token_index].type == Token::Type::End) {
            // 1. If parser’s state is "init":
            if (parser.m_state == State::Initial) {
                // NOTE: If we reached the end of the string in the "init" state, then we failed to find a protocol
                //       terminator and this has to be a relative URLPattern constructor string.

                // 1. Run rewind given parser.
                parser.rewind();

                // NOTE: We next determine at which component the relative pattern begins. Relative pathnames are
                //       most common, but URLs and URLPattern constructor strings can begin with the search or hash
                //       components as well.

                // 2. If the result of running is a hash prefix given parser is true, then run change state given parser,
                //    "hash" and 1.
                if (parser.is_a_hash_prefix()) {
                    parser.change_state(State::Hash, 1);
                }
                // 3. Otherwise if the result of running is a search prefix given parser is true:
                else if (parser.is_a_search_prefix()) {
                    // 1. Run change state given parser, "search" and 1.
                    parser.change_state(State::Search, 1);
                }
                // 4. Otherwise:
                else {
                    // 1. Run change state given parser, "pathname" and 0.
                    parser.change_state(State::Pathname, 0);
                }

                // 5. Increment parser’s token index by parser’s token increment.
                parser.m_token_index += parser.m_token_increment;

                // 6. Continue.
                continue;
            }

            // 2. If parser’s state is "authority":
            if (parser.m_state == State::Authority) {
                // NOTE: If we reached the end of the string in the "authority" state, then we failed to find an
                //            "@". Therefore there is no username or password.

                // 1. Run rewind and set state given parser, and "hostname".
                parser.rewind_and_set_state(State::Hostname);

                // 2. Increment parser’s token index by parser’s token increment.
                parser.m_token_index += parser.m_token_increment;

                // 3. Continue.
                continue;
            }

            // 3. Run change state given parser, "done" and 0.
            parser.change_state(State::Done, 0);

            // 4. Break.
            break;
        }

        // 3. If the result of running is a group open given parser is true:
        if (parser.is_a_group_open()) {
            // NOTE: We ignore all code points within "{ ... }" pattern groupings. It would not make sense to allow
            //            a URL component boundary to lie within a grouping; e.g. "https://example.c{om/fo}o". While not
            //            supported within well formed pattern strings, we handle nested groupings here to avoid parser
            //            confusion.
            //
            // It is not necessary to perform this logic for regexp or named groups since those values are collapsed into
            // individual tokens by the tokenize algorithm.

            // 1. Increment parser’s group depth by 1.
            ++parser.m_group_depth;

            // 2. Increment parser’s token index by parser’s token increment.
            parser.m_token_index += parser.m_token_increment;

            // 3. Continue.
            continue;
        }

        // 4. If parser’s group depth is greater than 0:
        if (parser.m_group_depth > 0) {
            // 1. If the result of running is a group close given parser is true, then decrement parser’s group depth by 1.
            if (parser.is_a_group_close()) {
                VERIFY(parser.m_group_depth != 0);
                --parser.m_group_depth;
            }
            // 2. Otherwise:
            else {
                // 1. Increment parser’s token index by parser’s token increment.
                parser.m_token_index += parser.m_token_increment;

                // 2. Continue.
                continue;
            }
        }

        // 5. Switch on parser’s state and run the associated steps:
        switch (parser.m_state) {
        // -> "init", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-init%E2%91%A2
        case State::Initial: {
            // 1. If the result of running is a protocol suffix given parser is true:
            if (parser.is_a_protocol_suffix()) {
                // 1. Run rewind and set state given parser and "protocol".
                parser.rewind_and_set_state(State::Protocol);
            }
            break;
        }
        // -> "protocol", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-protocol%E2%91%A0
        case State::Protocol: {
            // 1. If the result of running is a protocol suffix given parser is true:
            if (parser.is_a_protocol_suffix()) {
                // 1. Run compute protocol matches a special scheme flag given parser.
                TRY(parser.compute_protocol_matches_a_special_scheme_flag());

                // NOTE: We need to eagerly compile the protocol component to determine if it matches any special
                //       schemes. If it does then certain special rules apply. It determines if the pathname
                //       defaults to a "/" and also whether we will look for the username, password, hostname, and
                //       port components. Authority slashes can also cause us to look for these components as well.
                //       Otherwise we treat this as an "opaque path URL" and go straight to the pathname component.

                // 2. Let next state be "pathname".
                auto next_state = State::Pathname;

                // 3. Let skip be 1.
                u32 skip = 1;

                // 4. If the result of running next is authority slashes given parser is true:
                if (parser.next_is_authority_slashes()) {
                    // 1. Set next state to "authority".
                    next_state = State::Authority;

                    // 2. Set skip to 3.
                    skip = 3;
                }
                // 5. Otherwise if parser’s protocol matches a special scheme flag is true, then set next state to "authority".
                else if (parser.m_protocol_matches_a_special_scheme) {
                    next_state = State::Authority;
                }

                // 6. Run change state given parser, next state, and skip.
                parser.change_state(next_state, skip);
            }
            break;
        }
        // -> "authority", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-authority%E2%91%A3
        case State::Authority: {
            // 1. If the result of running is an identity terminator given parser is true, then run rewind and set state
            //    given parser and "username".
            if (parser.is_an_identity_terminator()) {
                parser.rewind_and_set_state(State::Username);
            }
            // 2. Otherwise if any of the following are true:
            //     * the result of running is a pathname start given parser;
            //     * the result of running is a search prefix given parser; or
            //     * the result of running is a hash prefix given parser,
            //    then run rewind and set state given parser and "hostname".
            else if (parser.is_a_pathname_start()
                || parser.is_a_search_prefix()
                || parser.is_a_hash_prefix()) {
                parser.rewind_and_set_state(State::Hostname);
            }
            break;
        }
        // -> "username", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-username%E2%91%A0
        case State::Username: {
            // 1. If the result of running is a password prefix given parser is true, then run change state given
            //    parser, "password", and 1.
            if (parser.is_a_password_prefix()) {
                parser.change_state(State::Password, 1);
            }
            // 2. Otherwise if the result of running is an identity terminator given parser is true, then run change
            //    state given parser, "hostname", and 1.
            else if (parser.is_an_identity_terminator()) {
                parser.change_state(State::Hostname, 1);
            }
            break;
        }
        // -> "password", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-password%E2%91%A0
        case State::Password: {
            // 1. If the result of running is an identity terminator given parser is true, then run change state
            //    given parser, "hostname", and 1.
            if (parser.is_an_identity_terminator())
                parser.change_state(State::Hostname, 1);
            break;
        }
        // -> "hostname", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-hostname%E2%91%A3
        case State::Hostname: {
            // 1. If the result of running is an IPv6 open given parser is true, then increment parser’s hostname
            //    IPv6 bracket depth by 1.
            if (parser.is_an_ipv6_open()) {
                ++parser.m_hostname_ipv6_bracket_depth;
            }
            // 2. Otherwise if the result of running is an IPv6 close given parser is true, then decrement parser’s
            //    hostname IPv6 bracket depth by 1.
            else if (parser.is_an_ipv6_close()) {
                VERIFY(parser.m_hostname_ipv6_bracket_depth != 0);
                --parser.m_hostname_ipv6_bracket_depth;
            }
            // 3. Otherwise if the result of running is a port prefix given parser is true and parser’s hostname IPv6
            //    bracket depth is zero, then run change state given parser, "port", and 1.
            else if (parser.is_a_port_prefix() && parser.m_hostname_ipv6_bracket_depth == 0) {
                parser.change_state(State::Port, 1);
            }
            // 4. Otherwise if the result of running is a pathname start given parser is true, then run change state
            //    given parser, "pathname", and 0.
            else if (parser.is_a_pathname_start()) {
                parser.change_state(State::Pathname, 0);
            }
            // 5. Otherwise if the result of running is a search prefix given parser is true, then run change state
            //    given parser, "search", and 1.
            else if (parser.is_a_search_prefix()) {
                parser.change_state(State::Search, 1);
            }
            // 6. Otherwise if the result of running is a hash prefix given parser is true, then run change state
            //    given parser, "hash", and 1.
            else if (parser.is_a_hash_prefix()) {
                parser.change_state(State::Hash, 1);
            }
            break;
        }
        // -> "port", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-port%E2%91%A0
        case State::Port: {
            // 1. If the result of running is a pathname start given parser is true, then run change state given
            //    parser, "pathname", and 0.
            if (parser.is_a_pathname_start()) {
                parser.change_state(State::Pathname, 0);
            }
            // 2. Otherwise if the result of running is a search prefix given parser is true, then run change state
            //   given parser, "search", and 1.
            else if (parser.is_a_search_prefix()) {
                parser.change_state(State::Search, 1);
            }
            // 3. Otherwise if the result of running is a hash prefix given parser is true, then run change state given
            //    parser, "hash", and 1.
            else if (parser.is_a_hash_prefix()) {
                parser.change_state(State::Hash, 1);
            }
            break;
        }
        // -> "pathname", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-pathname%E2%91%A3
        case State::Pathname: {
            // 1. If the result of running is a search prefix given parser is true, then run change state given parser,
            //    "search", and 1.
            if (parser.is_a_search_prefix()) {
                parser.change_state(State::Search, 1);
            }
            // 2. Otherwise if the result of running is a hash prefix given parser is true, then run change state given
            //    parser, "hash", and 1.
            else if (parser.is_a_hash_prefix()) {
                parser.change_state(State::Hash, 1);
            }
            break;
        }
        // -> "search", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-search%E2%91%A3
        case State::Search: {
            // 1. If the result of running is a hash prefix given parser is true, then run change state given parser,
            //   "hash", and 1.
            if (parser.is_a_hash_prefix())
                parser.change_state(State::Hash, 1);
            break;
        }
        // -> "hash", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-hash%E2%91%A4
        case State::Hash: {
            // 1. Do nothing.
            break;
        }
        // -> "done", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-done%E2%91%A0
        case State::Done: {
            // 1. Assert: This step is never reached.
            VERIFY_NOT_REACHED();
        }
        }

        // 6. Increment parser’s token index by parser’s token increment.
        parser.m_token_index += parser.m_token_increment;
    }

    // 3. If parser’s result contains "hostname" and not "port", then set parser’s result["port"] to the empty string.
    if (parser.m_result.hostname.has_value() && !parser.m_result.port.has_value())
        parser.m_result.port = String {};

    // NOTE: This is special-cased because when an author does not specify a port, they usually intend the default
    //       port. If any port is acceptable, the author can specify it as a wildcard explicitly. For example,
    //       "https://example.com/*" does not match URLs beginning with "https://example.com:8443/", which is a
    //       different origin.

    // 4. Return parser’s result.
    return parser.m_result;
}

// https://urlpattern.spec.whatwg.org/#make-a-component-string
String ConstructorStringParser::make_a_component_string() const
{
    // 1. Assert: parser’s token index is less than parser’s token list's size.
    VERIFY(m_token_index < m_token_list.size());

    // 2. Let token be parser’s token list[parser’s token index].
    auto const& token = m_token_list[m_token_index];

    // 3. Let component start token be the result of running get a safe token given parser and parser’s component start.
    auto const& component_start_token = get_a_safe_token(m_component_start);

    // 4. Let component start input index be component start token’s index.
    auto component_start_input_index = component_start_token.index;

    // 5. Let end index be token’s index.
    auto end_index = token.index;

    // 6. Return the code point substring from component start input index to end index within parser’s input.
    auto sub_view = m_input.unicode_substring_view(component_start_input_index, end_index - component_start_input_index);
    return MUST(String::from_utf8(sub_view.as_string()));
}

// https://urlpattern.spec.whatwg.org/#compute-protocol-matches-a-special-scheme-flag
PatternErrorOr<void> ConstructorStringParser::compute_protocol_matches_a_special_scheme_flag()
{
    // FIXME: Implement this.
    return {};
}

Optional<String> const& ConstructorStringParser::result_for_active_state() const
{
    switch (m_state) {
    case State::Protocol:
        return m_result.protocol;
    case State::Username:
        return m_result.username;
    case State::Password:
        return m_result.password;
    case State::Hostname:
        return m_result.hostname;
    case State::Port:
        return m_result.port;
    case State::Pathname:
        return m_result.pathname;
    case State::Search:
        return m_result.search;
    case State::Hash:
        return m_result.hash;
    case State::Initial:
    case State::Authority:
    case State::Done:
        break;
    }
    VERIFY_NOT_REACHED();
}

void ConstructorStringParser::set_result_for_active_state(Optional<String> value)
{
    switch (m_state) {
    case State::Protocol:
        m_result.protocol = move(value);
        break;
    case State::Username:
        m_result.username = move(value);
        break;
    case State::Password:
        m_result.password = move(value);
        break;
    case State::Hostname:
        m_result.hostname = move(value);
        break;
    case State::Port:
        m_result.port = move(value);
        break;
    case State::Pathname:
        m_result.pathname = move(value);
        break;
    case State::Search:
        m_result.search = move(value);
        break;
    case State::Hash:
        m_result.hash = move(value);
        break;
    case State::Initial:
    case State::Authority:
    case State::Done:
        VERIFY_NOT_REACHED();
    }
}

// https://urlpattern.spec.whatwg.org/#change-state
void ConstructorStringParser::change_state(State new_state, u32 skip)
{
    // 1. If parser’s state is not "init", not "authority", and not "done", then set parser’s result[parser’s state] to
    //    the result of running make a component string given parser.
    if (m_state != State::Initial && m_state != State::Authority && m_state != State::Done)
        set_result_for_active_state(make_a_component_string());

    // 2. If parser’s state is not "init" and new state is not "done", then:
    if (m_state != State::Initial && new_state != State::Done) {
        // 1. If parser’s state is "protocol", "authority", "username", or "password"; new state is "port", "pathname",
        //    "search", or "hash"; and parser’s result["hostname"] does not exist, then set parser’s result["hostname"]
        //    to the empty string.
        if (first_is_one_of(m_state, State::Protocol, State::Authority, State::Username, State::Password)
            && first_is_one_of(new_state, State::Port, State::Pathname, State::Search, State::Hash)
            && !m_result.hostname.has_value()) {
            m_result.hostname = String {};
        }

        // 2. If parser’s state is "protocol", "authority", "username", "password", "hostname", or "port"; new state is
        //    "search" or "hash"; and parser’s result["pathname"] does not exist, then:
        if (first_is_one_of(m_state, State::Protocol, State::Authority, State::Username, State::Password, State::Hostname, State::Port)
            && first_is_one_of(new_state, State::Search, State::Hash)
            && !m_result.pathname.has_value()) {
            // 1. If parser’s protocol matches a special scheme flag is true, then set parser’s result["pathname"] to "/".
            if (m_protocol_matches_a_special_scheme) {
                m_result.pathname = "/"_string;
            }
            // 2. Otherwise, set parser’s result["pathname"] to the empty string.
            else {
                m_result.pathname = String {};
            }
        }

        // 3. If parser’s state is "protocol", "authority", "username", "password", "hostname", "port", or "pathname";
        //    new state is "hash"; and parser’s result["search"] does not exist, then set parser’s result["search"]
        //    to the empty string.
        if (first_is_one_of(m_state, State::Protocol, State::Authority, State::Username, State::Password, State::Hostname, State::Port, State::Pathname)
            && new_state == State::Hash
            && !m_result.search.has_value()) {
            m_result.search = String {};
        }
    }

    // 3. Set parser’s state to new state.
    m_state = new_state;

    // 4. Increment parser’s token index by skip.
    m_token_index += skip;

    // 5. Set parser’s component start to parser’s token index.
    m_component_start = m_token_index;

    // 6. Set parser’s token increment to 0.
    m_token_increment = 0;
}

// https://urlpattern.spec.whatwg.org/#next-is-authority-slashes
bool ConstructorStringParser::next_is_authority_slashes() const
{
    // 1. If the result of running is a non-special pattern char given parser, parser’s token index + 1, and "/" is false,
    //    then return false.
    if (!is_a_non_special_pattern_char(m_token_index + 1, '/'))
        return false;

    // 2. If the result of running is a non-special pattern char given parser, parser’s token index + 2, and "/" is false,
    //    then return false.
    if (!is_a_non_special_pattern_char(m_token_index + 2, '/'))
        return false;

    // 3. Return true.
    return true;
}

// https://urlpattern.spec.whatwg.org/#is-an-identity-terminator
bool ConstructorStringParser::is_an_identity_terminator() const
{
    // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and "@".
    return is_a_non_special_pattern_char(m_token_index, '@');
}

// https://urlpattern.spec.whatwg.org/#is-a-password-prefix
bool ConstructorStringParser::is_a_password_prefix() const
{
    // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and ":".
    return is_a_non_special_pattern_char(m_token_index, ':');
}

// https://urlpattern.spec.whatwg.org/#is-a-port-prefix
bool ConstructorStringParser::is_a_port_prefix() const
{
    // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and ":".
    return is_a_non_special_pattern_char(m_token_index, ':');
}

// https://urlpattern.spec.whatwg.org/#is-a-pathname-start
bool ConstructorStringParser::is_a_pathname_start() const
{
    // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and "/".
    return is_a_non_special_pattern_char(m_token_index, '/');
}

// https://urlpattern.spec.whatwg.org/#is-a-search-prefix
bool ConstructorStringParser::is_a_search_prefix() const
{
    // 1. If result of running is a non-special pattern char given parser, parser’s token index and "?" is true,
    //    then return true.
    if (is_a_non_special_pattern_char(m_token_index, '?'))
        return true;

    // 2. If parser’s token list[parser’s token index]'s value is not "?", then return false.
    if (m_token_list[m_token_index].value != "?"sv)
        return false;

    // 3. Let previous index be parser’s token index − 1.
    // 4. If previous index is less than 0, then return true.
    if (m_token_index == 0)
        return true;
    auto previous_index = m_token_index - 1;

    // 5. Let previous token be the result of running get a safe token given parser and previous index.
    auto const& previous_token = get_a_safe_token(previous_index);

    // 6. If any of the following are true, then return false:
    //    * previous token’s type is "name".
    //    * previous token’s type is "regexp".
    //    * previous token’s type is "close".
    //    * previous token’s type is "asterisk".
    if (previous_token.type == Token::Type::Name
        || previous_token.type == Token::Type::Regexp
        || previous_token.type == Token::Type::Close
        || previous_token.type == Token::Type::Asterisk) {
        return false;
    }

    // 7. Return true.
    return true;
}

// https://urlpattern.spec.whatwg.org/#is-a-protocol-suffix
bool ConstructorStringParser::is_a_protocol_suffix() const
{
    // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and ":".
    return is_a_non_special_pattern_char(m_token_index, ':');
}

// https://urlpattern.spec.whatwg.org/#is-a-hash-prefix
bool ConstructorStringParser::is_a_hash_prefix() const
{
    // 1. Return the result of running is a non-special pattern char given parser, parser’s token index and "#".
    return is_a_non_special_pattern_char(m_token_index, '#');
}

// https://urlpattern.spec.whatwg.org/#is-a-group-open
bool ConstructorStringParser::is_a_group_open() const
{
    // 1. If parser’s token list[parser’s token index]'s type is "open", then return true.
    if (m_token_list[m_token_index].type == Token::Type::Open)
        return true;

    // 2. Otherwise return false.
    return false;
}

// https://urlpattern.spec.whatwg.org/#is-a-group-close
bool ConstructorStringParser::is_a_group_close() const
{
    // 1. If parser’s token list[parser’s token index]'s type is "close", then return true.
    if (m_token_list[m_token_index].type == Token::Type::Close)
        return true;

    // 2. Otherwise return false.
    return false;
}

// https://urlpattern.spec.whatwg.org/#is-an-ipv6-open
bool ConstructorStringParser::is_an_ipv6_open() const
{
    // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and "[".
    return is_a_non_special_pattern_char(m_token_index, '[');
}

// https://urlpattern.spec.whatwg.org/#is-an-ipv6-close
bool ConstructorStringParser::is_an_ipv6_close() const
{
    // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and "]".
    return is_a_non_special_pattern_char(m_token_index, ']');
}

// https://urlpattern.spec.whatwg.org/#get-a-safe-token
Token const& ConstructorStringParser::get_a_safe_token(u32 index) const
{
    // 1. If index is less than parser’s token list's size, then return parser’s token list[index].
    if (index < m_token_list.size())
        return m_token_list[index];

    // 2. Assert: parser’s token list's size is greater than or equal to 1.
    VERIFY(!m_token_list.is_empty());

    // 3. Let last index be parser’s token list's size − 1.
    // 4. Let token be parser’s token list[last index].
    auto const& token = m_token_list.last();

    // 5. Assert: token’s type is "end".
    VERIFY(token.type == Token::Type::End);

    // 6. Return token.
    return token;
}

// https://urlpattern.spec.whatwg.org/#is-a-non-special-pattern-char
bool ConstructorStringParser::is_a_non_special_pattern_char(u32 index, char value) const
{
    // 1. Let token be the result of running get a safe token given parser and index.
    auto const& token = get_a_safe_token(index);

    // 2. If token’s value is not value, then return false.
    if (token.value.is_empty() || token.value.bytes().first() != value)
        return false;

    // 3. If any of the following are true:
    //     * token’s type is "char";
    //     * token’s type is "escaped-char"; or
    //     * token’s type is "invalid-char",
    //    then return true.
    if (token.type == Token::Type::Char
        || token.type == Token::Type::EscapedChar
        || token.type == Token::Type::InvalidChar) {
        return true;
    }

    // 4. Return false.
    return false;
}

// https://urlpattern.spec.whatwg.org/#rewind
void ConstructorStringParser::rewind()
{
    // 1. Set parser’s token index to parser’s component start.
    m_token_index = m_component_start;

    // 2. Set parser’s token increment to 0.
    m_token_increment = 0;
}

// https://urlpattern.spec.whatwg.org/#rewind-and-set-state
void ConstructorStringParser::rewind_and_set_state(State state)
{
    // 1. Run rewind given parser.
    rewind();

    // 2. Set parser’s state to state.
    m_state = state;
}

}
