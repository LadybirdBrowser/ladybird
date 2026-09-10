/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{PageSelectorList, ParsedRuleChild, ParsedRuleKind, ParsedStyleSheet, parse_shared_stylesheet};
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::query_parser::css_query_evaluate_supports;
use crate::css::parser::syntax::{SyntaxNode, SyntaxType};
use crate::css::selector_parser::StyleNestingParent;
use crate::css::serialize::serialize_style_value_to_utf16;
use crate::css::style_rule::StyleRule;
use std::sync::Arc;

fn declarations(source: &str) -> crate::css::declaration_block::DeclarationBlock {
    let source = utf16(source);
    let contexts = [super::RuleContext::Style as u8];
    unsafe {
        let parse = std::sync::Arc::from_raw(super::rust_parse_css_block_syntax(
            view(&source),
            contexts.as_ptr(),
            contexts.len(),
            &super::tests::parse_context(),
            false,
        ));
        *Box::from_raw(super::rust_css_syntax_parse_declaration_block(&*parse))
    }
}

fn first_value(block: &crate::css::declaration_block::DeclarationBlock) -> Vec<u16> {
    serialize_style_value_to_utf16(&block.data().properties[0].value).unwrap()
}

fn utf16(value: &str) -> Vec<u16> {
    value.encode_utf16().collect()
}

fn view(source: &[u16]) -> FfiUtf16View {
    FfiUtf16View {
        utf16: source.as_ptr(),
        length: source.len(),
        ..Default::default()
    }
}

fn parse(source: &[u16], context: &crate::css::parser::value_parser::ParseContext) -> Arc<ParsedStyleSheet> {
    unsafe { parse_shared_stylesheet(TokenizerInput::Utf16(source), context) }
}

fn parse_shared_on_worker(source: &'static str) -> Arc<ParsedStyleSheet> {
    std::thread::spawn(move || {
        let source = utf16(source);
        let context = super::tests::parse_context();
        let parse = || unsafe { parse_shared_stylesheet(TokenizerInput::Utf16(&source), &context) };
        let first = parse();
        let second = parse();
        assert!(Arc::ptr_eq(&first, &second));
        drop(first);
        assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
        second
    })
    .join()
    .unwrap()
}

#[test]
fn builder_constructs_one_rule_tree_with_dense_unique_identities() {
    fn visit(rule: &Arc<super::ParsedRule>, identities: &mut std::collections::BTreeSet<u64>) {
        assert_eq!(Arc::strong_count(rule), 1);
        assert!(identities.insert(rule.local_identity));
        for child in &rule.children {
            match child {
                ParsedRuleChild::Rule(rule) => visit(rule, identities),
                ParsedRuleChild::Declarations(declarations) => assert!(identities.insert(declarations.local_identity)),
            }
        }
    }
    let rules = super::parse_stylesheet(
        b"@media all { .parent { & {} color: red; @supports (display: grid) { width: 13px; } } } @font-face { font-family: Test; src: url(font.woff); }",
    );
    let context = super::tests::parse_context();
    let mut builder = super::SyntaxParseBuilder::new(&context, false);
    builder.append_roots(&rules);
    assert_eq!(builder.rules.len(), 2);
    let mut identities = std::collections::BTreeSet::new();
    for rule in &builder.rules {
        visit(rule, &mut identities);
    }
    assert_eq!(
        identities.into_iter().collect::<Vec<_>>(),
        (0..builder.identity_count).collect::<Vec<_>>()
    );
    let font = &builder.rules[1];
    let ParsedRuleChild::Declarations(declarations) = &font.children[0] else {
        panic!("expected descriptors")
    };
    assert!(Arc::ptr_eq(
        font.descriptor_block.as_ref().unwrap(),
        declarations.descriptor_block.as_ref().unwrap()
    ));
}

#[test]
fn style_rule_mutation_forks_current_selectors_and_declarations() {
    let sheet = parse_shared_on_worker(".親 { width: 13px; --色: 緑; }");
    assert_eq!(sheet.rules.len(), 1);
    let parsed = &sheet.rules[0];
    let rule = StyleRule::new(
        parsed.selector_list.clone().unwrap(),
        parsed.declaration_block.clone().unwrap(),
    );
    let independent = StyleRule::new(
        parsed.selector_list.clone().unwrap(),
        parsed.declaration_block.clone().unwrap(),
    );
    drop(sheet);
    let shared = StyleRule::new(rule.selectors(), rule.declarations.data());
    let original_selectors = rule.selectors();
    assert!(Arc::ptr_eq(&original_selectors, &shared.selectors()));
    let mut block = rule.declarations.clone();
    let retained_block = rule.declarations.clone();
    block.replace(&declarations("width: 31px; --色: 青"));
    assert_eq!(block.identity(), retained_block.identity());
    assert_eq!(first_value(&retained_block), utf16("31px"));
    assert!(rule.set_selector_text(TokenizerInput::Utf16(&utf16(".子")), &[], StyleNestingParent::Style));
    let changed_selectors = rule.selectors();
    assert!(!Arc::ptr_eq(&changed_selectors, &original_selectors));
    assert!(!rule.set_selector_text(TokenizerInput::Utf16(&utf16("[")), &[], StyleNestingParent::None));
    assert!(Arc::ptr_eq(&changed_selectors, &rule.selectors()));
    let snapshot = StyleRule::new(rule.selectors(), rule.declarations.data());
    assert!(Arc::ptr_eq(&changed_selectors, &snapshot.selectors()));
    for original in [&shared, &independent] {
        assert!(Arc::ptr_eq(&original_selectors, &original.selectors()));
        assert_eq!(first_value(&original.declarations), utf16("13px"));
    }
    drop(rule);
    block.replace(&declarations("width: 47px"));
    assert_eq!(first_value(&snapshot.declarations), utf16("31px"));
    assert_eq!(first_value(&retained_block), utf16("47px"));
}

