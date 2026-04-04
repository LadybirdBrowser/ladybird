/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::pattern::Component;
use crate::pattern::Init;
use crate::pattern::Options;
use crate::pattern::PatternErrorOr;
use crate::pattern::Token;
use crate::pattern::Tokenizer;
use crate::pattern::canonicalize_a_protocol;
use crate::pattern::protocol_component_matches_a_special_scheme;
use crate::pattern::tokenizer::Policy as TokenizerPolicy;
use crate::pattern::tokenizer::Type as TokenType;

// https://urlpattern.spec.whatwg.org/#constructor-string-parser
pub struct ConstructorStringParser {
    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-input
    // A constructor string parser has an associated input, a string, which must be set upon creation.
    pub input: String,

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-token-list
    // A constructor string parser has an associated token list, a token list, which must be set upon creation.
    pub token_list: Vec<Token>,

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-result
    // A constructor string parser has an associated result, a URLPatternInit, initially set to a new URLPatternInit.
    pub result: Init,

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-component-start
    // A constructor string parser has an associated component start, a number, initially set to 0.
    pub component_start: u32,

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-token-index
    // A constructor string parser has an associated token index, a number, initially set to 0.
    pub token_index: u32,

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-token-increment
    // A constructor string parser has an associated token increment, a number, initially set to 1.
    pub token_increment: u32,

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-group-depth
    // A constructor string parser has an associated group depth, a number, initially set to 0.
    pub group_depth: u32,

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-hostname-ipv6-bracket-depth
    // A constructor string parser has an associated hostname IPv6 bracket depth, a number, initially set to 0.
    pub hostname_ipv6_bracket_depth: u32,

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-protocol-matches-a-special-scheme-flag
    // A constructor string parser has an associated protocol matches a special scheme flag, a boolean, initially set to false.
    pub protocol_matches_a_special_scheme: bool,

    // https://urlpattern.spec.whatwg.org/#constructor-string-parser-state
    // A constructor string parser has an associated state, a string, initially set to "init".
    pub state: State,

    input_code_points: Vec<char>,
}

// https://urlpattern.spec.whatwg.org/#constructor-string-parser-state
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum State {
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
}

impl ConstructorStringParser {
    pub(crate) fn new(input: &str, token_list: Vec<Token>) -> Self {
        Self {
            input: input.to_string(),
            token_list,
            result: Init::default(),
            component_start: 0,
            token_index: 0,
            token_increment: 1,
            group_depth: 0,
            hostname_ipv6_bracket_depth: 0,
            protocol_matches_a_special_scheme: false,
            state: State::Initial,
            input_code_points: input.chars().collect(),
        }
    }

