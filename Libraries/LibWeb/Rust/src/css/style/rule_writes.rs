/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The writes the host makes of its document's style sheets to the style engine. The host compiles a sheet's rules
//! itself and writes them, named by their native identities, which the engine resolves to rules of its own as it
//! applies the writes: where the frame is here, as the host makes them, and beside a frame in flight, once it is taken
//! in. The host numbers the sheets and keeps which rules it published to each, so a write to a sheet never asks the
//! engine anything.

use super::StyleEngine;
use super::bridge::{
    BoundScopeChain, FfiCascadeOrigin, intern_native_text, publish_rule_declarations, publish_style_rule,
    publish_style_rule_selectors, publish_user_agent_style_rule,
};
use super::compiler::NamespaceScope;
use super::program::{CascadeLayerID, RuleID, RuleKind, SheetID, StyleSheetObjectID};
use super::tree::TreeScopeID;
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::declaration_block::DeclarationBlockData;
use crate::css::selector::CompiledSelector;
use crate::css::selector_parser::RustParsedSelectorList;
use crate::css::style_sheet::NativeStyleSheet;
use crate::fast_hash::FastMap;
use std::cell::RefCell;
use std::sync::Arc;

/// How many native identities share one word of a sheet's table: a parsed sheet numbers its rules in one range.
const IDENTITIES_PER_WORD: u64 = u128::BITS as u64;

/// The rules the host published to each sheet of its document's style engine, by native identity, which the host
/// reads where it would otherwise ask the engine which of a sheet's rules it has. The engine numbers its sheets in the
/// order they are added and never reuses a number, so the host numbers them as it adds them.
#[derive(Default)]
pub(crate) struct PublishedRules {
    /// The identities each sheet holds, by the sheet's number less one, as one bit per identity.
    sheets: RefCell<Vec<FastMap<u64, u128>>>,
}

impl PublishedRules {
    /// Numbers the next sheet the host adds, as the boundary does: one more than the engine's own number.
    pub(crate) fn mint_sheet(&self) -> u32 {
        let mut sheets = self.sheets.borrow_mut();
        sheets.push(FastMap::default());
        u32::try_from(sheets.len()).expect("sheet identity space exhausted")
    }

    /// Whether the host published the rule `identity` to the sheet `sheet`.
    pub(crate) fn contains(&self, sheet: u32, identity: u64) -> bool {
        let sheets = self.sheets.borrow();
        sheet
            .checked_sub(1)
            .and_then(|index| sheets.get(index as usize))
            .and_then(|words| words.get(&(identity / IDENTITIES_PER_WORD)))
            .is_some_and(|word| word & bit(identity) != 0)
    }

    /// Notes that the host published the rule `identity` to the sheet `sheet`.
    pub(crate) fn insert(&self, sheet: u32, identity: u64) {
        let mut sheets = self.sheets.borrow_mut();
        *sheets[sheet as usize - 1]
            .entry(identity / IDENTITIES_PER_WORD)
            .or_default() |= bit(identity);
    }

    /// Takes the rule `identity` out of the sheet `sheet`, answering whether the host had published it there.
    pub(crate) fn remove(&self, sheet: u32, identity: u64) -> bool {
        let mut sheets = self.sheets.borrow_mut();
        let Some(words) = sheet.checked_sub(1).and_then(|index| sheets.get_mut(index as usize)) else {
            return false;
        };
        let key = identity / IDENTITIES_PER_WORD;
        let Some(word) = words.get_mut(&key) else {
            return false;
        };
        let held = *word & bit(identity) != 0;
        *word &= !bit(identity);
        if *word == 0 {
            words.remove(&key);
        }
        held
    }

    /// Forgets every rule of the sheet `sheet`, whose rules the engine is to replace.
    pub(crate) fn forget(&self, sheet: u32) {
        let mut sheets = self.sheets.borrow_mut();
        if let Some(words) = sheet.checked_sub(1).and_then(|index| sheets.get_mut(index as usize)) {
            *words = FastMap::default();
        }
    }