#[test]
fn counter_style_mutation_preserves_names_and_descriptor_snapshots() {
    use crate::css::counter_style::*;
    use crate::css::descriptor_block::{rust_descriptor_block_remove, rust_descriptor_block_set};
    let sheet = parse_shared_on_worker("@counter-style 初 { prefix: 'A'; } @counter-style 別 { prefix: 'B'; }");
    let rule = FfiCounterStyle::new(sheet.rules[0].counter_style.clone().unwrap());
    let other = FfiCounterStyle::new(sheet.rules[1].counter_style.clone().unwrap());
    drop(sheet);
    assert_eq!(rule.name().units(), utf16("初"));
    unsafe {
        let snapshot = |rule: &FfiCounterStyle| {
            FfiCounterStyle::new(Arc::new(CounterStyleData {
                name: rule.name(),
                descriptors: rule.descriptors(),
            }))
        };
        let shared = snapshot(&rule);
        let retained = &rule;
        let mut descriptors = Box::from_raw(rust_counter_style_descriptors(&rule));
        let other_descriptors = Box::from_raw(rust_counter_style_descriptors(&other));
        let data = other_descriptors.data();
        let replacement = data.descriptors[0].view();
        assert!(rust_descriptor_block_set(&mut descriptors, &replacement));
        assert!(rust_counter_style_set_name(&rule, view(&utf16("UPPER-ROMAN"))));
        assert_eq!(retained.name().units(), utf16("upper-roman"));
        assert_eq!(shared.name().units(), utf16("初"));
        assert!(!rust_counter_style_set_name(&rule, view(&utf16("DiSc"))));
        assert!(!rust_counter_style_set_name(&rule, view(&utf16("NONE"))));
        let changed = snapshot(&rule);
        assert_eq!(changed.name().units(), utf16("upper-roman"));
        assert_eq!(
            serialize_style_value_to_utf16(&changed.descriptors().descriptors[0].value).unwrap(),
            utf16("\"B\"")
        );
        assert_eq!(
            serialize_style_value_to_utf16(&shared.descriptors().descriptors[0].value).unwrap(),
            utf16("\"A\"")
        );
        assert!(rust_descriptor_block_remove(
            &mut descriptors,
            replacement.id,
            replacement.name
        ));
        assert!(retained.descriptors().descriptors.is_empty());
        assert_eq!(changed.descriptors().descriptors.len(), 1);
        assert!(rust_counter_style_set_name(&rule, view(&utf16("名前 空白"))));
        assert_eq!(retained.name().units(), utf16("名前 空白"));
        assert_eq!(changed.name().units(), utf16("upper-roman"));
    }
}

#[test]
fn font_feature_mutation_preserves_live_owners_and_shared_maps() {
    use crate::css::font_feature_values::*;
    use crate::css::rule::{NativeRuleList, RulePayload};
    let sheet = parse_shared_on_worker(
        "@font-feature-values 色 { @styleset { first: 1 2; 色: 3; first: 4; invalid: -1; } @styleset { second: 5; 色: 6 7; } @character-variant { cv: 8 9; bad: 1 2 3; } @annotation { a: 10; } @ornaments { o: 11; } @stylistic { s: 12; } @swash { w: 13; } @historical-forms { h: 14; } }",
    );
    assert_eq!(sheet.rules.len(), 1);
    let rules = NativeRuleList::from_parsed(sheet);
    let rule = rules.rule_at(0);
    let RulePayload::FontFeatureValues(values) = &rule.payload else {
        unreachable!()
    };
    fn get(owner: &FontFeatureValuesRule, kind: FontFeatureValuesRuleKind, name: &str) -> Option<Vec<u32>> {
        unsafe {
            let index = rust_font_feature_values_find(owner, kind, view(&utf16(name)));
            if index == usize::MAX {
                return None;
            }
            let entry = rust_font_feature_values_at(owner, kind, index);
            Some(std::slice::from_raw_parts(entry.values, entry.count).to_vec())
        }
    }
    let kind = FontFeatureValuesRuleKind::Styleset;
    unsafe {
        let shared = FontFeatureValuesRule::new(values.snapshot());
        assert_eq!(rust_font_feature_values_count(values, kind), 3);
        let entry = rust_font_feature_values_at(values, kind, 1);
        assert_eq!(entry.name.to_utf16().unwrap(), utf16("色"));
        assert_eq!(std::slice::from_raw_parts(entry.values, entry.count), [6, 7]);
        assert_eq!(entry.values, rust_font_feature_values_at(&shared, kind, 1).values);
        rust_font_feature_values_set(values, kind, entry.name, [29].as_ptr(), 1);
        assert_eq!(get(&shared, kind, "色").unwrap(), [6, 7]);
        assert_eq!(get(values, kind, "色").unwrap(), [29]);
        assert!(rust_font_feature_values_remove(values, kind, view(&utf16("first"))));
        assert_eq!(rust_font_feature_values_count(&shared, kind), 3);
        let family_name = utf16("新しい名前");
        rust_font_feature_values_set_families(values, &view(&family_name), 1);
        assert_eq!(shared.snapshot().families[0].units(), utf16("色"));
        drop(shared);
        let retained_rule = rule.clone();
        let RulePayload::FontFeatureValues(live) = &retained_rule.payload else {
            unreachable!()
        };
        let snapshot = FontFeatureValuesRule::new(values.snapshot());
        assert_eq!(values.snapshot().families[0].units(), utf16("新しい名前"));
        assert_eq!(
            rust_font_feature_values_count(values, FontFeatureValuesRuleKind::CharacterVariant),
            1
        );
        for (kind, name, expected) in [
            (FontFeatureValuesRuleKind::CharacterVariant, "cv", &[8, 9][..]),
            (FontFeatureValuesRuleKind::Annotation, "a", &[10][..]),
            (FontFeatureValuesRuleKind::Ornaments, "o", &[11][..]),
            (FontFeatureValuesRuleKind::Stylistic, "s", &[12][..]),
            (FontFeatureValuesRuleKind::Swash, "w", &[13][..]),
            (FontFeatureValuesRuleKind::HistoricalForms, "h", &[14][..]),
        ] {
            assert_eq!(get(values, kind, name).unwrap(), expected);
        }
        rust_font_feature_values_set(live, kind, view(&utf16("色")), [31, 37].as_ptr(), 2);
        assert_eq!(get(values, kind, "色").unwrap(), [31, 37]);
        assert_eq!(get(&snapshot, kind, "色").unwrap(), [29]);
        let edited_snapshot = FontFeatureValuesRule::new(live.snapshot());
        assert!(rust_font_feature_values_remove(live, kind, view(&utf16("色"))));
        assert!(get(values, kind, "色").is_none());
        assert_eq!(get(&edited_snapshot, kind, "色").unwrap(), [31, 37]);
        rust_font_feature_values_clear(live, kind);
        assert_eq!(rust_font_feature_values_count(values, kind), 0);
        assert_eq!(rust_font_feature_values_count(&snapshot, kind), 2);
        let first_family = utf16("別の名前");
        let second_family = utf16("ASCII");
        let families = [view(&first_family), view(&second_family)];
        rust_font_feature_values_set_families(live, families.as_ptr(), families.len());
        assert_eq!(values.snapshot().families.len(), 2);
        assert_eq!(values.snapshot().families[0].units(), first_family);
        assert_eq!(values.snapshot().families[1].units(), second_family);
        assert_eq!(snapshot.snapshot().families[0].units(), utf16("新しい名前"));
        drop((rule, rules));
        assert_eq!(live.snapshot().families.len(), 2);
    }
}

