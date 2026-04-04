/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::BTreeMap;
use std::fmt;
use std::rc::Rc;

use libregex_rust::ast::Flags;
use libregex_rust::regex::Regex;
use libregex_rust::vm::VmResult;

use crate::pattern::EncodingCallback;
use crate::pattern::ErrorInfo;
use crate::pattern::Options;
use crate::pattern::Part;
use crate::pattern::PatternErrorOr;
use crate::pattern::PatternParser;
use crate::pattern::escape_a_regexp_string;
use crate::pattern::full_wildcard_regexp_value;
use crate::pattern::generate_a_pattern_string;
use crate::pattern::generate_a_segment_wildcard_regexp;
use crate::pattern::part::Modifier as PartModifier;
use crate::pattern::part::Type as PartType;
use crate::url::special_schemes;

// https://urlpattern.spec.whatwg.org/#component
#[derive(Clone, Default)]
pub struct Component {
    // https://urlpattern.spec.whatwg.org/#component-pattern-string
    // pattern string, a well formed pattern string
    pub pattern_string: String,

    // https://urlpattern.spec.whatwg.org/#component-regular-expression
    // regular expression, a RegExp
    pub regular_expression: Option<RegularExpression>,

    // https://urlpattern.spec.whatwg.org/#component-group-name-list
    // group name list, a list of strings
    pub group_name_list: Vec<String>,

    // https://urlpattern.spec.whatwg.org/#component-has-regexp-groups
    // has regexp groups, a boolean
    pub has_regexp_groups: bool,
}

#[derive(Clone)]
pub struct RegularExpression(Rc<Regex>);

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum GroupMatch {
    String(String),
    Empty,
}

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatterncomponentresult
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Result {
    pub input: String,
    pub groups: BTreeMap<String, GroupMatch>,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ExecutionResult {
    pub success: bool,
    pub captures: Vec<Option<String>>,
}

// https://urlpattern.spec.whatwg.org/#generate-a-regular-expression-and-name-list
struct RegularExpressionAndNameList {
    regular_expression: String,
    name_list: Vec<String>,
}

impl Component {
    // https://urlpattern.spec.whatwg.org/#compile-a-component
    pub fn compile(input: &str, encoding_callback: EncodingCallback, options: &Options) -> PatternErrorOr<Self> {
        // 1. Let part list be the result of running parse a pattern string given input, options, and encoding callback.
        let part_list = PatternParser::parse(input, options, encoding_callback)?;

        // 2. Let (regular expression string, name list) be the result of running generate a regular expression and name
        //    list given part list and options.
        let RegularExpressionAndNameList {
            regular_expression: regular_expression_string,
            name_list,
        } = generate_a_regular_expression_and_name_list(&part_list, options);

        // 3. Let flags be an empty string.
        // NOTE: These flags match the flags for the empty string of the LibJS RegExp implementation.
        let mut flags = Flags::default();

        // 4. If options’s ignore case is true then set flags to "vi".
        if options.ignore_case {
            flags.unicode_sets = true;
            flags.ignore_case = true;
        }
        // 5. Otherwise set flags to "v"
        else {
            flags.unicode_sets = true;
        }

        // 6. Let regular expression be RegExpCreate(regular expression string, flags). If this throws an exception, catch
        //    it, and throw a TypeError.
        let regex = Regex::compile(&regular_expression_string, flags)
            .map_err(|error| ErrorInfo::new(format!("RegExp compile error: {error}")))?;

        // 7. Let pattern string be the result of running generate a pattern string given part list and options.
        let pattern_string = generate_a_pattern_string(&part_list, options);

        // 8. Let has regexp groups be false.
        let mut has_regexp_groups = false;

        // 9. For each part of part list:
        for part in &part_list {
            // 1. If part’s type is "regexp", then set has regexp groups to true.
            if part.r#type == PartType::Regexp {
                has_regexp_groups = true;
                break;
            }
        }

        // 10. Return a new component whose pattern string is pattern string, regular expression is regular expression,
        //     group name list is name list, and has regexp groups is has regexp groups.
        Ok(Self {
            pattern_string,
            regular_expression: Some(RegularExpression::new(regex)),
            group_name_list: name_list,
            has_regexp_groups,
        })
    }

    // https://urlpattern.spec.whatwg.org/#create-a-component-match-result
    pub fn create_match_result(&self, input: &str, exec_result: &ExecutionResult) -> Result {
        // 1. Let result be a new URLPatternComponentResult.
        // 2. Set result["input"] to input.
        let mut result = Result {
            input: input.to_string(),
            ..Result::default()
        };

        // 3. Let groups be a record<USVString, (USVString or undefined)>.
        let mut groups = BTreeMap::new();

        // 4. Let index be 1.
        // 5. While index is less than or equal to component’s group name list’s size:
        assert!(exec_result.captures.len() == self.group_name_list.len());
        for index in 1..=self.group_name_list.len() {
            // 1. Let name be component’s group name list[index − 1].
            let name = self.group_name_list[index - 1].clone();

            // 2. Let value be Get(execResult, ToString(index)).
            // 3. Set groups[name] to value.
            let capture = &exec_result.captures[index - 1];
            if let Some(capture) = capture {
                groups.insert(name, GroupMatch::String(capture.clone()));
            } else {
                groups.insert(name, GroupMatch::Empty);
            }

            // 4. Increment index by 1.
        }

        // 6. Set result["groups"] to groups.
        result.groups = groups;

        // 7. Return result.
        result
    }

    pub fn execute(&self, input: &str) -> ExecutionResult {
        let Some(regular_expression) = self.regular_expression.as_ref() else {
            return ExecutionResult::default();
        };

        let utf16_input: Vec<u16> = input.encode_utf16().collect();
        let capture_count = regular_expression.capture_count() as usize;
        let mut captures = vec![-1; (capture_count + 1) * 2];
        let match_result = regular_expression.exec_into(&utf16_input, 0, &mut captures);
        if match_result != VmResult::Match {
            return ExecutionResult::default();
        }

        let mut result = ExecutionResult {
            success: true,
            captures: Vec::with_capacity(self.group_name_list.len()),
        };

        for index in 1..=self.group_name_list.len() {
            let start = captures[index * 2];
            let end = captures[index * 2 + 1];
            if start < 0 || end < 0 {
                result.captures.push(None);
                continue;
            }

            let capture = String::from_utf16_lossy(&utf16_input[start as usize..end as usize]);
            result.captures.push(Some(capture));
        }

        result
    }

    pub fn matches(&self, input: &str) -> bool {
        let Some(regular_expression) = self.regular_expression.as_ref() else {
            return false;
        };

        let utf16_input: Vec<u16> = input.encode_utf16().collect();
        regular_expression.test(&utf16_input, 0) == VmResult::Match
    }
}

