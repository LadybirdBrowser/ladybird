/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use ak::{GenericLexer, Utf16String};
use libweb_html_tokenizer::tokenizer::{CharacterReferenceContext, decode_character_references};
use libwebcommon_rust::infra::character_types::is_ascii_whitespace;

use super::parser::collect_a_webvtt_timestamp;
use crate::bindings::CallContext;
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::global_object::window_from_global_object;
use crate::css::css_rule::wrap_cpp_cell;

// Each node names its parent by index, with index zero reserved for the fragment.
#[repr(C)]
struct DomNode {
    parent: usize,
    kind: Kind,
    value: usize,
    classes: usize,
}

unsafe extern "C" {
    fn web_vtt_create_fragment(document: *mut c_void, nodes: *const DomNode, length: usize) -> *mut c_void;
}

#[derive(Clone, Copy, PartialEq)]
#[repr(u8)]
enum Kind {
    Text,
    Timestamp,
    Class,
    Italic,
    Bold,
    Underline,
    Ruby,
    RubyText,
    Voice,
    Language,
}

impl Kind {
    fn from_tag(name: &[u16]) -> Option<Self> {
        // The longest recognized tag name is four ASCII characters.
        if name.len() > 4 {
            return None;
        }
        Some(match String::from_utf16(name).ok()?.as_str() {
            "c" => Self::Class,
            "i" => Self::Italic,
            "b" => Self::Bold,
            "u" => Self::Underline,
            "ruby" => Self::Ruby,
            "rt" => Self::RubyText,
            "v" => Self::Voice,
            "lang" => Self::Language,
            _ => return None,
        })
    }
}

struct Node {
    kind: Kind,
    parent: usize,
    value: Utf16String,
    classes: Utf16String,
}

fn normalized_annotation(input: &[u16]) -> Utf16String {
    // https://w3c.github.io/webvtt/#consume-an-html-character-reference
    // AD-HOC: WebVTT says this context is 'not "as part of an attribute"' for missing semicolons. All three other
    // browser engines keep "&notit;" literal in voice annotations, so decode annotations in the attribute context.
    let decoded = decode_character_references(input, CharacterReferenceContext::Attribute);
    let words: Vec<_> = decoded
        .split(|unit| is_ascii_whitespace((*unit).into()))
        .filter(|word| !word.is_empty())
        .collect();
    Utf16String::from_utf16(&words.join(&u16::from(b' ')))
}

fn timestamp(input: &[u16]) -> Option<Utf16String> {
    let mut lexer = GenericLexer::new(input);
    collect_a_webvtt_timestamp(&mut lexer)?;
    if !lexer.is_eof() {
        return None;
    }
    // Keep the digits so rounding a floating-point time cannot change the milliseconds.
    let mut parts = input.split(|unit| *unit == u16::from(b':'));
    let first = parts.next()?;
    let second = parts.next()?;
    let mut formatted = Vec::new();
    if let Some(seconds) = parts.next() {
        let first_nonzero = first
            .iter()
            .position(|unit| *unit != u16::from(b'0'))
            .unwrap_or(first.len() - 1);
        let hours = &first[first_nonzero..];
        if hours.len() == 1 {
            formatted.push(u16::from(b'0'));
        }
        formatted.extend_from_slice(hours);
        formatted.push(u16::from(b':'));
        formatted.extend_from_slice(second);
        formatted.push(u16::from(b':'));
        formatted.extend_from_slice(seconds);
    } else {
        formatted.extend("00:".encode_utf16());
        formatted.extend_from_slice(input);
    }
    Some(Utf16String::from_utf16(&formatted))
}

enum Token<'a> {
    Text(Utf16String),
    Start {
        name: &'a [u16],
        classes: Vec<&'a [u16]>,
        annotation: Utf16String,
    },
    End(&'a [u16]),
    Timestamp(&'a [u16]),
}

fn take_until<'a>(input: &mut &'a [u16], delimiter: impl Fn(u16) -> bool) -> &'a [u16] {
    let end = input.iter().position(|&unit| delimiter(unit)).unwrap_or(input.len());
    let (value, rest) = input.split_at(end);
    *input = rest;
    value
}

