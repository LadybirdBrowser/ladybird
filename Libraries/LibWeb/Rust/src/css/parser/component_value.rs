/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_tokenizer::{
    ParserSource, ParserString, ParserToken, ParserTokenKind, SmallParserTokenList, SourcePosition,
};
use smallvec::SmallVec;

const MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH: usize = 256;

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum ComponentKind {
    Token(ParserTokenKind),
    Function {
        name: ParserString,
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
    pub original_source_text: ParserSource,
    pub opening_source_length: usize,
    pub closing_source_length: usize,
    pub start_position: SourcePosition,
    pub end_position: SourcePosition,
}

impl ComponentValue {
    pub(crate) fn end_of_file() -> Self {
        Self {
            kind: ComponentKind::Token(ParserTokenKind::EndOfFile),
            original_source_text: ParserSource::empty(),
            opening_source_length: 0,
            closing_source_length: 0,
            start_position: SourcePosition::default(),
            end_position: SourcePosition::default(),
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
            ComponentKind::Token(ParserTokenKind::Ident(value)) => Some(value.as_ref()),
            _ => None,
        }
    }

    pub(crate) fn string(&self) -> Option<&[u16]> {
        match &self.kind {
            ComponentKind::Token(ParserTokenKind::String(value)) => Some(value.as_ref()),
            _ => None,
        }
    }

    pub(crate) fn function(&self) -> Option<(&[u16], &[ComponentValue])> {
        match &self.kind {
            ComponentKind::Function { name, values } => Some((name.as_ref(), values)),
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
        value.original_source_text.append_to(target);
    }
}

fn is_same_token_kind(left: &ParserTokenKind, right: &ParserTokenKind) -> bool {
    std::mem::discriminant(left) == std::mem::discriminant(right)
}

fn consume_component_values(
    tokens: &mut [ParserToken],
    position: &mut usize,
    ending: Option<&ParserTokenKind>,
    depth: usize,
    take_tokens: bool,
) -> Result<Vec<ComponentValue>, ()> {
    if depth > MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH {
        return Err(());
    }

    let mut values = Vec::with_capacity(4);
    while let Some(token) = tokens.get(*position) {
        if ending.is_some_and(|ending| is_same_token_kind(&token.kind, ending)) {
            break;
        }

        values.push(consume_a_component_value(tokens, position, depth, take_tokens)?);
    }
    Ok(values)
}

pub(crate) fn consume_a_component_value(
    tokens: &mut [ParserToken],
    position: &mut usize,
    depth: usize,
    take_tokens: bool,
) -> Result<ComponentValue, ()> {
    if depth > MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH {
        return Err(());
    }
    let token = if take_tokens {
        let token = tokens.get_mut(*position).ok_or(())?;
        std::mem::replace(
            token,
            ParserToken {
                kind: ParserTokenKind::EndOfFile,
                source: ParserSource::empty(),
                start_position: SourcePosition::default(),
                end_position: SourcePosition::default(),
            },
        )
    } else {
        tokens.get(*position).ok_or(())?.clone()
    };
    *position += 1;
    let opening_source_length = token.source.len();
    let mut closing_source_length = 0;
    let start_position = token.start_position;
    let mut end_position = token.end_position;
    let original_source_text;
    let kind = match token.kind {
        ParserTokenKind::Function(name) => {
            let function_values = consume_component_values(
                tokens,
                position,
                Some(&ParserTokenKind::CloseParen),
                depth + 1,
                take_tokens,
            )?;
            let last_child_source = function_values.last().map(|value| &value.original_source_text);
            let mut closing_source = None;
            if let Some(closing) = tokens.get(*position)
                && matches!(closing.kind, ParserTokenKind::CloseParen)
            {
                let closing = &tokens[*position];
                *position += 1;
                closing_source = Some(&closing.source);
                closing_source_length = closing.source.len();
                end_position = closing.end_position;
            }
            original_source_text = ParserSource::covering(
                &token.source,
                closing_source.or(last_child_source).unwrap_or(&token.source),
            )
            .unwrap_or_else(|| {
                let mut source = token.source.to_vec();
                append_original_source_text(&mut source, &function_values);
                if let Some(closing_source) = closing_source {
                    closing_source.append_to(&mut source);
                }
                ParserSource::Owned(source.into_boxed_slice())
            });
            ComponentKind::Function {
                name,
                values: function_values.into_boxed_slice(),
            }
        }
        opening @ (ParserTokenKind::OpenSquare | ParserTokenKind::OpenParen | ParserTokenKind::OpenCurly) => {
            let ending = match opening {
                ParserTokenKind::OpenSquare => ParserTokenKind::CloseSquare,
                ParserTokenKind::OpenParen => ParserTokenKind::CloseParen,
                ParserTokenKind::OpenCurly => ParserTokenKind::CloseCurly,
                _ => unreachable!(),
            };
            let block_values = consume_component_values(tokens, position, Some(&ending), depth + 1, take_tokens)?;
            let last_child_source = block_values.last().map(|value| &value.original_source_text);
            let mut closing_source = None;
            if let Some(closing) = tokens.get(*position)
                && is_same_token_kind(&closing.kind, &ending)
            {
                let closing = &tokens[*position];
                *position += 1;
                closing_source = Some(&closing.source);
                closing_source_length = closing.source.len();
                end_position = closing.end_position;
            }
            original_source_text = ParserSource::covering(
                &token.source,
                closing_source.or(last_child_source).unwrap_or(&token.source),
            )
            .unwrap_or_else(|| {
                let mut source = token.source.to_vec();
                append_original_source_text(&mut source, &block_values);
                if let Some(closing_source) = closing_source {
                    closing_source.append_to(&mut source);
                }
                ParserSource::Owned(source.into_boxed_slice())
            });
            ComponentKind::SimpleBlock {
                opening,
                values: block_values.into_boxed_slice(),
            }
        }
        kind => {
            original_source_text = token.source;
            ComponentKind::Token(kind)
        }
    };
    Ok(ComponentValue {
        kind,
        original_source_text,
        opening_source_length,
        closing_source_length,
        start_position,
        end_position,
    })
}

// https://drafts.csswg.org/css-syntax-3/#consume-list-of-component-values
pub(crate) trait ComponentValueTokens {
    fn into_owned_tokens(self) -> Vec<ParserToken>;
}

impl ComponentValueTokens for Vec<ParserToken> {
    fn into_owned_tokens(self) -> Vec<ParserToken> {
        self
    }
}

impl ComponentValueTokens for &[ParserToken] {
    fn into_owned_tokens(self) -> Vec<ParserToken> {
        self.to_vec()
    }
}

pub(crate) fn consume_a_list_of_component_values(tokens: impl ComponentValueTokens) -> Result<Vec<ComponentValue>, ()> {
    let mut tokens = tokens.into_owned_tokens();
    let mut position = 0;
    consume_component_values(&mut tokens, &mut position, None, 0, true)
}

pub(crate) fn consume_a_small_list_of_component_values(
    mut tokens: SmallParserTokenList,
) -> Result<SmallVec<[ComponentValue; 8]>, ()> {
    let mut values = SmallVec::new();
    let mut position = 0;
    while position < tokens.len() {
        values.push(consume_a_component_value(&mut tokens, &mut position, 0, true)?);
    }
    Ok(values)
}

#[cfg(test)]
mod tests {
    use super::{ComponentKind, consume_a_list_of_component_values};
    use crate::css::css_tokenizer::{ParserTokenKind, tokenize_for_parser};

    fn parse(input: &str) -> Vec<super::ComponentValue> {
        consume_a_list_of_component_values(tokenize_for_parser(input.as_bytes())).unwrap()
    }

    fn utf16(value: &str) -> Box<[u16]> {
        value.encode_utf16().collect()
    }

    #[test]
    fn consumes_nested_functions_and_blocks() {
        let values = parse("outer([inner({ value })]) tail");
        assert_eq!(values.len(), 3);
        assert_eq!(
            values[0].original_source_text.to_vec(),
            utf16("outer([inner({ value })])").as_ref()
        );
        let ComponentKind::Function { name, values } = &values[0].kind else {
            panic!("expected a function");
        };
        assert_eq!(name.as_ref(), utf16("outer").as_ref());
        let ComponentKind::SimpleBlock { values, .. } = &values[0].kind else {
            panic!("expected a square block");
        };
        let ComponentKind::Function { name, values } = &values[0].kind else {
            panic!("expected a nested function");
        };
        assert_eq!(name.as_ref(), utf16("inner").as_ref());
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
        assert_eq!(
            values[0].original_source_text.to_vec(),
            utf16("function([value").as_ref()
        );
        let ComponentKind::Function { values, .. } = &values[0].kind else {
            panic!("expected a function");
        };
        assert!(matches!(values[0].kind, ComponentKind::SimpleBlock { .. }));
    }
}