// https://urlpattern.spec.whatwg.org/#protocol-component-matches-a-special-scheme
pub fn protocol_component_matches_a_special_scheme(protocol_component: &Component) -> bool {
    // 1. Let special scheme list be a list populated with all of the special schemes.
    // 2. For each scheme of special scheme list:
    for scheme in special_schemes() {
        // 1. Let test result be RegExpBuiltinExec(protocol component’s regular expression, scheme).
        let test_result = protocol_component.matches(scheme);

        // 2. If test result is not null, then return true.
        if test_result {
            return true;
        }
    }

    // 3. Return false.
    false
}

impl RegularExpression {
    fn new(regex: Regex) -> Self {
        Self(Rc::new(regex))
    }

    fn capture_count(&self) -> u32 {
        self.0.capture_count()
    }

    fn exec_into(&self, input: &[u16], start: usize, out: &mut [i32]) -> VmResult {
        self.0.exec_into(input, start, out)
    }

    fn test(&self, input: &[u16], start: usize) -> VmResult {
        self.0.test(input, start)
    }
}

impl fmt::Debug for Component {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Component")
            .field("pattern_string", &self.pattern_string)
            .field(
                "regular_expression",
                &self.regular_expression.as_ref().map(|_| "RegularExpression"),
            )
            .field("group_name_list", &self.group_name_list)
            .field("has_regexp_groups", &self.has_regexp_groups)
            .finish()
    }
}

impl fmt::Debug for RegularExpression {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("RegularExpression(..)")
    }
}

