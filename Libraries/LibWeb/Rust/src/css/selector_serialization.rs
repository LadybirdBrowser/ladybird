/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS selector serialization.

use std::ffi::c_void;

use super::selector::{
    AnPlusBPattern, AttributeCaseType, AttributeMatchType, Combinator, CompiledSelector, FfiStringView, NamespaceType,
    PseudoClassParameterType, PseudoElementValue, QualifiedName, RustSelector, SimpleSelector,
};
use super::serialize::{StringUnits, TextSink, serialize_a_string, serialize_an_identifier};

struct NamespaceContext<'a> {
    has_default_namespace: bool,
    prefixes_mapping_to_default: &'a [&'a [u16]],
}

fn push_units(sink: &mut TextSink, units: &[u16]) {
    for &unit in units {
        sink.push_code_unit(unit);
    }
}

fn serialize_identifier(sink: &mut TextSink, value: &[u16]) {
    serialize_an_identifier(sink, &StringUnits::Utf16(value));
}

fn maps_to_default(qualified_name: &QualifiedName, context: &NamespaceContext<'_>) -> bool {
    context.has_default_namespace
        && qualified_name.namespace_type == NamespaceType::Named
        && context
            .prefixes_mapping_to_default
            .iter()
            .any(|prefix| *prefix == qualified_name.namespace.as_ref())
}

fn should_skip_universal(qualified_name: &QualifiedName, context: &NamespaceContext<'_>) -> bool {
    qualified_name.namespace_type == NamespaceType::Default
        || (qualified_name.namespace_type == NamespaceType::Any && !context.has_default_namespace)
        || maps_to_default(qualified_name, context)
}

fn serialize_qualified_name(
    sink: &mut TextSink,
    qualified_name: &QualifiedName,
    is_universal: bool,
    context: &NamespaceContext<'_>,
) {
    if qualified_name.namespace_type == NamespaceType::Named && !maps_to_default(qualified_name, context) {
        serialize_identifier(sink, &qualified_name.namespace);
        sink.push_ascii("|");
    } else if qualified_name.namespace_type == NamespaceType::Any && context.has_default_namespace {
        sink.push_ascii("*|");
    }
    if qualified_name.namespace_type == NamespaceType::None {
        sink.push_ascii("|");
    }
    if is_universal {
        sink.push_ascii("*");
    } else {
        serialize_identifier(sink, &qualified_name.name);
    }
}

fn serialize_an_plus_b(sink: &mut TextSink, pattern: AnPlusBPattern) {
    if pattern.step_size == 0 {
        sink.push_ascii(&pattern.offset.to_string());
        return;
    }
    match pattern.step_size {
        1 => sink.push_ascii("n"),
        -1 => sink.push_ascii("-n"),
        value => {
            sink.push_ascii(&value.to_string());
            sink.push_ascii("n");
        }
    }
    if pattern.offset > 0 {
        sink.push_ascii("+");
        sink.push_ascii(&pattern.offset.to_string());
    } else if pattern.offset < 0 {
        sink.push_ascii(&pattern.offset.to_string());
    }
}

fn serialize_selector_list(
    sink: &mut TextSink,
    selectors: &[std::rc::Rc<CompiledSelector>],
    context: &NamespaceContext<'_>,
) {
    for (index, selector) in selectors.iter().enumerate() {
        if index != 0 {
            sink.push_ascii(", ");
        }
        serialize_selector(sink, selector, context);
    }
}