#[test]
fn keyframe_mutation_preserves_exposed_declarations_and_snapshots() {
    use crate::css::declaration_block::{rust_declaration_block_identity as identity, rust_declaration_block_replace};
    use crate::css::property_metadata::property_id;
    use crate::css::rule::*;
    use std::rc::Rc;

    let sheet = parse_shared_on_worker(
        "@keyframes 色 { from, 50% { width: 13px !important; height: 17px; } 25% { width: 19px; } to { width: 29px; } }",
    );
    let ParsedRuleChild::Rule(first) = &sheet.rules[0].children[0] else {
        unreachable!()
    };
    let data = first.keyframe.as_ref().unwrap();
    assert_eq!(&*data.keys, [0.0, 50.0]);
    assert_eq!(data.declarations.properties.len(), 1);
    assert_eq!(data.declarations.properties[0].property_id, property_id::HEIGHT);
    let rules = NativeRuleList::from_parsed(sheet.clone());
    let shared = NativeRuleList::from_parsed(sheet);
    let frames = rules.rule_at(0);
    let original_frames = shared.rule_at(0);
    let retained = frames.clone();
    let second_retained = retained.clone();
    let declarations_at =
        |list: &NativeRuleList, index| unsafe { Box::from_raw(rust_rule_declarations(&list.rule_at(index))) };
    unsafe {
        let children = &*rust_rule_children(&frames);
        let originals = &*rust_rule_children(&original_frames);
        let owners = RULE_OWNER_ALLOCATIONS.get();
        assert_eq!(rust_keyframes_find_rule(&frames, view(&utf16("from, 50%"))), 0);
        assert_eq!(rust_keyframes_find_rule(&frames, view(&utf16("50%"))), usize::MAX);
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), owners);
        let mut early = declarations_at(children, 1);
        let later = children.rule_at(1);
        let later_declarations = Box::from_raw(rust_rule_declarations(&later));
        assert_eq!(identity(&early), identity(&later_declarations));
        rust_declaration_block_replace(&mut early, &declarations("width: 23px"));
        assert_eq!(first_value(&later_declarations), utf16("23px"));
        assert_eq!(first_value(&declarations_at(originals, 1)), utf16("19px"));
        let exposed = children.rule_at(2);
        let mut exposed_declarations = Box::from_raw(rust_rule_declarations(&exposed));
        rust_declaration_block_replace(&mut exposed_declarations, &declarations("width: 31px"));
        let original = declarations_at(originals, 2);
        assert_eq!(first_value(&declarations_at(children, 2)), utf16("31px"));
        assert_eq!(first_value(&original), utf16("29px"));
        let snapshot = exposed_declarations.data();
        rust_rule_list_remove(children, 0);
        assert_eq!(rust_rule_list_count(&*rust_rule_children(&retained)), 2);
        assert_eq!(identity(&declarations_at(children, 0)), identity(&early));
        assert_eq!(identity(&declarations_at(children, 1)), identity(&exposed_declarations));
        rust_rule_list_remove(children, 1);
        rust_declaration_block_replace(&mut exposed_declarations, &original);
        rust_rule_list_insert(children, 1, Rc::as_ptr(&exposed));
        assert_eq!(identity(&declarations_at(children, 1)), identity(&exposed_declarations));
        assert_eq!(
            serialize_style_value_to_utf16(&snapshot.properties[0].value).unwrap(),
            utf16("31px")
        );
        rust_keyframes_set_name(&frames, view(&utf16("新しい名前")));
        assert_eq!(
            rust_rule_payload(&retained).name.to_utf16().unwrap(),
            utf16("新しい名前")
        );
        assert_eq!(
            rust_rule_payload(&original_frames).name.to_utf16().unwrap(),
            utf16("色")
        );
        drop((rules, frames, shared, original_frames));
        let children = &*rust_rule_children(&second_retained);
        assert_eq!(rust_rule_list_count(children), 2);
        assert_eq!(identity(&declarations_at(children, 1)), identity(&exposed_declarations));
        rust_rule_list_remove(children, 0);
        assert_eq!(rust_rule_list_count(&*rust_rule_children(&retained)), 1);
        assert_eq!(first_value(&later_declarations), utf16("23px"));
        assert_eq!(rust_keyframes_find_rule(&retained, view(&utf16("to"))), 0);
        assert_eq!(first_value(&exposed_declarations), utf16("29px"));
    }
}

#[test]
fn worker_block_merging_and_shorthand_expansion_need_no_host_callbacks() {
    use crate::css::property_metadata::property_id;
    use crate::css::style_value::StyleValueData;
    std::thread::spawn(|| {
        let block = declarations("color: red !important; --色: first; @unknown {} color: blue; --色: last");
        let data = block.data();
        assert_eq!(data.properties.len(), 1);
        assert_eq!(data.properties[0].property_id, property_id::COLOR);
        assert!(data.properties[0].important);
        assert_eq!(data.custom_properties.len(), 1);
        let sheet = parse(
            &utf16("@page { @top-left { color: red !important; @unknown {} color: blue; } }"),
            &super::tests::parse_context(),
        );
        let ParsedRuleChild::Rule(margin) = &sheet.rules[0].children[0] else {
            panic!()
        };
        assert_eq!(margin.rule_kind, ParsedRuleKind::Margin);
        let margin = margin.declaration_block.clone().unwrap();
        drop(sheet);
        assert_eq!(margin.properties.len(), 1);
        assert!(margin.properties[0].important);
        let sheet = parse(&utf16("a { margin: var(--gap) }"), &super::tests::parse_context());
        assert_eq!(sheet.declarations.len(), 1);
        let expanded = sheet.rules[0].declaration_block.clone().unwrap();
        drop(sheet);
        assert_eq!(expanded.properties.len(), 5);
        assert_eq!(expanded.properties[0].property_id, property_id::MARGIN);
        for property in &expanded.properties[1..] {
            assert!(matches!(&*property.value, StyleValueData::PendingSubstitution { .. }));
        }
        drop((block, data, margin, expanded));
        assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
    })
    .join()
    .unwrap();
}

#[test]
fn value_graphs_are_shared_and_destroyed_without_worker_callbacks() {
    use crate::css::style_value::{StyleValueData, rust_style_value_equals};
    let sheet = parse_shared_on_worker(
        "a { grid-template-columns: repeat(2, [é] minmax(10px, 1fr)); width: calc(1em + 2px); --custom: var(--é, [nested tokens]); content: '😀'; background-image: url(image.png); filter: url(image.png) }",
    );
    assert_eq!(sheet.declarations.len(), 6);
    let values: Vec<_> = sheet
        .declarations
        .iter()
        .map(|declaration| declaration.parsed_value.clone().unwrap())
        .collect();
    drop(sheet);
    let copies = [values.clone(), values.clone()];
    drop(values);
    let threads = copies.map(|values| {
        std::thread::spawn(move || {
            for value in values {
                for _ in 0..100 {
                    let copy = value.clone();
                    assert!(unsafe { rust_style_value_equals(Arc::as_ptr(&value), Arc::as_ptr(&copy)) });
                    assert!(!serialize_style_value_to_utf16(&copy).unwrap().is_empty());
                }
            }
            assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
        })
    });
    for thread in threads {
        thread.join().unwrap();
    }

    let sheet = parse_shared_on_worker(
        "a { position-anchor: --shared-name; color: ReD; grid-template-columns: [shared-name] 1fr; content: 'text' }",
    );
    assert_eq!(sheet.declarations.len(), 4);
    assert!(
        sheet
            .declarations
            .iter()
            .all(|declaration| declaration.parsed_value.is_some())
    );
    let value = sheet.declarations[0].parsed_value.clone().unwrap();
    drop(sheet);
    let StyleValueData::CustomIdent { custom_ident } = &*value else {
        panic!()
    };
    assert_eq!(custom_ident.units(), utf16("--shared-name"));
    let second = parse(
        &utf16("b { position-anchor: --shared-name }"),
        &super::tests::parse_context(),
    );
    assert_eq!(second.declarations.len(), 1);
    let other = second.declarations[0].parsed_value.as_ref().unwrap();
    assert!(unsafe { rust_style_value_equals(Arc::as_ptr(&value), Arc::as_ptr(other)) });
}