fn generate_a_regular_expression_and_name_list(part_list: &[Part], options: &Options) -> RegularExpressionAndNameList {
    // 1. Let result be "^".
    let mut result = String::from("^");

    // 2. Let name list be a new list.
    let mut name_list = Vec::new();

    // 3. For each part of part list:
    for part in part_list {
        // 1. If part’s type is "fixed-text":
        if part.r#type == PartType::FixedText {
            // 1. If part’s modifier is "none", then append the result of running escape a regexp string given part’s
            //    value to the end of result.
            if part.modifier == PartModifier::None {
                result.push_str(&escape_a_regexp_string(&part.value));
            }
            // 2. Otherwise:
            else {
                // 1. Append "(?:" to the end of result.
                result.push_str("(?:");

                // 2. Append the result of running escape a regexp string given part’s value to the end of result.
                result.push_str(&escape_a_regexp_string(&part.value));

                // 3. Append ")" to the end of result.
                result.push(')');

                // 4. Append the result of running convert a modifier to a string given part’s modifier to the end of result.
                result.push_str(Part::convert_modifier_to_string(part.modifier));
            }

            // 3. Continue.
            continue;
        }

        // 2. Assert: part’s name is not the empty string.
        assert!(!part.name.is_empty());

        // 3. Append part’s name to name list.
        name_list.push(part.name.clone());

        // 4. Let regexp value be part’s value.
        let mut regexp_value = part.value.clone();

        // 5. If part’s type is "segment-wildcard", then set regexp value to the result of running generate a segment wildcard regexp given options.
        if part.r#type == PartType::SegmentWildcard {
            regexp_value = generate_a_segment_wildcard_regexp(options);
        }
        // 6. Otherwise if part’s type is "full-wildcard", then set regexp value to full wildcard regexp value.
        else if part.r#type == PartType::FullWildcard {
            regexp_value = full_wildcard_regexp_value.to_string();
        }

        // 7. If part’s prefix is the empty string and part’s suffix is the empty string:
        if part.prefix.is_empty() && part.suffix.is_empty() {
            // 1. If part’s modifier is "none" or "optional", then:
            if part.modifier == PartModifier::None || part.modifier == PartModifier::Optional {
                // 1. Append "(" to the end of result.
                result.push('(');

                // 2. Append regexp value to the end of result.
                result.push_str(&regexp_value);

                // 3. Append ")" to the end of result.
                result.push(')');

                // 4. Append the result of running convert a modifier to a string given part’s modifier to the end of result.
                result.push_str(Part::convert_modifier_to_string(part.modifier));
            }
            // 2. Otherwise:
            else {
                // 1. Append "((?:" to the end of result.
                result.push_str("((?:");

                // 2. Append regexp value to the end of result.
                result.push_str(&regexp_value);

                // 3. Append ")" to the end of result.
                result.push(')');

                // 4. Append the result of running convert a modifier to a string given part’s modifier to the end of result.
                result.push_str(Part::convert_modifier_to_string(part.modifier));

                // 5. Append ")" to the end of result.
                result.push(')');
            }

            // 3. Continue.
            continue;
        }

        // 8. If part’s modifier is "none" or "optional":
        if part.modifier == PartModifier::None || part.modifier == PartModifier::Optional {
            // 1. Append "(?:" to the end of result.
            result.push_str("(?:");

            // 2. Append the result of running escape a regexp string given part’s prefix to the end of result.
            result.push_str(&escape_a_regexp_string(&part.prefix));

            // 3. Append "(" to the end of result.
            result.push('(');

            // 4. Append regexp value to the end of result.
            result.push_str(&regexp_value);

            // 5. Append ")" to the end of result.
            result.push(')');

            // 6. Append the result of running escape a regexp string given part’s suffix to the end of result.
            result.push_str(&escape_a_regexp_string(&part.suffix));

            // 7. Append ")" to the end of result.
            result.push(')');

            // 8. Append the result of running convert a modifier to a string given part’s modifier to the end of result.
            result.push_str(Part::convert_modifier_to_string(part.modifier));

            // 9. Continue.
            continue;
        }

        // 9. Assert: part’s modifier is "zero-or-more" or "one-or-more".
        assert!(part.modifier == PartModifier::ZeroOrMore || part.modifier == PartModifier::OneOrMore);

        // 10. Assert: part’s prefix is not the empty string or part’s suffix is not the empty string.
        assert!(!part.prefix.is_empty() || !part.suffix.is_empty());

        // 11. Append "(?:" to the end of result.
        result.push_str("(?:");

        // 12. Append the result of running escape a regexp string given part’s prefix to the end of result.
        result.push_str(&escape_a_regexp_string(&part.prefix));

        // 13. Append "((?:" to the end of result.
        result.push_str("((?:");

        // 14. Append regexp value to the end of result.
        result.push_str(&regexp_value);

        // 15. Append ")(?:" to the end of result.
        result.push_str(")(?:");

        // 16. Append the result of running escape a regexp string given part’s suffix to the end of result.
        result.push_str(&escape_a_regexp_string(&part.suffix));

        // 17. Append the result of running escape a regexp string given part’s prefix to the end of result.
        result.push_str(&escape_a_regexp_string(&part.prefix));

        // 18. Append "(?:" to the end of result.
        result.push_str("(?:");

        // 19. Append regexp value to the end of result.
        result.push_str(&regexp_value);

        // 20. Append "))*)" to the end of result.
        result.push_str("))*)");

        // 21. Append the result of running escape a regexp string given part’s suffix to the end of result.
        result.push_str(&escape_a_regexp_string(&part.suffix));

        // 22. Append ")" to the end of result.
        result.push(')');

        // 23. If part’s modifier is "zero-or-more" then append "?" to the end of result.
        if part.modifier == PartModifier::ZeroOrMore {
            result.push('?');
        }
    }

    // 4. Append "$" to the end of result.
    result.push('$');

    // 5. Return (result, name list).
    RegularExpressionAndNameList {
        regular_expression: result,
        name_list,
    }
}
