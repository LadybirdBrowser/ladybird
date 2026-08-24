/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The semantic stylesheet program.
//!
//! CSSOM objects and the program are deliberately separate things. CSSOM objects preserve API
//! identity, ownership, indices, and serialization behaviour; the program is an immutable,
//! content-keyable representation used for computation. Editing a CSSOM object creates a new
//! semantic version without replacing the wrapper object, which is what lets a collection reflect
//! an API-required mutation synchronously while the semantic work stays queued until the next
//! required observation.
//!
//! Identity is split finely on purpose. A declaration edit preserves the rule's identity and its
//! selector program while creating a new rule version and declaration block, so selector truth is
//! not invalidated by an edit that cannot change it. A selector edit preserves CSSOM identity but
//! changes the selector program, so exactly the match relation is affected.
//!
//! Nested group rules form a persistent tree. Editing one descendant creates new versions only
//! along the path to it; unchanged selectors, declarations, and conditions keep their identities,
//! and a group rule detached from a sheet takes its subtree with it while shared descendants stay
//! alive if something else still references them.

pub use crate::css::cascaded_properties::CascadeOrigin;

use super::capacity::ShallowCapacityBytes;
use super::capacity::capacity_bytes;
use super::cascade::CascadeOperator;
use super::cascade::SpecifiedValueID;
use super::column::BitColumn;
use super::column::Column;
use super::fast_hash::FastMap as HashMap;
use super::fast_hash::FastSet as HashSet;
use super::index::StyleAtomID;
use super::instrumentation::Counters;
use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::order::OrderMaintenance;
use super::order::OrderToken;
use super::transaction::ProgramVersion;
use super::tree::TreeScopeID;

define_id! {
    /// Identity of a CSSOM `CSSStyleSheet` wrapper object. Assigned by C++, which owns the wrapper.
    pub struct StyleSheetObjectID(pub);
}
define_id! {
    /// Semantic identity of a sheet, stable across edits to its contents.
    pub struct SheetID(pub);
}
define_id! {
    /// Semantic identity of a rule, stable across edits to its declarations or selector.
    pub struct RuleID(pub);
}
define_id! {
    /// Immutable identity of a compiled selector program.
    pub struct SelectorProgramID(pub);
}
define_id! {
    /// Dense document identity of one entry in a compiled selector program.
    pub struct EntryID(pub);
}

/// One longhand property a rule declares, and whether it declares it important. Importance is part
/// of the identity because it moves the declaration to a different rung of the cascade, not because
/// it is a different property.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct DeclaredProperty {
    pub property: u16,
    pub important: bool,
    pub operator: CascadeOperator,
    pub value: SpecifiedValueID,
}

define_id! {
    /// Immutable identity of a declaration block, whether it came from a rule or a style node.
    pub struct DeclarationBlockID(pub);
}
define_id! {
    /// Collision-checked semantic identity of one complete rule declaration inventory.
    default pub(super) struct SemanticDeclarationID(pub(super));
}
define_id! {
    /// Immutable identity of a compiled condition program: `@media`, `@supports`, `@container`.
    pub struct ActivationPredicateID(pub);
}

define_id! {
    /// Identity of a cascade layer: the interned qualified layer name. Rules reference the identity
    /// rather than a copied numeric rank, so each tree scope can order the same name independently
    /// and a layer topology change does not rewrite every declaration that lives in it.
    pub struct CascadeLayerID(pub);
}

impl CascadeLayerID {
    /// The implicit outer layer that unlayered rules belong to.
    pub const UNLAYERED: Self = Self(0);
}

define_id! {
    /// Identity of an `@scope` scope. Distinct from a tree scope: one is a CSS construct, the other
    /// is a DOM one.
    pub struct StyleScopeID(pub);
}

impl StyleScopeID {
    /// Not inside any `@scope`.
    pub const NONE: Self = Self(0);
}

/// What kind of rule a program node is. The kinds exist so that a program delta can be routed to
/// the consumers that care: editing a keyframes rule begins at animations referencing its name,
/// not at every element in the scope.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RuleKind {
    Style,
    Media,
    CounterStyle,
    FontFeatureValues,
    Keyframes,
    Property,
    Function,
}

impl RuleKind {
    /// Whether the rule contributes declarations through selector matching.
    #[must_use]
    pub fn matches_elements(self) -> bool {
        self == Self::Style
    }
}

/// The immutable semantic contents of one rule version.
///
/// The sheet order token is deliberately absent: rules reference their sheet, and the sheet holds
/// the token. Moving a sheet therefore updates one token instead of rewriting every rule in it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RuleVersion {
    pub rule: RuleID,
    pub kind: RuleKind,
    /// The name a rule declares, for the rules that reach their consumers by name rather than by
    /// matching anything: the animation a `@keyframes` describes, the custom property an `@property`
    /// registers. Which index the name is looked up in is decided by the rule's kind.
    pub declared_name: Option<StyleAtomID>,
    /// Whether an `@property` registration carries an initial value. One that does gives the property
    /// a value on every element, whether or not the element mentions it, so what it reaches cannot be
    /// found by asking who uses the name.
    pub registration_has_initial_value: bool,
    pub selector_program: Option<SelectorProgramID>,
    pub declaration_block: Option<DeclarationBlockID>,
    pub activation_predicate: Option<ActivationPredicateID>,
    pub layer: CascadeLayerID,
    pub scope: StyleScopeID,
}

impl RuleVersion {
    #[must_use]
    pub fn new(rule: RuleID, kind: RuleKind) -> Self {
        Self {
            rule,
            kind,
            declared_name: None,
            registration_has_initial_value: false,
            selector_program: None,
            declaration_block: None,
            activation_predicate: None,
            layer: CascadeLayerID::UNLAYERED,
            scope: StyleScopeID::NONE,
        }
    }
}

/// One sheet's attachment to one style scope. A compiled program attaches to several scopes
/// without duplicating its selectors or declarations; only the scope-specific materializations
/// stay separate.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Attachment {
    pub tree_scope: TreeScopeID,
    pub order: OrderToken,
}

struct Sheet {
    origin: CascadeOrigin,
    /// Advances whenever this sheet's immutable selector or static-cascade dispatch changes.
    dispatch_version: u64,
    /// Top-level rules, in source order.
    rules: Vec<RuleID>,
    rule_order: OrderMaintenance,
    attachments: Vec<Attachment>,
    /// A disabled sheet keeps its program, identities, and order tokens; only its activation
    /// changes, which is why `disabled` is a predicate update rather than a detach.
    enabled: bool,
    /// Whether the sheet's own conditions currently hold. A sheet whose media does not match
    /// contributes nothing, and a rule inside it can reach no element.
    conditions_hold: bool,
    live: bool,
}

struct Rule {
    sheet: SheetID,
    /// Whether the conditions of the groups this rule sits inside currently hold. A rule behind an
    /// `@media` that does not match keeps its identity, its position and its compiled selector; it
    /// simply decides nothing, which is the same shape a disabled sheet has.
    conditions_hold: bool,
    /// Whether this rule sits in a cascade layer. Layer order decides between declarations that are
    /// in layers, so a rule outside every layer cannot change hands when that order moves.
    in_a_layer: bool,
    parent: Option<RuleID>,
    children: Vec<RuleID>,
    version_slot: u32,
    nested_order: OrderToken,
    live: bool,
    gated_by_container_query: bool,
    declared_properties: Vec<DeclaredProperty>,
    declarations_are_complete: bool,
    semantic_declaration: SemanticDeclarationID,
}

struct SemanticDeclarationEntry {
    id: SemanticDeclarationID,
    representative: RuleID,
}

/// The document's stylesheet program: identities, versions, order, and attachment.
pub struct StyleSheetProgram {
    sheets: Vec<Sheet>,
    rules: Vec<Rule>,
    rule_versions: Vec<RuleVersion>,
    semantic_declarations: HashMap<u64, Vec<SemanticDeclarationEntry>>,
    next_semantic_declaration_id: u32,