    /// The identity of the first rule after the subtree of the rule `identity` in `native`, the native sheet whose
    /// rules the host publishes to the sheet `sheet`, that the host published there, or 0 for none.
    pub(crate) fn successor(&self, sheet: u32, native: &NativeStyleSheet, identity: u64) -> u64 {
        crate::css::rule::mutation::successor(native, identity, |identity| self.contains(sheet, identity))
    }
}

fn bit(identity: u64) -> u128 {
    1 << (identity % IDENTITIES_PER_WORD)
}

/// The namespaces a sheet declares, as text, which the engine interns as it compiles the sheet's selectors.
pub(crate) struct NamespaceTexts {
    declared: Box<[DeclaredNamespace]>,
}

/// A declared namespace prefix, empty for the default namespace, and the namespace it names.
struct DeclaredNamespace {
    prefix: Box<[u16]>,
    uri: Box<[u16]>,
}

impl NamespaceTexts {
    /// The namespaces `native` declares, or none where it declares none.
    pub(crate) fn of(native: &NativeStyleSheet) -> Option<Arc<Self>> {
        let mut declared = Vec::new();
        native.rules().for_each_namespace(|namespace| {
            declared.push(DeclaredNamespace {
                prefix: namespace.prefix.units().into(),
                uri: namespace.uri.units().into(),
            });
        });
        (!declared.is_empty()).then(|| {
            Arc::new(Self {
                declared: declared.into(),
            })
        })
    }

    /// The namespaces `texts` declares, as the atoms `intern` interns their texts as.
    pub(super) fn scope(texts: Option<&Self>, mut intern: impl FnMut(&[u16]) -> super::StyleAtomID) -> NamespaceScope {
        let mut scope = NamespaceScope::default();
        for DeclaredNamespace { prefix, uri } in texts.map_or(&[][..], |texts| &texts.declared) {
            let uri = if uri.is_empty() {
                super::StyleAtomID::NONE
            } else {
                intern(uri)
            };
            if prefix.is_empty() {
                scope.default = Some(uri);
            } else {
                scope.by_prefix.push((intern(prefix), uri));
            }
        }
        scope
    }
}

/// A style rule's bound selectors, the scopes it is inside, and the namespaces its sheet declares.
pub(crate) struct BoundSelectors {
    pub(crate) selectors: Arc<RustParsedSelectorList>,
    pub(crate) scope: Arc<BoundScopeChain>,
    pub(crate) namespaces: Option<Arc<NamespaceTexts>>,
}

impl BoundSelectors {
    /// Lends `publish` the selectors and the scopes they are inside, with the namespaces their sheet declares, which
    /// the engine interns first.
    fn compile<R>(
        &self,
        engine: &mut StyleEngine,
        publish: impl FnOnce(&mut StyleEngine, &[&CompiledSelector], NamespaceScope, &BoundScopeChain) -> R,
    ) -> R {
        let namespaces = NamespaceTexts::scope(self.namespaces.as_deref(), |text| intern_native_text(engine, text));
        publish(engine, &self.compiled(), namespaces, &self.scope)
    }

    /// Publishes the selectors of `rule`, a rule of a user-agent sheet, with the program the process compiled for them,
    /// or answers `None` where the rule is not one or the engine compiles its own.
    fn publish_user_agent(&self, engine: &mut StyleEngine, sheet: u32, before: u32, rule: u64) -> Option<u32> {
        let namespaces = self.namespaces.as_deref();
        publish_user_agent_style_rule(engine, sheet, before, rule, &self.compiled(), namespaces, &self.scope)
    }

    fn compiled(&self) -> Vec<&CompiledSelector> {
        self.selectors.selectors.iter().map(AsRef::as_ref).collect()
    }
}

/// What a compiled rule is to the engine.
pub(crate) enum CompiledRuleKind {
    /// A style rule, or nested declarations, which matches elements.
    Style {
        selectors: BoundSelectors,
        gated_by_container_query: bool,
    },
    /// A rule found by the name it declares: `@property` or `@keyframes`.
    Named { kind: RuleKind, name: Box<[u16]> },
    /// A rule that neither matches nor is found by name: `@font-feature-values`, `@counter-style` or `@function`.
    NonMatching(RuleKind),
}

