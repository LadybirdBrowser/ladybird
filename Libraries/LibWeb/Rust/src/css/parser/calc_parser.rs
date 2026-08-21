/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Front-end parser for CSS calculations.

// Calculation trees are main-thread-only because some leaves retain C++ style values.
#![allow(clippy::arc_with_non_send_sync)]

use crate::css::calc::{
    CalcNode, CalcNumericType, CalcNumericValue, resolve_as_for_value_type, simplify_parsed_calculation,
};
use crate::css::css_tokenizer::ParserTokenKind;
use crate::css::math_functions::{MathFunction, math_function_from_name};
use crate::css::parser::component_value::{ComponentKind, ComponentValue};
use crate::css::parser::value_parser::{
    ANGLE_UNIT_NAMES, FLEX_UNIT_NAMES, FREQUENCY_UNIT_NAMES, RESOLUTION_UNIT_NAMES, TIME_UNIT_NAMES,
};
use crate::css::style_compute::LENGTH_UNIT_NAMES;
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CalcParseError {
    Invalid,
    NotHandled,
}

type Result<T> = std::result::Result<T, CalcParseError>;

fn equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&left, &right)| u8::try_from(left).is_ok_and(|left| left.eq_ignore_ascii_case(&right)))
}

fn unit_index(unit: &[u16], names: &[&str]) -> Option<u8> {
    names
        .iter()
        .position(|name| equals_ascii_case_insensitive(unit, name.as_bytes()))
        .and_then(|index| u8::try_from(index).ok())
}

struct CalculationParser<'a> {
    values: Vec<&'a ComponentValue>,
    position: usize,
    percentages_resolve_as: Option<u8>,
}

impl<'a> CalculationParser<'a> {
    fn new(values: &'a [ComponentValue], percentages_resolve_as: Option<u8>) -> Self {
        Self {
            values: values.iter().filter(|value| !value.is_whitespace()).collect(),
            position: 0,
            percentages_resolve_as,
        }
    }

    fn parse(mut self) -> Result<Arc<CalcNode>> {
        if self.values.is_empty() {
            return Err(CalcParseError::Invalid);
        }
        let root = self.parse_sum()?;
        if self.position != self.values.len() {
            return Err(CalcParseError::Invalid);
        }
        simplify_parsed_calculation(root, self.percentages_resolve_as)
            .map(|(root, _)| root)
            .ok_or(CalcParseError::Invalid)
    }

    fn parse_sum(&mut self) -> Result<Arc<CalcNode>> {
        let mut children = vec![self.parse_product()?];
        while let Some(operator) = self.peek_operator()
            && matches!(operator, b'+' | b'-')
        {
            self.position += 1;
            let child = self.parse_product()?;
            children.push(if operator == b'-' {
                Arc::new(CalcNode::Negate(child))
            } else {
                child
            });
        }
        Ok(match children.as_slice() {
            [child] => child.clone(),
            _ => Arc::new(CalcNode::Sum(children)),
        })
    }

    fn parse_product(&mut self) -> Result<Arc<CalcNode>> {
        let mut product = self.parse_leaf()?;
        while let Some(operator) = self.peek_operator()
            && matches!(operator, b'*' | b'/')
        {
            self.position += 1;
            let child = self.parse_leaf()?;
            let child = if operator == b'/' {
                Arc::new(CalcNode::Invert(child))
            } else {
                child
            };
            // NB: ValueParsing.cpp groups the first binary product in a run,
            //     then repeats. Preserve that left nesting so simplification
            //     can fold a canonical prefix before an unresolved unit.
            product = Arc::new(CalcNode::Product(vec![product, child]));
        }
        Ok(product)
    }

    fn peek_operator(&self) -> Option<u8> {
        [b'+', b'-', b'*', b'/'].into_iter().find(|operator| {
            self.values
                .get(self.position)
                .is_some_and(|value| value.is_delim(*operator))
        })
    }