    /// Sheet order within each tree context. Sheets in different tree contexts are ordered by the
    /// encapsulation component of cascade priority, not by these tokens.
    sheet_order: Column<Option<OrderMaintenance>>,
    /// Sheets attached to each tree scope, in the order described by `sheet_order`.
    ///
    /// A constructed sheet commonly has thousands of attachments. Recovering one scope's sheets
    /// by scanning every sheet and every attachment would make adding those scopes quadratic.
    sheets_by_scope: Column<Vec<SheetID>>,
    /// Qualified layer-name ranks in each tree scope. Important declarations reverse the rank
    /// through the cascade-priority operator rather than maintaining a second order.
    layer_ranks: Column<Option<HashMap<CascadeLayerID, u32>>>,

    scopes_using_document_sheets: BitColumn,
    version: ProgramVersion,
    routing_liveness_version: u64,
    capacity_bytes: u64,
    charged_bytes: u64,
}

impl StyleSheetProgram {
    pub(super) fn collect_atoms(&self, atoms: &mut HashSet<StyleAtomID>) -> u64 {
        let mut visited = 0_u64;
        for rule in &self.rules {
            if !rule.live {
                continue;
            }
            visited += 1;
            let version = self.rule_versions[rule.version_slot as usize];
            if let Some(name) = version.declared_name {
                atoms.insert(name);
            }
            if version.layer != CascadeLayerID::UNLAYERED {
                atoms.insert(StyleAtomID(version.layer.0));
            }
        }
        for ranks in self.layer_ranks.iter().flatten() {
            visited += u64::try_from(ranks.len()).expect("layer rank count exceeds u64");
            atoms.extend(
                ranks
                    .keys()
                    .filter(|&&layer| layer != CascadeLayerID::UNLAYERED)
                    .map(|layer| StyleAtomID(layer.0)),
            );
        }
        visited
    }

    #[must_use]
    pub fn new() -> Self {
        Self {
            sheets: Vec::new(),
            rules: Vec::new(),
            rule_versions: Vec::new(),
            semantic_declarations: HashMap::default(),
            next_semantic_declaration_id: 1,
            sheet_order: Column::default(),
            sheets_by_scope: Column::default(),
            layer_ranks: Column::default(),
            scopes_using_document_sheets: BitColumn::default(),
            version: ProgramVersion(1),
            routing_liveness_version: 1,
            capacity_bytes: 0,
            charged_bytes: 0,
        }
    }

    #[must_use]
    pub fn version(&self) -> ProgramVersion {
        self.version
    }

    #[must_use]
    pub fn routing_liveness_version(&self) -> u64 {
        self.routing_liveness_version
    }

    #[must_use]
    pub fn rule_count(&self) -> u32 {
        u32::try_from(self.rules.len()).expect("rule identity space exhausted")
    }

    fn bump_version(&mut self) {
        self.version = ProgramVersion(self.version.0 + 1);
    }

    fn bump_routing_liveness_version(&mut self) {
        self.routing_liveness_version = self
            .routing_liveness_version
            .checked_add(1)
            .expect("routing liveness version overflow");
    }

    fn bump_sheet_dispatch_version(&mut self, sheet: SheetID) {
        let version = &mut self.sheets[sheet.0 as usize].dispatch_version;
        *version = version.checked_add(1).expect("sheet dispatch version overflow");
    }

    fn bump_rule_sheet_dispatch_version(&mut self, rule: RuleID) {
        self.bump_sheet_dispatch_version(self.rules[rule.0 as usize].sheet);
    }

    fn record_capacity_change(&mut self, previous: u64, current: u64) {
        if current >= previous {
            self.capacity_bytes += current - previous;
        } else {
            self.capacity_bytes -= previous - current;
        }
    }

    // -- Sheets ------------------------------------------------------------------------------

    pub fn add_sheet(&mut self, _object: StyleSheetObjectID, origin: CascadeOrigin) -> SheetID {
        let id = SheetID(u32::try_from(self.sheets.len()).expect("sheet identity space exhausted"));
        let previous_capacity = (self.sheets.capacity() * size_of::<Sheet>()) as u64;
        self.sheets.push(Sheet {
            origin,
            dispatch_version: 0,
            rules: Vec::new(),
            rule_order: OrderMaintenance::new(),
            attachments: Vec::new(),
            enabled: true,
            conditions_hold: true,
            live: true,
        });
        self.record_capacity_change(previous_capacity, (self.sheets.capacity() * size_of::<Sheet>()) as u64);
        id
    }

    #[must_use]
    pub fn sheet_dispatch_version(&self, sheet: SheetID) -> u64 {
        self.sheets[sheet.0 as usize].dispatch_version
    }

    fn sheet_order_mut(&mut self, tree_scope: TreeScopeID) -> &mut OrderMaintenance {
        let index = tree_scope.0 as usize;
        self.capacity_bytes += self.sheet_order.ensure(index);
        self.sheet_order[index].get_or_insert_with(OrderMaintenance::new)
    }