    // https://urlpattern.spec.whatwg.org/#parse-a-constructor-string
    pub fn parse(input: &str) -> PatternErrorOr<Init> {
        // 1. Let parser be a new constructor string parser whose input is input and token list is the result of running
        //    tokenize given input and "lenient".
        let mut parser = Self::new(input, Tokenizer::tokenize(input, TokenizerPolicy::Lenient)?);

        // 2. While parser’s token index is less than parser’s token list size:
        while (parser.token_index as usize) < parser.token_list.len() {
            // 1. Set parser’s token increment to 1.
            parser.token_increment = 1;

            // NOTE: On every iteration of the parse loop the parser’s token index will be incremented by its token
            //       increment value. Typically this means incrementing by 1, but at certain times it is set to zero.
            //       The token increment is then always reset back to 1 at the top of the loop.

            // 2. If parser’s token list[parser’s token index]'s type is "end" then:
            if parser.token_list[parser.token_index as usize].r#type == TokenType::End {
                // 1. If parser’s state is "init":
                if parser.state == State::Initial {
                    // NOTE: If we reached the end of the string in the "init" state, then we failed to find a protocol
                    //       terminator and this has to be a relative URLPattern constructor string.

                    // 1. Run rewind given parser.
                    parser.rewind();

                    // NOTE: We next determine at which component the relative pattern begins. Relative pathnames are
                    //       most common, but URLs and URLPattern constructor strings can begin with the search or hash
                    //       components as well.

                    // 2. If the result of running is a hash prefix given parser is true, then run change state given parser,
                    //    "hash" and 1.
                    if parser.is_a_hash_prefix() {
                        parser.change_state(State::Hash, 1);
                    }
                    // 3. Otherwise if the result of running is a search prefix given parser is true:
                    else if parser.is_a_search_prefix() {
                        // 1. Run change state given parser, "search" and 1.
                        parser.change_state(State::Search, 1);
                    }
                    // 4. Otherwise:
                    else {
                        // 1. Run change state given parser, "pathname" and 0.
                        parser.change_state(State::Pathname, 0);
                    }

                    // 5. Increment parser’s token index by parser’s token increment.
                    parser.token_index += parser.token_increment;

                    // 6. Continue.
                    continue;
                }

                // 2. If parser’s state is "authority":
                if parser.state == State::Authority {
                    // NOTE: If we reached the end of the string in the "authority" state, then we failed to find an
                    //            "@". Therefore there is no username or password.

                    // 1. Run rewind and set state given parser, and "hostname".
                    parser.rewind_and_set_state(State::Hostname);

                    // 2. Increment parser’s token index by parser’s token increment.
                    parser.token_index += parser.token_increment;

                    // 3. Continue.
                    continue;
                }

                // 3. Run change state given parser, "done" and 0.
                parser.change_state(State::Done, 0);

                // 4. Break.
                break;
            }

            // 3. If the result of running is a group open given parser is true:
            if parser.is_a_group_open() {
                // NOTE: We ignore all code points within "{ ... }" pattern groupings. It would not make sense to allow
                //            a URL component boundary to lie within a grouping; e.g. "https://example.c{om/fo}o". While not
                //            supported within well formed pattern strings, we handle nested groupings here to avoid parser
                //            confusion.
                //
                // It is not necessary to perform this logic for regexp or named groups since those values are collapsed into
                // individual tokens by the tokenize algorithm.

                // 1. Increment parser’s group depth by 1.
                parser.group_depth += 1;

                // 2. Increment parser’s token index by parser’s token increment.
                parser.token_index += parser.token_increment;

                // 3. Continue.
                continue;
            }

            // 4. If parser’s group depth is greater than 0:
            if parser.group_depth > 0 {
                // 1. If the result of running is a group close given parser is true, then decrement parser’s group depth by 1.
                if parser.is_a_group_close() {
                    assert!(parser.group_depth != 0);
                    parser.group_depth -= 1;
                }
                // 2. Otherwise:
                else {
                    // 1. Increment parser’s token index by parser’s token increment.
                    parser.token_index += parser.token_increment;

                    // 2. Continue.
                    continue;
                }
            }

            // 5. Switch on parser’s state and run the associated steps:
            match parser.state {
                // -> "init", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-init%E2%91%A2
                State::Initial => {
                    // 1. If the result of running is a protocol suffix given parser is true:
                    if parser.is_a_protocol_suffix() {
                        // 1. Run rewind and set state given parser and "protocol".
                        parser.rewind_and_set_state(State::Protocol);
                    }
                }
                // -> "protocol", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-protocol%E2%91%A0
                State::Protocol => {
                    // 1. If the result of running is a protocol suffix given parser is true:
                    if parser.is_a_protocol_suffix() {
                        // 1. Run compute protocol matches a special scheme flag given parser.
                        parser.compute_protocol_matches_a_special_scheme_flag()?;

                        // NOTE: We need to eagerly compile the protocol component to determine if it matches any special
                        //       schemes. If it does then certain special rules apply. It determines if the pathname
                        //       defaults to a "/" and also whether we will look for the username, password, hostname, and
                        //       port components. Authority slashes can also cause us to look for these components as well.
                        //       Otherwise we treat this as an "opaque path URL" and go straight to the pathname component.

                        // 2. Let next state be "pathname".
                        let mut next_state = State::Pathname;

                        // 3. Let skip be 1.
                        let mut skip = 1;

                        // 4. If the result of running next is authority slashes given parser is true:
                        if parser.next_is_authority_slashes() {
                            // 1. Set next state to "authority".
                            next_state = State::Authority;

                            // 2. Set skip to 3.
                            skip = 3;
                        }
                        // 5. Otherwise if parser’s protocol matches a special scheme flag is true, then set next state to "authority".
                        else if parser.protocol_matches_a_special_scheme {
                            next_state = State::Authority;
                        }

                        // 6. Run change state given parser, next state, and skip.
                        parser.change_state(next_state, skip);
                    }
                }
                // -> "authority", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-authority%E2%91%A3
                State::Authority => {
                    // 1. If the result of running is an identity terminator given parser is true, then run rewind and set state
                    //    given parser and "username".
                    if parser.is_an_identity_terminator() {
                        parser.rewind_and_set_state(State::Username);
                    }
                    // 2. Otherwise if any of the following are true:
                    //     * the result of running is a pathname start given parser;
                    //     * the result of running is a search prefix given parser; or
                    //     * the result of running is a hash prefix given parser,
                    //    then run rewind and set state given parser and "hostname".
                    else if parser.is_a_pathname_start() || parser.is_a_search_prefix() || parser.is_a_hash_prefix() {
                        parser.rewind_and_set_state(State::Hostname);
                    }
                }
                // -> "username", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-username%E2%91%A0
                State::Username => {
                    // 1. If the result of running is a password prefix given parser is true, then run change state given
                    //    parser, "password", and 1.
                    if parser.is_a_password_prefix() {
                        parser.change_state(State::Password, 1);
                    }
                    // 2. Otherwise if the result of running is an identity terminator given parser is true, then run change
                    //    state given parser, "hostname", and 1.
                    else if parser.is_an_identity_terminator() {
                        parser.change_state(State::Hostname, 1);
                    }
                }
                // -> "password", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-password%E2%91%A0
                State::Password => {
                    // 1. If the result of running is an identity terminator given parser is true, then run change state
                    //    given parser, "hostname", and 1.
                    if parser.is_an_identity_terminator() {
                        parser.change_state(State::Hostname, 1);
                    }
                }
                // -> "hostname", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-hostname%E2%91%A3
                State::Hostname => {
                    // 1. If the result of running is an IPv6 open given parser is true, then increment parser’s hostname
                    //    IPv6 bracket depth by 1.
                    if parser.is_an_ipv6_open() {
                        parser.hostname_ipv6_bracket_depth += 1;
                    }
                    // 2. Otherwise if the result of running is an IPv6 close given parser is true, then decrement parser’s
                    //    hostname IPv6 bracket depth by 1.
                    else if parser.is_an_ipv6_close() {
                        assert!(parser.hostname_ipv6_bracket_depth != 0);
                        parser.hostname_ipv6_bracket_depth -= 1;
                    }
                    // 3. Otherwise if the result of running is a port prefix given parser is true and parser’s hostname IPv6
                    //    bracket depth is zero, then run change state given parser, "port", and 1.
                    else if parser.is_a_port_prefix() && parser.hostname_ipv6_bracket_depth == 0 {
                        parser.change_state(State::Port, 1);
                    }
                    // 4. Otherwise if the result of running is a pathname start given parser is true, then run change state
                    //    given parser, "pathname", and 0.
                    else if parser.is_a_pathname_start() {
                        parser.change_state(State::Pathname, 0);
                    }
                    // 5. Otherwise if the result of running is a search prefix given parser is true, then run change state
                    //    given parser, "search", and 1.
                    else if parser.is_a_search_prefix() {
                        parser.change_state(State::Search, 1);
                    }
                    // 6. Otherwise if the result of running is a hash prefix given parser is true, then run change state
                    //    given parser, "hash", and 1.
                    else if parser.is_a_hash_prefix() {
                        parser.change_state(State::Hash, 1);
                    }
                }
                // -> "port", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-port%E2%91%A0
                State::Port => {
                    // 1. If the result of running is a pathname start given parser is true, then run change state given
                    //    parser, "pathname", and 0.
                    if parser.is_a_pathname_start() {
                        parser.change_state(State::Pathname, 0);
                    }
                    // 2. Otherwise if the result of running is a search prefix given parser is true, then run change state
                    //   given parser, "search", and 1.
                    else if parser.is_a_search_prefix() {
                        parser.change_state(State::Search, 1);
                    }
                    // 3. Otherwise if the result of running is a hash prefix given parser is true, then run change state given
                    //    parser, "hash", and 1.
                    else if parser.is_a_hash_prefix() {
                        parser.change_state(State::Hash, 1);
                    }
                }
                // -> "pathname", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-pathname%E2%91%A3
                State::Pathname => {
                    // 1. If the result of running is a search prefix given parser is true, then run change state given parser,
                    //    "search", and 1.
                    if parser.is_a_search_prefix() {
                        parser.change_state(State::Search, 1);
                    }
                    // 2. Otherwise if the result of running is a hash prefix given parser is true, then run change state given
                    //    parser, "hash", and 1.
                    else if parser.is_a_hash_prefix() {
                        parser.change_state(State::Hash, 1);
                    }
                }
                // -> "search", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-search%E2%91%A3
                State::Search => {
                    // 1. If the result of running is a hash prefix given parser is true, then run change state given parser,
                    //   "hash", and 1.
                    if parser.is_a_hash_prefix() {
                        parser.change_state(State::Hash, 1);
                    }
                }
                // -> "hash", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-hash%E2%91%A4
                State::Hash => {
                    // 1. Do nothing.
                }
                // -> "done", https://urlpattern.spec.whatwg.org/#ref-for-constructor-string-parser-state-done%E2%91%A0
                State::Done => {
                    // 1. Assert: This step is never reached.
                    unreachable!();
                }
            }

            // 6. Increment parser’s token index by parser’s token increment.
            parser.token_index += parser.token_increment;
        }