    fn parse_leaf(&mut self) -> Result<Arc<CalcNode>> {
        let value = self.values.get(self.position).ok_or(CalcParseError::Invalid)?;
        if self.peek_operator().is_some() {
            return Err(CalcParseError::Invalid);
        }
        self.position += 1;
        match &value.kind {
            ComponentKind::SimpleBlock {
                opening: ParserTokenKind::OpenParen,
                values,
            } => parse_a_calculation(values, self.percentages_resolve_as),
            ComponentKind::Function { name, values } => {
                if math_function_from_name(name).is_none() {
                    return Err(CalcParseError::NotHandled);
                }
                parse_a_calc_function_node(name, values, self.percentages_resolve_as)
            }
            ComponentKind::Token(ParserTokenKind::Ident(identifier)) => parse_calc_keyword(identifier),
            ComponentKind::Token(ParserTokenKind::Number { value, number_type }) => {
                Ok(Arc::new(CalcNode::Numeric(CalcNumericValue::Number {
                    value: *value,
                    number_type: *number_type as u8,
                })))
            }
            ComponentKind::Token(ParserTokenKind::Percentage { value, .. }) => {
                Ok(Arc::new(CalcNode::Numeric(CalcNumericValue::Percentage(*value))))
            }
            ComponentKind::Token(ParserTokenKind::Dimension { value, unit, .. }) => {
                let numeric = if let Some(unit) = unit_index(unit, &LENGTH_UNIT_NAMES) {
                    CalcNumericValue::Length { value: *value, unit }
                } else if let Some(unit) = unit_index(unit, &ANGLE_UNIT_NAMES) {
                    CalcNumericValue::Angle { value: *value, unit }
                } else if let Some(unit) = unit_index(unit, &FLEX_UNIT_NAMES) {
                    CalcNumericValue::Flex { value: *value, unit }
                } else if let Some(unit) = unit_index(unit, &FREQUENCY_UNIT_NAMES) {
                    CalcNumericValue::Frequency { value: *value, unit }
                } else if let Some(unit) = unit_index(unit, &RESOLUTION_UNIT_NAMES) {
                    CalcNumericValue::Resolution { value: *value, unit }
                } else if let Some(unit) = unit_index(unit, &TIME_UNIT_NAMES) {
                    CalcNumericValue::Time { value: *value, unit }
                } else {
                    return Err(CalcParseError::Invalid);
                };
                Ok(Arc::new(CalcNode::Numeric(numeric)))
            }
            _ => Err(CalcParseError::Invalid),
        }
    }
}

fn parse_calc_keyword(identifier: &[u16]) -> Result<Arc<CalcNode>> {
    let value = if equals_ascii_case_insensitive(identifier, b"e") {
        std::f64::consts::E
    } else if equals_ascii_case_insensitive(identifier, b"pi") {
        std::f64::consts::PI
    } else if equals_ascii_case_insensitive(identifier, b"infinity") {
        f64::INFINITY
    } else if equals_ascii_case_insensitive(identifier, b"-infinity") {
        f64::NEG_INFINITY
    } else if equals_ascii_case_insensitive(identifier, b"nan") {
        f64::NAN
    } else {
        return Err(CalcParseError::Invalid);
    };
    Ok(Arc::new(CalcNode::Numeric(CalcNumericValue::Number {
        value,
        number_type: 0,
    })))
}

fn split_arguments(values: &[ComponentValue]) -> Vec<&[ComponentValue]> {
    values.split(ComponentValue::is_comma).collect()
}

fn parse_argument(values: &[ComponentValue], percentages_resolve_as: Option<u8>) -> Result<Arc<CalcNode>> {
    parse_a_calculation(values, percentages_resolve_as)
}

fn argument_type(node: &Arc<CalcNode>, percentages_resolve_as: Option<u8>) -> Result<CalcNumericType> {
    simplify_parsed_calculation(node.clone(), percentages_resolve_as)
        .map(|(_, numeric_type)| numeric_type)
        .ok_or(CalcParseError::Invalid)
}

fn accepts_number(numeric_type: &CalcNumericType, percentages_resolve_as: Option<u8>) -> bool {
    numeric_type.matches_number(resolve_as_for_value_type(percentages_resolve_as))
}

fn accepts_dimension_or_percentage(numeric_type: &CalcNumericType) -> bool {
    numeric_type.matches_dimension_category() || numeric_type.matches_percentage()
}

fn require_same(types: &[CalcNumericType]) -> Result<()> {
    types
        .windows(2)
        .all(|types| types[0] == types[1])
        .then_some(())
        .ok_or(CalcParseError::Invalid)
}