#[test]
fn url_text_clones_share_native_storage_and_outlive_their_input() {
    use crate::css::style_value::{RetainedString, rust_css_url_text_view};
    std::thread::spawn(|| {
        for spelling in ["image.png", "café😀.png"] {
            let input = utf16(spelling);
            let text = RetainedString::from_utf16(&input).unwrap();
            let copy = text.clone();
            let original_view = rust_css_url_text_view(&text);
            let copied_view = rust_css_url_text_view(&copy);
            assert_eq!(original_view.ascii, copied_view.ascii);
            assert_eq!(original_view.utf16, copied_view.utf16);
            assert_eq!(original_view.length, copied_view.length);
            assert_eq!(original_view.utf16.is_null(), spelling.is_ascii());
            drop((input, text));
            let mut retained = Vec::new();
            copy.units().append_to(&mut retained);
            assert_eq!(retained, utf16(spelling));
        }
        assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
    })
    .join()
    .unwrap();
}

#[test]
fn worker_selector_normalization_survives_binding() {
    use crate::css::selector_parser::*;
    use crate::css::selector_serialization::serialize_selector_without_namespaces;
    fn text(parsed: &RustParsedSelectorList) -> Vec<u16> {
        unsafe {
            assert_eq!(rust_parsed_selector_list_length(parsed), 1);
            let bound = parsed.bind();
            let selector = Box::from_raw(rust_bound_selector_list_selector(&bound, 0));
            drop(bound);
            serialize_selector_without_namespaces(&selector)
        }
    }
    fn visit(rule: &super::ParsedRule, selectors: &mut Vec<Vec<u16>>, scopes: &mut Vec<Vec<u16>>) {
        for child in &rule.children {
            if let ParsedRuleChild::Rule(child) = child {
                visit(child, selectors, scopes);
            }
        }
        if let Some(parsed) = &rule.selector_list {
            selectors.push(text(parsed));
        }
        if rule.rule_kind == ParsedRuleKind::Scope {
            let scope = rule.scope_selectors.as_ref().unwrap();
            scopes.push(text(scope.start.as_ref().unwrap()));
            scopes.push(text(scope.end.as_ref().unwrap()));
        }
    }
    let sheet = parse_shared_on_worker(
        ".親 { > .子 {} :is(&, .他) {} @media all { .孫 {} } @scope (> .範囲) to (> .限界) { .内側 {} > .直下 {} .子 { .深い {} } } }",
    );
    let mut selectors = Vec::new();
    let mut scopes = Vec::new();
    for rule in &sheet.rules {
        visit(rule, &mut selectors, &mut scopes);
    }
    drop(sheet);
    assert_eq!(
        selectors,
        [
            "& > .子",
            ":is(&, .他)",
            "& .孫",
            ".内側",
            "> .直下",
            "& .深い",
            ".子",
            ".親"
        ]
        .map(utf16)
    );
    assert_eq!(scopes, ["& > .範囲", "> .限界"].map(utf16));
}

#[test]
fn import_preludes_outlive_the_worker_and_shared_sheet() {
    let sheet = parse_shared_on_worker(
        "@import 'a.css' layer(雪) supports(display: grid); @import url('b.css') layer supports(display: unknown); @import 'c.css'; @supports (display: grid) {}",
    );
    assert_eq!(sheet.rules.len(), 4);
    let named = sheet.rules[0].import_rule.clone().unwrap();
    let anonymous = sheet.rules[1].import_rule.clone().unwrap();
    let unlayered = sheet.rules[2].import_rule.clone().unwrap();
    let supports = sheet.rules[3].supports_condition.clone().unwrap();
    drop(sheet);
    assert_eq!(named.layer.as_ref().unwrap().units(), utf16("雪"));
    assert!(anonymous.layer.as_ref().unwrap().units().is_empty());
    assert!(unlayered.layer.is_none());
    assert!(unlayered.supports.is_none());
    unsafe {
        assert_eq!(
            css_query_evaluate_supports(Arc::as_ptr(named.supports.as_ref().unwrap())),
            1
        );
        assert_eq!(
            css_query_evaluate_supports(Arc::as_ptr(anonymous.supports.as_ref().unwrap())),
            0
        );
        assert_eq!(css_query_evaluate_supports(Arc::as_ptr(&supports)), 1);
    }
    assert_eq!(
        serialize_style_value_to_utf16(&named.url).unwrap(),
        utf16("url(\"a.css\")")
    );
}

unsafe extern "C" fn collect_text(context: *mut std::ffi::c_void, text: *const u16, length: usize) {
    let result = unsafe { &mut *context.cast::<Vec<u16>>() };
    result.extend_from_slice(unsafe { std::slice::from_raw_parts(text, length) });
}

fn page_text(selectors: &PageSelectorList) -> Vec<u16> {
    let mut text = Vec::<u16>::new();
    unsafe { super::rust_page_selector_list_serialize(selectors, (&raw mut text).cast(), collect_text) };
    text
}

#[test]
fn page_selectors_outlive_the_sheet_and_replace_independently() {
    let sheet = parse_shared_on_worker("@page 雪:left:first, :blank { margin: 7px; } @page {}");
    assert_eq!(sheet.rules.len(), 2);
    let selectors = sheet.rules[0].page_selector_list.clone().unwrap();
    let empty = sheet.rules[1].page_selector_list.clone().unwrap();
    drop(sheet);
    assert_eq!(selectors.selectors.len(), 2);
    assert_eq!(page_text(&selectors), utf16("雪:left:first, :blank"));
    let replacement =
        PageSelectorList::new(super::parse_page_selector_list(utf16("invoice:RIGHT").as_slice()).unwrap());
    assert_eq!(page_text(&replacement), utf16("invoice:right"));
    assert!(super::parse_page_selector_list(utf16("invoice:unknown").as_slice()).is_none());
    let mut shared = selectors.clone();
    assert!(Arc::ptr_eq(&shared, &selectors));
    shared = Arc::new(PageSelectorList::new(
        super::parse_page_selector_list(utf16("氷:right").as_slice()).unwrap(),
    ));
    assert_eq!(page_text(&shared), utf16("氷:right"));
    assert_eq!(page_text(&selectors), utf16("雪:left:first, :blank"));
    assert!(empty.selectors.is_empty());
    assert!(page_text(&empty).is_empty());
    assert!(super::parse_page_selector_list(b"".as_slice()).is_some());
}

