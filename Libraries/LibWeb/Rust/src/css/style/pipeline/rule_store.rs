/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rules the engine decides with.
//!
//! CSSOM objects and the rule store are deliberately separate things. CSSOM objects preserve API
//! identity, ownership, indices, and serialization behavior; the store is an immutable,
//! content-keyable representation used for computation. Editing a CSSOM object creates a new
//! semantic version without replacing the wrapper object, which is what lets a collection reflect
//! an API-required mutation synchronously while the semantic work stays queued until the next
//! required observation.
//!
//! Identity is split finely on purpose. A declaration edit preserves the rule's identity and its
//! selectors while creating a new rule version and declaration block, so selector truth is not
//! invalidated by an edit that cannot change it. A selector edit preserves CSSOM identity but
//! changes the selectors, so exactly the match relation is affected.
//!
//! Nested group rules form a persistent tree. Editing one descendant creates new versions only
//! along the path to it; unchanged selectors, declarations, and conditions keep their identities,
//! and a group rule detached from a sheet takes its subtree with it while shared descendants stay
//! alive if something else still references them.

mod identities;
pub(in crate::css::style) mod native;
mod rule_records;
mod rule_versions;
pub(in crate::css::style) use native::TargetPages;
pub(in crate::css::style) use rule_records::RuleRecordPages;
pub(in crate::css::style) use rule_versions::RuleVersionPages;
mod sheet_occurrences;
mod staging;
mod user_agent;
use crate::css::declaration_block::DeclarationEntry;
use crate::css::declaration_block::{DeclarationBlockHeader, SharedDeclarationBlock};
pub use identities::*;
use rule_records::RuleRecordTable;
use rule_versions::RuleVersionTable;
pub(in crate::css::style) use sheet_occurrences::ScopeSheetOccurrences;
pub(in crate::css::style) use staging::{ProgramStaging, RuleChanges, SheetRuleReplacement};
use user_agent::UserAgentBinding;
pub(in crate::css::style) use user_agent::{UserAgentRules, UserAgentVariant};

use super::super::column::BitColumn;
use super::super::column::Column;
use super::super::fast_hash::FastMap as HashMap;
use super::super::fast_hash::FastSet as HashSet;
use super::super::index::StyleAtomID;
use super::super::order::OrderMaintenance;
use super::super::order::OrderToken;
use super::super::tree::TreeScopeID;
use crate::css::style_value::RetainedStyleValueData;
use std::hash::{Hash, Hasher};
use std::num::NonZeroU32;
use std::sync::Arc;

/// One sheet's attachment to one style scope. A sheet attaches to several scopes
/// without duplicating its selectors or declarations; only the scope-specific materializations
/// stay separate.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Attachment {
    pub tree_scope: TreeScopeID,
    pub order: OrderToken,
}

#[derive(Clone)]
struct Sheet {
    origin: CascadeOrigin,
    /// Top-level rules, in source order.
    rules: Arc<Vec<RuleID>>,
    rule_order: Arc<OrderMaintenance>,
    /// Whether `rules` and `rule_order` are the ones the process shares, which documents whose
    /// sheets hold their rules alike share. An edit copies them first.
    rules_shared: bool,
    attachments: Vec<Attachment>,
    /// A disabled sheet keeps its program, identities, and order tokens; only its activation
    /// changes, which is why `disabled` is a predicate update rather than a detach.
    enabled: bool,
    /// Whether the sheet's own conditions currently hold. A sheet whose media does not match
    /// contributes nothing, and a rule inside it can reach no element.
    conditions_hold: bool,
    live: bool,
    /// The arenas the selector lists of the sheet's rules are in.
    selectors: SheetSelectors,
    /// The sheet of the process's user agent rules this one stands for, which holds its rules.
    user_agent: Option<SheetID>,
}

/// The arenas the selector lists of one sheet's rules are in: the sheet's parse, and each CSSOM
/// edit's, laid out one after another in one space, so that a rule version names its list by one
/// number. An arena never takes the place of another, so a number names one list for as long as
/// the sheet lives.
#[derive(Clone, Default)]
struct SheetSelectors {
    /// Each arena, by where it starts in the space, in increasing order of starts.
    arenas: Vec<(u32, crate::css::selector::SelectorListRef)>,
    /// Where each arena starts, by its serial.
    starts: HashMap<crate::css::selector_arena::SelectorArenaSerial, u32>,
    /// Where the next arena starts.
    next: u32,
    /// How many arenas the sheet kept when it last let go of those no rule named.
    kept: usize,
}

impl SheetSelectors {
    /// Where `list` starts in the space, which takes its arena where it has not yet.
    fn start_of(&mut self, list: &crate::css::selector::SelectorListRef) -> SheetListStart {
        let Some(offset) = list.start_offset() else {
            return SheetListStart::NONE;
        };
        let base = match self.starts.get(&list.serial()) {
            Some(&base) => base,
            None => {
                let base = self.next;
                self.next = base
                    .checked_add(list.arena_len())
                    .filter(|&next| next < SheetListStart::NONE.0)
                    .expect("sheet selector space exhausted");
                self.arenas.push((base, list.clone()));
                self.starts.insert(list.serial(), base);
                base
            }
        };
        SheetListStart(base + offset)
    }

    /// The list starting at `start`.
    fn list(&self, start: SheetListStart) -> Option<crate::css::selector::SelectorListRef> {
        if start == SheetListStart::NONE {
            return None;
        }
        let index = self.arenas.partition_point(|&(base, _)| base <= start.0);
        let (base, arena) = &self.arenas[index.checked_sub(1).expect("a list starts in an arena of its sheet")];
        Some(arena.list_at_offset(start.0 - base))
    }

    /// Whether the sheet took enough arenas since it last let go of some that it is worth looking
    /// for those no rule names.
    fn wants_letting_go(&self) -> bool {
        self.arenas.len() > 2 * self.kept + 4
    }

    /// Keep only the arenas some list of `named` starts in.
    fn keep_named(&mut self, named: &mut [SheetListStart]) {
        named.sort_unstable_by_key(|start| start.0);
        let mut named = named.iter().map(|start| start.0).peekable();
        let ends: Vec<u32> = self
            .arenas
            .iter()
            .skip(1)
            .map(|&(base, _)| base)
            .chain(std::iter::once(self.next))
            .collect();
        let mut index = 0;
        self.arenas.retain(|&(base, _)| {
            let end = ends[index];
            index += 1;
            while named.next_if(|&start| start < base).is_some() {}
            named.peek().is_some_and(|&start| start < end)
        });
        self.starts = self
            .arenas
            .iter()
            .map(|(base, arena)| (arena.serial(), *base))
            .collect();
        self.kept = self.arenas.len();
    }

    fn heap_size(&self) -> usize {
        crate::heap_size::vec_bytes(&self.arenas) + crate::heap_size::map_bytes(&self.starts)
    }
}

#[derive(Clone)]
struct Rule {
    sheet: SheetID,
    /// Whether the conditions of the groups this rule sits inside currently hold. A rule behind an
    /// `@media` that does not match keeps its identity, its position and its selectors; it
    /// simply decides nothing, which is the same shape a disabled sheet has.
    conditions_hold: bool,
    /// Whether this rule sits in a cascade layer. Layer order decides between declarations that are
    /// in layers, so a rule outside every layer cannot change hands when that order moves.
    in_a_layer: bool,
    parent: Option<RuleID>,
    children_slot: Option<NonZeroU32>,
    nested_order: OrderToken,
    live: bool,
    gated_by_container_query: bool,
    /// The rule's declaration block, which holds what the cascade reads of it.
    declarations: Option<SharedDeclarationBlock>,
}

// NB: Every rule of every sheet a document attaches has one.
const _: () = assert!(size_of::<Rule>() == 32);

impl Rule {
    fn sharing_key(&self) -> impl Eq + Hash + '_ {
        let Self {
            sheet,
            conditions_hold,
            in_a_layer,
            parent,
            children_slot,
            nested_order,
            live,
            gated_by_container_query,
            declarations,
        } = self;
        (
            sheet,
            conditions_hold,
            in_a_layer,
            parent,
            children_slot,
            nested_order,
            live,
            gated_by_container_query,
            declarations.as_ref().map(SharedDeclarationBlock::as_ptr),
        )
    }
}