fn require_consistent(types: &[CalcNumericType]) -> Result<()> {
    let mut determined: Option<CalcNumericType> = None;
    for numeric_type in types {
        determined = Some(match determined {
            Some(previous) => previous.added_to(numeric_type).ok_or(CalcParseError::Invalid)?,
            None => *numeric_type,
        });
    }
    Ok(())
}

fn parse_variadic(
    arguments: &[&[ComponentValue]],
    percentages_resolve_as: Option<u8>,
    constructor: impl FnOnce(Vec<Arc<CalcNode>>) -> CalcNode,
) -> Result<Arc<CalcNode>> {
    if arguments.is_empty() {
        return Err(CalcParseError::Invalid);
    }
    let nodes = arguments
        .iter()
        .map(|argument| parse_argument(argument, percentages_resolve_as))
        .collect::<Result<Vec<_>>>()?;
    let types = nodes
        .iter()
        .map(|node| argument_type(node, percentages_resolve_as))
        .collect::<Result<Vec<_>>>()?;
    if !types.iter().all(accepts_dimension_or_percentage) {
        return Err(CalcParseError::Invalid);
    }
    require_consistent(&types)?;
    Ok(Arc::new(constructor(nodes)))
}

fn parse_fixed_arguments(
    arguments: &[&[ComponentValue]],
    count: usize,
    percentages_resolve_as: Option<u8>,
) -> Result<Vec<Arc<CalcNode>>> {
    if arguments.len() != count {
        return Err(CalcParseError::Invalid);
    }
    arguments
        .iter()
        .map(|argument| parse_argument(argument, percentages_resolve_as))
        .collect()
}