    /// Attach a sheet to a style scope at the end of that scope's sheet order.
    pub fn attach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) -> Attachment {
        let order_axis = self.sheet_order_mut(tree_scope);
        let previous_order_capacity = order_axis.capacity_bytes();
        let order = order_axis.insert_last();
        let current_order_capacity = order_axis.capacity_bytes();
        self.record_capacity_change(previous_order_capacity, current_order_capacity);
        let attachment = Attachment { tree_scope, order };
        let attachments = &mut self.sheets[sheet.0 as usize].attachments;
        let previous_attachment_capacity = (attachments.capacity() * size_of::<Attachment>()) as u64;
        attachments.push(attachment);
        let current_attachment_capacity = (attachments.capacity() * size_of::<Attachment>()) as u64;
        self.record_capacity_change(previous_attachment_capacity, current_attachment_capacity);
        let scope_index = tree_scope.0 as usize;
        self.capacity_bytes += self.sheets_by_scope.ensure(scope_index);
        let previous_scope_capacity = (self.sheets_by_scope[scope_index].capacity() * size_of::<SheetID>()) as u64;
        self.sheets_by_scope[scope_index].push(sheet);
        let current_scope_capacity = (self.sheets_by_scope[scope_index].capacity() * size_of::<SheetID>()) as u64;
        self.record_capacity_change(previous_scope_capacity, current_scope_capacity);
        self.bump_routing_liveness_version();
        self.bump_version();
        attachment
    }

    /// Attach a sheet immediately before another sheet's attachment in the same scope. Loading
    /// order does not determine cascade order: a linked or imported sheet receives its position
    /// from document and rule order before or while it loads, and attachment reuses that position.
    pub fn attach_sheet_before(&mut self, sheet: SheetID, before: Attachment) -> Attachment {
        let order_axis = self.sheet_order_mut(before.tree_scope);
        let previous_order_capacity = order_axis.capacity_bytes();
        let order = order_axis.insert_before(before.order);
        let current_order_capacity = order_axis.capacity_bytes();
        self.record_capacity_change(previous_order_capacity, current_order_capacity);
        let attachment = Attachment {
            tree_scope: before.tree_scope,
            order,
        };
        let attachments = &mut self.sheets[sheet.0 as usize].attachments;
        let previous_attachment_capacity = (attachments.capacity() * size_of::<Attachment>()) as u64;
        attachments.push(attachment);
        let current_attachment_capacity = (attachments.capacity() * size_of::<Attachment>()) as u64;
        self.record_capacity_change(previous_attachment_capacity, current_attachment_capacity);
        let scope_index = before.tree_scope.0 as usize;
        self.capacity_bytes += self.sheets_by_scope.ensure(scope_index);
        let position = self.sheets_by_scope[scope_index]
            .iter()
            .position(|&candidate| {
                self.attachment_in_scope(candidate, before.tree_scope)
                    .is_some_and(|attachment| attachment.order == before.order)
            })
            .expect("an insertion target must remain attached");
        let previous_scope_capacity = (self.sheets_by_scope[scope_index].capacity() * size_of::<SheetID>()) as u64;
        self.sheets_by_scope[scope_index].insert(position, sheet);
        let current_scope_capacity = (self.sheets_by_scope[scope_index].capacity() * size_of::<SheetID>()) as u64;
        self.record_capacity_change(previous_scope_capacity, current_scope_capacity);
        self.bump_routing_liveness_version();
        self.bump_version();
        attachment
    }

    pub fn detach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) {
        let attachments = &mut self.sheets[sheet.0 as usize].attachments;
        let Some(position) = attachments
            .iter()
            .position(|attachment| attachment.tree_scope == tree_scope)
        else {
            return;
        };
        let attachment = attachments.remove(position);
        if let Some(order) = self.sheet_order.get_mut(tree_scope.0 as usize).and_then(Option::as_mut) {
            let previous_capacity = order.capacity_bytes();
            order.remove(attachment.order);
            let current_capacity = order.capacity_bytes();
            self.record_capacity_change(previous_capacity, current_capacity);
        }
        if let Some(sheets) = self.sheets_by_scope.get_mut(tree_scope.0 as usize)
            && let Some(position) = sheets.iter().position(|&candidate| candidate == sheet)
        {
            sheets.remove(position);
        }
        self.bump_routing_liveness_version();
        self.bump_version();
    }

    /// Move a sheet within its scope. Only the sheet's own order token is written; the program
    /// nodes of its rules are untouched, because they reference the sheet rather than copying its
    /// position.
    #[cfg(test)]
    pub fn move_sheet_before(&mut self, sheet: SheetID, before: Attachment) {
        let tree_scope = before.tree_scope;
        self.detach_sheet(sheet, tree_scope);
        self.attach_sheet_before(sheet, before);
    }

    /// Enabling or disabling a sheet updates its activation, not its attachment: identities, order
    /// tokens, and compiled programs all survive.
    pub fn set_sheet_enabled(&mut self, sheet: SheetID, enabled: bool) {
        if self.sheets[sheet.0 as usize].enabled == enabled {
            return;
        }
        self.sheets[sheet.0 as usize].enabled = enabled;
        self.bump_routing_liveness_version();
        self.bump_version();
    }

    #[must_use]
    pub fn sheet_is_enabled(&self, sheet: SheetID) -> bool {
        self.sheets[sheet.0 as usize].enabled
    }

    /// Record whether a sheet's own conditions hold. This is activation, not attachment: the sheet
    /// keeps its program, its identities, and its position.
    pub fn set_sheet_conditions_hold(&mut self, sheet: SheetID, conditions_hold: bool) {
        if self.sheets[sheet.0 as usize].conditions_hold == conditions_hold {
            return;
        }
        self.sheets[sheet.0 as usize].conditions_hold = conditions_hold;
        self.bump_routing_liveness_version();
        self.bump_version();
    }

    #[must_use]
    pub fn sheet_conditions_hold(&self, sheet: SheetID) -> bool {
        self.sheets[sheet.0 as usize].conditions_hold
    }

    /// Whether a sheet currently contributes declarations at all.
    #[must_use]
    pub fn sheet_contributes(&self, sheet: SheetID) -> bool {
        let sheet = &self.sheets[sheet.0 as usize];
        sheet.live && sheet.enabled && sheet.conditions_hold
    }

    /// Whether a rule can decide anything right now: it is still in its sheet, and its sheet is
    /// contributing and attached somewhere.
    ///
    /// A detached sheet still contributes in the sense that its declarations are intact, which is
    /// what makes detaching it a change worth routing. It decides nothing while it is out, though,
    /// so a mutation cannot reach an element through one of its rules.
    #[must_use]
    pub fn rule_can_decide(&self, rule: RuleID) -> bool {
        if !self.rules[rule.0 as usize].live || !self.rules[rule.0 as usize].conditions_hold {
            return false;
        }
        let sheet = self.rules[rule.0 as usize].sheet;
        self.sheet_contributes(sheet) && !self.sheets[sheet.0 as usize].attachments.is_empty()
    }

    /// Record whether the conditions of the groups a rule sits inside hold. Like a sheet's own
    /// conditions this is activation, not existence: the rule keeps its identity and its place.
    /// Returns whether the answer moved.
    pub fn set_rule_conditions_hold(&mut self, rule: RuleID, conditions_hold: bool) -> bool {
        if self.rules[rule.0 as usize].conditions_hold == conditions_hold {
            return false;
        }
        self.rules[rule.0 as usize].conditions_hold = conditions_hold;
        self.bump_routing_liveness_version();
        self.bump_version();
        true
    }

    #[must_use]
    pub fn rule_conditions_hold(&self, rule: RuleID) -> bool {
        self.rules[rule.0 as usize].conditions_hold
    }

    pub fn set_rule_in_a_layer(&mut self, rule: RuleID, in_a_layer: bool) {
        if self.rules[rule.0 as usize].in_a_layer == in_a_layer {
            return;
        }
        self.rules[rule.0 as usize].in_a_layer = in_a_layer;
        self.bump_rule_sheet_dispatch_version(rule);
    }

    #[must_use]
    pub fn rule_is_in_a_layer(&self, rule: RuleID) -> bool {
        self.rules[rule.0 as usize].in_a_layer
    }

    /// Every live rule in this scope that sits in a cascade layer.
    #[must_use]
    pub fn rules_in_a_layer_in_scope(&self, scope: TreeScopeID) -> Vec<RuleID> {
        self.sheets_in_scope(scope)
            .into_iter()
            .flat_map(|sheet| self.rules_in_sheet(sheet))
            .filter(|rule| self.rules[rule.0 as usize].in_a_layer)
            .collect()
    }

    #[must_use]
    pub fn sheet_origin(&self, sheet: SheetID) -> CascadeOrigin {
        self.sheets[sheet.0 as usize].origin
    }

    #[must_use]
    pub fn sheet_attachments(&self, sheet: SheetID) -> &[Attachment] {
        &self.sheets[sheet.0 as usize].attachments
    }

    /// The style scopes a sheet is attached to. A sheet adopted by several shadow roots decides in
    /// each of them and in none of the others.
    #[must_use]
    pub fn sheet_scopes(&self, sheet: SheetID) -> Vec<TreeScopeID> {
        let mut scopes: Vec<TreeScopeID> = self.sheets[sheet.0 as usize]
            .attachments
            .iter()
            .map(|attachment| attachment.tree_scope)
            .collect();
        scopes.sort_unstable();
        scopes.dedup();
        scopes
    }

    /// Sheets attached to `tree_scope`, in cascade order.
    #[must_use]
    pub fn sheets_in_scope(&self, tree_scope: TreeScopeID) -> Vec<SheetID> {
        self.sheets_by_scope
            .get(tree_scope.0 as usize)
            .cloned()
            .unwrap_or_default()
    }

    pub(crate) fn set_sheets_in_scope(&mut self, tree_scope: TreeScopeID, sheets: &[SheetID]) {
        let index = tree_scope.0 as usize;
        self.capacity_bytes += self.sheet_order.ensure(index);
        self.capacity_bytes += self.sheets_by_scope.ensure(index);
        let previous_scope_capacity = (self.sheets_by_scope[index].capacity() * size_of::<SheetID>()) as u64;
        let previous_sheets = std::mem::replace(&mut self.sheets_by_scope[index], sheets.to_vec());
        let mut attachment_presence: Vec<_> = previous_sheets
            .iter()
            .chain(sheets)
            .map(|&sheet| (sheet, self.sheet_is_attached_somewhere(sheet)))
            .collect();
        attachment_presence.sort_unstable_by_key(|&(sheet, _)| sheet);
        attachment_presence.dedup_by_key(|entry| entry.0);
        let current_scope_capacity = (self.sheets_by_scope[index].capacity() * size_of::<SheetID>()) as u64;
        self.record_capacity_change(previous_scope_capacity, current_scope_capacity);
        for sheet in previous_sheets {
            self.sheets[sheet.0 as usize]
                .attachments
                .retain(|attachment| attachment.tree_scope != tree_scope);
        }
        let mut order = OrderMaintenance::new();
        for &sheet in sheets {
            let attachment = Attachment {
                tree_scope,
                order: order.insert_last(),
            };
            let attachments = &mut self.sheets[sheet.0 as usize].attachments;
            let previous_capacity = (attachments.capacity() * size_of::<Attachment>()) as u64;
            attachments.push(attachment);
            let current_capacity = (attachments.capacity() * size_of::<Attachment>()) as u64;
            self.record_capacity_change(previous_capacity, current_capacity);
        }
        let previous_order_capacity = self.sheet_order[index]
            .as_ref()
            .map_or(0, OrderMaintenance::capacity_bytes);
        let current_order_capacity = order.capacity_bytes();
        self.sheet_order[index] = Some(order);
        self.record_capacity_change(previous_order_capacity, current_order_capacity);
        if attachment_presence
            .into_iter()
            .any(|(sheet, was_attached)| was_attached != self.sheet_is_attached_somewhere(sheet))
        {
            self.bump_routing_liveness_version();
        }
        self.bump_version();
    }

    // -- Rules -------------------------------------------------------------------------------

    /// Append a rule to a sheet or to a grouping rule.
    pub fn append_rule(&mut self, sheet: SheetID, parent: Option<RuleID>, kind: RuleKind) -> RuleID {
        let order_axis = &mut self.sheets[sheet.0 as usize].rule_order;
        let previous_capacity = order_axis.capacity_bytes();
        let order = order_axis.insert_last();
        let current_capacity = order_axis.capacity_bytes();
        self.record_capacity_change(previous_capacity, current_capacity);
        self.create_rule(sheet, parent, kind, order, None, true)
    }

    pub(crate) fn reserve_rule(&mut self, sheet: SheetID, parent: Option<RuleID>, kind: RuleKind) -> RuleID {
        let order_axis = &mut self.sheets[sheet.0 as usize].rule_order;
        let previous_capacity = order_axis.capacity_bytes();
        let order = order_axis.insert_last();
        let current_capacity = order_axis.capacity_bytes();
        self.record_capacity_change(previous_capacity, current_capacity);
        self.create_rule(sheet, parent, kind, order, None, false)
    }

    /// Insert a rule immediately before an existing sibling. Existing rules are neither renumbered
    /// nor rematched: the new rule takes an order token between its neighbours.
    pub fn insert_rule_before(&mut self, before: RuleID, kind: RuleKind) -> RuleID {
        let sheet = self.rules[before.0 as usize].sheet;
        let parent = self.rules[before.0 as usize].parent;
        let sibling_order = self.rules[before.0 as usize].nested_order;
        let order_axis = &mut self.sheets[sheet.0 as usize].rule_order;
        let previous_capacity = order_axis.capacity_bytes();
        let order = order_axis.insert_before(sibling_order);
        let current_capacity = order_axis.capacity_bytes();
        self.record_capacity_change(previous_capacity, current_capacity);
        self.create_rule(sheet, parent, kind, order, Some(before), true)
    }

    pub(crate) fn reserve_rule_before(&mut self, before: RuleID, kind: RuleKind) -> RuleID {
        let sheet = self.rules[before.0 as usize].sheet;
        let parent = self.rules[before.0 as usize].parent;
        let sibling_order = self.rules[before.0 as usize].nested_order;
        let order_axis = &mut self.sheets[sheet.0 as usize].rule_order;
        let previous_capacity = order_axis.capacity_bytes();
        let order = order_axis.insert_before(sibling_order);
        let current_capacity = order_axis.capacity_bytes();
        self.record_capacity_change(previous_capacity, current_capacity);
        self.create_rule(sheet, parent, kind, order, Some(before), false)
    }

    fn create_rule(
        &mut self,
        sheet: SheetID,
        parent: Option<RuleID>,
        kind: RuleKind,
        order: OrderToken,
        before: Option<RuleID>,
        live: bool,
    ) -> RuleID {
        let id = RuleID(u32::try_from(self.rules.len()).expect("rule identity space exhausted"));
        let version_slot = self.allocate_rule_version(RuleVersion::new(id, kind));
        let previous_rule_capacity = (self.rules.capacity() * size_of::<Rule>()) as u64;
        self.rules.push(Rule {
            sheet,
            conditions_hold: true,
            in_a_layer: false,
            parent,
            children: Vec::new(),
            version_slot,
            nested_order: order,
            live,
            gated_by_container_query: false,
            declared_properties: Vec::new(),
            declarations_are_complete: false,
            semantic_declaration: SemanticDeclarationID::default(),
        });
        self.record_capacity_change(
            previous_rule_capacity,
            (self.rules.capacity() * size_of::<Rule>()) as u64,
        );

        let siblings = match parent {
            Some(parent) => &mut self.rules[parent.0 as usize].children,
            None => &mut self.sheets[sheet.0 as usize].rules,
        };
        let previous_sibling_capacity = (siblings.capacity() * size_of::<RuleID>()) as u64;
        match before.and_then(|before| siblings.iter().position(|sibling| *sibling == before)) {
            Some(position) => siblings.insert(position, id),
            None => siblings.push(id),
        }
        let current_sibling_capacity = (siblings.capacity() * size_of::<RuleID>()) as u64;
        self.record_capacity_change(previous_sibling_capacity, current_sibling_capacity);

        if live {
            self.bump_sheet_dispatch_version(sheet);
            self.bump_routing_liveness_version();
            self.bump_version();
        }
        id
    }

    #[must_use]
    pub fn rule_sheet(&self, rule: RuleID) -> SheetID {
        self.rules[rule.0 as usize].sheet
    }

    #[must_use]
    #[cfg(test)]
    pub fn rule_children(&self, rule: RuleID) -> &[RuleID] {
        &self.rules[rule.0 as usize].children
    }

    #[must_use]
    pub fn rule_is_live(&self, rule: RuleID) -> bool {
        self.rules[rule.0 as usize].live
    }

    #[must_use]
    pub fn sheet_is_attached_somewhere(&self, sheet: SheetID) -> bool {
        !self.sheets[sheet.0 as usize].attachments.is_empty()
    }

    pub fn sheets_with_attachment_state(&self) -> impl Iterator<Item = (SheetID, bool)> + '_ {
        self.sheets.iter().enumerate().map(|(index, sheet)| {
            (
                SheetID(u32::try_from(index).expect("sheet identity space exhausted")),
                !sheet.attachments.is_empty(),
            )
        })
    }

    pub fn live_selector_programs(&self) -> impl Iterator<Item = (RuleID, SelectorProgramID)> + '_ {
        self.rules.iter().enumerate().filter_map(|(index, rule)| {
            let version = self.rule_versions[rule.version_slot as usize];
            (rule.live).then_some((
                RuleID(u32::try_from(index).expect("rule identity space exhausted")),
                version.selector_program?,
            ))
        })
    }

    #[must_use]
    pub fn rule_version(&self, rule: RuleID) -> RuleVersion {
        self.rule_versions[self.rules[rule.0 as usize].version_slot as usize]
    }

    /// Replace one rule's semantic contents after the transaction carrying its previous value has
    /// been consumed. The rule identity, its position, and every identity inside the version that
    /// did not change are preserved, so a consumer can tell a declaration edit from a selector edit
    /// by comparing components.
    pub fn replace_rule_version(&mut self, rule: RuleID, contents: RuleVersion) {
        assert_eq!(contents.rule, rule, "a rule version must name its own rule");
        let slot = self.rules[rule.0 as usize].version_slot;
        let selector_changed = self.rule_versions[slot as usize].selector_program != contents.selector_program;
        self.rule_versions[slot as usize] = contents;
        self.bump_rule_sheet_dispatch_version(rule);
        if selector_changed {
            self.bump_routing_liveness_version();
        }
        self.bump_version();
    }

    pub(crate) fn replace_reserved_rule_version(&mut self, rule: RuleID, contents: RuleVersion) {
        assert_eq!(contents.rule, rule, "a rule version must name its own rule");
        let slot = self.rules[rule.0 as usize].version_slot;
        self.rule_versions[slot as usize] = contents;
        self.bump_rule_sheet_dispatch_version(rule);
    }

    pub(crate) fn set_rule_liveness(&mut self, changes: &[(RuleID, bool)]) {
        let mut changed = false;
        for &(rule, _) in changes.iter().filter(|(_, live)| *live) {
            changed |= !self.rules[rule.0 as usize].live;
            self.rules[rule.0 as usize].live = true;
        }

        let departing: HashSet<RuleID> = changes
            .iter()
            .filter_map(|&(rule, live)| (!live).then_some(rule))
            .collect();
        let roots: Vec<RuleID> = departing
            .iter()
            .copied()
            .filter(|&rule| {
                self.rules[rule.0 as usize]
                    .parent
                    .is_none_or(|parent| !departing.contains(&parent))
            })
            .collect();
        for root in roots {
            let sheet = self.rules[root.0 as usize].sheet;
            let parent = self.rules[root.0 as usize].parent;
            let siblings = match parent {
                Some(parent) => &mut self.rules[parent.0 as usize].children,
                None => &mut self.sheets[sheet.0 as usize].rules,
            };
            if let Some(position) = siblings.iter().position(|&sibling| sibling == root) {
                siblings.remove(position);
            }

            let mut removed = Vec::new();
            self.collect_subtree(root, &mut removed);
            for rule in removed {
                changed |= self.rules[rule.0 as usize].live;
                self.rules[rule.0 as usize].live = false;
                let order = self.rules[rule.0 as usize].nested_order;
                let order_axis = &mut self.sheets[sheet.0 as usize].rule_order;
                let previous_capacity = order_axis.capacity_bytes();
                order_axis.remove(order);
                let current_capacity = order_axis.capacity_bytes();
                self.record_capacity_change(previous_capacity, current_capacity);
            }
        }
        if changed {
            let sheets: HashSet<SheetID> = changes
                .iter()
                .map(|(rule, _)| self.rules[rule.0 as usize].sheet)
                .collect();
            for sheet in sheets {
                self.bump_sheet_dispatch_version(sheet);
            }
            self.bump_routing_liveness_version();
            self.bump_version();
        }
    }

    /// Remove a rule and the subtree rooted at it. Deleting a group rule detaches its whole
    /// subtree; the identities are retired together in one operation rather than one rule at a
    /// time.
    pub fn remove_rule(&mut self, rule: RuleID) -> Vec<RuleID> {
        let sheet = self.rules[rule.0 as usize].sheet;
        let parent = self.rules[rule.0 as usize].parent;
        let siblings = match parent {
            Some(parent) => &mut self.rules[parent.0 as usize].children,
            None => &mut self.sheets[sheet.0 as usize].rules,
        };
        if let Some(position) = siblings.iter().position(|sibling| *sibling == rule) {
            siblings.remove(position);
        }

        let mut removed = Vec::new();
        self.collect_subtree(rule, &mut removed);
        for &id in &removed {
            self.rules[id.0 as usize].live = false;
            let order = self.rules[id.0 as usize].nested_order;
            let order_axis = &mut self.sheets[sheet.0 as usize].rule_order;
            let previous_capacity = order_axis.capacity_bytes();
            order_axis.remove(order);
            let current_capacity = order_axis.capacity_bytes();
            self.record_capacity_change(previous_capacity, current_capacity);
        }
        self.bump_sheet_dispatch_version(sheet);
        self.bump_routing_liveness_version();
        self.bump_version();
        removed
    }

    fn collect_subtree(&self, rule: RuleID, out: &mut Vec<RuleID>) {
        out.push(rule);
        for &child in &self.rules[rule.0 as usize].children {
            self.collect_subtree(child, out);
        }
    }

    #[must_use]
    pub(crate) fn rule_subtree(&self, rule: RuleID) -> Vec<RuleID> {
        let mut rules = Vec::new();
        self.collect_subtree(rule, &mut rules);
        rules
    }

    #[must_use]
    pub(crate) fn all_top_level_rules_in_sheet(&self, sheet: SheetID) -> &[RuleID] {
        &self.sheets[sheet.0 as usize].rules
    }

    #[must_use]
    pub(crate) fn all_rules_in_sheet(&self, sheet: SheetID) -> Vec<RuleID> {
        let mut rules = Vec::new();
        for &top_level in &self.sheets[sheet.0 as usize].rules {
            self.collect_subtree(top_level, &mut rules);
        }
        rules.sort_by(|first, second| {
            self.sheets[sheet.0 as usize].rule_order.compare(
                self.rules[first.0 as usize].nested_order,
                self.rules[second.0 as usize].nested_order,
            )
        });
        rules
    }

    /// Every live style rule of a sheet, in cascade order within that sheet.
    #[must_use]
    pub fn rules_in_sheet(&self, sheet: SheetID) -> Vec<RuleID> {
        let top_level_rules = &self.sheets[sheet.0 as usize].rules;
        if top_level_rules
            .iter()
            .all(|rule| self.rules[rule.0 as usize].children.is_empty())
        {
            return top_level_rules
                .iter()
                .copied()
                .filter(|rule| self.rules[rule.0 as usize].live)
                .collect();
        }

        let mut rules: Vec<RuleID> = Vec::new();
        for &top_level in top_level_rules {
            self.collect_live_subtree(top_level, &mut rules);
        }
        rules.sort_by(|first, second| {
            self.sheets[sheet.0 as usize].rule_order.compare(
                self.rules[first.0 as usize].nested_order,
                self.rules[second.0 as usize].nested_order,
            )
        });
        rules
    }

    fn collect_live_subtree(&self, rule: RuleID, out: &mut Vec<RuleID>) {
        if !self.rules[rule.0 as usize].live {
            return;
        }
        out.push(rule);
        for &child in &self.rules[rule.0 as usize].children {
            self.collect_live_subtree(child, out);
        }
    }

    // -- Layers ------------------------------------------------------------------------------

    pub(crate) fn layer_ranks_in_order(layers: &[CascadeLayerID]) -> HashMap<CascadeLayerID, u32> {
        let mut ranks = HashMap::with_capacity_and_hasher(layers.len(), Default::default());
        for &layer in layers {
            if layer == CascadeLayerID::UNLAYERED || ranks.contains_key(&layer) {
                continue;
            }
            ranks.insert(
                layer,
                u32::try_from(ranks.len()).expect("cascade layer count exhausted"),
            );
        }
        ranks
    }

    #[must_use]
    pub fn layer_order_matches(&self, scope: TreeScopeID, layers: &[CascadeLayerID]) -> bool {
        let ranks = Self::layer_ranks_in_order(layers);
        self.layer_ranks
            .get(scope.0 as usize)
            .and_then(Option::as_ref)
            .map_or(ranks.is_empty(), |current| current == &ranks)
    }

    /// Put the named layers of `scope` in declaration order, ahead of its implicit outer layer.
    ///
    /// The parser publishes qualified name identities rather than ranks. This table is the one
    /// place that derives ranks, so shadow scopes can order the same spelling independently.
    pub fn set_layer_order(&mut self, scope: TreeScopeID, layers: &[CascadeLayerID]) -> bool {
        let ranks = Self::layer_ranks_in_order(layers);
        self.set_layer_ranks(scope, ranks)
    }

    pub(crate) fn set_layer_ranks(&mut self, scope: TreeScopeID, ranks: HashMap<CascadeLayerID, u32>) -> bool {
        let index = scope.0 as usize;
        self.capacity_bytes += self.layer_ranks.ensure(index);
        let initialized = self.layer_ranks[index].is_some();
        if !initialized && ranks.is_empty() {
            return false;
        }
        if self.layer_ranks[index].as_ref() == Some(&ranks) {
            return false;
        }
        let previous_capacity = self.layer_ranks[index].as_ref().map_or(0, |ranks| {
            (ranks.capacity() * size_of::<(CascadeLayerID, u32)>()) as u64
        });
        let current_capacity = (ranks.capacity() * size_of::<(CascadeLayerID, u32)>()) as u64;
        self.layer_ranks[index] = Some(ranks);
        self.record_capacity_change(previous_capacity, current_capacity);
        if initialized {
            self.bump_version();
        }
        true
    }

    #[must_use]
    pub fn layer_order_key(&self, scope: TreeScopeID) -> Vec<(CascadeLayerID, u32)> {
        let mut key: Vec<_> = self
            .layer_ranks
            .get(scope.0 as usize)
            .and_then(Option::as_ref)
            .into_iter()
            .flat_map(|ranks| ranks.iter().map(|(&layer, &rank)| (layer, rank)))
            .collect();
        key.sort_unstable_by_key(|(layer, _)| layer.0);
        key
    }

    #[must_use]
    pub fn layer_order_is_initialized(&self, scope: TreeScopeID) -> bool {
        self.layer_ranks.get(scope.0 as usize).is_some_and(Option::is_some)
    }

    /// The comparable rank of a layer. A topology change rewrites these ranks without touching any
    /// rule that references the layer.
    #[must_use]
    pub fn layer_rank(&self, scope: TreeScopeID, layer: CascadeLayerID) -> (u64, u64) {
        (u64::from(self.layer_index(scope, layer)), 0)
    }

    /// The dense rank C++ cascade consumers use. The implicit outer layer and an as-yet undeclared
    /// name sort after every named layer in the scope.
    #[must_use]
    pub fn layer_index(&self, scope: TreeScopeID, layer: CascadeLayerID) -> u32 {
        let ranks = self.layer_ranks.get(scope.0 as usize).and_then(Option::as_ref);
        ranks.and_then(|ranks| ranks.get(&layer).copied()).unwrap_or_else(|| {
            ranks.map_or(0, |ranks| {
                u32::try_from(ranks.len()).expect("cascade layer count exhausted")
            })
        })
    }

    #[must_use]
    pub fn layer_ranks(&self, scope: TreeScopeID) -> HashMap<CascadeLayerID, u32> {
        self.layer_ranks
            .get(scope.0 as usize)
            .and_then(Option::as_ref)
            .cloned()
            .unwrap_or_default()
    }

    /// Record that a rule sits behind a container query.
    ///
    /// A container query asks about the element being styled rather than about the document, so it
    /// is not something a rule's activation can answer once: two elements matching the same rule
    /// can disagree about it. Until the style-layout coordinator can answer it, matching reports
    /// the rule and the consumer applies the query, which is what the matcher already does.
    pub fn set_rule_gated_by_container_query(&mut self, rule: RuleID, gated: bool) {
        if self.rules[rule.0 as usize].gated_by_container_query == gated {
            return;
        }
        self.rules[rule.0 as usize].gated_by_container_query = gated;
        self.bump_rule_sheet_dispatch_version(rule);
    }

    #[must_use]
    pub fn rule_is_gated_by_container_query(&self, rule: RuleID) -> bool {
        self.rules[rule.0 as usize].gated_by_container_query
    }

    /// Record which longhand properties a rule declares, and which of them it marks important.
    ///
    /// The cascade needs this to know which properties a rule can win: only a property some added,
    /// removed, modified or reprioritized rule declares is a candidate for a winner change.
    pub fn set_rule_declared_properties(
        &mut self,
        rule: RuleID,
        declared: Vec<DeclaredProperty>,
        declarations_are_complete: bool,
    ) {
        if self.rules[rule.0 as usize].semantic_declaration != SemanticDeclarationID::default() {
            self.invalidate_semantic_declarations();
        }
        let entry = &mut self.rules[rule.0 as usize];
        let previous_capacity = (entry.declared_properties.capacity() * size_of::<DeclaredProperty>()) as u64;
        entry.declared_properties = declared;
        let current_capacity = (entry.declared_properties.capacity() * size_of::<DeclaredProperty>()) as u64;
        entry.declarations_are_complete = declarations_are_complete;
        self.record_capacity_change(previous_capacity, current_capacity);
        self.bump_rule_sheet_dispatch_version(rule);
    }

    fn invalidate_semantic_declarations(&mut self) {
        if self.semantic_declarations.is_empty() {
            return;
        }
        let previous_payload_capacity = self
            .semantic_declarations
            .values()
            .map(ShallowCapacityBytes::shallow_capacity_bytes)
            .sum::<u64>();
        self.semantic_declarations.clear();
        self.record_capacity_change(previous_payload_capacity, 0);
        for rule in &mut self.rules {
            rule.semantic_declaration = SemanticDeclarationID::default();
        }
    }

    /// Lazily name equal complete declaration inventories alike. Keeping representatives only for
    /// rules that cross the FFI boundary avoids charging ordinary stylesheet parsing for a C++
    /// style-sharing optimization. IDs are never reused, so invalidation cannot alias a cached ID.
    pub(super) fn ensure_semantic_declaration(&mut self, rule: RuleID) -> SemanticDeclarationID {
        let rule_index = rule.0 as usize;
        if !self.rules[rule_index].declarations_are_complete {
            return SemanticDeclarationID::default();
        }
        if self.rules[rule_index].semantic_declaration != SemanticDeclarationID::default() {
            return self.rules[rule_index].semantic_declaration;
        }
        let hash = super::intern_table::content_hash(&self.rules[rule.0 as usize].declared_properties);
        let previous_map_capacity = self.semantic_declarations.shallow_capacity_bytes();
        let previous_bucket_capacity = self
            .semantic_declarations
            .get(&hash)
            .map_or(0, semantic_declaration_bucket_capacity_bytes);
        let existing = self.semantic_declarations.get(&hash).and_then(|bucket| {
            bucket.iter().find_map(|entry| {
                let representative = entry.representative;
                (self.rules[representative.0 as usize].declared_properties
                    == self.rules[rule.0 as usize].declared_properties)
                    .then_some(entry.id)
            })
        });
        let id = match existing {
            Some(id) => id,
            None => {
                let id = SemanticDeclarationID(self.next_semantic_declaration_id);
                self.next_semantic_declaration_id = self
                    .next_semantic_declaration_id
                    .checked_add(1)
                    .expect("semantic declaration identity space exhausted");
                self.semantic_declarations
                    .entry(hash)
                    .or_default()
                    .push(SemanticDeclarationEntry {
                        id,
                        representative: rule,
                    });
                id
            }
        };
        let current_bucket_capacity =
            semantic_declaration_bucket_capacity_bytes(self.semantic_declarations.get(&hash).unwrap());
        self.record_capacity_change(previous_bucket_capacity, current_bucket_capacity);
        self.record_capacity_change(
            previous_map_capacity,
            self.semantic_declarations.shallow_capacity_bytes(),
        );
        let entry = &mut self.rules[rule.0 as usize];
        entry.semantic_declaration = id;
        id
    }

    #[must_use]
    pub fn declared_properties_of(&self, rule: RuleID) -> &[DeclaredProperty] {
        &self.rules[rule.0 as usize].declared_properties
    }

    #[must_use]
    pub fn declarations_are_complete_for(&self, rule: RuleID) -> bool {
        self.rules[rule.0 as usize].declarations_are_complete
    }

    /// Record that a tree scope's rules include the document's author sheets as well as its own,
    /// which is what a shadow root built from the document's styles does.
    pub fn set_scope_uses_document_sheets(&mut self, tree_scope: TreeScopeID) {
        let index = tree_scope.0 as usize;
        self.capacity_bytes += self.scopes_using_document_sheets.set(index, true).1;
    }

    #[must_use]
    pub fn scope_uses_document_sheets(&self, tree_scope: TreeScopeID) -> bool {
        self.scopes_using_document_sheets.contains(tree_scope.0 as usize)
    }

    /// The attachment of a sheet in one tree scope, if it is attached there.
    #[must_use]
    pub fn attachment_in_scope(&self, sheet: SheetID, tree_scope: TreeScopeID) -> Option<Attachment> {
        self.sheets[sheet.0 as usize]
            .attachments
            .iter()
            .find(|attachment| attachment.tree_scope == tree_scope)
            .copied()
    }

    /// The comparable rank of a sheet within a tree scope.
    #[must_use]
    pub fn sheet_rank(&self, attachment: Attachment) -> (u64, u64) {
        self.sheet_order
            .get(attachment.tree_scope.0 as usize)
            .and_then(Option::as_ref)
            .expect("an attachment implies its scope has an order")
            .rank(attachment.order)
    }

    /// The comparable rank of a rule within its sheet.
    #[must_use]
    pub fn rule_rank(&self, rule: RuleID) -> (u64, u64) {
        let sheet = self.rules[rule.0 as usize].sheet;
        self.sheets[sheet.0 as usize]
            .rule_order
            .rank(self.rules[rule.0 as usize].nested_order)
    }

    // -- Accounting --------------------------------------------------------------------------

    fn allocate_rule_version(&mut self, contents: RuleVersion) -> u32 {
        let slot = u32::try_from(self.rule_versions.len()).expect("rule version space exhausted");
        let previous_capacity = (self.rule_versions.capacity() * size_of::<RuleVersion>()) as u64;
        self.rule_versions.push(contents);
        self.record_capacity_change(
            previous_capacity,
            (self.rule_versions.capacity() * size_of::<RuleVersion>()) as u64,
        );
        slot
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [self.capacity_bytes];
            nested [];
            skip [
                self.sheets,
                self.rules,
                self.rule_versions,
                self.semantic_declarations,
                self.next_semantic_declaration_id,
                self.sheet_order,
                self.sheets_by_scope,
                self.layer_ranks,
                self.scopes_using_document_sheets,
                self.version,
                self.charged_bytes,
            ];
        }
    }

    #[cfg(test)]
    fn recompute_capacity_bytes(&self) -> u64 {
        let sheet_payload: usize = self
            .sheets
            .iter()
            .map(|sheet| {
                sheet.rules.capacity() * size_of::<RuleID>() + sheet.attachments.capacity() * size_of::<Attachment>()
            })
            .sum();
        let rule_payload: usize = self
            .rules
            .iter()
            .map(|rule| {
                rule.children.capacity() * size_of::<RuleID>()
                    + rule.declared_properties.capacity() * size_of::<DeclaredProperty>()
            })
            .sum();
        let orders: u64 = self
            .sheet_order
            .iter()
            .flatten()
            .map(OrderMaintenance::capacity_bytes)
            .sum::<u64>()
            + self
                .sheets
                .iter()
                .map(|sheet| sheet.rule_order.capacity_bytes())
                .sum::<u64>();
        let layer_rank_payload = self
            .layer_ranks
            .iter()
            .flatten()
            .map(|ranks| ranks.capacity() * size_of::<(CascadeLayerID, u32)>())
            .sum::<usize>();
        let scope_sheet_payload = self
            .sheets_by_scope
            .iter()
            .map(|sheets| sheets.capacity() * size_of::<SheetID>())
            .sum::<usize>();
        let semantic_declaration_payload = self
            .semantic_declarations
            .values()
            .map(semantic_declaration_bucket_capacity_bytes)
            .sum::<u64>();
        capacity_bytes! {
            shallow [self.sheets, self.rules, self.rule_versions, self.semantic_declarations, self.sheet_order, self.sheets_by_scope, self.layer_ranks];
            cached [];
            nested [
                self.scopes_using_document_sheets.capacity_bytes(),
                sheet_payload,
                rule_payload,
                layer_rank_payload,
                scope_sheet_payload,
                semantic_declaration_payload,
                orders,
            ];
            skip [self.next_semantic_declaration_id, self.version, self.capacity_bytes, self.charged_bytes];
        }
    }

    /// Reconcile the program's Tier-2 charge after a batch of edits.
    pub fn settle_memory(&mut self, memory: &mut MemoryController, _counters: &mut Counters) {
        let current = self.capacity_bytes();
        if current > self.charged_bytes {
            memory.reserve_required(MemoryCategory::RuleProgram, current - self.charged_bytes);
        } else if self.charged_bytes > current {
            memory.release(MemoryCategory::RuleProgram, self.charged_bytes - current);
        }
        self.charged_bytes = current;
        #[cfg(test)]
        assert_eq!(current, self.recompute_capacity_bytes());
    }
}