fn serialize_simple_selector(sink: &mut TextSink, selector: &SimpleSelector, context: &NamespaceContext<'_>) {
    match selector {
        SimpleSelector::Universal(qualified_name) => serialize_qualified_name(sink, qualified_name, true, context),
        SimpleSelector::TagName(qualified_name) => serialize_qualified_name(sink, qualified_name, false, context),
        SimpleSelector::Id(name) => {
            sink.push_ascii("#");
            serialize_identifier(sink, &name.name);
        }
        SimpleSelector::Class(name) => {
            sink.push_ascii(".");
            serialize_identifier(sink, &name.name);
        }
        SimpleSelector::Attribute(attribute) => {
            sink.push_ascii("[");
            match attribute.qualified_name.namespace_type {
                NamespaceType::Named => {
                    serialize_identifier(sink, &attribute.qualified_name.namespace);
                    sink.push_ascii("|");
                }
                NamespaceType::Any => sink.push_ascii("*|"),
                NamespaceType::Default | NamespaceType::None => {}
            }
            serialize_identifier(sink, &attribute.qualified_name.name);
            if attribute.match_type != AttributeMatchType::HasAttribute {
                sink.push_ascii(match attribute.match_type {
                    AttributeMatchType::HasAttribute => unreachable!(),
                    AttributeMatchType::ExactValue => "=",
                    AttributeMatchType::ContainsWord => "~=",
                    AttributeMatchType::ContainsString => "*=",
                    AttributeMatchType::StartsWithSegment => "|=",
                    AttributeMatchType::StartsWithString => "^=",
                    AttributeMatchType::EndsWithString => "$=",
                });
                serialize_a_string(sink, &StringUnits::Utf16(&attribute.value));
                match attribute.case_type {
                    AttributeCaseType::Sensitive => sink.push_ascii(" s"),
                    AttributeCaseType::Insensitive => sink.push_ascii(" i"),
                    AttributeCaseType::Default => {}
                }
            }
            sink.push_ascii("]");
        }
        SimpleSelector::PseudoClass(pseudo_class) => {
            sink.push_ascii(":");
            sink.push_ascii(pseudo_class.pseudo_class.name());
            let metadata = pseudo_class.pseudo_class.metadata();
            let has_arguments = metadata.is_valid_as_function
                && (!metadata.is_valid_as_identifier
                    || !pseudo_class.argument_selector_list.is_empty()
                    || !pseudo_class.levels.is_empty());
            if !has_arguments {
                return;
            }
            sink.push_ascii("(");
            match metadata.parameter_type {
                PseudoClassParameterType::None => {}
                PseudoClassParameterType::AnPlusB => {
                    serialize_an_plus_b(sink, pseudo_class.an_plus_b_pattern);
                }
                PseudoClassParameterType::AnPlusBOf => {
                    serialize_an_plus_b(sink, pseudo_class.an_plus_b_pattern);
                    if !pseudo_class.argument_selector_list.is_empty() {
                        sink.push_ascii(" of ");
                        serialize_selector_list(sink, &pseudo_class.argument_selector_list, context);
                    }
                }
                PseudoClassParameterType::CompoundSelector
                | PseudoClassParameterType::ForgivingSelectorList
                | PseudoClassParameterType::ForgivingRelativeSelectorList
                | PseudoClassParameterType::RelativeSelectorList
                | PseudoClassParameterType::SelectorList => {
                    serialize_selector_list(sink, &pseudo_class.argument_selector_list, context);
                }
                PseudoClassParameterType::Ident => {
                    serialize_identifier(sink, pseudo_class.identifier.as_ref().unwrap());
                }
                PseudoClassParameterType::LanguageRanges => {
                    for (index, language) in pseudo_class.languages.iter().enumerate() {
                        if index != 0 {
                            sink.push_ascii(", ");
                        }
                        if language.is_string {
                            serialize_a_string(sink, &StringUnits::Utf16(&language.value));
                        } else {
                            serialize_identifier(sink, &language.value);
                        }
                    }
                }
                PseudoClassParameterType::LevelList => {
                    for (index, level) in pseudo_class.levels.iter().enumerate() {
                        if index != 0 {
                            sink.push_ascii(", ");
                        }
                        sink.push_ascii(&level.to_string());
                    }
                }
            }
            sink.push_ascii(")");
        }
        SimpleSelector::PseudoElement(pseudo_element) => {
            sink.push_ascii("::");
            if let Some(name) = &pseudo_element.serialized_name {
                serialize_identifier(sink, name);
            } else {
                sink.push_ascii(pseudo_element.pseudo_element.name());
            }
            match &pseudo_element.value {
                PseudoElementValue::None => {}
                PseudoElementValue::CompoundSelector(selector) => {
                    sink.push_ascii("(");
                    serialize_selector(sink, selector, context);
                    sink.push_ascii(")");
                }
                PseudoElementValue::Identifiers(identifiers) => {
                    sink.push_ascii("(");
                    for (index, identifier) in identifiers.iter().enumerate() {
                        if index != 0 {
                            sink.push_ascii(" ");
                        }
                        serialize_identifier(sink, identifier);
                    }
                    sink.push_ascii(")");
                }
                PseudoElementValue::TransitionName { is_universal, value } => {
                    sink.push_ascii("(");
                    if *is_universal {
                        sink.push_ascii("*");
                    } else {
                        serialize_identifier(sink, value);
                    }
                    sink.push_ascii(")");
                }
            }
        }
        SimpleSelector::Nesting => sink.push_ascii("&"),
        SimpleSelector::Invalid(source) => push_units(sink, source),
    }
}

