/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::read::RuleRef;
use super::{NativeRule, NativeRuleList, NativeRuleType, RulePayload};
use crate::css::style_sheet::NativeStyleSheet;
use std::ops::ControlFlow;
use std::rc::Rc;

// A false continuation skips descendants. Keyframe blocks belong to their definition and never
// occupy independent positions in the cascade program. Import conditions do not change ordering.
fn visit_rule(
    rule: &NativeRule,
    sheet: &NativeStyleSheet,
    detached_import: Option<&NativeStyleSheet>,
    visit: &mut impl FnMut(&NativeRule) -> ControlFlow<(), bool>,
) -> ControlFlow<()> {
    if !visit(rule)? {
        return ControlFlow::Continue(());
    }
    if rule.rule_type == NativeRuleType::Import {
        let imported = sheet.imported_sheet(rule.identity);
        if let Some(imported) = detached_import.or(imported.as_deref()) {
            visit_list(imported.rules(), imported, visit)?;
        }
    } else if rule.rule_type != NativeRuleType::Keyframes
        && let Some(children) = &rule.children
    {
        visit_list(children, sheet, visit)?;
    }
    ControlFlow::Continue(())
}

fn visit_list(
    list: &NativeRuleList,
    sheet: &NativeStyleSheet,
    visit: &mut impl FnMut(&NativeRule) -> ControlFlow<(), bool>,
) -> ControlFlow<()> {
    for rule in list.materialized_rules().iter() {
        visit_rule(rule, sheet, None, visit)?;
    }
    ControlFlow::Continue(())
}

pub(crate) fn successor(sheet: &NativeStyleSheet, identity: u64, mut compiled_id: impl FnMut(u64) -> u32) -> u32 {
    fn walk(
        rule: RuleRef<'_>,
        sheet: &NativeStyleSheet,
        identity: u64,
        found: &mut bool,
        compiled_id: &mut impl FnMut(u64) -> u32,
    ) -> ControlFlow<u32> {
        if rule.identity() == identity {
            *found = true;
            return ControlFlow::Continue(());
        }
        if *found {
            let id = compiled_id(rule.identity());
            if id != 0 {
                return ControlFlow::Break(id);
            }
        }
        let mut successor = 0;
        let mut visit = |child: RuleRef<'_>, sheet: &NativeStyleSheet| {
            if let ControlFlow::Break(id) = walk(child, sheet, identity, found, compiled_id) {
                successor = id;
                ControlFlow::Break(())
            } else {
                ControlFlow::Continue(())
            }
        };
        if rule.rule_type() == NativeRuleType::Import {
            if let Some(imported) = sheet.imported_sheet(rule.identity()) {
                let _ = imported.rules().visit_rules(&mut |child| visit(child, &imported));
            }
        } else if rule.rule_type() != NativeRuleType::Keyframes {
            let _ = rule.visit_children(&mut |child| visit(child, sheet));
        }
        if successor != 0 {
            ControlFlow::Break(successor)
        } else {
            ControlFlow::Continue(())
        }
    }
    let mut found = false;
    let mut successor = 0;
    let _ = sheet.rules().visit_rules(&mut |rule| {
        if let ControlFlow::Break(id) = walk(rule, sheet, identity, &mut found, &mut compiled_id) {
            successor = id;
            ControlFlow::Break(())
        } else {
            ControlFlow::Continue(())
        }
    });
    successor
}

/// Snapshot native owners before any host notifications or engine mutation can reenter.
///
/// # Safety
/// The root and all descendants must belong to live Rc allocations.
pub(crate) unsafe fn removed_rules(
    rule: &NativeRule,
    sheet: &NativeStyleSheet,
    detached_import: Option<&NativeStyleSheet>,
) -> Vec<Rc<NativeRule>> {
    let mut rules = Vec::new();
    let _ = visit_rule(rule, sheet, detached_import, &mut |rule| {
        let pointer = std::ptr::from_ref(rule);
        unsafe { Rc::increment_strong_count(pointer) };
        rules.push(unsafe { Rc::from_raw(pointer) });
        ControlFlow::Continue(true)
    });
    rules
}