impl PartialEq for Rule {
    fn eq(&self, other: &Self) -> bool {
        self.sharing_key() == other.sharing_key()
    }
}

impl Eq for Rule {}

impl Hash for Rule {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.sharing_key().hash(state);
    }
}

type WrittenValueChecks = super::pipeline::compute::WrittenValueChecks;

/// What the cascade reads of a declaration block beside its entries and values, in one
/// allocation the block holds: whether any declaration may move layout geometry, then the checks
/// of each longhand's written value, parallel to the block's values. It depends on the block's
/// content alone, so every engine publishing the block shares it.
pub(crate) struct BlockCascadeData {
    length: u32,
    may_affect_layout_geometry: bool,
}

const _: () = assert!(size_of::<BlockCascadeData>() == 8);
const _: () = assert!(align_of::<WrittenValueChecks>() <= align_of::<BlockCascadeData>());
const _: () = assert!(size_of::<BlockCascadeData>().is_multiple_of(align_of::<WrittenValueChecks>()));

impl BlockCascadeData {
    fn layout(length: usize) -> std::alloc::Layout {
        let too_many = "block declares too many properties";
        let bytes = length
            .checked_mul(size_of::<WrittenValueChecks>())
            .and_then(|bytes| bytes.checked_add(size_of::<Self>()))
            .expect(too_many);
        std::alloc::Layout::from_size_align(bytes, align_of::<Self>()).expect(too_many)
    }

    /// What the cascade reads of `block`.
    pub(in crate::css::style) fn new(block: &DeclarationBlockHeader) -> std::ptr::NonNull<Self> {
        let properties = block.properties();
        let length = properties.values().len();
        let may_affect_layout_geometry = !block.custom_properties().is_empty()
            || properties
                .entries()
                .iter()
                .any(|entry| crate::css::property_metadata::property_may_affect_layout_geometry(entry.property()));
        let layout = Self::layout(length);
        // SAFETY: The layout is nonzero, and the header and each check are written once.
        unsafe {
            let base = std::alloc::alloc(layout);
            if base.is_null() {
                std::alloc::handle_alloc_error(layout);
            }
            base.cast::<Self>().write(Self {
                length: u32::try_from(length).expect("block declares too many properties"),
                may_affect_layout_geometry,
            });
            let checks = base.add(size_of::<Self>()).cast::<WrittenValueChecks>();
            let written = RetainedStyleValueData::slice_from_shared(properties.values());
            for (index, (entry, value)) in properties.entries().iter().zip(written).enumerate() {
                checks
                    .add(index)
                    .write(WrittenValueChecks::prepare(entry.property(), value));
            }
            std::ptr::NonNull::new_unchecked(base.cast())
        }
    }

    /// The checks of each longhand's written value.
    pub(in crate::css::style) fn checks(&self) -> &[WrittenValueChecks] {
        // SAFETY: The allocation holds `length` checks after the header.
        unsafe {
            ak::slice_from_raw_parts(
                std::ptr::from_ref(self)
                    .cast::<u8>()
                    .add(size_of::<Self>())
                    .cast::<WrittenValueChecks>(),
                self.length as usize,
            )
        }
    }

    pub(in crate::css::style) fn heap_bytes(&self) -> usize {
        Self::layout(self.length as usize).size()
    }

    /// Frees the data `data` points at.
    ///
    /// # Safety
    /// `data` must come from `new` and be freed once.
    pub(crate) unsafe fn free(data: std::ptr::NonNull<Self>) {
        // SAFETY: The data is live until here, and holds nothing that needs dropping.
        unsafe {
            let layout = Self::layout(data.as_ref().length as usize);
            std::alloc::dealloc(data.as_ptr().cast(), layout);
        }
    }
}

/// What a rule declares: its declaration block, whose engine-independent cascade data the block
/// holds, and the custom properties among its declarations, named in the engine's atoms.
#[derive(Clone, Default)]
pub(in crate::css::style) struct RuleDeclarations {
    block: Option<SharedDeclarationBlock>,
    custom_declarations: Box<[CustomDeclaration]>,
}

impl RuleDeclarations {
    /// The declarations of `block`, whose custom properties the engine names `custom_names`, and
    /// whether this made the block's cascade data.
    pub(in crate::css::style) fn of_block(block: SharedDeclarationBlock, custom_names: &[StyleAtomID]) -> (Self, bool) {
        let made = block.make_cascade_data(BlockCascadeData::new);
        let custom_declarations = block
            .custom_properties()
            .iter()
            .zip(custom_names)
            .map(|(property, &name)| {
                CustomDeclaration::new(name, property.declaration.important, property.declaration.operator)
            })
            .collect();
        let declarations = Self {
            block: Some(block),
            custom_declarations,
        };
        (declarations, made)
    }

    /// The longhands the rule declares, in declaration order.
    pub(in crate::css::style) fn declared_properties(&self) -> &[DeclarationEntry] {
        self.block.as_ref().map_or(&[], |block| block.properties().entries())
    }

    /// The custom properties the rule declares, in declaration order.
    pub(in crate::css::style) fn custom_declarations(&self) -> &[CustomDeclaration] {
        &self.custom_declarations
    }

    fn written_values(&self) -> &[RetainedStyleValueData] {
        self.block.as_ref().map_or(&[], |block| {
            RetainedStyleValueData::slice_from_shared(block.properties().values())
        })
    }

    fn custom_written_values(&self) -> &[RetainedStyleValueData] {
        self.block.as_ref().map_or(&[], |block| {
            RetainedStyleValueData::slice_from_shared(block.custom_properties().values())
        })
    }
}

impl PartialEq for RuleDeclarations {
    fn eq(&self, other: &Self) -> bool {
        let same_block = match (&self.block, &other.block) {
            (Some(block), Some(other)) => std::ptr::eq(block.as_ptr(), other.as_ptr()),
            (None, None) => true,
            _ => false,
        };
        self.custom_declarations == other.custom_declarations
            && (same_block
                || (self.declared_properties() == other.declared_properties()
                    && self.written_values() == other.written_values()
                    && self.custom_written_values() == other.custom_written_values()))
    }
}

/// The document's stylesheet program: identities, versions, order, and attachment.
#[derive(Clone)]
pub struct RuleStore {
    /// What the host published of each rule beside its program: its declarations, the conditions of
    /// the container rules around it, and the identity of the rule object it came from.
    pub(in crate::css::style) native: native::NativeRuleRegistry,
    /// The custom properties each rule declaring any declares, named in the engine's atoms.
    custom_declarations: HashMap<RuleID, Box<[CustomDeclaration]>>,
    sheets: Vec<Sheet>,
    rules: RuleRecordTable,
    rule_children: Vec<Vec<RuleID>>,
    /// How many rules declare a custom property. A document without any resolves no environment
    /// of its own, and a cascade need not look.
    rules_declaring_custom_properties: usize,
    /// How many rules sit behind a container query. A document without any answers that no rule
    /// is gated without reading the rule's record.
    rules_gated_by_container_query: usize,
    rule_versions: RuleVersionTable,

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
    /// The `@scope` levels the scoped style rules are inside, by the scope their versions name,
    /// from one on.
    scope_chains: Vec<Arc<super::super::bridge::BoundScopeChain>>,
    /// The process's user agent rules the document's user agent sheets stand for, which the
    /// document reads rather than recording them.
    user_agent: Option<UserAgentBinding>,
}

impl RuleStore {
    /// The scope a rule version inside the `@scope` levels `chain` names.
    pub(in crate::css::style) fn scope_of_chain(
        &mut self,
        chain: &super::super::bridge::BoundScopeChain,
    ) -> StyleScopeID {
        if chain.levels.is_empty() {
            return StyleScopeID::NONE;
        }
        self.scope_chains.push(Arc::new(chain.clone()));
        StyleScopeID(u32::try_from(self.scope_chains.len()).expect("style scope space exhausted"))
    }