/// One rule the host compiled, as owned data the engine publishes.
pub(crate) struct CompiledRule {
    pub(crate) identity: u64,
    pub(crate) kind: CompiledRuleKind,
    pub(crate) declarations: Option<Arc<DeclarationBlockData>>,
    /// The native sheet the rule comes from, which may be a sheet the rule's sheet imports.
    pub(crate) source_identity: u64,
    pub(crate) conditions_hold: bool,
    /// Whether the rule cascades in the named layer `layer_name`.
    pub(crate) in_a_layer: bool,
    pub(crate) layer_name: Arc<[u16]>,
    /// The container conditions the rule is inside, innermost first.
    pub(crate) containers: Box<[Arc<ContainerConditionsData>]>,
}

impl CompiledRule {
    fn publish(self, engine: &mut StyleEngine, sheet: u32, before: u32) {
        let place = sheet
            .checked_sub(1)
            .map(|sheet| (SheetID(sheet), before.checked_sub(1).map(RuleID)));
        let rule = match &self.kind {
            CompiledRuleKind::Style { selectors, .. } => {
                let id = selectors
                    .publish_user_agent(engine, sheet, before, self.identity)
                    .unwrap_or_else(|| {
                        selectors.compile(engine, |engine, compiled, namespaces, scope| {
                            publish_style_rule(engine, sheet, before, compiled, namespaces, scope)
                        })
                    });
                if let Some(declarations) = &self.declarations {
                    publish_rule_declarations(engine, id, declarations);
                }
                id.checked_sub(1).map(RuleID)
            }
            CompiledRuleKind::Named { kind, name } => {
                let name = intern_native_text(engine, name);
                place.map(|(sheet, before)| match kind {
                    RuleKind::Property => engine.add_property_rule(sheet, before, name),
                    RuleKind::Keyframes => engine.add_keyframes_rule(sheet, before, name),
                    _ => unreachable!("only @property and @keyframes rules are found by name"),
                })
            }
            CompiledRuleKind::NonMatching(kind) => {
                assert!(
                    matches!(
                        kind,
                        RuleKind::FontFeatureValues | RuleKind::CounterStyle | RuleKind::Function
                    ),
                    "only rules of a non-matching kind are published without a name"
                );
                place.map(|(sheet, before)| engine.add_non_matching_rule(sheet, before, *kind))
            }
        };
        let Some(rule) = rule else {
            return;
        };
        if matches!(
            self.kind,
            CompiledRuleKind::Style {
                gated_by_container_query: true,
                ..
            }
        ) {
            engine.set_rule_gated_by_container_query(rule);
        }
        if !self.conditions_hold {
            engine.set_rule_conditions_hold(rule, false);
        }
        if self.in_a_layer {
            let layer = intern_native_text(engine, &self.layer_name).0;
            engine.set_rule_in_a_layer(rule);
            engine.set_rule_layer(rule, CascadeLayerID(layer));
        }
        engine.register_native_rule(
            rule,
            self.identity,
            self.declarations,
            self.source_identity,
            &self.layer_name,
            self.containers,
        );
    }
}

/// A write to a document's style sheets, which the engine applies in the order the host made it.
pub(crate) enum RuleWrite {
    /// The engine adds the sheet the host numbered `sheet`, which cascades in `origin`.
    AddSheet {
        sheet: u32,
        object: u32,
        origin: FfiCascadeOrigin,
    },
    /// The engine begins to replace the rules of `sheet`, keeping the identities of those the replacement keeps.
    BeginSheetRulesReplacement(u32),
    /// Rules the host compiled into `sheet`, in order, ahead of the rule `before` names, or at the sheet's end.
    PublishRules {
        sheet: u32,
        before: u64,
        rules: Vec<CompiledRule>,
    },
    /// A style rule keeps its place and its declarations, and selects what `selectors` select.
    ReplaceSelectors { identity: u64, selectors: BoundSelectors },
    /// Rules leave the engine, in the order the host removed them.
    RemoveRules(Box<[u64]>),
    /// The rule `identity` declares `declarations`.
    RuleDeclarations {
        identity: u64,
        declarations: Option<Arc<DeclarationBlockData>>,
    },
    /// Whether the conditions of each rule hold.
    RuleConditions(Box<[(u64, bool)]>),
    /// The order of the cascade layers of a tree scope, by qualified name, the unnamed outer layer as an empty one.
    LayerOrder { tree_scope: u32, names: Vec<Vec<u16>> },
}