fn semantic_declaration_bucket_capacity_bytes(bucket: &Vec<SemanticDeclarationEntry>) -> u64 {
    bucket.shallow_capacity_bytes()
}

impl Default for StyleSheetProgram {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn program_with_sheet() -> (StyleSheetProgram, SheetID) {
        let mut program = StyleSheetProgram::new();
        let sheet = program.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        program.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        (program, sheet)
    }

    #[test]
    fn a_declaration_edit_keeps_the_rule_and_its_selector_program() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);

        let mut contents = program.rule_version(rule);
        contents.selector_program = Some(SelectorProgramID(9));
        contents.declaration_block = Some(DeclarationBlockID(1));
        program.replace_rule_version(rule, contents);
        let first = program.rule_version(rule);

        contents.declaration_block = Some(DeclarationBlockID(2));
        program.replace_rule_version(rule, contents);
        let second = program.rule_version(rule);

        assert_ne!(first, second, "a declaration edit creates a new rule version");
        assert_eq!(
            first.selector_program, second.selector_program,
            "and leaves the selector program identity alone"
        );
        assert_eq!(program.rule_version(rule), second);
    }

    #[test]
    fn a_selector_edit_changes_only_the_selector_program() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);

        let mut contents = program.rule_version(rule);
        contents.selector_program = Some(SelectorProgramID(1));
        contents.declaration_block = Some(DeclarationBlockID(5));
        program.replace_rule_version(rule, contents);

        contents.selector_program = Some(SelectorProgramID(2));
        program.replace_rule_version(rule, contents);

        assert_eq!(
            program.rule_version(rule).declaration_block,
            Some(DeclarationBlockID(5))
        );
    }

    #[test]
    fn routing_liveness_ignores_declarations_but_tracks_selector_and_activation_changes() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);
        let mut contents = program.rule_version(rule);
        contents.selector_program = Some(SelectorProgramID(1));
        program.replace_rule_version(rule, contents);
        let selector_version = program.routing_liveness_version();

        contents.declaration_block = Some(DeclarationBlockID(1));
        program.replace_rule_version(rule, contents);
        assert_eq!(program.routing_liveness_version(), selector_version);

        contents.selector_program = Some(SelectorProgramID(2));
        program.replace_rule_version(rule, contents);
        let replacement_version = program.routing_liveness_version();
        assert!(replacement_version > selector_version);

        assert!(program.set_rule_conditions_hold(rule, false));
        assert!(program.routing_liveness_version() > replacement_version);
    }

    #[test]
    fn complete_equal_declarations_share_a_collision_checked_identity() {
        let (mut program, sheet) = program_with_sheet();
        let first = program.append_rule(sheet, None, RuleKind::Style);
        let second = program.append_rule(sheet, None, RuleKind::Style);
        let third = program.append_rule(sheet, None, RuleKind::Style);
        let never_interned = program.append_rule(sheet, None, RuleKind::Style);
        let declared = |value| DeclaredProperty {
            property: 1,
            important: false,
            operator: CascadeOperator::Declared,
            value: SpecifiedValueID(value),
        };

        program.set_rule_declared_properties(first, vec![declared(10)], true);
        program.set_rule_declared_properties(second, vec![declared(10)], true);
        program.set_rule_declared_properties(third, vec![declared(20)], true);

        let first_identity = program.ensure_semantic_declaration(first);
        let second_identity = program.ensure_semantic_declaration(second);
        let third_identity = program.ensure_semantic_declaration(third);
        assert_ne!(first_identity, SemanticDeclarationID::default());
        assert_eq!(first_identity, second_identity);
        assert_ne!(first_identity, third_identity);

        program.set_rule_declared_properties(never_interned, vec![declared(30)], true);
        assert_eq!(program.ensure_semantic_declaration(first), first_identity);

        program.set_rule_declared_properties(second, vec![declared(10)], false);
        assert_eq!(
            program.ensure_semantic_declaration(second),
            SemanticDeclarationID::default()
        );
        assert_ne!(program.ensure_semantic_declaration(first), first_identity);
        assert_eq!(program.capacity_bytes(), program.recompute_capacity_bytes());
    }

    #[test]
    fn repeated_rule_version_replacement_keeps_program_storage_flat() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);
        let initial_bytes = program.capacity_bytes();

        for index in 0..128_u32 {
            let mut contents = program.rule_version(rule);
            contents.selector_program = Some(SelectorProgramID(index % 2));
            contents.declaration_block = Some(DeclarationBlockID(index % 2));
            program.replace_rule_version(rule, contents);
        }

        assert_eq!(program.capacity_bytes(), initial_bytes);
    }

    #[test]
    fn inserting_a_rule_does_not_renumber_its_neighbours() {
        let (mut program, sheet) = program_with_sheet();
        let first = program.append_rule(sheet, None, RuleKind::Style);
        let last = program.append_rule(sheet, None, RuleKind::Style);
        let first_version = program.rule_version(first);
        let last_version = program.rule_version(last);

        let middle = program.insert_rule_before(last, RuleKind::Style);

        assert_eq!(program.rules_in_sheet(sheet), vec![first, middle, last]);
        assert_eq!(program.rule_version(first), first_version);
        assert_eq!(program.rule_version(last), last_version);
    }

    #[test]
    fn a_grouping_rule_owns_its_subtree() {
        let (mut program, sheet) = program_with_sheet();
        let media = program.append_rule(sheet, None, RuleKind::Media);
        let inner = program.append_rule(sheet, Some(media), RuleKind::Style);
        let nested = program.append_rule(sheet, Some(inner), RuleKind::Style);
        let outside = program.append_rule(sheet, None, RuleKind::Style);

        assert_eq!(program.rule_children(media), &[inner]);
        assert_eq!(program.rules_in_sheet(sheet), vec![media, inner, nested, outside]);

        let removed = program.remove_rule(media);
        assert_eq!(removed, vec![media, inner, nested]);
        assert!(!program.rule_is_live(inner));
        assert!(program.rule_is_live(outside));
        assert_eq!(program.rules_in_sheet(sheet), vec![outside]);
    }

    #[test]
    fn moving_a_sheet_writes_one_token_and_leaves_its_rules_alone() {
        let mut program = StyleSheetProgram::new();
        let first = program.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let second = program.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
        program.attach_sheet(first, TreeScopeID::DOCUMENT);
        let second_attachment = program.attach_sheet(second, TreeScopeID::DOCUMENT);
        let rule = program.append_rule(first, None, RuleKind::Style);
        let version = program.rule_version(rule);

        assert_eq!(program.sheets_in_scope(TreeScopeID::DOCUMENT), vec![first, second]);
        program.move_sheet_before(first, second_attachment);

        assert_eq!(program.sheets_in_scope(TreeScopeID::DOCUMENT), vec![first, second]);
        assert_eq!(program.rule_version(rule), version, "rules reference the sheet's token");
    }

    #[test]
    fn replacing_scope_sheets_tracks_global_attachment_liveness() {
        let mut program = StyleSheetProgram::new();
        let first = program.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        let second = program.add_sheet(StyleSheetObjectID(2), CascadeOrigin::Author);
        program.append_rule(first, None, RuleKind::Style);
        program.append_rule(second, None, RuleKind::Style);
        let unattached_version = program.routing_liveness_version();

        program.set_sheets_in_scope(TreeScopeID::DOCUMENT, &[first, second]);
        let attached_version = program.routing_liveness_version();
        assert!(attached_version > unattached_version);

        program.set_sheets_in_scope(TreeScopeID::DOCUMENT, &[second, first]);
        assert_eq!(program.routing_liveness_version(), attached_version);

        program.set_sheets_in_scope(TreeScopeID::DOCUMENT, &[]);
        assert!(program.routing_liveness_version() > attached_version);
    }

    #[test]
    fn one_program_attaches_to_several_scopes() {
        let mut program = StyleSheetProgram::new();
        let sheet = program.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        program.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        program.attach_sheet(sheet, TreeScopeID(1));
        let rule = program.append_rule(sheet, None, RuleKind::Style);

        assert_eq!(program.sheet_attachments(sheet).len(), 2);
        assert_eq!(program.sheets_in_scope(TreeScopeID(1)), vec![sheet]);
        assert_eq!(
            program.rules_in_sheet(sheet),
            vec![rule],
            "the rules are not duplicated"
        );

        program.detach_sheet(sheet, TreeScopeID(1));
        assert!(program.sheets_in_scope(TreeScopeID(1)).is_empty());
        assert_eq!(program.sheets_in_scope(TreeScopeID::DOCUMENT), vec![sheet]);
    }

    #[test]
    fn disabling_a_sheet_keeps_its_program_and_position() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);
        let version = program.rule_version(rule);

        program.set_sheet_enabled(sheet, false);
        assert!(!program.sheet_is_enabled(sheet));
        assert_eq!(program.sheets_in_scope(TreeScopeID::DOCUMENT), vec![sheet]);
        assert_eq!(program.rule_version(rule), version);
    }

    #[test]
    fn layer_topology_changes_ranks_without_touching_rules() {
        let mut program = StyleSheetProgram::new();
        let base = CascadeLayerID(1);
        let theme = CascadeLayerID(2);
        let shadow = TreeScopeID(1);
        assert!(!program.set_layer_order(TreeScopeID::DOCUMENT, &[]));
        assert!(!program.layer_order_is_initialized(TreeScopeID::DOCUMENT));
        assert!(program.set_layer_order(TreeScopeID::DOCUMENT, &[base, theme]));
        assert!(program.layer_rank(TreeScopeID::DOCUMENT, base) < program.layer_rank(TreeScopeID::DOCUMENT, theme));
        assert!(program.set_layer_order(shadow, &[theme, base]));
        assert!(program.layer_rank(shadow, theme) < program.layer_rank(shadow, base));
        assert!(!program.set_layer_order(shadow, &[theme, base]));
        // An unlayered declaration wins against every layered one of the same origin, so the
        // implicit outer layer sorts after every named one however they were declared.
        assert!(
            program.layer_rank(TreeScopeID::DOCUMENT, theme)
                < program.layer_rank(TreeScopeID::DOCUMENT, CascadeLayerID::UNLAYERED)
        );
        assert!(program.layer_rank(shadow, base) < program.layer_rank(shadow, CascadeLayerID::UNLAYERED));
        assert!(program.set_layer_order(TreeScopeID::DOCUMENT, &[]));
        assert_eq!(
            program.layer_rank(TreeScopeID::DOCUMENT, base),
            program.layer_rank(TreeScopeID::DOCUMENT, CascadeLayerID::UNLAYERED)
        );
    }

    #[test]
    fn program_bytes_are_charged_to_tier_two() {
        use super::super::memory::DeviceClass;

        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut counters = Counters::new();
        let (mut program, sheet) = program_with_sheet();
        for _ in 0..100 {
            program.append_rule(sheet, None, RuleKind::Style);
        }
        program.settle_memory(&mut memory, &mut counters);
        assert_eq!(
            memory.bytes_in_category(MemoryCategory::RuleProgram),
            program.capacity_bytes()
        );
        assert!(memory.bytes_in_category(MemoryCategory::RuleProgram) > 0);
    }

    #[test]
    fn every_structural_edit_advances_the_program_version() {
        let (mut program, sheet) = program_with_sheet();
        let before = program.version();
        let rule = program.append_rule(sheet, None, RuleKind::Style);
        assert!(program.version() > before);

        let before = program.version();
        program.remove_rule(rule);
        assert!(program.version() > before);
    }
}