    /// The `@scope` levels `scope` names, or none for a rule inside no `@scope`.
    pub(in crate::css::style) fn scope_chain(
        &self,
        scope: StyleScopeID,
    ) -> Option<&Arc<super::super::bridge::BoundScopeChain>> {
        (scope != StyleScopeID::NONE).then(|| &self.scope_chains[scope.0 as usize - 1])
    }

    /// The bytes of what the cascade reads of the declarations of the store's rules: each block's
    /// cascade data once, with the values its declarations were written with counted aside, and
    /// the rules' custom declarations.
    pub(in crate::css::style) fn declarations_heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::HeapSize;
        let blocks = self
            .rules
            .iter()
            .filter_map(|rule| rule.declarations.as_ref())
            .map(|block| {
                let Some(data) = block.cascade_data() else {
                    return 0;
                };
                if !sizer.first_sight(std::ptr::from_ref(data)) {
                    return 0;
                }
                data.heap_bytes()
                    + RetainedStyleValueData::slice_from_shared(block.properties().values()).heap_size(sizer)
                    + RetainedStyleValueData::slice_from_shared(block.custom_properties().values()).heap_size(sizer)
            })
            .sum::<usize>();
        blocks
            + crate::heap_size::map_bytes(&self.custom_declarations)
            + self
                .custom_declarations
                .values()
                .map(|declarations| size_of_val(&**declarations))
                .sum::<usize>()
    }

    /// The bytes of the store's sheets and rules. The native rule registry and the rules'
    /// declarations are not counted.
    pub(in crate::css::style) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::{map_bytes, vec_bytes};
        let sheets = vec_bytes(&self.sheets)
            + self
                .sheets
                .iter()
                .map(|sheet| {
                    // NB: Documents whose sheets hold their rules alike share them, which count once.
                    let rules = match sizer.arc_shallow(&sheet.rules) {
                        0 => 0,
                        header => header + vec_bytes(&sheet.rules),
                    };
                    let rule_order = match sizer.arc_shallow(&sheet.rule_order) {
                        0 => 0,
                        header => header + sheet.rule_order.heap_size(),
                    };
                    rules + rule_order + vec_bytes(&sheet.attachments) + sheet.selectors.heap_size()
                })
                .sum::<usize>();
        let order = self.sheet_order.heap_size()
            + self
                .sheet_order
                .iter()
                .flatten()
                .map(OrderMaintenance::heap_size)
                .sum::<usize>();
        let sheets_by_scope =
            self.sheets_by_scope.heap_size() + self.sheets_by_scope.iter().map(vec_bytes).sum::<usize>();
        let layer_ranks =
            self.layer_ranks.heap_size() + self.layer_ranks.iter().flatten().map(map_bytes).sum::<usize>();
        sheets
            + self.rules.heap_size(sizer, |_, _| 0)
            + vec_bytes(&self.rule_children)
            + self.rule_children.iter().map(vec_bytes).sum::<usize>()
            + self.rule_versions.heap_size(sizer)
            + order
            + sheets_by_scope
            + layer_ranks
            + self.scopes_using_document_sheets.heap_size()
            + self.user_agent.as_ref().map_or(0, UserAgentBinding::heap_size)
    }

    pub(in crate::css::style) fn collect_atoms(&self, atoms: &mut HashSet<StyleAtomID>) -> u64 {
        let mut visited = 0_u64;
        for (id, rule) in self.rules.iter().enumerate() {
            if !rule.live {
                continue;
            }
            visited += 1;
            for declaration in self.custom_declarations_of(RuleID(id as u32)) {
                visited += 1;
                atoms.insert(declaration.name);
            }
            let version = &self.rule_versions[id];
            if let Some(name) = version.declared_name() {
                atoms.insert(name);
            }
            if version.layer != CascadeLayerID::UNLAYERED {
                atoms.insert(StyleAtomID(version.layer.0));
            }
        }
        // NB: What the host and the summaries read of a rule's selectors names the atoms of their
        //     arena, which a sweep has to keep the document's names of.
        for sheet in self.sheets.iter().filter(|sheet| sheet.live) {
            for (_, arena) in &sheet.selectors.arenas {
                let leased = arena.leased_atoms();
                visited += u64::try_from(leased.len()).expect("atom count exceeds u64");
                atoms.extend(leased.iter().copied());
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
            native: Default::default(),
            custom_declarations: HashMap::default(),
            sheets: Vec::new(),
            rules: RuleRecordTable::default(),
            rule_children: Vec::new(),
            rules_declaring_custom_properties: 0,
            rules_gated_by_container_query: 0,
            rule_versions: RuleVersionTable::default(),
            sheet_order: Column::default(),
            sheets_by_scope: Column::default(),
            layer_ranks: Column::default(),
            scopes_using_document_sheets: BitColumn::default(),
            scope_chains: Vec::new(),
            user_agent: None,
        }
    }

    // -- Sheets ------------------------------------------------------------------------------

    pub fn add_sheet(&mut self, origin: CascadeOrigin) -> SheetID {
        let id = SheetID(u32::try_from(self.sheets.len()).expect("sheet identity space exhausted"));
        self.sheets.push(Sheet {
            origin,
            rules: Arc::default(),
            rule_order: Arc::default(),
            rules_shared: false,
            attachments: Vec::new(),
            enabled: true,
            conditions_hold: true,
            live: true,
            selectors: SheetSelectors::default(),
            user_agent: None,
        });

        id
    }

    #[cfg(test)]
    fn sheet_order_mut(&mut self, tree_scope: TreeScopeID) -> &mut OrderMaintenance {
        let index = tree_scope.0 as usize;
        self.sheet_order.ensure(index);
        self.sheet_order[index].get_or_insert_with(OrderMaintenance::new)
    }

    /// Enabling or disabling a sheet updates its activation, not its attachment: identities, order
    /// tokens, and compiled programs all survive.
    pub fn set_sheet_enabled(&mut self, sheet: SheetID, enabled: bool) {
        if self.sheets[sheet.0 as usize].enabled == enabled {
            return;
        }
        self.sheets[sheet.0 as usize].enabled = enabled;
    }

    /// Record whether a sheet's own conditions hold. This is activation, not attachment: the sheet
    /// keeps its program, its identities, and its position.
    pub fn set_sheet_conditions_hold(&mut self, sheet: SheetID, conditions_hold: bool) {
        if self.sheets[sheet.0 as usize].conditions_hold == conditions_hold {
            return;
        }
        self.sheets[sheet.0 as usize].conditions_hold = conditions_hold;
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
    #[inline]
    #[must_use]
    pub fn rule_can_decide(&self, rule: RuleID) -> bool {
        if !self.rule_is_live(rule) || !self.rule_conditions_hold(rule) {
            return false;
        }
        let sheet = self.rule_sheet(rule);
        self.sheet_contributes(sheet) && !self.sheets[sheet.0 as usize].attachments.is_empty()
    }

    /// Record whether the conditions of the groups a rule sits inside hold. Like a sheet's own
    /// conditions this is activation, not existence: the rule keeps its identity and its place.
    /// Returns whether the answer moved.
    pub fn set_rule_conditions_hold(&mut self, rule: RuleID, conditions_hold: bool) -> bool {
        if rule.in_user_agent_rules().is_some() {
            return self
                .user_agent_binding_mut()
                .set_rule_conditions_hold(rule, conditions_hold);
        }
        if self.rules[rule.0 as usize].conditions_hold == conditions_hold {
            return false;
        }
        self.rules[rule.0 as usize].conditions_hold = conditions_hold;
        true
    }

    #[must_use]
    pub fn rule_conditions_hold(&self, rule: RuleID) -> bool {
        if rule.in_user_agent_rules().is_some() {
            return self.user_agent_binding().rule_conditions_hold(rule);
        }
        self.rules[rule.0 as usize].conditions_hold
    }

    pub fn set_rule_in_a_layer(&mut self, rule: RuleID, in_a_layer: bool) {
        if self.rules[rule.0 as usize].in_a_layer == in_a_layer {
            return;
        }
        self.rules[rule.0 as usize].in_a_layer = in_a_layer;
    }

    #[must_use]
    pub fn rule_is_in_a_layer(&self, rule: RuleID) -> bool {
        self.record(rule).in_a_layer
    }

    #[must_use]
    pub fn sheet_origin(&self, sheet: SheetID) -> CascadeOrigin {
        self.sheets[sheet.0 as usize].origin
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
    pub fn sheets_in_scope(&self, tree_scope: TreeScopeID) -> &[SheetID] {
        self.sheets_by_scope
            .get(tree_scope.0 as usize)
            .map_or(&[], Vec::as_slice)
    }

    /// The tree scopes with sheets attached, the document first.
    pub fn scopes_with_sheets(&self) -> impl Iterator<Item = TreeScopeID> + '_ {
        self.sheets_by_scope
            .iter()
            .enumerate()
            .filter(|(_, sheets)| !sheets.is_empty())
            .map(|(index, _)| TreeScopeID(u32::try_from(index).expect("tree scope identity space exhausted")))
    }

    pub(crate) fn set_sheets_in_scope(&mut self, tree_scope: TreeScopeID, sheets: Vec<SheetID>) {
        let index = tree_scope.0 as usize;
        self.sheet_order.ensure(index);
        self.sheets_by_scope.ensure(index);
        let previous_sheets = std::mem::replace(&mut self.sheets_by_scope[index], sheets);

        // The sheets that stay keep their positions where they keep their order, so that a sheet
        // arriving or leaving moves the rank of no other: only a reordering ranks them anew.
        let previous: HashSet<SheetID> = previous_sheets.iter().copied().collect();
        let current: HashSet<SheetID> = self.sheets_by_scope[index].iter().copied().collect();
        let kept_before = previous_sheets.iter().filter(|sheet| current.contains(sheet));
        let kept_after = self.sheets_by_scope[index]
            .iter()
            .filter(|sheet| previous.contains(sheet));
        if self.sheet_order[index].is_some() && kept_before.eq(kept_after) {
            let order = self.sheet_order[index].as_mut().expect("the scope has an order");
            for sheet in previous_sheets.iter().filter(|sheet| !current.contains(sheet)) {
                let attachments = &mut self.sheets[sheet.0 as usize].attachments;
                if let Some(position) = attachments
                    .iter()
                    .position(|attachment| attachment.tree_scope == tree_scope)
                {
                    order.remove(attachments.remove(position).order);
                }
            }
            let mut next: Option<OrderToken> = None;
            for position in (0..self.sheets_by_scope[index].len()).rev() {
                let sheet = self.sheets_by_scope[index][position];
                let attachments = &mut self.sheets[sheet.0 as usize].attachments;
                if previous.contains(&sheet) {
                    next = attachments
                        .iter()
                        .find(|attachment| attachment.tree_scope == tree_scope)
                        .map(|attachment| attachment.order);
                    continue;
                }
                let token = match next {
                    Some(before) => order.insert_before(before),
                    None => order.insert_last(),
                };
                attachments.push(Attachment {
                    tree_scope,
                    order: token,
                });
                next = Some(token);
            }
            return;
        }

        for sheet in previous_sheets {
            self.sheets[sheet.0 as usize]
                .attachments
                .retain(|attachment| attachment.tree_scope != tree_scope);
        }
        let mut order = OrderMaintenance::new();
        for position in 0..self.sheets_by_scope[index].len() {
            let sheet = self.sheets_by_scope[index][position];
            let attachment = Attachment {
                tree_scope,
                order: order.insert_last(),
            };
            let attachments = &mut self.sheets[sheet.0 as usize].attachments;
            attachments.push(attachment);
        }

        self.sheet_order[index] = Some(order);
    }

    // -- Rules -------------------------------------------------------------------------------

    pub(crate) fn reserve_rule(&mut self, sheet: SheetID, parent: Option<RuleID>, kind: RuleKind) -> RuleID {
        let order_axis = self.rule_order_mut(sheet);

        let order = order_axis.insert_last();

        self.create_rule(sheet, parent, kind, order, None, false)
    }

    pub(crate) fn reserve_rule_before(&mut self, before: RuleID, kind: RuleKind) -> RuleID {
        let sheet = self.rules[before.0 as usize].sheet;
        let parent = self.rules[before.0 as usize].parent;
        let sibling_order = self.rules[before.0 as usize].nested_order;
        let order_axis = self.rule_order_mut(sheet);

        let order = order_axis.insert_before(sibling_order);

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
        // NB: Every rule has one version, at its own number in the version table.
        self.rule_versions.push(RuleVersion::new(kind));
        debug_assert_eq!(self.rule_versions.len(), self.rules.len() + 1);

        self.rules.push(Rule {
            sheet,
            conditions_hold: true,
            in_a_layer: false,
            parent,
            children_slot: None,
            nested_order: order,
            live,
            gated_by_container_query: false,
            declarations: None,
        });

        let siblings = match parent {
            Some(parent) => {
                let slot = self.ensure_rule_children_slot(parent);
                &mut self.rule_children[slot]
            }
            None => self.top_level_rules_mut(sheet),
        };
        match before.and_then(|before| siblings.iter().position(|sibling| *sibling == before)) {
            Some(position) => siblings.insert(position, id),
            None => siblings.push(id),
        }

        id
    }

    #[must_use]
    pub fn rule_sheet(&self, rule: RuleID) -> SheetID {
        match rule.in_user_agent_rules() {
            Some(rule) => self.user_agent_binding().sheet_of(rule),
            None => self.rules[rule.0 as usize].sheet,
        }
    }

    /// The record of `rule`, which the process holds where it is a user agent rule.
    #[inline]
    fn record(&self, rule: RuleID) -> &Rule {
        match rule.in_user_agent_rules() {
            Some(rule) => &self.user_agent_binding().store().rules[rule.0 as usize],
            None => &self.rules[rule.0 as usize],
        }
    }

    #[must_use]
    pub(in crate::css::style) fn rule_children(&self, rule: RuleID) -> &[RuleID] {
        debug_assert!(
            rule.in_user_agent_rules().is_none(),
            "a user agent rule's children are the process's"
        );
        self.rules[rule.0 as usize]
            .children_slot
            .map_or(&[], |slot| self.rule_children[(slot.get() - 1) as usize].as_slice())
    }

    fn ensure_rule_children_slot(&mut self, rule: RuleID) -> usize {
        if let Some(slot) = self.rules[rule.0 as usize].children_slot {
            return (slot.get() - 1) as usize;
        }
        let index = self.rule_children.len();
        let slot = NonZeroU32::new(
            u32::try_from(index)
                .expect("rule children space exhausted")
                .checked_add(1)
                .expect("rule children space exhausted"),
        )
        .unwrap();

        self.rule_children.push(Vec::new());

        self.rules[rule.0 as usize].children_slot = Some(slot);
        index
    }

    #[must_use]
    pub fn rule_is_live(&self, rule: RuleID) -> bool {
        self.record(rule).live
    }

    #[inline]
    #[must_use]
    pub fn rule_version(&self, rule: RuleID) -> &RuleVersion {
        match rule.in_user_agent_rules() {
            Some(rule) => &self.user_agent_binding().store().rule_versions[rule.0 as usize],
            None => &self.rule_versions[rule.0 as usize],
        }
    }

    /// The selectors of the version `version` of a rule of `sheet`.
    #[must_use]
    pub(in crate::css::style) fn selectors_of(
        &self,
        sheet: SheetID,
        version: &RuleVersion,
    ) -> Option<crate::css::selector::SelectorListRef> {
        let held = &self.sheets[sheet.0 as usize];
        match held.user_agent {
            Some(sheet) => self.user_agent_binding().store().selectors_of(sheet, version),
            None => held.selectors.list(version.selectors),
        }
    }

    /// The selectors of `rule`.
    #[must_use]
    pub(in crate::css::style) fn rule_selectors(&self, rule: RuleID) -> Option<crate::css::selector::SelectorListRef> {
        self.selectors_of(self.rule_sheet(rule), self.rule_version(rule))
    }

    /// Where `list` starts among the selectors of `sheet`, which a version of one of its rules
    /// names it by.
    pub(in crate::css::style) fn start_of_selectors(
        &mut self,
        sheet: SheetID,
        list: &crate::css::selector::SelectorListRef,
    ) -> SheetListStart {
        self.sheets[sheet.0 as usize].selectors.start_of(list)
    }

    /// Whether a sheet took enough selector arenas since it last let go of some that letting go of
    /// those no rule names is worth it.
    #[must_use]
    pub(in crate::css::style) fn wants_letting_go_of_selectors(&self) -> bool {
        self.sheets.iter().any(|sheet| sheet.selectors.wants_letting_go())
    }

    /// Let go of the selector arenas of each sheet that no version of its rules names, where the
    /// sheet took many since it last did. Only the committed versions are asked, so this is for
    /// once no transaction stages any.
    pub(in crate::css::style) fn let_go_of_unnamed_selectors(&mut self) {
        let mut named: HashMap<SheetID, Vec<SheetListStart>> = HashMap::default();
        for (id, rule) in self.rules.iter().enumerate() {
            if !self.sheets[rule.sheet.0 as usize].selectors.wants_letting_go() {
                continue;
            }
            let start = self.rule_versions[id].selectors;
            if start != SheetListStart::NONE {
                named.entry(rule.sheet).or_default().push(start);
            }
        }
        for (index, sheet) in self.sheets.iter_mut().enumerate() {
            if sheet.selectors.wants_letting_go() {
                let id = SheetID(u32::try_from(index).expect("sheet identity space exhausted"));
                sheet
                    .selectors
                    .keep_named(named.get_mut(&id).map_or(&mut [], |named| &mut named[..]));
            }
        }
    }

    /// Replace one rule's semantic contents after the transaction carrying its previous value has
    /// been consumed. The rule identity, its position, and every identity inside the version that
    /// did not change are preserved, so a consumer can tell a declaration edit from a selector edit
    /// by comparing components.
    pub fn replace_rule_version(&mut self, rule: RuleID, contents: RuleVersion) {
        self.rule_versions.set(rule.0 as usize, contents);
    }

    pub(crate) fn replace_reserved_rule_version(&mut self, rule: RuleID, contents: RuleVersion) {
        self.rule_versions.set(rule.0 as usize, contents);
    }

    pub(crate) fn set_rule_liveness(&mut self, changes: &[(RuleID, bool)]) {
        for &(rule, _) in changes.iter().filter(|(_, live)| *live) {
            self.set_rule_live(rule, true);
        }

        let departing: HashSet<RuleID> = changes
            .iter()
            .filter_map(|&(rule, live)| (!live).then_some(rule))
            .collect();
        for &root in &departing {
            if self.rules[root.0 as usize]
                .parent
                .is_some_and(|parent| departing.contains(&parent))
            {
                continue;
            }
            self.detach_rule_subtree(root);
        }
    }

    fn set_rule_live(&mut self, rule: RuleID, live: bool) -> bool {
        if self.rules[rule.0 as usize].live == live {
            return false;
        }
        let entry = &mut self.rules[rule.0 as usize];
        if self.custom_declarations.contains_key(&rule) {
            if live {
                self.rules_declaring_custom_properties += 1;
            } else {
                self.rules_declaring_custom_properties -= 1;
            }
        }
        if entry.gated_by_container_query {
            if live {
                self.rules_gated_by_container_query += 1;
            } else {
                self.rules_gated_by_container_query -= 1;
            }
        }
        entry.live = live;
        true
    }

    fn detach_rule_subtree(&mut self, rule: RuleID) -> Vec<RuleID> {
        let sheet = self.rules[rule.0 as usize].sheet;
        let parent = self.rules[rule.0 as usize].parent;
        let siblings = match parent {
            Some(parent) => {
                let slot = self.rules[parent.0 as usize]
                    .children_slot
                    .expect("a parent has a child list");
                &mut self.rule_children[(slot.get() - 1) as usize]
            }
            None => self.top_level_rules_mut(sheet),
        };
        if let Some(position) = siblings.iter().position(|sibling| *sibling == rule) {
            siblings.remove(position);
        }

        let removed = self.rule_subtree(rule);
        for &id in &removed {
            self.set_rule_live(id, false);
            // A rule that left its sheet never decides again, so its selectors are let go with it
            // rather than kept alive, and their names, for nothing.
            let slot = id.0 as usize;
            if self.rule_versions[slot].selectors != SheetListStart::NONE {
                let mut version = self.rule_versions[slot].clone();
                version.selectors = SheetListStart::NONE;
                self.rule_versions.set(slot, version);
            }
            let order = self.rules[id.0 as usize].nested_order;
            let order_axis = self.rule_order_mut(sheet);

            order_axis.remove(order);
        }
        removed
    }

    fn collect_subtree(&self, rule: RuleID, out: &mut Vec<RuleID>) {
        out.push(rule);
        for &child in self.rule_children(rule) {
            self.collect_subtree(child, out);
        }
    }

    #[must_use]
    pub(crate) fn rule_subtree(&self, rule: RuleID) -> Vec<RuleID> {
        if let Some(rule) = rule.in_user_agent_rules() {
            let rules = self.user_agent_binding().store().rule_subtree(rule);
            return rules.into_iter().map(RuleID::of_user_agent).collect();
        }
        let mut rules = Vec::new();
        self.collect_subtree(rule, &mut rules);
        rules
    }

    #[must_use]
    pub(crate) fn all_top_level_rules_in_sheet(&self, sheet: SheetID) -> &[RuleID] {
        self.sheets[sheet.0 as usize].rules.as_slice()
    }

    #[must_use]
    pub(crate) fn all_rules_in_sheet(&self, sheet: SheetID) -> Vec<RuleID> {
        if let Some(sheet) = self.sheets[sheet.0 as usize].user_agent {
            let rules = self.user_agent_binding().store().all_rules_in_sheet(sheet);
            return rules.into_iter().map(RuleID::of_user_agent).collect();
        }
        let mut rules = Vec::new();
        for &top_level in self.sheets[sheet.0 as usize].rules.iter() {
            self.collect_subtree(top_level, &mut rules);
        }
        rules.sort_by_key(|rule| {
            self.sheets[sheet.0 as usize]
                .rule_order
                .rank(self.rules[rule.0 as usize].nested_order)
        });
        rules
    }

    /// Every live style rule of a sheet, in cascade order within that sheet.
    #[must_use]
    pub fn rules_in_sheet(&self, sheet: SheetID) -> Vec<RuleID> {
        if let Some(sheet) = self.sheets[sheet.0 as usize].user_agent {
            let rules = self.user_agent_binding().store().rules_in_sheet(sheet);
            return rules.into_iter().map(RuleID::of_user_agent).collect();
        }
        let top_level_rules = &self.sheets[sheet.0 as usize].rules;
        if top_level_rules.iter().all(|rule| self.rule_children(*rule).is_empty()) {
            return top_level_rules
                .iter()
                .copied()
                .filter(|rule| self.rules[rule.0 as usize].live)
                .collect();
        }

        let mut rules: Vec<RuleID> = Vec::new();
        for &top_level in top_level_rules.iter() {
            self.collect_live_subtree(top_level, &mut rules);
        }
        rules.sort_by_key(|rule| {
            self.sheets[sheet.0 as usize]
                .rule_order
                .rank(self.rules[rule.0 as usize].nested_order)
        });
        rules
    }

    fn collect_live_subtree(&self, rule: RuleID, out: &mut Vec<RuleID>) {
        if !self.rules[rule.0 as usize].live {
            return;
        }
        out.push(rule);
        for &child in self.rule_children(rule) {
            self.collect_live_subtree(child, out);
        }
    }

    // -- Layers ------------------------------------------------------------------------------

    pub(crate) fn layer_ranks_in_order(layers: &[CascadeLayerID]) -> HashMap<CascadeLayerID, u32> {
        let mut ranks = HashMap::with_capacity_and_hasher(layers.len(), Default::default());
        for &layer in layers {
            if layer == CascadeLayerID::UNLAYERED {
                continue;
            }
            let next_rank = ranks.len();
            ranks
                .entry(layer)
                .or_insert_with(|| u32::try_from(next_rank).expect("cascade layer count exhausted"));
        }
        ranks
    }

    #[must_use]
    pub fn layer_ranks_match(&self, scope: TreeScopeID, ranks: &HashMap<CascadeLayerID, u32>) -> bool {
        self.layer_ranks
            .get(scope.0 as usize)
            .and_then(Option::as_ref)
            .map_or(ranks.is_empty(), |current| current == ranks)
    }

    pub(crate) fn set_layer_ranks(&mut self, scope: TreeScopeID, ranks: HashMap<CascadeLayerID, u32>) -> bool {
        let index = scope.0 as usize;
        self.layer_ranks.ensure(index);
        let initialized = self.layer_ranks[index].is_some();
        if !initialized && ranks.is_empty() {
            return false;
        }
        if self.layer_ranks[index].as_ref() == Some(&ranks) {
            return false;
        }
        self.layer_ranks[index] = Some(ranks);

        true
    }

    #[must_use]
    pub fn layer_order_is_initialized(&self, scope: TreeScopeID) -> bool {
        self.layer_ranks.get(scope.0 as usize).is_some_and(Option::is_some)
    }

    /// The comparable rank of a layer. A topology change rewrites these ranks without touching any
    /// rule that references the layer.
    #[must_use]
    pub fn layer_rank(&self, scope: TreeScopeID, layer: CascadeLayerID) -> u64 {
        u64::from(self.layer_index(scope, layer))
    }

    /// The dense rank C++ cascade consumers use. The implicit outer layer and an as-yet undeclared
    /// name sort after every named layer in the scope.
    #[must_use]
    pub fn layer_index(&self, scope: TreeScopeID, layer: CascadeLayerID) -> u32 {
        let Some(ranks) = self.layer_ranks.get(scope.0 as usize).and_then(Option::as_ref) else {
            return 0;
        };
        if layer != CascadeLayerID::UNLAYERED
            && let Some(&rank) = ranks.get(&layer)
        {
            return rank;
        }
        u32::try_from(ranks.len()).expect("cascade layer count exhausted")
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
        if self.rules[rule.0 as usize].live {
            if gated {
                self.rules_gated_by_container_query += 1;
            } else {
                self.rules_gated_by_container_query -= 1;
            }
        }
    }

    #[must_use]
    pub fn rule_is_gated_by_container_query(&self, rule: RuleID) -> bool {
        self.rules_gated_by_container_query != 0 && self.record(rule).gated_by_container_query
    }

    /// Record the declarations a rule declares.
    pub(in crate::css::style) fn set_rule_declarations(&mut self, rule: RuleID, declarations: RuleDeclarations) {
        let RuleDeclarations {
            block,
            custom_declarations,
        } = declarations;
        let live = self.rules[rule.0 as usize].live;
        self.rules[rule.0 as usize].declarations = block;
        let declared_custom_properties_before = live && self.custom_declarations.contains_key(&rule);
        if custom_declarations.is_empty() {
            self.custom_declarations.remove(&rule);
        } else {
            self.custom_declarations.insert(rule, custom_declarations);
        }
        match (
            declared_custom_properties_before,
            live && self.custom_declarations.contains_key(&rule),
        ) {
            (false, true) => self.rules_declaring_custom_properties += 1,
            (true, false) => self.rules_declaring_custom_properties -= 1,
            _ => {}
        }
    }

    /// What a rule declares.
    #[must_use]
    pub(in crate::css::style) fn rule_declarations_of(&self, rule: RuleID) -> RuleDeclarations {
        RuleDeclarations {
            block: self.record(rule).declarations.clone(),
            custom_declarations: self.custom_declarations_of(rule).into(),
        }
    }

    fn block_of(&self, rule: RuleID) -> Option<&SharedDeclarationBlock> {
        self.record(rule).declarations.as_ref()
    }

    /// The longhands a rule declares, in declaration order.
    #[must_use]
    pub fn declared_properties_of(&self, rule: RuleID) -> &[DeclarationEntry] {
        self.block_of(rule).map_or(&[], |block| block.properties().entries())
    }

    /// The custom properties a rule declares, in declaration order.
    #[must_use]
    pub fn custom_declarations_of(&self, rule: RuleID) -> &[CustomDeclaration] {
        if let Some(rule) = rule.in_user_agent_rules() {
            return self.user_agent_binding().store().custom_declarations_of(rule);
        }
        self.custom_declarations
            .get(&rule)
            .map_or(&[], |declarations| declarations)
    }

    /// Whether any rule declares a custom property.
    #[must_use]
    pub fn any_rule_declares_custom_properties(&self) -> bool {
        self.rules_declaring_custom_properties != 0
            || self
                .user_agent
                .as_ref()
                .is_some_and(|binding| binding.store().rules_declaring_custom_properties != 0)
    }

    /// The values a rule's custom declarations were written with, parallel to them, or nothing
    /// when the rule arrived without them.
    #[must_use]
    pub fn custom_written_values_of(&self, rule: RuleID) -> &[RetainedStyleValueData] {
        self.block_of(rule).map_or(&[], |block| {
            RetainedStyleValueData::slice_from_shared(block.custom_properties().values())
        })
    }

    #[must_use]
    pub fn written_values_of(&self, rule: RuleID) -> &[RetainedStyleValueData] {
        self.block_of(rule).map_or(&[], |block| {
            RetainedStyleValueData::slice_from_shared(block.properties().values())
        })
    }

    fn cascade_data_of(&self, rule: RuleID) -> Option<&BlockCascadeData> {
        self.block_of(rule).map(|block| {
            block
                .cascade_data()
                .expect("a rule's published block holds its cascade data")
        })
    }

    /// Whether a match of this rule may move geometry.
    pub(in crate::css::style) fn rule_may_affect_layout_geometry(&self, rule: RuleID) -> bool {
        self.cascade_data_of(rule)
            .is_some_and(|data| data.may_affect_layout_geometry)
    }

    /// The checks of the values the rule's declarations were written with, parallel to them.
    pub(in crate::css::style) fn written_value_checks_of(
        &self,
        rule: RuleID,
    ) -> &[super::pipeline::compute::WrittenValueChecks] {
        self.cascade_data_of(rule).map_or(&[], |data| data.checks())
    }

    pub(in crate::css::style) fn written_value_checks(
        &self,
        rule: RuleID,
        index: usize,
    ) -> super::pipeline::compute::WrittenValueChecks {
        self.cascade_data_of(rule)
            .expect("a rule declaring a property holds its block")
            .checks()[index]
    }

    /// Whether the rule's declarations are all in the winner columns. A rule declaring custom
    /// properties is not: they never reach the columns.
    pub fn declarations_are_complete_for(&self, rule: RuleID) -> bool {
        self.custom_declarations_of(rule).is_empty()
    }

    /// Record that a tree scope's rules include the document's author sheets as well as its own,
    /// which is what a shadow root built from the document's styles does.
    pub fn set_scope_uses_document_sheets(&mut self, tree_scope: TreeScopeID) {
        self.scopes_using_document_sheets.set(tree_scope.0 as usize, true);
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
    pub fn sheet_rank(&self, attachment: Attachment) -> u64 {
        self.sheet_order
            .get(attachment.tree_scope.0 as usize)
            .and_then(Option::as_ref)
            .expect("an attachment implies its scope has an order")
            .rank(attachment.order)
    }

    /// How many times the rule order of a sheet was relabeled, which moves the ranks of its rules
    /// without moving their order.
    #[must_use]
    pub(in crate::css::style) fn rule_order_relabels(&self, sheet: SheetID) -> u64 {
        self.sheets[sheet.0 as usize].rule_order.relabels()
    }

    /// The comparable rank of a rule within its sheet.
    #[must_use]
    pub fn rule_rank(&self, rule: RuleID) -> u64 {
        if let Some(rule) = rule.in_user_agent_rules() {
            return self.user_agent_binding().store().rule_rank(rule);
        }
        let sheet = self.rules[rule.0 as usize].sheet;
        self.sheets[sheet.0 as usize]
            .rule_order
            .rank(self.rules[rule.0 as usize].nested_order)
    }

    // -- Accounting --------------------------------------------------------------------------

    pub(in crate::css::style) fn share_rule_storage(&mut self, process_indexes: Option<&super::ProcessRuleIndexes>) {
        if let Some(process_indexes) = process_indexes {
            self.rule_versions.share(&process_indexes.rule_version_pages);
            self.rules.share(&process_indexes.rule_record_pages);
            self.native.targets.share(&process_indexes.target_pages);
            for sheet in self.sheets.iter_mut().filter(|sheet| !sheet.rules_shared) {
                process_indexes.share_sheet_rules(&mut sheet.rules, &mut sheet.rule_order);
                sheet.rules_shared = true;
            }
        }
    }

    /// The top-level rules of `sheet`, to edit, which no other document's sheet holds then.
    fn top_level_rules_mut(&mut self, sheet: SheetID) -> &mut Vec<RuleID> {
        let sheet = &mut self.sheets[sheet.0 as usize];
        sheet.rules_shared = false;
        Arc::make_mut(&mut sheet.rules)
    }

    /// The rule order of `sheet`, to edit, which no other document's sheet holds then.
    fn rule_order_mut(&mut self, sheet: SheetID) -> &mut OrderMaintenance {
        let sheet = &mut self.sheets[sheet.0 as usize];
        sheet.rules_shared = false;
        Arc::make_mut(&mut sheet.rule_order)
    }
}

impl Default for RuleStore {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
impl RuleStore {
    /// Attach a sheet to a style scope at the end of that scope's sheet order.
    pub fn attach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) -> Attachment {
        let order_axis = self.sheet_order_mut(tree_scope);

        let order = order_axis.insert_last();

        let attachment = Attachment { tree_scope, order };
        let attachments = &mut self.sheets[sheet.0 as usize].attachments;
        attachments.push(attachment);

        let scope_index = tree_scope.0 as usize;
        self.sheets_by_scope.ensure(scope_index);
        self.sheets_by_scope[scope_index].push(sheet);
        attachment
    }

    /// Attach a sheet immediately before another sheet's attachment in the same scope. Loading
    /// order does not determine cascade order: a linked or imported sheet receives its position
    /// from document and rule order before or while it loads, and attachment reuses that position.
    pub fn attach_sheet_before(&mut self, sheet: SheetID, before: Attachment) -> Attachment {
        let order_axis = self.sheet_order_mut(before.tree_scope);

        let order = order_axis.insert_before(before.order);

        let attachment = Attachment {
            tree_scope: before.tree_scope,
            order,
        };
        let attachments = &mut self.sheets[sheet.0 as usize].attachments;
        attachments.push(attachment);

        let scope_index = before.tree_scope.0 as usize;
        self.sheets_by_scope.ensure(scope_index);
        let position = self.sheets_by_scope[scope_index]
            .iter()
            .position(|&candidate| {
                self.attachment_in_scope(candidate, before.tree_scope)
                    .is_some_and(|attachment| attachment.order == before.order)
            })
            .expect("an insertion target must remain attached");
        self.sheets_by_scope[scope_index].insert(position, sheet);
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
            order.remove(attachment.order);
        }
        if let Some(sheets) = self.sheets_by_scope.get_mut(tree_scope.0 as usize)
            && let Some(position) = sheets.iter().position(|&candidate| candidate == sheet)
        {
            sheets.remove(position);
        }
    }

    /// Move a sheet within its scope. Only the sheet's own order token is written; the program
    /// nodes of its rules are untouched, because they reference the sheet rather than copying its
    /// position.
    pub fn move_sheet_before(&mut self, sheet: SheetID, before: Attachment) {
        let tree_scope = before.tree_scope;
        self.detach_sheet(sheet, tree_scope);
        self.attach_sheet_before(sheet, before);
    }

    pub fn sheet_is_enabled(&self, sheet: SheetID) -> bool {
        self.sheets[sheet.0 as usize].enabled
    }

    /// Append a rule to a sheet or to a grouping rule.
    pub fn append_rule(&mut self, sheet: SheetID, parent: Option<RuleID>, kind: RuleKind) -> RuleID {
        let order_axis = self.rule_order_mut(sheet);

        let order = order_axis.insert_last();

        self.create_rule(sheet, parent, kind, order, None, true)
    }

    /// Insert a rule immediately before an existing sibling. Existing rules are neither renumbered
    /// nor rematched: the new rule takes an order token between its neighbors.
    pub fn insert_rule_before(&mut self, before: RuleID, kind: RuleKind) -> RuleID {
        let sheet = self.rules[before.0 as usize].sheet;
        let parent = self.rules[before.0 as usize].parent;
        let sibling_order = self.rules[before.0 as usize].nested_order;
        let order_axis = self.rule_order_mut(sheet);

        let order = order_axis.insert_before(sibling_order);

        self.create_rule(sheet, parent, kind, order, Some(before), true)
    }

    /// How many rules the store itself holds.
    pub(in crate::css::style) fn rule_count(&self) -> usize {
        self.rules.len()
    }

    /// Remove a rule and the subtree rooted at it. Deleting a group rule detaches its whole
    /// subtree; the identities are retired together in one operation rather than one rule at a
    /// time.
    pub fn remove_rule(&mut self, rule: RuleID) -> Vec<RuleID> {
        let removed = self.detach_rule_subtree(rule);
        removed
    }

    /// Put the named layers of `scope` in declaration order, ahead of its implicit outer layer.
    ///
    /// The parser publishes qualified name identities rather than ranks. This table is the one
    /// place that derives ranks, so shadow scopes can order the same spelling independently.
    pub fn set_layer_order(&mut self, scope: TreeScopeID, layers: &[CascadeLayerID]) -> bool {
        let ranks = Self::layer_ranks_in_order(layers);
        self.set_layer_ranks(scope, ranks)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::cascade::CascadeOperator;

    /// The selectors `text` writes.
    fn list(text: &str) -> crate::css::selector::SelectorListRef {
        let parsed = crate::css::selector_parser::parse_selector_list(
            &text.encode_utf16().collect::<Vec<u16>>()[..],
            &[],
            crate::css::selector_parser::SelectorType::Standalone,
            crate::css::selector_parser::SelectorParsingMode::Standard,
        )
        .expect("a test writes valid selectors");
        crate::css::selector::SelectorListRef::pack(&parsed.selectors)
    }

    #[test]
    fn rules_read_the_written_values_of_their_blocks() {
        let (mut first, first_sheet) = program_with_sheet();
        let (mut second, second_sheet) = program_with_sheet();
        let first_rule = first.append_rule(first_sheet, None, RuleKind::Style);
        let second_rule = second.append_rule(second_sheet, None, RuleKind::Style);
        let block = |keyword| {
            crate::css::style::pipeline::test_support::declaration_block(&[(
                crate::css::property_metadata::property_id::COLOR,
                false,
                crate::css::style_value::StyleValueData::Keyword { keyword },
            )])
        };
        let shared = block(1);
        first.set_rule_declarations(first_rule, RuleDeclarations::of_block(shared.clone(), &[]).0);
        second.set_rule_declarations(second_rule, RuleDeclarations::of_block(shared, &[]).0);
        assert!(first.rule_declarations_of(first_rule) == second.rule_declarations_of(second_rule));
        second.set_rule_declarations(second_rule, RuleDeclarations::of_block(block(2), &[]).0);
        assert!(first.rule_declarations_of(first_rule) != second.rule_declarations_of(second_rule));
        assert!(matches!(
            first.written_values_of(first_rule)[0].data(),
            crate::css::style_value::StyleValueData::Keyword { keyword: 1 }
        ));
        drop(first);
        assert!(matches!(
            second.written_values_of(second_rule)[0].data(),
            crate::css::style_value::StyleValueData::Keyword { keyword: 2 }
        ));
    }

    fn program_with_sheet() -> (RuleStore, SheetID) {
        let mut program = RuleStore::new();
        let sheet = program.add_sheet(CascadeOrigin::Author);
        program.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        (program, sheet)
    }

    #[test]
    fn custom_property_presence_tracks_live_rules() {
        let (mut program, sheet) = program_with_sheet();
        let group = program.append_rule(sheet, None, RuleKind::Media);
        let rule = program.append_rule(sheet, Some(group), RuleKind::Style);
        let declarations = || RuleDeclarations {
            block: None,
            custom_declarations: Box::new([CustomDeclaration::new(StyleAtomID(1), false, CascadeOperator::Initial)]),
        };
        program.set_rule_declarations(rule, declarations());
        assert!(program.any_rule_declares_custom_properties());
        program.remove_rule(group);
        assert!(!program.any_rule_declares_custom_properties());

        let reserved = program.reserve_rule(sheet, None, RuleKind::Style);
        program.set_rule_declarations(reserved, declarations());
        assert!(!program.any_rule_declares_custom_properties());
        program.set_rule_liveness(&[(reserved, true), (reserved, true)]);
        assert!(program.any_rule_declares_custom_properties());
        program.set_rule_liveness(&[(reserved, false)]);
        assert!(!program.any_rule_declares_custom_properties());
        program.set_rule_declarations(reserved, RuleDeclarations::default());
        assert!(!program.any_rule_declares_custom_properties());
    }

    #[test]
    fn a_declaration_edit_keeps_the_rule_and_its_selectors() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);

        let mut contents = program.rule_version(rule).clone();
        contents.selectors = program.start_of_selectors(sheet, &list(".c"));
        contents.declaration_block = Some(DeclarationBlockID::new(1));
        program.replace_rule_version(rule, contents.clone());
        let first = program.rule_version(rule).clone();

        contents.declaration_block = Some(DeclarationBlockID::new(2));
        program.replace_rule_version(rule, contents);
        let second = program.rule_version(rule).clone();

        assert_ne!(first, second, "a declaration edit creates a new rule version");
        assert_eq!(first.selectors, second.selectors, "and leaves the selectors alone");
        assert_eq!(*program.rule_version(rule), second);
    }

    #[test]
    fn a_selector_edit_changes_only_the_selectors() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);

        let mut contents = program.rule_version(rule).clone();
        contents.selectors = program.start_of_selectors(sheet, &list(".c"));
        contents.declaration_block = Some(DeclarationBlockID::new(5));
        program.replace_rule_version(rule, contents.clone());

        contents.selectors = program.start_of_selectors(sheet, &list(".c"));
        program.replace_rule_version(rule, contents);

        assert_eq!(
            program.rule_version(rule).declaration_block,
            Some(DeclarationBlockID::new(5))
        );
    }

    #[test]
    fn repeated_rule_version_replacement_keeps_storage_flat() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);

        let selector_lists = [list(".a"), list(".b")];
        for index in 0..128_u32 {
            let mut contents = program.rule_version(rule).clone();
            contents.selectors = program.start_of_selectors(sheet, &selector_lists[index as usize % 2]);
            contents.declaration_block = Some(DeclarationBlockID::new(index % 2 + 1));
            program.replace_rule_version(rule, contents);
        }
    }

    #[test]
    fn a_sheet_lets_go_of_the_selector_arenas_no_rule_names() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);
        let lists: Vec<_> = (0..16).map(|index| list(&format!(".c{index}"))).collect();
        let mut starts = Vec::new();
        for selectors in &lists {
            let mut contents = program.rule_version(rule).clone();
            contents.selectors = program.start_of_selectors(sheet, selectors);
            starts.push(contents.selectors);
            program.replace_rule_version(rule, contents);
        }
        starts.dedup();
        assert_eq!(
            starts.len(),
            lists.len(),
            "each arena the sheet takes starts its lists anew"
        );
        assert!(program.wants_letting_go_of_selectors());
        program.let_go_of_unnamed_selectors();
        assert!(!program.wants_letting_go_of_selectors());
        assert_eq!(program.sheets[sheet.0 as usize].selectors.arenas.len(), 1);
        let selectors = program.rule_selectors(rule).expect("the rule keeps its selectors");
        assert!(selectors.view().identity() == lists.last().unwrap().view().identity());
    }

    #[test]
    fn inserting_a_rule_does_not_renumber_its_neighbours() {
        let (mut program, sheet) = program_with_sheet();
        let first = program.append_rule(sheet, None, RuleKind::Style);
        let last = program.append_rule(sheet, None, RuleKind::Style);
        let first_version = program.rule_version(first).clone();
        let last_version = program.rule_version(last).clone();

        let middle = program.insert_rule_before(last, RuleKind::Style);

        assert_eq!(program.rules_in_sheet(sheet), vec![first, middle, last]);
        assert_eq!(*program.rule_version(first), first_version);
        assert_eq!(*program.rule_version(last), last_version);
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
        let mut program = RuleStore::new();
        let first = program.add_sheet(CascadeOrigin::Author);
        let second = program.add_sheet(CascadeOrigin::Author);
        program.attach_sheet(first, TreeScopeID::DOCUMENT);
        let second_attachment = program.attach_sheet(second, TreeScopeID::DOCUMENT);
        let rule = program.append_rule(first, None, RuleKind::Style);
        let version = program.rule_version(rule).clone();

        assert_eq!(program.sheets_in_scope(TreeScopeID::DOCUMENT), vec![first, second]);
        program.move_sheet_before(first, second_attachment);

        assert_eq!(program.sheets_in_scope(TreeScopeID::DOCUMENT), vec![first, second]);
        assert_eq!(
            *program.rule_version(rule),
            version,
            "rules reference the sheet's token"
        );
    }

    #[test]
    fn disabling_a_sheet_keeps_its_program_and_position() {
        let (mut program, sheet) = program_with_sheet();
        let rule = program.append_rule(sheet, None, RuleKind::Style);
        let version = program.rule_version(rule).clone();

        program.set_sheet_enabled(sheet, false);
        assert!(!program.sheet_is_enabled(sheet));
        assert_eq!(program.sheets_in_scope(TreeScopeID::DOCUMENT), vec![sheet]);
        assert_eq!(*program.rule_version(rule), version);
    }

    #[test]
    fn layer_topology_changes_ranks_without_touching_rules() {
        let mut program = RuleStore::new();
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
        let (mut program, sheet) = program_with_sheet();
        for _ in 0..100 {
            program.append_rule(sheet, None, RuleKind::Style);
        }
    }

    #[test]
    fn container_gate_count_tracks_rule_liveness() {
        let (mut program, sheet) = program_with_sheet();
        let parent = program.append_rule(sheet, None, RuleKind::Style);
        let child = program.append_rule(sheet, Some(parent), RuleKind::Style);
        program.set_rule_gated_by_container_query(parent, true);
        program.set_rule_gated_by_container_query(child, true);
        assert_eq!(program.rules_gated_by_container_query, 2);

        program.remove_rule(parent);
        assert_eq!(program.rules_gated_by_container_query, 0);
        assert!(!program.set_rule_live(parent, false));
        assert_eq!(program.rules_gated_by_container_query, 0);

        program.set_rule_gated_by_container_query(child, false);
        program.set_rule_gated_by_container_query(child, true);
        assert_eq!(program.rules_gated_by_container_query, 0);
        program.set_rule_liveness(&[(parent, true), (child, true)]);
        assert_eq!(program.rules_gated_by_container_query, 2);
        assert!(program.rule_is_gated_by_container_query(child));
        program.set_rule_liveness(&[(parent, true), (child, true)]);
        assert_eq!(program.rules_gated_by_container_query, 2);
        program.set_rule_gated_by_container_query(parent, false);
        program.set_rule_gated_by_container_query(child, false);
        assert_eq!(program.rules_gated_by_container_query, 0);
    }
}
