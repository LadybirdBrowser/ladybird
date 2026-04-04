/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::module_inception)]

mod canonicalization;
mod component;
mod constructor_string_parser;
mod init;
mod options;
mod part;
mod pattern;
mod pattern_error;
mod pattern_parser;
mod string;
mod tokenizer;

pub use canonicalization::canonicalize_a_hash;
pub use canonicalization::canonicalize_a_hostname;
pub use canonicalization::canonicalize_a_password;
pub use canonicalization::canonicalize_a_pathname;
pub use canonicalization::canonicalize_a_port;
pub use canonicalization::canonicalize_a_protocol;
pub use canonicalization::canonicalize_a_search;
pub use canonicalization::canonicalize_a_username;
pub use canonicalization::canonicalize_an_ipv6_hostname;
pub use canonicalization::canonicalize_an_opaque_pathname;
pub use component::Component;
pub use component::GroupMatch;
pub use component::RegularExpression;
pub use component::Result as ComponentResult;
pub use component::protocol_component_matches_a_special_scheme;
pub use constructor_string_parser::ConstructorStringParser;
pub use init::Init;
pub use init::PatternProcessType;
pub use init::process_a_url_pattern_init;
pub use options::Options;
pub use part::Part;
pub use pattern::IgnoreCase;
pub use pattern::Input;
pub use pattern::MatchInput;
pub use pattern::Pattern;
pub use pattern::Result;
pub use pattern_error::ErrorInfo;
pub use pattern_error::PatternErrorOr;
pub use pattern_parser::EncodingCallback;
pub use pattern_parser::PatternParser;
pub use string::escape_a_pattern_string;
pub use string::escape_a_regexp_string;
pub use string::full_wildcard_regexp_value;
pub use string::generate_a_pattern_string;
pub use string::generate_a_segment_wildcard_regexp;
pub use tokenizer::Token;
pub use tokenizer::Tokenizer;