        // 3. If parser’s result contains "hostname" and not "port", then set parser’s result["port"] to the empty string.
        if parser.result.hostname.is_some() && parser.result.port.is_none() {
            parser.result.port = Some(String::new());
        }

        // NOTE: This is special-cased because when an author does not specify a port, they usually intend the default
        //       port. If any port is acceptable, the author can specify it as a wildcard explicitly. For example,
        //       "https://example.com/*" does not match URLs beginning with "https://example.com:8443/", which is a
        //       different origin.

        // 4. Return parser’s result.
        Ok(parser.result)
    }

    // https://urlpattern.spec.whatwg.org/#make-a-component-string
    pub(crate) fn make_a_component_string(&self) -> String {
        // 1. Assert: parser’s token index is less than parser’s token list's size.
        assert!((self.token_index as usize) < self.token_list.len());

        // 2. Let token be parser’s token list[parser’s token index].
        let token = &self.token_list[self.token_index as usize];

        // 3. Let component start token be the result of running get a safe token given parser and parser’s component start.
        let component_start_token = self.get_a_safe_token(self.component_start);

        // 4. Let component start input index be component start token’s index.
        let component_start_input_index = component_start_token.index;

        // 5. Let end index be token’s index.
        let end_index = token.index;

        // 6. Return the code point substring from component start input index to end index within parser’s input.
        self.input_code_points[component_start_input_index as usize..end_index as usize]
            .iter()
            .collect()
    }

    // https://urlpattern.spec.whatwg.org/#compute-protocol-matches-a-special-scheme-flag
    pub(crate) fn compute_protocol_matches_a_special_scheme_flag(&mut self) -> PatternErrorOr<()> {
        // 1. Let protocol string be the result of running make a component string given parser.
        let protocol_string = self.make_a_component_string();

        // 2. Let protocol component be the result of compiling a component given protocol string, canonicalize a protocol, and default options.
        let protocol_component = Component::compile(
            &protocol_string,
            Box::new(canonicalize_a_protocol),
            &Options::default_(),
        )?;

        // 3. If the result of running protocol component matches a special scheme given protocol component is true, then set parser’s protocol matches a special scheme flag to true.
        if protocol_component_matches_a_special_scheme(&protocol_component) {
            self.protocol_matches_a_special_scheme = true;
        }

        Ok(())
    }

    pub(crate) fn set_result_for_active_state(&mut self, value: Option<String>) {
        match self.state {
            State::Protocol => self.result.protocol = value,
            State::Username => self.result.username = value,
            State::Password => self.result.password = value,
            State::Hostname => self.result.hostname = value,
            State::Port => self.result.port = value,
            State::Pathname => self.result.pathname = value,
            State::Search => self.result.search = value,
            State::Hash => self.result.hash = value,
            State::Initial | State::Authority | State::Done => unreachable!(),
        }
    }

    // https://urlpattern.spec.whatwg.org/#change-state
    pub(crate) fn change_state(&mut self, new_state: State, skip: u32) {
        // 1. If parser’s state is not "init", not "authority", and not "done", then set parser’s result[parser’s state] to
        //    the result of running make a component string given parser.
        if self.state != State::Initial && self.state != State::Authority && self.state != State::Done {
            self.set_result_for_active_state(Some(self.make_a_component_string()));
        }

        // 2. If parser’s state is not "init" and new state is not "done", then:
        if self.state != State::Initial && new_state != State::Done {
            // 1. If parser’s state is "protocol", "authority", "username", or "password"; new state is "port", "pathname",
            //    "search", or "hash"; and parser’s result["hostname"] does not exist, then set parser’s result["hostname"]
            //    to the empty string.
            if matches!(
                self.state,
                State::Protocol | State::Authority | State::Username | State::Password
            ) && matches!(new_state, State::Port | State::Pathname | State::Search | State::Hash)
                && self.result.hostname.is_none()
            {
                self.result.hostname = Some(String::new());
            }

            // 2. If parser’s state is "protocol", "authority", "username", "password", "hostname", or "port"; new state is
            //    "search" or "hash"; and parser’s result["pathname"] does not exist, then:
            if matches!(
                self.state,
                State::Protocol | State::Authority | State::Username | State::Password | State::Hostname | State::Port
            ) && matches!(new_state, State::Search | State::Hash)
                && self.result.pathname.is_none()
            {
                // 1. If parser’s protocol matches a special scheme flag is true, then set parser’s result["pathname"] to "/".
                if self.protocol_matches_a_special_scheme {
                    self.result.pathname = Some("/".to_string());
                }
                // 2. Otherwise, set parser’s result["pathname"] to the empty string.
                else {
                    self.result.pathname = Some(String::new());
                }
            }

            // 3. If parser’s state is "protocol", "authority", "username", "password", "hostname", "port", or "pathname";
            //    new state is "hash"; and parser’s result["search"] does not exist, then set parser’s result["search"]
            //    to the empty string.
            if matches!(
                self.state,
                State::Protocol
                    | State::Authority
                    | State::Username
                    | State::Password
                    | State::Hostname
                    | State::Port
                    | State::Pathname
            ) && new_state == State::Hash
                && self.result.search.is_none()
            {
                self.result.search = Some(String::new());
            }
        }

        // 3. Set parser’s state to new state.
        self.state = new_state;

        // 4. Increment parser’s token index by skip.
        self.token_index += skip;

        // 5. Set parser’s component start to parser’s token index.
        self.component_start = self.token_index;

        // 6. Set parser’s token increment to 0.
        self.token_increment = 0;
    }

    // https://urlpattern.spec.whatwg.org/#next-is-authority-slashes
    pub(crate) fn next_is_authority_slashes(&self) -> bool {
        // 1. If the result of running is a non-special pattern char given parser, parser’s token index + 1, and "/" is false,
        //    then return false.
        if !self.is_a_non_special_pattern_char(self.token_index + 1, '/') {
            return false;
        }

        // 2. If the result of running is a non-special pattern char given parser, parser’s token index + 2, and "/" is false,
        //    then return false.
        if !self.is_a_non_special_pattern_char(self.token_index + 2, '/') {
            return false;
        }

        // 3. Return true.
        true
    }

    // https://urlpattern.spec.whatwg.org/#is-an-identity-terminator
    pub(crate) fn is_an_identity_terminator(&self) -> bool {
        // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and "@".
        self.is_a_non_special_pattern_char(self.token_index, '@')
    }

    // https://urlpattern.spec.whatwg.org/#is-a-password-prefix
    pub(crate) fn is_a_password_prefix(&self) -> bool {
        // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and ":".
        self.is_a_non_special_pattern_char(self.token_index, ':')
    }

    // https://urlpattern.spec.whatwg.org/#is-a-port-prefix
    pub(crate) fn is_a_port_prefix(&self) -> bool {
        // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and ":".
        self.is_a_non_special_pattern_char(self.token_index, ':')
    }

    // https://urlpattern.spec.whatwg.org/#is-a-pathname-start
    pub(crate) fn is_a_pathname_start(&self) -> bool {
        // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and "/".
        self.is_a_non_special_pattern_char(self.token_index, '/')
    }

    // https://urlpattern.spec.whatwg.org/#is-a-search-prefix
    pub(crate) fn is_a_search_prefix(&self) -> bool {
        // 1. If result of running is a non-special pattern char given parser, parser’s token index and "?" is true,
        //    then return true.
        if self.is_a_non_special_pattern_char(self.token_index, '?') {
            return true;
        }

        // 2. If parser’s token list[parser’s token index]'s value is not "?", then return false.
        if self.token_list[self.token_index as usize].value != "?" {
            return false;
        }

        // 3. Let previous index be parser’s token index − 1.
        // 4. If previous index is less than 0, then return true.
        if self.token_index == 0 {
            return true;
        }
        let previous_index = self.token_index - 1;

        // 5. Let previous token be the result of running get a safe token given parser and previous index.
        let previous_token = self.get_a_safe_token(previous_index);

        // 6. If any of the following are true, then return false:
        //    * previous token’s type is "name".
        //    * previous token’s type is "regexp".
        //    * previous token’s type is "close".
        //    * previous token’s type is "asterisk".
        if matches!(
            previous_token.r#type,
            TokenType::Name | TokenType::Regexp | TokenType::Close | TokenType::Asterisk
        ) {
            return false;
        }

        // 7. Return true.
        true
    }

    // https://urlpattern.spec.whatwg.org/#is-a-protocol-suffix
    pub(crate) fn is_a_protocol_suffix(&self) -> bool {
        // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and ":".
        self.is_a_non_special_pattern_char(self.token_index, ':')
    }

    // https://urlpattern.spec.whatwg.org/#is-a-hash-prefix
    pub(crate) fn is_a_hash_prefix(&self) -> bool {
        // 1. Return the result of running is a non-special pattern char given parser, parser’s token index and "#".
        self.is_a_non_special_pattern_char(self.token_index, '#')
    }

    // https://urlpattern.spec.whatwg.org/#is-a-group-open
    pub(crate) fn is_a_group_open(&self) -> bool {
        // 1. If parser’s token list[parser’s token index]'s type is "open", then return true.
        if self.token_list[self.token_index as usize].r#type == TokenType::Open {
            return true;
        }

        // 2. Otherwise return false.
        false
    }

    // https://urlpattern.spec.whatwg.org/#is-a-group-close
    pub(crate) fn is_a_group_close(&self) -> bool {
        // 1. If parser’s token list[parser’s token index]'s type is "close", then return true.
        if self.token_list[self.token_index as usize].r#type == TokenType::Close {
            return true;
        }

        // 2. Otherwise return false.
        false
    }

    // https://urlpattern.spec.whatwg.org/#is-an-ipv6-open
    pub(crate) fn is_an_ipv6_open(&self) -> bool {
        // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and "[".
        self.is_a_non_special_pattern_char(self.token_index, '[')
    }

    // https://urlpattern.spec.whatwg.org/#is-an-ipv6-close
    pub(crate) fn is_an_ipv6_close(&self) -> bool {
        // 1. Return the result of running is a non-special pattern char given parser, parser’s token index, and "]".
        self.is_a_non_special_pattern_char(self.token_index, ']')
    }

    // https://urlpattern.spec.whatwg.org/#get-a-safe-token
    pub(crate) fn get_a_safe_token(&self, index: u32) -> &Token {
        // 1. If index is less than parser’s token list's size, then return parser’s token list[index].
        if (index as usize) < self.token_list.len() {
            return &self.token_list[index as usize];
        }

        // 2. Assert: parser’s token list's size is greater than or equal to 1.
        assert!(!self.token_list.is_empty());

        // 3. Let last index be parser’s token list's size − 1.
        // 4. Let token be parser’s token list[last index].
        let token = self.token_list.last().unwrap();

        // 5. Assert: token’s type is "end".
        assert!(token.r#type == TokenType::End);

        // 6. Return token.
        token
    }

    // https://urlpattern.spec.whatwg.org/#is-a-non-special-pattern-char
    pub(crate) fn is_a_non_special_pattern_char(&self, index: u32, value: char) -> bool {
        // 1. Let token be the result of running get a safe token given parser and index.
        let token = self.get_a_safe_token(index);

        // 2. If token’s value is not value, then return false.
        if token.value.is_empty() || token.value.chars().next().unwrap() != value {
            return false;
        }

        // 3. If any of the following are true:
        //     * token’s type is "char";
        //     * token’s type is "escaped-char"; or
        //     * token’s type is "invalid-char",
        //    then return true.
        if matches!(
            token.r#type,
            TokenType::Char | TokenType::EscapedChar | TokenType::InvalidChar
        ) {
            return true;
        }

        // 4. Return false.
        false
    }

    // https://urlpattern.spec.whatwg.org/#rewind
    pub(crate) fn rewind(&mut self) {
        // 1. Set parser’s token index to parser’s component start.
        self.token_index = self.component_start;

        // 2. Set parser’s token increment to 0.
        self.token_increment = 0;
    }

    // https://urlpattern.spec.whatwg.org/#rewind-and-set-state
    pub(crate) fn rewind_and_set_state(&mut self, state: State) {
        // 1. Run rewind given parser.
        self.rewind();

        // 2. Set parser’s state to state.
        self.state = state;
    }
}
