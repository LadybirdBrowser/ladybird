/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_tokenizer::{ParserToken, ParserTokenKind};

const MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH: usize = 256;

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum ComponentKind {
    Token(ParserTokenKind),
    Function {
        name: Box<[u16]>,
        values: Box<[ComponentValue]>,
    },
    SimpleBlock {
        opening: ParserTokenKind,
        values: Box<[ComponentValue]>,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ComponentValue {
    pub kind: ComponentKind,
    pub original_source_text: Box<[u16]>,
}

impl ComponentValue {
    pub(crate) fn end_of_file() -> Self {
        Self {
            kind: ComponentKind::Token(ParserTokenKind::EndOfFile),
            original_source_text: Box::new([]),
        }
    }

    pub(crate) fn is_whitespace(&self) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Whitespace))
    }

    pub(crate) fn is_comma(&self) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Comma))
    }

    pub(crate) fn is_colon(&self) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Colon))
    }

    pub(crate) fn is(&self, expected: &ParserTokenKind) -> bool {
        matches!(&self.kind, ComponentKind::Token(kind) if is_same_token_kind(kind, expected))
    }

    pub(crate) fn is_delim(&self, expected: u8) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Delim(value)) if value == u32::from(expected))
    }

    pub(crate) fn ident(&self) -> Option<&[u16]> {
        match &self.kind {
            ComponentKind::Token(ParserTokenKind::Ident(value)) => Some(value),
            _ => None,
        }
    }

    pub(crate) fn string(&self) -> Option<&[u16]> {
        match &self.kind {
            ComponentKind::Token(ParserTokenKind::String(value)) => Some(value),
            _ => None,
        }
    }

    pub(crate) fn function(&self) -> Option<(&[u16], &[ComponentValue])> {
        match &self.kind {
            ComponentKind::Function { name, values } => Some((name, values)),
            _ => None,
        }
    }

    pub(crate) fn square_block(&self) -> Option<&[ComponentValue]> {
        match &self.kind {
            ComponentKind::SimpleBlock {
                opening: ParserTokenKind::OpenSquare,
                values,
            } => Some(values),
            _ => None,
        }
    }
}

fn append_original_source_text(target: &mut Vec<u16>, values: &[ComponentValue]) {
    for value in values {
        target.extend_from_slice(&value.original_source_text);
    }
}

fn is_same_token_kind(left: &ParserTokenKind, right: &ParserTokenKind) -> bool {
    std::mem::discriminant(left) == std::mem::discriminant(right)
}

fn consume_component_values(
    tokens: &[ParserToken],
    position: &mut usize,
    ending: Option<&ParserTokenKind>,
    depth: usize,
) -> Result<Vec<ComponentValue>, ()> {
    if depth > MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH {
        return Err(());
    }

    let mut values = Vec::new();
    while let Some(token) = tokens.get(*position) {
        if ending.is_some_and(|ending| is_same_token_kind(&token.kind, ending)) {
            break;
        }

        *position += 1;
        let mut original_source_text = token.source.to_vec();
        let kind = match &token.kind {
            ParserTokenKind::Function(name) => {
                let function_values =
                    consume_component_values(tokens, position, Some(&ParserTokenKind::CloseParen), depth + 1)?;
                append_original_source_text(&mut original_source_text, &function_values);
                if let Some(closing) = tokens.get(*position)
                    && matches!(closing.kind, ParserTokenKind::CloseParen)
                {
                    closing.source.append_to(&mut original_source_text);
                    *position += 1;
                }
                ComponentKind::Function {
                    name: name.as_ref().to_vec().into_boxed_slice(),
                    values: function_values.into_boxed_slice(),
                }
            }
            ParserTokenKind::OpenSquare | ParserTokenKind::OpenParen | ParserTokenKind::OpenCurly => {
                let ending = match token.kind {
                    ParserTokenKind::OpenSquare => ParserTokenKind::CloseSquare,
                    ParserTokenKind::OpenParen => ParserTokenKind::CloseParen,
                    ParserTokenKind::OpenCurly => ParserTokenKind::CloseCurly,
                    _ => unreachable!(),
                };
                let block_values = consume_component_values(tokens, position, Some(&ending), depth + 1)?;
                append_original_source_text(&mut original_source_text, &block_values);
                if let Some(closing) = tokens.get(*position)
                    && is_same_token_kind(&closing.kind, &ending)
                {
                    closing.source.append_to(&mut original_source_text);
                    *position += 1;
                }
                ComponentKind::SimpleBlock {
                    opening: token.kind.clone(),
                    values: block_values.into_boxed_slice(),
                }
            }
            _ => ComponentKind::Token(token.kind.clone()),
        };
        values.push(ComponentValue {
            kind,
            original_source_text: original_source_text.into_boxed_slice(),
        });
    }
    Ok(values)
}

// https://drafts.csswg.org/css-syntax-3/#consume-list-of-component-values
pub(crate) fn consume_a_list_of_component_values(tokens: &[ParserToken]) -> Result<Vec<ComponentValue>, ()> {
    let mut position = 0;
    consume_component_values(tokens, &mut position, None, 0)
}

#[cfg(test)]
mod tests {
    use super::{ComponentKind, consume_a_list_of_component_values};
    use crate::css::css_tokenizer::{ParserTokenKind, tokenize_for_parser};

    fn parse(input: &str) -> Vec<super::ComponentValue> {
        consume_a_list_of_component_values(&tokenize_for_parser(input.as_bytes())).unwrap()
    }

    fn utf16(value: &str) -> Box<[u16]> {
        value.encode_utf16().collect()
    }

    #[test]
    fn consumes_nested_functions_and_blocks() {
        let values = parse("outer([inner({ value })]) tail");
        assert_eq!(values.len(), 3);
        assert_eq!(values[0].original_source_text, utf16("outer([inner({ value })])"));
        let ComponentKind::Function { name, values } = &values[0].kind else {
            panic!("expected a function");
        };
        assert_eq!(name, &utf16("outer"));
        let ComponentKind::SimpleBlock { values, .. } = &values[0].kind else {
            panic!("expected a square block");
        };
        let ComponentKind::Function { name, values } = &values[0].kind else {
            panic!("expected a nested function");
        };
        assert_eq!(name, &utf16("inner"));
        let ComponentKind::SimpleBlock { values, .. } = &values[0].kind else {
            panic!("expected a nested curly block");
        };
        assert_eq!(values.len(), 3);
    }

    #[test]
    fn retains_bad_string_and_bad_url_tokens() {
        let values = parse("\"bad\n url(foo\"bar)");
        assert!(matches!(
            values[0].kind,
            ComponentKind::Token(ParserTokenKind::BadString)
        ));
        assert!(
            values
                .iter()
                .any(|value| matches!(value.kind, ComponentKind::Token(ParserTokenKind::BadUrl)))
        );
    }

    #[test]
    fn closes_unterminated_constructs_at_eof() {
        let values = parse("function([value");
        assert_eq!(values.len(), 1);
        assert_eq!(values[0].original_source_text, utf16("function([value"));
        let ComponentKind::Function { values, .. } = &values[0].kind else {
            panic!("expected a function");
        };
        assert!(matches!(values[0].kind, ComponentKind::SimpleBlock { .. }));
    }
}
