/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_tokenizer::TokenizerInput;
use crate::css::declaration_block::{DeclarationBlock, DeclarationBlockData};
use crate::css::selector_parser::{
    RustParsedSelectorList, SelectorParsingMode, SelectorType, StyleNestingParent, parse_selector_list,
};
use std::cell::RefCell;
use std::sync::Arc;

pub(crate) struct StyleRule {
    selectors: RefCell<Arc<RustParsedSelectorList>>,
    pub(crate) declarations: DeclarationBlock,
}

impl StyleRule {
    pub(crate) fn new(selectors: Arc<RustParsedSelectorList>, declarations: Arc<DeclarationBlockData>) -> Self {
        Self {
            selectors: RefCell::new(selectors),
            declarations: DeclarationBlock::new(declarations),
        }
    }

    pub(crate) fn selectors(&self) -> Arc<RustParsedSelectorList> {
        self.selectors.borrow().clone()
    }

    pub(crate) fn set_selector_text(
        &self,
        input: TokenizerInput<'_>,
        namespaces: &[TokenizerInput<'_>],
        nesting_parent: StyleNestingParent,
    ) -> bool {
        let selector_type = if nesting_parent == StyleNestingParent::None {
            SelectorType::Standalone
        } else {
            SelectorType::Relative
        };
        let Ok(selectors) = parse_selector_list(input, namespaces, selector_type, SelectorParsingMode::Standard) else {
            return false;
        };
        *self.selectors.borrow_mut() =
            Arc::new(RustParsedSelectorList::new(selectors).adapt_for_nesting(nesting_parent));
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::rule::{NativeRuleList, rust_rule_list_at, rust_rule_list_visit_images};
    use std::ffi::c_void;
    use std::rc::Rc;

    fn rules(source: &str) -> Rc<NativeRuleList> {
        let units: Vec<_> = source.encode_utf16().collect();
        let source = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context: ParseContext = unsafe { std::mem::zeroed() };
        let parsed = unsafe { parse_shared_stylesheet(source, &context) };
        crate::css::rule::NativeRuleList::from_parsed(parsed)
    }

    #[test]
    fn native_publication_and_image_discovery_keep_style_data_immutable() {
        use crate::css::style::StyleEngine;
        use crate::css::style::bridge::publish_rule_declarations;
        use crate::css::style::memory::DeviceClass;
        use crate::css::style::program::{CascadeOrigin, RuleKind, StyleSheetObjectID};
        let rules = rules(".文字 { width: 13px; transition: width 1s; background-image: url(image.png); }");
        let rule = unsafe { &*rust_rule_list_at(&rules, 0) };
        let style = rule.style_rule();
        assert!(style.declarations.is_immutable());
        assert!(Arc::ptr_eq(&style.selectors(), &style.selectors.borrow()));
        let declarations = rule.cascade_declarations().unwrap();
        assert!(Arc::ptr_eq(&declarations, &style.declarations.data()));
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let id = engine.add_non_matching_rule(sheet, None, RuleKind::Style);
        assert!(publish_rule_declarations(&mut engine, id.0 + 1, &declarations));
        extern "C" fn image(context: *mut c_void, _: *const c_void) {
            unsafe {
                *context.cast::<usize>() += 1;
            }
        }
        let mut images = 0_usize;
        unsafe {
            rust_rule_list_visit_images(&rules, (&raw mut images).cast(), image);
        }
        assert_eq!(images, 1);
        assert!(style.declarations.is_immutable());
    }

    #[test]
    fn cascade_targets_retain_immutable_snapshots_without_promoting_style_owners() {
        use crate::css::media_list::MediaList;
        use crate::css::property_metadata::property_id;
        use crate::css::rule::{rust_rule_list_clear, rust_rule_retain};
        use crate::css::style::StyleEngine;
        use crate::css::style::bridge::{
            FfiNativeRuleTarget, style_engine_native_rule_declarations_changed, style_engine_native_rule_target,
        };
        use crate::css::style::memory::DeviceClass;
        use crate::css::style::program::{CascadeOrigin, RuleKind, StyleSheetObjectID};
        use crate::css::style_sheet::NativeStyleSheet;

        let rules = rules(".文字 { width: 13px; --幅: 19px; }");
        let media = MediaList::new(Default::default());
        let source = NativeStyleSheet::new(rules.clone(), media);
        let source_lifetime = Rc::downgrade(&source);
        let source_identity = source.identity();
        let rule = unsafe { &*rust_rule_list_at(&rules, 0) };
        let retained_rule = unsafe { Rc::from_raw(rust_rule_retain(rule)) };
        let rule_lifetime = Rc::downgrade(&retained_rule);
        drop(retained_rule);
        let style = rule.style_rule();
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let sheet = engine.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let id = engine.add_non_matching_rule(sheet, None, RuleKind::Style);
        unsafe {
            engine.register_native_rule(
                id,
                crate::css::rule::rust_rule_identity(rule),
                rule.cascade_declarations(),
                source.identity(),
                &[],
                &[],
            )
        };
        // All fields are integers, nullable pointers, booleans, or enums whose zero variant is valid.
        let mut target: FfiNativeRuleTarget = unsafe { std::mem::zeroed() };
        assert!(unsafe { style_engine_native_rule_target((&raw const engine).cast(), id.0 + 1, &mut target) });
        let snapshot = unsafe { Arc::from_raw(target.declarations.cast::<DeclarationBlockData>()) };
        assert_eq!(target.source_identity, source_identity);
        assert!(style.declarations.is_immutable());
        assert!(Arc::ptr_eq(&snapshot, &style.declarations.data()));
        let identity = target.identity;
        let version = target.declaration_version;

        let mut declarations = style.declarations.clone();
        assert!(declarations.remove(property_id::WIDTH));
        unsafe extern "C" fn notify(_: *mut c_void, _: u32) {}
        unsafe {
            style_engine_native_rule_declarations_changed(
                (&raw mut engine).cast(),
                std::ptr::from_ref(rule).cast(),
                std::ptr::null_mut(),
                notify,
            );
        }
        assert!(unsafe { style_engine_native_rule_target((&raw const engine).cast(), id.0 + 1, &mut target) });
        let edited_snapshot = unsafe { Arc::from_raw(target.declarations.cast::<DeclarationBlockData>()) };
        assert_eq!(target.identity, identity);
        assert_ne!(target.declaration_version, version);
        assert!(!Arc::ptr_eq(&snapshot, &edited_snapshot));
        drop(declarations);
        rust_rule_list_clear(&rules);
        drop(source);
        drop(rules);
        assert!(source_lifetime.upgrade().is_none());
        assert!(rule_lifetime.upgrade().is_none());
        // The program reads its published snapshot, not a live document-local rule record.
        assert!(unsafe { style_engine_native_rule_target((&raw const engine).cast(), id.0 + 1, &mut target) });
        let surviving_snapshot = unsafe { Arc::from_raw(target.declarations.cast::<DeclarationBlockData>()) };
        assert!(Arc::ptr_eq(&edited_snapshot, &surviving_snapshot));
        assert_eq!(target.identity, identity);
        assert_eq!(target.source_identity, source_identity);
        engine.remove_style_rule(id);
        assert!(!unsafe { style_engine_native_rule_target((&raw const engine).cast(), id.0 + 1, &mut target) });
        drop(engine);
        assert!(
            snapshot
                .properties
                .iter()
                .any(|property| property.property_id == property_id::WIDTH)
        );
        assert!(edited_snapshot.properties.is_empty());
        assert_eq!(
            snapshot.custom_properties[0].name.units(),
            "--幅".encode_utf16().collect::<Vec<_>>()
        );
        assert_eq!(
            edited_snapshot.custom_properties[0].name.units(),
            snapshot.custom_properties[0].name.units()
        );
    }

    #[test]
    fn retained_rules_share_mutation_without_changing_parsed_snapshots() {
        use crate::css::property_metadata::property_id;
        let rules = rules(".文字 { width: 13px; height: 19px; }");
        let rule = unsafe { &*rust_rule_list_at(&rules, 0) };
        let style = rule.style_rule();
        let snapshot = style.declarations.data();
        let fork = StyleRule::new(style.selectors(), style.declarations.data());
        assert!(style.declarations.is_immutable());
        assert!(fork.declarations.is_immutable());
        let retained_rule = unsafe { Rc::from_raw(crate::css::rule::rust_rule_retain(rule)) };
        let retained = retained_rule.style_rule();
        assert!(std::ptr::eq(style, retained));
        let mut declarations = retained.declarations.clone();
        assert!(declarations.remove(property_id::WIDTH));
        assert!(
            !style
                .declarations
                .data()
                .properties
                .iter()
                .any(|property| property.property_id == property_id::WIDTH)
        );
        assert!(Arc::ptr_eq(&fork.declarations.data(), &snapshot));
        assert!(
            snapshot
                .properties
                .iter()
                .any(|property| property.property_id == property_id::WIDTH)
        );
        let selectors: Vec<_> = ".編集".encode_utf16().collect();
        assert!(retained.set_selector_text(TokenizerInput::Utf16(&selectors), &[], StyleNestingParent::None));
        assert!(Arc::ptr_eq(&style.selectors(), &retained.selectors()));
        assert!(!Arc::ptr_eq(&style.selectors(), &fork.selectors()));
    }
}