// https://w3c.github.io/webvtt/#webvtt-cue-text-tokenizer
fn next_token<'a>(input: &mut &'a [u16]) -> Token<'a> {
    // 2. Let tokenizer state be WebVTT data state.
    // NB: A run of data ends immediately before the next tag. Only ampersands enter the HTML character reference state.
    if !input.starts_with(&[u16::from(b'<')]) {
        let text = take_until(input, |unit| unit == u16::from(b'<'));
        return Token::Text(Utf16String::from_utf16(&decode_character_references(
            text,
            CharacterReferenceContext::Data,
        )));
    }

    // WebVTT tag state
    *input = &input[1..];
    let mut tag = take_until(input, |unit| unit == u16::from(b'>'));
    if !input.is_empty() {
        *input = &input[1..];
    }

    // WebVTT end tag state
    if let Some(name) = tag.strip_prefix(&[u16::from(b'/')]) {
        return Token::End(name);
    }
    // WebVTT timestamp tag state
    if tag.first().is_some_and(|unit| (0x30..=0x39).contains(unit)) {
        return Token::Timestamp(tag);
    }

    // WebVTT start tag state
    // NB: CR belongs to the name or class. The annotation itself uses all ASCII whitespace.
    let ends_name_or_class = |unit| matches!(unit, 0x09 | 0x0a | 0x0c | 0x20 | 0x2e);
    let name = take_until(&mut tag, ends_name_or_class);

    // WebVTT start tag class state
    let mut classes = Vec::new();
    while let Some(rest) = tag.strip_prefix(&[u16::from(b'.')]) {
        tag = rest;
        classes.push(take_until(&mut tag, ends_name_or_class));
    }

    // WebVTT start tag annotation state
    let annotation = normalized_annotation(tag);
    Token::Start {
        name,
        classes,
        annotation,
    }
}

// https://w3c.github.io/webvtt/#webvtt-cue-text-parsing-rules
fn parse(mut input: &[u16]) -> Vec<Node> {
    // 3. Let result be a list of WebVTT Node Objects, initially empty.
    let mut result: Vec<Node> = Vec::new();
    // 4. Let current be the WebVTT Internal Node Object result.
    let mut current = 0;
    // NB: For getCueAsHTML(), the DOM's lang attributes represent the language scopes.

    // 7. Loop: If position is past the end of input, return result and abort these steps.
    while !input.is_empty() {
        // 8. Let token be the result of invoking the WebVTT cue text tokenizer.
        // 9. Run the appropriate steps given the type of token:
        match next_token(&mut input) {
            Token::Text(value) => {
                // Append the newly created WebVTT Text Object to current.
                result.push(Node {
                    kind: Kind::Text,
                    parent: current,
                    value,
                    classes: Utf16String::default(),
                });
            }
            Token::Start {
                name,
                mut classes,
                annotation,
            } => {
                let Some(kind) = Kind::from_tag(name) else {
                    continue;
                };
                // If current is a WebVTT Ruby Object, then attach a WebVTT Ruby Text Object.
                if kind == Kind::RubyText && (current == 0 || result[current - 1].kind != Kind::Ruby) {
                    continue;
                }
                // Set the new object's list of applicable classes to the list of classes in the token,
                // excluding any classes that are the empty string.
                classes.retain(|class| !class.is_empty());
                let classes = Utf16String::from_utf16(&classes.join(&u16::from(b' ')));
                let value = if matches!(kind, Kind::Voice | Kind::Language) {
                    annotation
                } else {
                    Utf16String::default()
                };
                // Append the newly created node object to current.
                result.push(Node {
                    kind,
                    parent: current,
                    value,
                    classes,
                });
                // Let current be the newly created node object.
                current = result.len();
            }
            Token::End(name) => {
                if current == 0 {
                    continue;
                }
                let node = &result[current - 1];
                if let Some(kind) = Kind::from_tag(name) {
                    if kind == node.kind {
                        current = node.parent;
                    } else if kind == Kind::Ruby && node.kind == Kind::RubyText {
                        current = result[node.parent - 1].parent;
                    }
                }
            }
            Token::Timestamp(text) => {
                // Collect a WebVTT timestamp.
                if let Some(value) = timestamp(text) {
                    result.push(Node {
                        kind: Kind::Timestamp,
                        parent: current,
                        value,
                        classes: Utf16String::default(),
                    });
                }
            }
        }
    }
    result
}

pub(super) fn create_fragment(cx: &CallContext, text: &Utf16String) -> CppInterface {
    let nodes = parse(&text.to_utf16());
    let dom: Vec<_> = nodes
        .iter()
        .map(|node| DomNode {
            parent: node.parent,
            kind: node.kind,
            value: node.value.raw_identity(),
            classes: node.classes.raw_identity(),
        })
        .collect();
    // https://w3c.github.io/webvtt/#dom-vttcue-getcueashtml
    // AD-HOC: The spec uses the entry document. Use the cue's document so borrowing the method across windows
    // preserves the owner document of the fragment and all of its children.
    let window = window_from_global_object(cx.realm().global_object()).expect("VTTCue is exposed only on Window");
    let document = window.associated_document();
    // SAFETY: The document and the strings in nodes stay live while LibWeb builds and returns the fragment.
    let fragment = unsafe { CppCell::from_raw(web_vtt_create_fragment(document.as_ptr(), dom.as_ptr(), dom.len())) };
    wrap_cpp_cell(cx, &fragment)
}