#[test]
fn function_signatures_own_parameters_after_the_sheet_is_dropped() {
    let sheet = parse_shared_on_worker(
        "@function --幅(--長さ <length>: 8px, --名: '雪', --未) returns <length> { result: var(--長さ); } @function --空() {}",
    );
    assert_eq!(sheet.rules.len(), 2);
    let signature = sheet.rules[0].function_signature.clone().unwrap();
    let empty = sheet.rules[1].function_signature.clone().unwrap();
    drop(sheet);
    assert_eq!(signature.name.units(), utf16("--幅"));
    assert_eq!(signature.parameters.len(), 3);
    assert_eq!(*signature.return_type, SyntaxNode::Type(SyntaxType::Length));
    assert_eq!(*signature.parameters[0].syntax, SyntaxNode::Type(SyntaxType::Length));
    assert_eq!(*signature.parameters[2].syntax, SyntaxNode::Universal);
    assert!(signature.parameters[2].default_value.is_none());
    let parameter = &signature.parameters[1];
    assert_eq!(parameter.name.units(), utf16("--名"));
    assert_eq!(
        serialize_style_value_to_utf16(parameter.default_value.as_ref().unwrap()).unwrap(),
        utf16("\"雪\"")
    );
    assert_eq!(empty.name.units(), utf16("--空"));
    assert!(empty.parameters.is_empty());
    assert_eq!(*empty.return_type, SyntaxNode::Universal);
}

#[test]
fn property_rules_own_syntax_and_initial_values_after_the_sheet_is_dropped() {
    let sheet = parse_shared_on_worker(
        "@property --色 { syntax: '<length>'; inherits: false; initial-value: 8px; } @property --自由 { syntax: '*'; inherits: true; }",
    );
    assert_eq!(sheet.rules.len(), 2);
    let typed = sheet.rules[0].property_rule.clone().unwrap();
    let universal = sheet.rules[1].property_rule.clone().unwrap();
    drop(sheet);
    assert_eq!(typed.name.units(), utf16("--色"));
    assert_eq!(typed.syntax_source.units(), utf16("<length>"));
    assert_eq!(*typed.syntax, SyntaxNode::Type(SyntaxType::Length));
    assert!(!typed.inherits);
    assert_eq!(
        serialize_style_value_to_utf16(typed.initial_value.as_ref().unwrap()).unwrap(),
        utf16("8px")
    );
    assert_eq!(universal.name.units(), utf16("--自由"));
    assert!(universal.inherits);
    assert_eq!(*universal.syntax, SyntaxNode::Universal);
    assert!(universal.initial_value.is_none());
}

#[test]
fn namespace_rules_own_native_names_after_the_sheet_is_dropped() {
    let sheet = parse_shared_on_worker("@namespace ''; @namespace 色 'urn:名前'; 色|x {}");
    assert_eq!(sheet.rules.len(), 3);
    assert!(sheet.rules[2].selector_list.is_some());
    let default = sheet.rules[0].namespace_rule.clone().unwrap();
    let prefixed = sheet.rules[1].namespace_rule.clone().unwrap();
    drop(sheet);
    assert!(default.prefix.units().is_empty());
    assert!(default.uri.units().is_empty());
    assert_eq!(prefixed.prefix.units(), utf16("色"));
    assert_eq!(prefixed.uri.units(), utf16("urn:名前"));
}

#[test]
fn cache_keys_include_parsing_context_and_replay_side_effects() {
    let source = utf16(".é😀 { width: 13px; background-image: url(image.png) }");
    let mut context = super::tests::parse_context();
    let first = parse(&source, &context);
    let second = parse(&source, &context);
    assert!(Arc::ptr_eq(&first, &second));
    context.in_quirks_mode = true;
    let quirks = parse(&source, &context);
    assert!(!Arc::ptr_eq(&first, &quirks));
    context.in_quirks_mode = false;
    let base_url = b"https://example.com/directory/";
    context.document_base_url = base_url.as_ptr();
    context.document_base_url_length = base_url.len();
    let with_base_url = parse(&source, &context);
    assert!(!Arc::ptr_eq(&first, &with_base_url));
    context.document_base_url = std::ptr::null();
    context.document_base_url_length = 0;

    let mut lengths: crate::css::style_compute::FfiLengthResolutionContext = unsafe { std::mem::zeroed() };
    context.length_resolution_context = (&raw const lengths).cast();
    let with_lengths = parse(&source, &context);
    lengths.viewport_width = 800.0;
    context.length_resolution_context = (&raw const lengths).cast();
    let resized = parse(&source, &context);
    assert!(!Arc::ptr_eq(&with_lengths, &resized));
    let mut resolved_viewport_length = false;
    lengths.resolved_viewport_relative_length = &raw mut resolved_viewport_length;
    context.length_resolution_context = (&raw const lengths).cast();
    let tracked = parse(&source, &context);
    let tracked_again = parse(&source, &context);
    assert!(!Arc::ptr_eq(&tracked, &tracked_again));
    context.length_resolution_context = std::ptr::null();

    let mut random_index = 7;
    context.random_function_index = &raw mut random_index;
    let with_counter = parse(&source, &context);
    let final_index = random_index;
    random_index = 7;
    let with_counter_again = parse(&source, &context);
    assert_eq!(random_index, final_index);
    assert!(Arc::ptr_eq(&with_counter, &with_counter_again));
}

#[test]
fn cache_shares_ascii_and_utf16_sources() {
    let source = b".cached { width: 13px }";
    let context = super::tests::parse_context();
    let ascii = unsafe { parse_shared_stylesheet(TokenizerInput::Ascii(source), &context) };
    let wide = parse(&utf16(".cached { width: 13px }"), &context);
    assert!(Arc::ptr_eq(&ascii, &wide));
}

#[test]
fn retained_values_do_not_keep_the_sheet_in_the_cache() {
    let source = utf16(".weak-cache-owner { width: 13px }");
    let context = super::tests::parse_context();
    let first = parse(&source, &context);
    assert_eq!(first.declarations.len(), 1);
    let retained = first.declarations[0].parsed_value.clone().unwrap();
    let second_reference = retained.clone();
    let weak = Arc::downgrade(&first);
    drop(first);
    assert!(weak.upgrade().is_none());
    let second = parse(&source, &context);
    assert_eq!(second.declarations.len(), 1);
    assert!(!Arc::ptr_eq(
        &retained,
        second.declarations[0].parsed_value.as_ref().unwrap()
    ));
    assert_eq!(serialize_style_value_to_utf16(&retained).unwrap(), utf16("13px"));
    drop(retained);
    assert_eq!(
        serialize_style_value_to_utf16(&second_reference).unwrap(),
        utf16("13px")
    );
}