impl RuleWrite {
    pub(crate) fn apply(self, engine: &mut StyleEngine) {
        match self {
            Self::AddSheet { sheet, object, origin } => {
                let added = engine.add_sheet(StyleSheetObjectID(object), origin.decode());
                assert_eq!(added.0 + 1, sheet, "the engine numbers a sheet as its host did");
            }
            Self::BeginSheetRulesReplacement(sheet) => {
                if let Some(sheet) = sheet.checked_sub(1) {
                    engine.begin_sheet_rules_replacement(SheetID(sheet));
                }
            }
            Self::PublishRules { sheet, before, rules } => {
                let before = rule_number(engine, before);
                for rule in rules {
                    rule.publish(engine, sheet, before);
                }
            }
            Self::ReplaceSelectors { identity, selectors } => {
                let rule = rule_number(engine, identity);
                selectors.compile(engine, |engine, compiled, namespaces, scope| {
                    publish_style_rule_selectors(engine, rule, compiled, namespaces, scope);
                });
            }
            Self::RemoveRules(identities) => {
                for identity in identities {
                    if let Some(rule) = engine.native_rule_id(identity) {
                        engine.remove_style_rule(rule);
                    }
                }
            }
            Self::RuleDeclarations { identity, declarations } => {
                if let Some(rule) = engine.native_rule_id(identity) {
                    publish_native_rule_declarations(engine, declarations, rule);
                }
            }
            Self::RuleConditions(conditions) => {
                for (identity, holds) in conditions {
                    if let Some(rule) = engine.native_rule_id(identity) {
                        engine.set_rule_conditions_hold(rule, holds);
                    }
                }
            }
            Self::LayerOrder { tree_scope, names } => {
                let layers: Vec<_> = names
                    .iter()
                    .map(|name| {
                        CascadeLayerID(if name.is_empty() {
                            0
                        } else {
                            intern_native_text(engine, name).0
                        })
                    })
                    .collect();
                engine.set_layer_order(TreeScopeID(tree_scope), &layers);
            }
        }
    }
}

/// The engine's number of the rule the native identity `identity` names, as the boundary numbers it, or 0 for none.
fn rule_number(engine: &StyleEngine, identity: u64) -> u32 {
    engine.native_rule_id(identity).map_or(0, |rule| rule.0 + 1)
}

/// Publishes `declarations` as what the engine's rule `rule` declares.
pub(crate) fn publish_native_rule_declarations(
    engine: &mut StyleEngine,
    declarations: Option<Arc<DeclarationBlockData>>,
    rule: RuleID,
) {
    engine.native_rules.targets.get_mut(&rule).unwrap().declarations = declarations.clone();
    let version = engine.next_declaration_block_version();
    engine.record_rule_declarations_changed(rule, version);
    if let Some(declarations) = &declarations {
        publish_rule_declarations(engine, rule.0 + 1, declarations);
    }
}

/// Adds a sheet to the document's style engine, which cascades in `origin`, and answers the number the host gave it.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_add_sheet(
    host: &crate::render_state::DocumentHost,
    object: u32,
    origin: FfiCascadeOrigin,
) -> u32 {
    let sheet = host.published_rules().mint_sheet();
    host.write_rules(RuleWrite::AddSheet { sheet, object, origin });
    sheet
}

/// Begins to replace the rules of the sheet `sheet`, whose rules the host compiles again, or none.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_begin_sheet_rules_replacement(
    host: &crate::render_state::DocumentHost,
    sheet: u32,
) {
    host.published_rules().forget(sheet);
    host.write_rules(RuleWrite::BeginSheetRulesReplacement(sheet));
}