pub(crate) fn parse_a_calc_function_node(
    name: &[u16],
    values: &[ComponentValue],
    percentages_resolve_as: Option<u8>,
) -> Result<Arc<CalcNode>> {
    let function = math_function_from_name(name).ok_or(CalcParseError::NotHandled)?;
    if function == MathFunction::Calc {
        return parse_a_calculation(values, percentages_resolve_as);
    }

    let mut progress_no_clamp = false;
    let mut function_values = values;
    if function == MathFunction::Progress {
        let first_non_whitespace = values.iter().position(|value| !value.is_whitespace());
        if first_non_whitespace.is_some_and(|index| {
            values[index]
                .ident()
                .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"no-clamp"))
        }) {
            progress_no_clamp = true;
            function_values = &values[first_non_whitespace.unwrap() + 1..];
        }
    }
    let arguments = split_arguments(function_values);
    let root = match function {
        MathFunction::Min => parse_variadic(&arguments, percentages_resolve_as, CalcNode::Min)?,
        MathFunction::Max => parse_variadic(&arguments, percentages_resolve_as, CalcNode::Max)?,
        MathFunction::Hypot => parse_variadic(&arguments, percentages_resolve_as, CalcNode::Hypot)?,
        MathFunction::Abs | MathFunction::Sign => {
            let nodes = parse_fixed_arguments(&arguments, 1, percentages_resolve_as)?;
            if !accepts_dimension_or_percentage(&argument_type(&nodes[0], percentages_resolve_as)?) {
                return Err(CalcParseError::Invalid);
            }
            Arc::new(if function == MathFunction::Abs {
                CalcNode::Abs(nodes[0].clone())
            } else {
                CalcNode::Sign(nodes[0].clone())
            })
        }
        MathFunction::Sin | MathFunction::Cos | MathFunction::Tan => {
            let nodes = parse_fixed_arguments(&arguments, 1, percentages_resolve_as)?;
            let numeric_type = argument_type(&nodes[0], percentages_resolve_as)?;
            if !accepts_number(&numeric_type, percentages_resolve_as)
                && !numeric_type.matches_dimension(1, resolve_as_for_value_type(percentages_resolve_as))
            {
                return Err(CalcParseError::Invalid);
            }
            Arc::new(match function {
                MathFunction::Sin => CalcNode::Sin(nodes[0].clone()),
                MathFunction::Cos => CalcNode::Cos(nodes[0].clone()),
                _ => CalcNode::Tan(nodes[0].clone()),
            })
        }
        MathFunction::Asin | MathFunction::Acos | MathFunction::Atan | MathFunction::Sqrt | MathFunction::Exp => {
            let nodes = parse_fixed_arguments(&arguments, 1, percentages_resolve_as)?;
            if !accepts_number(
                &argument_type(&nodes[0], percentages_resolve_as)?,
                percentages_resolve_as,
            ) {
                return Err(CalcParseError::Invalid);
            }
            Arc::new(match function {
                MathFunction::Asin => CalcNode::Asin(nodes[0].clone()),
                MathFunction::Acos => CalcNode::Acos(nodes[0].clone()),
                MathFunction::Atan => CalcNode::Atan(nodes[0].clone()),
                MathFunction::Sqrt => CalcNode::Sqrt(nodes[0].clone()),
                _ => CalcNode::Exp(nodes[0].clone()),
            })
        }
        MathFunction::Atan2 | MathFunction::Pow | MathFunction::Mod | MathFunction::Rem => {
            let nodes = parse_fixed_arguments(&arguments, 2, percentages_resolve_as)?;
            let types = nodes
                .iter()
                .map(|node| argument_type(node, percentages_resolve_as))
                .collect::<Result<Vec<_>>>()?;
            match function {
                MathFunction::Atan2 => {
                    if !types.iter().all(accepts_dimension_or_percentage) {
                        return Err(CalcParseError::Invalid);
                    }
                    require_consistent(&types)?;
                }
                MathFunction::Pow => {
                    if !types
                        .iter()
                        .all(|numeric_type| accepts_number(numeric_type, percentages_resolve_as))
                    {
                        return Err(CalcParseError::Invalid);
                    }
                    require_consistent(&types)?;
                }
                MathFunction::Mod | MathFunction::Rem => {
                    if !types.iter().all(accepts_dimension_or_percentage) {
                        return Err(CalcParseError::Invalid);
                    }
                    require_same(&types)?;
                }
                _ => unreachable!(),
            }
            Arc::new(match function {
                MathFunction::Atan2 => CalcNode::Atan2 {
                    y: nodes[0].clone(),
                    x: nodes[1].clone(),
                },
                MathFunction::Pow => CalcNode::Pow {
                    base: nodes[0].clone(),
                    exponent: nodes[1].clone(),
                },
                MathFunction::Mod => CalcNode::Mod {
                    value: nodes[0].clone(),
                    modulus: nodes[1].clone(),
                },
                _ => CalcNode::Rem {
                    value: nodes[0].clone(),
                    divisor: nodes[1].clone(),
                },
            })
        }
        MathFunction::Clamp | MathFunction::Progress => {
            let nodes = parse_fixed_arguments(&arguments, 3, percentages_resolve_as)?;
            let types = nodes
                .iter()
                .map(|node| argument_type(node, percentages_resolve_as))
                .collect::<Result<Vec<_>>>()?;
            if !types.iter().all(accepts_dimension_or_percentage) {
                return Err(CalcParseError::Invalid);
            }
            require_consistent(&types)?;
            Arc::new(if function == MathFunction::Clamp {
                CalcNode::Clamp {
                    min: nodes[0].clone(),
                    center: nodes[1].clone(),
                    max: nodes[2].clone(),
                }
            } else {
                CalcNode::Progress {
                    no_clamp: progress_no_clamp,
                    progress: nodes[0].clone(),
                    from: nodes[1].clone(),
                    to: nodes[2].clone(),
                }
            })
        }
        MathFunction::Log => {
            if !(1..=2).contains(&arguments.len()) {
                return Err(CalcParseError::Invalid);
            }
            let mut nodes = arguments
                .iter()
                .map(|argument| parse_argument(argument, percentages_resolve_as))
                .collect::<Result<Vec<_>>>()?;
            if nodes.len() == 1 {
                nodes.push(parse_calc_keyword(&"e".encode_utf16().collect::<Vec<_>>())?);
            }
            let types = nodes
                .iter()
                .map(|node| argument_type(node, percentages_resolve_as))
                .collect::<Result<Vec<_>>>()?;
            if !types
                .iter()
                .all(|numeric_type| accepts_number(numeric_type, percentages_resolve_as))
            {
                return Err(CalcParseError::Invalid);
            }
            require_same(&types)?;
            Arc::new(CalcNode::Log {
                value: nodes[0].clone(),
                base: nodes[1].clone(),
            })
        }
        MathFunction::Round => {
            if !(2..=3).contains(&arguments.len()) {
                return Err(CalcParseError::Invalid);
            }
            let (strategy, arguments) = if arguments.len() == 3 {
                let strategy = parse_rounding_strategy(arguments[0]).ok_or(CalcParseError::Invalid)?;
                (strategy, &arguments[1..])
            } else {
                (1, arguments.as_slice())
            };
            let nodes = parse_fixed_arguments(arguments, 2, percentages_resolve_as)?;
            let types = nodes
                .iter()
                .map(|node| argument_type(node, percentages_resolve_as))
                .collect::<Result<Vec<_>>>()?;
            if !types.iter().all(accepts_dimension_or_percentage) {
                return Err(CalcParseError::Invalid);
            }
            require_consistent(&types)?;
            Arc::new(CalcNode::Round {
                strategy,
                value: nodes[0].clone(),
                interval: nodes[1].clone(),
            })
        }
        MathFunction::Random => return Err(CalcParseError::NotHandled),
        MathFunction::Calc => unreachable!(),
    };
    simplify_parsed_calculation(root, percentages_resolve_as)
        .map(|(root, _)| root)
        .ok_or(CalcParseError::Invalid)
}