#[test]
fn published_sheet_owns_non_ascii_declaration_metadata() {
    let sheet = parse_shared_on_worker(".é😀 { width: 13px; --é😀: value }");
    assert_eq!(sheet.rules.len(), 1);
    assert_eq!(sheet.declarations.len(), 2);
    assert!(sheet.rules[0].selector_list.is_some());
    let declaration = &sheet.declarations[1];
    assert_eq!(
        &sheet.values[declaration.name_offset..declaration.name_offset + declaration.name_length],
        utf16("--é😀")
    );
    assert_eq!(
        serialize_style_value_to_utf16(sheet.declarations[0].parsed_value.as_ref().unwrap()).unwrap(),
        utf16("13px")
    );
}

#[test]
fn independent_workers_read_the_same_immutable_graph() {
    use crate::css::selector_parser::rust_parsed_selector_list_length;
    let sheet = parse_shared_on_worker(
        ".é😀 { width: 13px } @supports (display: grid) { a { height: 7px } } @property --size { syntax: '<length>'; inherits: false; initial-value: 3px } @page :left { margin: 2px }",
    );
    let expected_value = sheet.declarations[0].parsed_value.clone().unwrap();
    let copies = [sheet.clone(), sheet.clone()];
    drop(sheet);
    let threads = copies.map(|sheet| {
        let expected_value = expected_value.clone();
        std::thread::spawn(move || {
            assert!(Arc::ptr_eq(
                sheet.declarations[0].parsed_value.as_ref().unwrap(),
                &expected_value
            ));
            assert_eq!(serialize_style_value_to_utf16(&expected_value).unwrap(), utf16("13px"));
            assert_eq!(sheet.rules.len(), 4);
            unsafe {
                assert_eq!(
                    rust_parsed_selector_list_length(Arc::as_ptr(sheet.rules[0].selector_list.as_ref().unwrap())),
                    1
                );
                assert_eq!(
                    css_query_evaluate_supports(Arc::as_ptr(sheet.rules[1].supports_condition.as_ref().unwrap())),
                    1
                );
            }
            let ParsedRuleChild::Rule(nested) = &sheet.rules[1].children[0] else {
                panic!()
            };
            unsafe {
                assert_eq!(
                    rust_parsed_selector_list_length(Arc::as_ptr(nested.selector_list.as_ref().unwrap())),
                    1
                )
            };
            assert_eq!(
                *sheet.rules[2].property_rule.as_ref().unwrap().syntax,
                SyntaxNode::Type(SyntaxType::Length)
            );
            assert_eq!(sheet.rules[3].page_selector_list.as_ref().unwrap().selectors.len(), 1);
            drop(sheet);
            drop(expected_value);
            assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
        })
    });
    for thread in threads {
        thread.join().unwrap();
    }
}

#[test]
fn scope_presence_and_native_selector_names_survive_worker_exit() {
    use crate::css::selector_parser::rust_parsed_selector_list_length;
    let sheet = parse_shared_on_worker(
        "@import 'data:text/css,'; @import 'data:text/css,' scope; @import 'data:text/css,' scope((.親) to (.限界)); @scope {} @scope (.親) to (.限界) {}",
    );
    assert_eq!(sheet.rules.len(), 5);
    assert!(sheet.rules[0].scope_selectors.is_none());
    let scopes: Vec<_> = sheet.rules[1..]
        .iter()
        .map(|rule| rule.scope_selectors.clone().unwrap())
        .collect();
    drop(sheet);
    for (index, scope) in scopes.iter().enumerate() {
        if index % 2 == 0 {
            assert!(scope.start.is_none());
            assert!(scope.end.is_none());
        } else {
            for (selectors, expected) in [
                (scope.start.as_ref().unwrap(), "親"),
                (scope.end.as_ref().unwrap(), "限界"),
            ] {
                unsafe {
                    assert_eq!(rust_parsed_selector_list_length(Arc::as_ptr(selectors)), 1);
                    assert_eq!(selectors.interned_names.len(), 1);
                    assert_eq!(selectors.interned_names[0].as_ref(), utf16(expected));
                }
            }
        }
    }
}

#[test]
fn shared_rule_tree_preserves_nested_declaration_lists() {
    let sheet = parse_shared_on_worker(
        "@media all { .a { width: 1px; @supports (display: grid) { width: 2px; .b { width: 3px; } height: 4px; } color: red; } } .c { width: 5px; }",
    );
    assert_eq!(sheet.rules.len(), 2);
    let media = sheet.rules[0].clone();
    let last = sheet.rules[1].clone();
    drop(sheet);
    assert_eq!(media.rule_kind, ParsedRuleKind::Media);
    assert_eq!(media.children.len(), 1);
    let ParsedRuleChild::Rule(style) = &media.children[0] else {
        panic!()
    };
    assert!(style.selector_list.is_some());
    assert_eq!(style.children.len(), 2);
    let ParsedRuleChild::Rule(supports) = &style.children[0] else {
        panic!()
    };
    assert_eq!(supports.rule_kind, ParsedRuleKind::Supports);
    assert_eq!(supports.children.len(), 3);
    for child in [&supports.children[0], &supports.children[2], &style.children[1]] {
        let ParsedRuleChild::Declarations(list) = child else {
            panic!()
        };
        assert!(list.declaration_block.is_some());
    }
    let ParsedRuleChild::Rule(nested) = &supports.children[1] else {
        panic!()
    };
    assert!(nested.selector_list.is_some());
    assert!(last.selector_list.is_some());
}

#[test]
fn container_conditions_own_names_and_queries_after_the_sheet_is_dropped() {
    use crate::css::parser::query_parser::{
        CONTAINER_QUERY_REQUIRES_STYLE, CONTAINER_QUERY_REQUIRES_WIDTH, css_query_container_requirements,
        css_query_serialize_condition,
    };
    let sheet =
        parse_shared_on_worker("@container 色😀 (width > 10px), 名前, style(--状態: 有効) { div { width: 13px } }");
    assert_eq!(sheet.rules.len(), 1);
    let conditions = sheet.rules[0].container_conditions.clone().unwrap();
    drop(sheet);
    let entries = &conditions.conditions;
    assert_eq!(entries.len(), 3);
    assert_eq!(entries[0].name.as_ref().unwrap().units(), utf16("色😀"));
    assert_eq!(entries[1].name.as_ref().unwrap().units(), utf16("名前"));
    assert!(entries[1].query.is_none());
    assert!(entries[2].name.is_none());
    unsafe {
        assert_ne!(
            css_query_container_requirements(Arc::as_ptr(entries[0].query.as_ref().unwrap()))
                & CONTAINER_QUERY_REQUIRES_WIDTH,
            0
        );
        let query = Arc::as_ptr(entries[2].query.as_ref().unwrap());
        assert_ne!(
            css_query_container_requirements(query) & CONTAINER_QUERY_REQUIRES_STYLE,
            0
        );
        let mut text = Vec::<u16>::new();
        assert!(css_query_serialize_condition(
            query,
            (&raw mut text).cast(),
            collect_text
        ));
        assert_eq!(text, utf16("style(--状態: 有効)"));
    }
}