fn serialize_selector(sink: &mut TextSink, selector: &CompiledSelector, context: &NamespaceContext<'_>) {
    if let Some(first) = selector.compound_selectors.first() {
        sink.push_ascii(match first.combinator {
            Combinator::ImmediateChild => "> ",
            Combinator::NextSibling => "+ ",
            Combinator::SubsequentSibling => "~ ",
            Combinator::Column => "|| ",
            Combinator::None | Combinator::Descendant | Combinator::PseudoElement => "",
        });
    }
    for (index, compound) in selector.compound_selectors.iter().enumerate() {
        if compound.simple_selectors.len() == 1
            && let SimpleSelector::Universal(qualified_name) = &compound.simple_selectors[0]
        {
            let followed_by_pseudo = selector
                .compound_selectors
                .get(index + 1)
                .is_some_and(|next| next.combinator == Combinator::PseudoElement);
            if !(compound.is_implicit_universal_anchor
                || followed_by_pseudo && should_skip_universal(qualified_name, context))
            {
                serialize_simple_selector(sink, &compound.simple_selectors[0], context);
            }
        } else {
            for simple in &compound.simple_selectors {
                if let SimpleSelector::Universal(qualified_name) = simple
                    && (compound.is_implicit_universal_anchor || should_skip_universal(qualified_name, context))
                {
                    continue;
                }
                serialize_simple_selector(sink, simple, context);
            }
        }
        if let Some(next) = selector.compound_selectors.get(index + 1) {
            sink.push_ascii(match next.combinator {
                Combinator::Descendant => " ",
                Combinator::ImmediateChild => " > ",
                Combinator::NextSibling => " + ",
                Combinator::SubsequentSibling => " ~ ",
                Combinator::Column => " || ",
                Combinator::None | Combinator::PseudoElement => "",
            });
        }
    }
}

#[repr(C)]
pub struct FfiSelectorSerializedText {
    pub data: *const u16,
    pub length: usize,
    pub storage: *mut c_void,
}

/// # Safety
/// `selector` and all prefix views must remain valid for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_serialize(
    selector: *const RustSelector,
    has_default_namespace: bool,
    prefixes_mapping_to_default: *const FfiStringView,
    prefix_count: usize,
) -> FfiSelectorSerializedText {
    unsafe {
        assert!(!selector.is_null());
        let prefix_views = if prefix_count == 0 {
            &[]
        } else {
            assert!(!prefixes_mapping_to_default.is_null());
            std::slice::from_raw_parts(prefixes_mapping_to_default, prefix_count)
        };
        let prefixes = prefix_views
            .iter()
            .map(|prefix| {
                if prefix.length == 0 {
                    &[][..]
                } else {
                    assert!(!prefix.data.is_null());
                    std::slice::from_raw_parts(prefix.data, prefix.length)
                }
            })
            .collect::<Vec<_>>();
        let context = NamespaceContext {
            has_default_namespace,
            prefixes_mapping_to_default: &prefixes,
        };
        let mut sink = TextSink::new();
        serialize_selector(&mut sink, (*selector).compiled(), &context);
        let storage = Box::new(sink.into_utf16());
        let result = FfiSelectorSerializedText {
            data: storage.as_ptr(),
            length: storage.len(),
            storage: std::ptr::null_mut(),
        };
        FfiSelectorSerializedText {
            storage: Box::into_raw(storage).cast(),
            ..result
        }
    }
}

/// # Safety
/// `storage` must be null or a pointer returned in `FfiSelectorSerializedText` that has not been released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_serialized_text_release(storage: *mut c_void) {
    unsafe {
        if !storage.is_null() {
            drop(Box::from_raw(storage.cast::<Vec<u16>>()));
        }
    }
}