fn parse_rounding_strategy(values: &[ComponentValue]) -> Option<u8> {
    let mut values = values.iter().filter(|value| !value.is_whitespace());
    let identifier = values.next()?.ident()?;
    if values.next().is_some() {
        return None;
    }
    if equals_ascii_case_insensitive(identifier, b"down") {
        Some(0)
    } else if equals_ascii_case_insensitive(identifier, b"nearest") {
        Some(1)
    } else if equals_ascii_case_insensitive(identifier, b"to-zero") {
        Some(2)
    } else if equals_ascii_case_insensitive(identifier, b"up") {
        Some(3)
    } else {
        None
    }
}

pub(crate) fn parse_a_calculation(
    values: &[ComponentValue],
    percentages_resolve_as: Option<u8>,
) -> Result<Arc<CalcNode>> {
    CalculationParser::new(values, percentages_resolve_as).parse()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_tokenizer::tokenize_for_parser;
    use crate::css::parser::component_value::consume_a_list_of_component_values;

    fn parse(source: &str, percentages_resolve_as: Option<u8>) -> Result<Arc<CalcNode>> {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(source.as_bytes())).unwrap();
        let (name, values) = values[0].function().unwrap();
        parse_a_calc_function_node(name, values, percentages_resolve_as)
    }

    #[test]
    fn parses_and_simplifies_calc_operators() {
        let node = parse("calc(1 + 2 * 3)", None).unwrap();
        assert!(matches!(
            &*node,
            CalcNode::Numeric(CalcNumericValue::Number { value: 7.0, .. })
        ));
        assert!(parse("calc(1 + * 2)", None).is_err());

        let node = parse("calc(1px / 1px * 10em * infinity)", Some(25)).unwrap();
        let CalcNode::Product(children) = &*node else {
            panic!("unresolved length product should remain a product");
        };
        assert_eq!(children.len(), 2);
    }

    #[test]
    fn parses_math_functions_and_checks_types() {
        for source in [
            "min(1px, 2px)",
            "clamp(1%, 2%, 3%)",
            "round(up, 7px, 2px)",
            "sin(90deg)",
            "atan2(1px, 2px)",
            "pow(2, 3)",
            "hypot(3px, 4px)",
            "log(8, 2)",
            "mod(7px, 4px)",
            "progress(no-clamp 1px, 0px, 2px)",
        ] {
            assert!(parse(source, Some(25)).is_ok(), "{source}");
        }
        assert!(parse("min(1px, 2s)", None).is_err());
        assert!(parse("sin(1px)", None).is_err());
    }

    #[test]
    fn defers_non_math_functions_inside_calculations() {
        assert!(matches!(
            parse("calc(anchor-size(width) + 1px)", None),
            Err(CalcParseError::NotHandled)
        ));
    }
}