#[test]
fn layer_names_outlive_the_shared_sheet() {
    let sheet = parse_shared_on_worker("@layer 色.子, emoji😀; @layer 色.子 {} @layer {}");
    assert_eq!(sheet.rules.len(), 3);
    let statement = sheet.rules[0].layer_names.clone().unwrap();
    let block = sheet.rules[1].layer_names.clone().unwrap();
    let anonymous = sheet.rules[2].layer_names.clone().unwrap();
    drop(sheet);
    assert_eq!(statement.names.len(), 2);
    assert_eq!(statement.names[0].units(), utf16("色.子"));
    assert_eq!(statement.names[1].units(), utf16("emoji😀"));
    assert_eq!(block.names.len(), 1);
    assert_eq!(block.names[0].units(), utf16("色.子"));
    assert_eq!(anonymous.names.len(), 1);
    assert!(anonymous.names[0].units().is_empty());
}

#[test]
fn parsed_anchor_and_font_values_retain_their_children() {
    use crate::css::style_value::StyleValueData;
    let block = declarations("width: anchor-size(width, 1px); left: anchor(left, 2px)");
    let data = block.data();
    assert_eq!(data.properties.len(), 2);
    let StyleValueData::AnchorSize { fallback_value, .. } = &*data.properties[0].value else {
        panic!()
    };
    let fallback = fallback_value.clone();
    let StyleValueData::Anchor {
        anchor_side,
        fallback_value,
        ..
    } = &*data.properties[1].value
    else {
        panic!()
    };
    let side = anchor_side.clone();
    let second_fallback = fallback_value.clone();
    drop((block, data));
    assert_eq!(serialize_style_value_to_utf16(fallback.data()).unwrap(), utf16("1px"));
    assert_eq!(serialize_style_value_to_utf16(side.data()).unwrap(), utf16("left"));
    assert_eq!(
        serialize_style_value_to_utf16(second_fallback.data()).unwrap(),
        utf16("2px")
    );

    let sheet = parse_shared_on_worker("@font-face { font-family: Graph; src: local('Font Name'); }");
    let descriptors = sheet.rules[0].descriptor_block.clone().unwrap();
    let value = &*descriptors.descriptors[1].value;
    assert_eq!(
        serialize_style_value_to_utf16(value).unwrap(),
        utf16("local(\"Font Name\")")
    );
    let StyleValueData::ValueList { values, .. } = value else {
        panic!()
    };
    let StyleValueData::FontSource { local_name, .. } = values.as_slice()[0].data() else {
        panic!()
    };
    let name = local_name.clone();
    drop((sheet, descriptors));
    assert_eq!(
        serialize_style_value_to_utf16(name.data()).unwrap(),
        utf16("\"Font Name\"")
    );
}

#[test]
fn calculation_nodes_retain_native_function_values() {
    use crate::css::calc::{CalcNode, CalcNumericValue};
    use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
    let node = CalcNode::Random {
        min: Arc::new(CalcNode::Numeric(CalcNumericValue::Number {
            value: 0.0,
            number_type: 0,
        })),
        max: Arc::new(CalcNode::Numeric(CalcNumericValue::Number {
            value: 1.0,
            number_type: 0,
        })),
        step: None,
        sharing: RetainedStyleValueData::from_owned(StyleValueData::RandomValueSharing {
            fixed_value: RetainedStyleValueData::from_owned(StyleValueData::Number { value: 0.25 }),
            is_auto: false,
            has_name: false,
            name: crate::css::css_string::CssString::from_utf16(&[]),
            element_shared: false,
        }),
    };
    let CalcNode::Random { sharing, .. } = &node else {
        panic!()
    };
    let sharing = sharing.clone();
    drop(node);
    assert_eq!(
        serialize_style_value_to_utf16(sharing.data()).unwrap(),
        utf16("fixed 0.25")
    );
    let node = CalcNode::NonMathFunction {
        value: RetainedStyleValueData::from_owned(StyleValueData::TreeCountingFunction {
            function: 0,
            computed_type: 0,
        }),
        numeric_type: Default::default(),
    };
    let CalcNode::NonMathFunction { value, .. } = &node else {
        panic!()
    };
    let value = value.clone();
    drop(node);
    assert_eq!(
        serialize_style_value_to_utf16(value.data()).unwrap(),
        utf16("sibling-count()")
    );
}

#[test]
fn declaration_image_traversal_skips_non_resource_values() {
    use crate::css::declaration_block::visit_declaration_images;
    use crate::css::style_value::StyleValueData;
    let block = declarations(
        "width: 13px; background-image: url(a), linear-gradient(red, blue); content: url(b) / 'text'; border-image-source: image-set(url(c) 1x, url(d) 2x); --ignored: url(e)",
    );
    let mut images = 0;
    let mut image_sets = 0;
    visit_declaration_images(&block.data(), &mut |value| match value {
        StyleValueData::Image { .. } => images += 1,
        StyleValueData::ImageSet { .. } => image_sets += 1,
        _ => panic!("Image traversal visited a non-resource value"),
    });
    assert_eq!(images, 2);
    assert_eq!(image_sets, 1);
}

#[test]
fn declaration_mutation_does_not_change_cached_or_nested_shared_blocks() {
    use crate::css::declaration_block::{
        DeclarationBlock, rust_declaration_block_remove, rust_declaration_block_remove_custom,
    };
    use crate::css::property_metadata::property_id;
    let source = utf16("a { margin: 13px; --色: green }");
    let context = super::tests::parse_context();
    let first = parse(&source, &context);
    let second = parse(&source, &context);
    let original = first.rules[0].declaration_block.clone().unwrap();
    assert!(Arc::ptr_eq(
        &original,
        second.rules[0].declaration_block.as_ref().unwrap()
    ));
    let mut changed = DeclarationBlock::new(original.clone());
    let independent = DeclarationBlock::new(original.clone());
    assert_eq!(changed.data().properties.len(), 4);
    unsafe {
        assert!(rust_declaration_block_remove(&mut changed, property_id::MARGIN_TOP));
        assert!(rust_declaration_block_remove_custom(&mut changed, view(&utf16("--色"))));
    }
    assert_eq!(independent.data().properties.len(), 4);
    assert_eq!(independent.data().custom_properties.len(), 1);
    let third = parse(&source, &context);
    assert!(Arc::ptr_eq(
        &original,
        third.rules[0].declaration_block.as_ref().unwrap()
    ));
    drop((first, second, third, original));
    assert_eq!(
        serialize_style_value_to_utf16(&independent.data().properties[0].value).unwrap(),
        utf16("13px")
    );
    assert_eq!(
        serialize_style_value_to_utf16(&independent.data().custom_properties[0].declaration.value).unwrap(),
        utf16("green")
    );

    let nested = parse_shared_on_worker(".parent { .child {} margin: var(--gap); --色: green; }");
    assert_eq!(nested.rules[0].children.len(), 2);
    let ParsedRuleChild::Declarations(list) = &nested.rules[0].children[1] else {
        panic!()
    };
    let original = list.declaration_block.clone().unwrap();
    drop(nested);
    let mut changed = DeclarationBlock::new(original.clone());
    let shared = DeclarationBlock::new(original);
    assert_eq!(changed.data().properties.len(), 5);
    assert_eq!(changed.data().custom_properties.len(), 1);
    unsafe { assert!(rust_declaration_block_remove(&mut changed, property_id::MARGIN_LEFT)) };
    assert_eq!(shared.data().properties.len(), 5);
}