pub(crate) fn declares_layer(rule: &NativeRule) -> bool {
    matches!(
        rule.rule_type,
        NativeRuleType::LayerStatement | NativeRuleType::LayerBlock
    ) || matches!(&rule.payload, RulePayload::Import { data, .. } if data.layer.is_some())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::media_list::MediaList;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::rule::rust_rule_list_remove;
    use crate::css::style_sheet::rust_style_sheet_set_import;

    fn sheet(source: &str) -> Rc<NativeStyleSheet> {
        let units: Vec<_> = source.encode_utf16().collect();
        let view = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context: ParseContext = unsafe { std::mem::zeroed() };
        let parsed = unsafe { parse_shared_stylesheet(view, &context) };
        let rules = crate::css::rule::NativeRuleList::from_parsed(parsed);
        let media = MediaList::new(Default::default());
        NativeStyleSheet::new(rules, media)
    }

    #[test]
    fn successor_skips_the_inserted_subtree_and_uncompiled_rules() {
        let sheet = sheet("@media all { .一 {} .二 {} } @layer 空; @supports (display: grid) { .三 {} } .四 {}");
        let rules = sheet.rules().materialized_rules();
        let first = &rules[0];
        let inside = first.children.as_ref().unwrap().materialized_rules()[0].identity;
        let next = rules[2].children.as_ref().unwrap().materialized_rules()[0].identity;
        let last = rules[3].identity;
        let compiled = |identity| {
            if identity == inside {
                1
            } else if identity == next {
                2
            } else if identity == last {
                3
            } else {
                0
            }
        };
        assert_eq!(successor(&sheet, first.identity, compiled), 2);
        assert_eq!(successor(&sheet, rules[2].identity, compiled), 3);
        assert_eq!(successor(&sheet, last, compiled), 0);
        assert_eq!(successor(&sheet, 0, compiled), 0);
    }

    #[test]
    fn imports_participate_in_order_and_detached_subtree_retirement() {
        let root = sheet("@import '一.css' layer(外); .最後 {}");
        let imported = sheet("@import '二.css'; .途中 {}");
        let nested = sheet("@keyframes 動き { from { opacity: 0 } to { opacity: 1 } } .先頭 {}");
        let import = root.rules().materialized_rules()[0].clone();
        let nested_import = imported.rules().materialized_rules()[0].clone();
        unsafe {
            rust_style_sheet_set_import(&root, import.identity, Rc::as_ptr(&imported));
            rust_style_sheet_set_import(&imported, nested_import.identity, Rc::as_ptr(&nested));
        }
        let middle = imported.rules().materialized_rules()[1].identity;
        let last = root.rules().materialized_rules()[1].identity;
        let compiled = |identity| {
            if identity == middle {
                7
            } else if identity == last {
                8
            } else {
                0
            }
        };
        assert_eq!(successor(&root, nested_import.identity, compiled), 7);
        assert_eq!(successor(&root, import.identity, compiled), 8);
        assert!(declares_layer(&import));
        assert!(!declares_layer(&nested_import));

        rust_rule_list_remove(root.rules(), 0);
        unsafe { rust_style_sheet_set_import(&root, import.identity, std::ptr::null()) };
        let removed = unsafe { removed_rules(&import, &root, Some(&imported)) };
        let kinds: Vec<_> = removed.iter().map(|rule| rule.rule_type).collect();
        assert!(
            kinds
                == [
                    NativeRuleType::Import,
                    NativeRuleType::Import,
                    NativeRuleType::Keyframes,
                    NativeRuleType::Style,
                    NativeRuleType::Style
                ]
        );
        assert!(!removed.iter().any(|rule| rule.identity == last));
        assert_eq!(removed.last().unwrap().identity, middle);
    }
}