#[test]
fn media_mutation_preserves_live_owners_and_shared_snapshots() {
    use crate::css::media_list::*;
    fn text(list: &MediaList) -> Vec<u16> {
        let mut text = Vec::<u16>::new();
        unsafe { rust_media_list_serialize(list, (&raw mut text).cast(), collect_text) };
        text
    }
    let sheet = parse_shared_on_worker("@import 'data:text/css,' screen, écran; @media all, print {}");
    assert_eq!(sheet.rules.len(), 2);
    let imported = MediaList::new(sheet.rules[0].media_list.clone().unwrap());
    let media = MediaList::new(sheet.rules[1].media_list.clone().unwrap());
    drop(sheet);
    assert_eq!(text(&imported), utf16("screen, écran"));
    assert_eq!(text(&media), utf16("all, print"));
    let retained = media.clone();
    unsafe {
        let shared = Box::from_raw(rust_media_list_share(&media));
        assert!(rust_media_list_evaluate(&media, std::mem::zeroed()));
        assert!(rust_media_list_matches(&retained));
        assert!(!rust_media_list_matches(&shared));
        assert!(matches!(
            rust_media_list_delete(&media, view(&utf16("all"))),
            MediaListDeleteResult::Removed
        ));
        assert!(!rust_media_list_matches(&media));
        assert_eq!(text(&retained), utf16("print"));
        assert_eq!(text(&shared), utf16("all, print"));
        assert!(!rust_media_list_append(&media, view(&utf16("PRINT"))));
        assert!(!rust_media_list_append(&media, view(&utf16("screen, print"))));
        assert!(matches!(
            rust_media_list_delete(&media, view(&utf16("screen, print"))),
            MediaListDeleteResult::Invalid
        ));
        assert!(matches!(
            rust_media_list_delete(&media, view(&utf16("screen"))),
            MediaListDeleteResult::NotFound
        ));
        rust_media_list_set_text(&media, view(&utf16("écran, écran")));
        assert!(matches!(
            rust_media_list_delete(&media, view(&utf16("écran"))),
            MediaListDeleteResult::Removed
        ));
        assert_eq!(rust_media_list_length(&retained), 0);
        assert!(rust_media_list_matches(&retained));
        let mut item = Vec::<u16>::new();
        assert!(!rust_media_list_item(
            &retained,
            0,
            (&raw mut item).cast(),
            collect_text
        ));
        assert!(rust_media_list_append(&media, view(&utf16(""))));
        assert!(rust_media_list_item(&retained, 0, (&raw mut item).cast(), collect_text));
        assert_eq!(item, utf16("not all"));
        drop(media);
        assert_eq!(text(&retained), utf16("not all"));
        assert_eq!(text(&shared), utf16("all, print"));
    }
}

#[test]
fn function_descriptor_blocks_retain_worker_parsed_values() {
    use crate::css::descriptor_block::FfiDescriptorBlock;
    let (data, parsed_values) = std::thread::spawn(|| {
        let source = utf16("--色: 13px; result: var(--色)");
        let parse = unsafe {
            super::parse_shared_block(
                TokenizerInput::Utf16(&source),
                vec![super::RuleContext::AtFunction],
                &super::tests::parse_context(),
                false,
            )
        };
        assert_eq!(parse.items.len(), 1);
        let ParsedRuleChild::Declarations(list) = &parse.items[0] else {
            panic!()
        };
        let data = list.descriptor_block.clone().unwrap();
        let block = unsafe { Box::from_raw(super::rust_css_syntax_parse_descriptor_block(&*parse)) };
        assert_eq!(block.data().descriptors.len(), 2);
        assert!(Arc::ptr_eq(
            &data.descriptors[0].value,
            &block.data().descriptors[0].value
        ));
        drop(parse);
        assert_eq!(data.descriptors[0].name.units(), utf16("--色"));
        assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
        (data, block.data())
    })
    .join()
    .unwrap();
    let descriptors = FfiDescriptorBlock::new(data);
    assert_eq!(descriptors.data().descriptors.len(), 2);
    assert!(Arc::ptr_eq(
        &descriptors.data().descriptors[1].value,
        &parsed_values.descriptors[1].value
    ));
    assert_eq!(
        serialize_style_value_to_utf16(&descriptors.data().descriptors[1].value).unwrap(),
        utf16("var(--色)")
    );
}

#[test]
fn descriptor_mutation_forks_worker_parsed_data() {
    use crate::css::descriptor_block::{FfiDescriptorBlock, rust_descriptor_block_set};
    use crate::css::descriptor_metadata::descriptor_metadata;
    let sheet = parse_shared_on_worker(
        "@page { margin-top: 13px; margin-left: 29px; margin-top: 17px } @font-face { font-family: 色; src: local(色) }",
    );
    assert_eq!(sheet.rules.len(), 2);
    let mut page = FfiDescriptorBlock::new(sheet.rules[0].descriptor_block.clone().unwrap());
    let shared = FfiDescriptorBlock::new(sheet.rules[0].descriptor_block.clone().unwrap());
    let font = sheet.rules[1].descriptor_block.clone().unwrap();
    drop(sheet);
    let data = page.data();
    assert_eq!(data.descriptors.len(), 2);
    assert_eq!(
        data.descriptors[0].id,
        descriptor_metadata(1, &utf16("margin-left")).unwrap().id
    );
    assert_eq!(
        data.descriptors[1].id,
        descriptor_metadata(1, &utf16("margin-top")).unwrap().id
    );
    let original = data.descriptors[1].value.clone();
    unsafe {
        let original = data.descriptors[1].view();
        let replacement = crate::css::descriptor_block::FfiDescriptor {
            name: original.name,
            id: original.id,
            value: Arc::as_ptr(&data.descriptors[0].value).cast(),
        };
        assert!(rust_descriptor_block_set(&mut page, &replacement));
    }
    assert!(Arc::ptr_eq(&shared.data().descriptors[1].value, &original));
    assert!(Arc::ptr_eq(
        &page.data().descriptors[1].value,
        &data.descriptors[0].value
    ));
    assert_eq!(font.descriptors.len(), 2);
    assert_eq!(
        serialize_style_value_to_utf16(&font.descriptors[0].value).unwrap(),
        utf16("色")
    );
}
