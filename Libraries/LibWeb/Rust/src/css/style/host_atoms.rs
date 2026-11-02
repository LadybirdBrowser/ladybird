/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The atoms a document's host interned for its engine, and what it worked out of the attribute names among them. The
//! host asks again for every attribute name and value it publishes, so a name it interned before is answered on the
//! host's own thread, without a lock or a write to the engine: one hash lookup on the name's raw identity.

use super::atoms::{AtomLease, ReclaimedStyleAtom};
use super::engine_calls::{EngineWrite, with_engine};
use super::index::{AttributeNameForms, StyleAtomID};
use crate::css::declaration_block::SharedDeclarationBlock;
use crate::css::ffi_support::{FfiUtf16View, ascii_lowercase};
use crate::css::parser::arbitrary_substitution::AttrNamesRead;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style_value::StyleValueData;
use crate::fast_hash::{FastMap, FastSet};
use crate::render_state::{ArenaChange, BegunRead, DocumentHost};

/// What the host interned for its document's engine.
#[derive(Default)]
pub(crate) struct HostAtoms {
    /// The atom of each name the host interned, by the name's raw identity, with a reference to the name: while the
    /// atom is here, no other string can take the identity.
    names: FastMap<usize, (StyleAtomID, RetainedUtf16FlyString)>,
    /// How many times the engine reclaimed atoms, which a cache the host keys by atoms is only good for one of.
    generation: u64,
    /// The languages whose tag the engine was given.
    languages_with_text: FastSet<StyleAtomID>,
    /// The name each attribute is published under, by its local name and its namespace, none for no namespace.
    attribute_names: FastMap<(StyleAtomID, StyleAtomID), StyleAtomID>,
    /// What the host published of each attribute name.
    published_attribute_names: FastMap<StyleAtomID, PublishedAttributeName>,
    /// The attribute names the host knows nothing reads the value text of, as of the engine's requirements numbered
    /// `value_text_requirements`.
    names_with_unread_value_text: FastSet<StyleAtomID>,
    value_text_requirements: u64,
    /// The values the host withheld the text of, by the attribute name they were published under, each by its atom with
    /// the raw identity of what it spells, which `names` holds a reference to while the atom lives: what the host hands
    /// the engine for exactly the names something comes to read, without asking the document again.
    withheld_value_texts: FastMap<StyleAtomID, FastMap<StyleAtomID, usize>>,
    /// The attribute names something reads the value text of. What selectors and `attr()`s read only grows, so this
    /// holds until the engine reclaims a name.
    names_with_read_value_text: FastSet<StyleAtomID>,
    /// The attribute names something reads. What selectors and `attr()`s read only grows, so this holds until the
    /// engine reclaims a name.
    names_read: FastSet<StyleAtomID>,
    /// The attribute names the host knows nothing reads, as of the selectors its last job left, which the host forgets
    /// when they move, and as of the `attr()` names read generation `names_unread_attr_generation`.
    names_known_unread: FastSet<StyleAtomID>,
    names_unread_attr_generation: u64,
    /// What the `attr()`s of the declarations the host handed the engine can read.
    attr_names_read: AttrNamesRead,
}

/// The other names an attribute name answers to, and the local name an `attr()` reads it by where it is in no namespace.
struct PublishedAttributeName {
    forms: AttributeNameForms,
    substitution_name: Option<Box<[u16]>>,
}

impl HostAtoms {
    /// The bytes the atoms' tables hold on the heap, but for the names, which the host's strings hold.
    pub(crate) fn heap_size(&self) -> usize {
        use crate::heap_size::{map_bytes, set_bytes};
        map_bytes(&self.names)
            + self.attr_names_read.heap_size()
            + set_bytes(&self.languages_with_text)
            + map_bytes(&self.attribute_names)
            + map_bytes(&self.published_attribute_names)
            + self
                .published_attribute_names
                .values()
                .filter_map(|name| name.substitution_name.as_deref())
                .map(size_of_val::<[u16]>)
                .sum::<usize>()
            + set_bytes(&self.names_with_unread_value_text)
            + map_bytes(&self.withheld_value_texts)
            + self.withheld_value_texts.values().map(map_bytes).sum::<usize>()
            + set_bytes(&self.names_with_read_value_text)
            + set_bytes(&self.names_read)
            + set_bytes(&self.names_known_unread)
    }

    /// Forgets the atoms the engine reclaimed, whose numbers may name other names from now on.
    pub(crate) fn forget(&mut self, reclaimed: &[ReclaimedStyleAtom]) {
        if reclaimed.is_empty() {
            return;
        }
        for reclaimed in reclaimed {
            // NB: The engine names the raw identity of every name it reclaimed, whether or not the host interned it.
            if let Some((atom, _)) = self.names.remove(&reclaimed.raw) {
                debug_assert_eq!(atom, reclaimed.atom);
            }
        }
        let reclaimed: FastSet<StyleAtomID> = reclaimed.iter().map(|reclaimed| reclaimed.atom).collect();
        self.languages_with_text
            .retain(|language| !reclaimed.contains(language));
        self.attribute_names
            .retain(|&(local, namespace), name| ![local, namespace, *name].iter().any(|atom| reclaimed.contains(atom)));
        self.published_attribute_names
            .retain(|name, _| !reclaimed.contains(name));
        self.names_with_unread_value_text
            .retain(|name| !reclaimed.contains(name));
        self.names_with_read_value_text.retain(|name| !reclaimed.contains(name));
        self.withheld_value_texts.retain(|name, values| {
            values.retain(|value, _| !reclaimed.contains(value));
            !reclaimed.contains(name) && !values.is_empty()
        });
        self.names_read.retain(|name| !reclaimed.contains(name));
        self.names_known_unread.retain(|name| !reclaimed.contains(name));
        self.generation += 1;
    }
}

/// The atom of the name whose raw identity is `raw`: the one the host interned it as, or else a reference the host
/// takes to the name's process-global atom, which the engine adopts as it applies the host's writes.
///
/// # Safety
/// `raw` must be the raw identity of a live `AK::Utf16FlyString`.
pub(crate) unsafe fn intern(host: &DocumentHost, raw: usize) -> StyleAtomID {
    let atoms = &host.engine_memo().atoms;
    if let Some(&(atom, _)) = atoms.borrow().names.get(&raw) {
        return atom;
    }
    // SAFETY: Guaranteed by the caller.
    let (lease, name) = unsafe {
        (
            AtomLease::acquire_raw(host.style_atoms(), raw),
            RetainedUtf16FlyString::from_borrowed_raw(raw),
        )
    };
    let atom = adopt(host, lease);
    atoms.borrow_mut().names.insert(raw, (atom, name));
    atom
}

/// The atom of `name` qualified by `namespace`, which the engine adopts as it does a name's.
fn intern_qualified(host: &DocumentHost, namespace: StyleAtomID, name: StyleAtomID) -> StyleAtomID {
    adopt(host, AtomLease::acquire_qualified(host.style_atoms(), namespace, name))
}

/// Queues the engine's adoption of the atom `lease` holds, and answers the atom.
fn adopt(host: &DocumentHost, lease: AtomLease) -> StyleAtomID {
    let atom = lease.atom();
    host.queue_change(ArenaChange::Engine(EngineWrite::AdoptAtom(lease)));
    atom
}

/// Interns the name whose raw identity is `raw` for the document.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, and `raw` the raw identity of a live
/// `AK::Utf16FlyString`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_intern_atom(host: &DocumentHost, raw: usize) -> u32 {
    // SAFETY: Guaranteed by the caller.
    unsafe { intern(host, raw) }.0
}

/// How many times the engine reclaimed atoms the host interned, which tells apart what the host keyed by the atoms
/// before and after.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_atom_generation(host: &DocumentHost) -> u64 {
    host.engine_memo().atoms.borrow().generation
}

/// Records the language the element `node` resolves to, or none for no element. The first time the host names a
/// language, the engine is also given its tag: a range is not a name, so `:lang()` compares against the tag itself.
#[unsafe(no_mangle)]
pub extern "C" fn style_engine_set_element_language(host: &DocumentHost, node: u32, language: u32, text: FfiUtf16View) {
    let tag_is_new = language != 0
        && !text.is_empty()
        && host
            .engine_memo()
            .atoms
            .borrow_mut()
            .languages_with_text
            .insert(StyleAtomID(language));
    let mut projection = host.engine_memo().projection.borrow_mut();
    let mut writes = projection.writes();
    if tag_is_new {
        // A range is not a name, so `:lang()` compares against the tag itself. It is recorded once per language
        // rather than once per element.
        writes.set_language_text(StyleAtomID(language), &text.to_utf16().unwrap_or_default());
    }
    if let Some(node) = super::tree::StyleNodeID::from_raw(node) {
        writes.set_element_language(node, StyleAtomID(language));
    }
}

/// Interns the name an attribute in `namespace` called `local_name` is published under, and has the engine know the
/// other names it answers to.
///
/// Three selectors ask three different questions of an attribute called `x`. `[ns|x]` reaches only the one in that
/// namespace, `[x]` reaches only the one in no namespace - which is what the bare local name is - and `[*|x]` reaches
/// whichever of them the element carries. The first two name exactly one of an element's attributes, so they are the
/// key: an element can hold `x` in several namespaces at once, and each is a fact with its own value. `[*|x]` asks
/// about all of them together, so the shared form is published as an identity of the name rather than as a fact of
/// its own, and one entry per attribute answers all three. Demand expansion revisits every live attribute, so the
/// forms are worked out once per name.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `local_name` the raw identity of a live
/// `AK::Utf16FlyString`, and `namespace` that of a live non-empty one, or zero for no namespace.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_intern_attribute_name(
    host: &DocumentHost,
    local_name: usize,
    namespace: usize,
) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let local = unsafe { intern(host, local_name) };
    // SAFETY: Guaranteed by the caller.
    let namespace = if namespace == 0 {
        StyleAtomID::NONE
    } else {
        unsafe { intern(host, namespace) }
    };
    if let Some(name) = host
        .engine_memo()
        .atoms
        .borrow()
        .attribute_names
        .get(&(local, namespace))
    {
        return name.0;
    }
    let in_namespace = |name| {
        if namespace.is_none() {
            name
        } else {
            intern_qualified(host, namespace, name)
        }
    };
    let name = in_namespace(local);
    // `[*|x]` names an attribute in any namespace, which no interned namespace is: none keys it.
    let mut forms = AttributeNameForms {
        local: intern_qualified(host, StyleAtomID::NONE, local),
        ..AttributeNameForms::default()
    };
    // SAFETY: Guaranteed by the caller.
    let local_name: Box<[u16]> = match unsafe { ak::utf16_string_units(&local_name) } {
        ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units) => {
            units.iter().map(|&unit| u16::from(unit)).collect()
        }
        ak::Utf16StringUnits::Utf16(units) => units.into(),
    };
    if local_name.iter().any(|&unit| ascii_lowercase(unit) != unit) {
        let folded =
            ak::Utf16FlyString::from_utf16(&local_name.iter().copied().map(ascii_lowercase).collect::<Vec<_>>());
        // SAFETY: The folded name is live for the call.
        let folded_local = unsafe { intern(host, folded.raw_identity()) };
        forms.folded_name = in_namespace(folded_local);
        forms.folded_local = intern_qualified(host, StyleAtomID::NONE, folded_local);
    }
    // An attr() reads an attribute in no namespace by its local name.
    let substitution_name = namespace.is_none().then_some(local_name);
    {
        let mut projection = host.engine_memo().projection.borrow_mut();
        projection.note_attribute_name_forms(name, forms);
        if let Some(local_name) = &substitution_name {
            projection.note_attribute_substitution_name(name, local_name);
        }
    }
    let mut atoms = host.engine_memo().atoms.borrow_mut();
    atoms.published_attribute_names.insert(
        name,
        PublishedAttributeName {
            forms,
            substitution_name,
        },
    );
    atoms.attribute_names.insert((local, namespace), name);
    name.0
}

/// Interns the value of an attribute published under `name` whose raw identity is `value`, and hands the engine what
/// it spells unless the host knows that nothing reads the value text of `name`. The engine keeps the text only where
/// something reads it.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, `name` an attribute name the host interned, and
/// `value` the raw identity of a live `AK::Utf16FlyString`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_intern_attribute_value(host: &DocumentHost, name: u32, value: usize) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let atom = unsafe { intern(host, value) };
    let name = StyleAtomID(name);
    if value_text_is_known_unread(host, name) {
        // NB: The atom's entry in `names` holds what it spells for as long as the atom lives.
        host.engine_memo()
            .atoms
            .borrow_mut()
            .withheld_value_texts
            .entry(name)
            .or_default()
            .insert(atom, value);
    } else {
        // SAFETY: Guaranteed by the caller.
        let text = unsafe { RetainedUtf16FlyString::from_borrowed_raw(value) };
        host.engine_memo()
            .projection
            .borrow_mut()
            .set_attribute_value_text(atom, text);
    }
    atom.0
}

/// Whether the host knows that nothing reads what the values of the attribute name `name` spell: no selector of the
/// engine, by `name` or the other forms the host published with it, and no `attr()`, by its local name if it has one.
/// The host knows what the selectors read where it queued no rule since its last job, and what `attr()`s read always,
/// as it hands the engine every declaration. Where it does not know, the host hands the engine each value's text, which the engine keeps only where
/// something reads it.
fn value_text_is_known_unread(host: &DocumentHost, name: StyleAtomID) -> bool {
    let atoms = host.engine_memo().atoms.borrow();
    if atoms.names_with_unread_value_text.contains(&name) {
        return true;
    }
    if atoms.names_with_read_value_text.contains(&name) {
        return false;
    }
    // The host interned every name it asks about, with its forms. Where it knows none, it hands the text over.
    let Some(PublishedAttributeName {
        forms,
        substitution_name,
    }) = atoms.published_attribute_names.get(&name)
    else {
        return false;
    };
    let read_by_attr = substitution_name
        .as_deref()
        .is_some_and(|name| atoms.attr_names_read.may_read(name));
    let read_by_selectors =
        host.known_selectors_read_value_text_of(&[name, forms.local, forms.folded_name, forms.folded_local]);
    drop(atoms);
    let mut atoms = host.engine_memo().atoms.borrow_mut();
    if read_by_attr || read_by_selectors == Some(true) {
        atoms.names_with_read_value_text.insert(name);
        return false;
    }
    let unread = read_by_selectors == Some(false);
    if unread {
        atoms.names_with_unread_value_text.insert(name);
    }
    unread
}

/// Brings what the host knows of the engine's selectors up to date with the writes it queued, where a rule it queued
/// since its last job left it not knowing: the host asks the engine, which applies them first.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_learn_selector_requirements(host: &DocumentHost, read: &BegunRead) {
    if host.known_selectors_test_local_link().is_none() {
        host.ask(read, |_| ());
    }
}

/// Whether the host knows that no selector of the engine tests `:local-link`, which spares it resolving the URL of each
/// link that arrives.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_local_link_is_known_untested(host: &DocumentHost) -> bool {
    host.known_selectors_test_local_link() == Some(false)
}

/// Whether the host knows that nothing of the engine reads the attribute name `name`: no selector tests it, by `name` or
/// the other forms the host published with it, and no `attr()` reads it, by its local name if it has one. Where it does
/// not know, the host hands the engine each change to the attribute.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_attribute_is_known_unread(host: &DocumentHost, name: u32) -> bool {
    let name = StyleAtomID(name);
    // The host knows nothing of the selectors where it queued a rule since its last job.
    if host
        .known_selector_attribute_value_text_requirements_version()
        .is_none()
    {
        return false;
    }
    let mut atoms = host.engine_memo().atoms.borrow_mut();
    let attr_generation = atoms.attr_names_read.generation();
    if atoms.names_read.contains(&name) {
        return false;
    }
    if atoms.names_unread_attr_generation != attr_generation {
        atoms.names_unread_attr_generation = attr_generation;
        atoms.names_known_unread.clear();
    } else if atoms.names_known_unread.contains(&name) {
        return true;
    }
    let Some(PublishedAttributeName {
        forms,
        substitution_name,
    }) = atoms.published_attribute_names.get(&name)
    else {
        return false;
    };
    let read = substitution_name
        .as_deref()
        .is_some_and(|name| atoms.attr_names_read.may_read(name))
        || host.known_selectors_test_attribute(&[name, forms.local, forms.folded_name, forms.folded_local])
            != Some(false);
    if read {
        atoms.names_read.insert(name);
    } else {
        atoms.names_known_unread.insert(name);
    }
    !read
}

/// Notes what the `attr()`s of the declarations `change` hands the engine can read. Every write of the host goes through
/// here on its way into the queue, and the matches name each write, so that a new write holding declarations has to say
/// so: an `attr()` nothing noted reads an attribute the host never handed over, as if it were absent.
pub(crate) fn note_attr_names_read_by_change(host: &DocumentHost, change: &ArenaChange) {
    use super::engine_calls::EngineWrite as Engine;
    use super::rule_writes::RuleWrite as Rule;
    match change {
        ArenaChange::Rule(write) => match write {
            Rule::PublishRules { rules, .. } => {
                for declarations in rules.iter().filter_map(|rule| rule.declarations.as_ref()) {
                    note_attr_names_read_by_block(host, declarations);
                }
            }
            Rule::RuleDeclarations {
                declarations: Some(declarations),
                ..
            } => note_attr_names_read_by_block(host, declarations),
            Rule::RuleDeclarations { declarations: None, .. }
            | Rule::AddSheet { .. }
            | Rule::AddUserAgentSheet { .. }
            | Rule::BeginSheetRulesReplacement(_)
            | Rule::ReplaceSelectors { .. }
            | Rule::RemoveRules(_)
            | Rule::RuleConditions(_)
            | Rule::LayerOrder { .. } => {}
        },
        ArenaChange::Engine(write) => match write {
            Engine::InlineStyle { data: Some(data), .. } => note_attr_names_read_by_block(host, data),
            Engine::PresentationalHints { properties, .. } => {
                note_attr_names_read_by(host, properties.iter().map(|property| &*property.value));
            }
            Engine::AnimationEffectDescriptions { effects, .. } => {
                for effect in effects {
                    note_attr_names_read_by(host, effect.declared_values());
                }
            }
            Engine::PositionTryRules { row, .. } => {
                for rule in row.values() {
                    note_attr_names_read_by_block(host, rule.declarations());
                }
            }
            Engine::InlineStyle { data: None, .. }
            | Engine::PublishFontFaces { .. }
            | Engine::ElementCustomPropertyData { .. }
            | Engine::PseudoElementCustomPropertyData { .. }
            | Engine::MintStyleNodes(_)
            | Engine::ElementUniqueNodeIds(_)
            | Engine::TextArrivals(_)
            | Engine::EndStyleUpdate { .. }
            | Engine::Publish(_)
            | Engine::MoveHover(_)
            | Engine::ShadowRoot { .. }
            | Engine::SlotAssignedNodes
            | Engine::TopLayerMove { .. }
            | Engine::BoxKindChange { .. }
            | Engine::RetireTexts(_)
            | Engine::TreeScopeRoot { .. }
            | Engine::TextData { .. }
            | Engine::AdoptAtom(_)
            | Engine::ParkRandomBaseValues { .. }
            | Engine::UnparkRandomBaseValues { .. }
            | Engine::NoteCustomPropertyEnvironment { .. }
            | Engine::AnimationKeyframes { .. }
            | Engine::AnimationEffectTimings { .. } => {}
        },
        _ => {}
    }
}

/// Moves with every name an `attr()` the host handed the engine can newly read.
pub(crate) fn attr_names_read_generation(host: &DocumentHost) -> u64 {
    host.engine_memo().atoms.borrow().attr_names_read.generation()
}

/// Notes what the `attr()`s of `values`, which the host is handing the engine, can read.
pub(crate) fn note_attr_names_read_by<'v>(host: &DocumentHost, values: impl Iterator<Item = &'v StyleValueData>) {
    for value in values {
        if let Some(unresolved) = super::custom_property_cascade::unresolved_reading_attributes(value) {
            host.engine_memo()
                .atoms
                .borrow_mut()
                .attr_names_read
                .note(unresolved.components().as_ref());
        }
    }
}

/// Notes what the `attr()`s of the declarations of `block`, which the host is handing the engine, can read.
pub(crate) fn note_attr_names_read_by_block(host: &DocumentHost, block: &SharedDeclarationBlock) {
    note_attr_names_read_by(
        host,
        block
            .properties()
            .values()
            .iter()
            .chain(block.custom_properties().values())
            .map(|value| &**value),
    );
}

/// Notes what the `attr()`s of `values`, tokens the host is handing the engine, can read.
pub(crate) fn note_attr_names_read_by_tokens(
    host: &DocumentHost,
    values: &[crate::css::parser::component_value::ComponentValue],
) {
    host.engine_memo().atoms.borrow_mut().attr_names_read.note(values);
}

/// Forgets which attribute names the host knew nothing reads, once the engine's selectors test other names.
pub(crate) fn forget_attribute_names_known_unread(host: &DocumentHost) {
    host.engine_memo().atoms.borrow_mut().names_known_unread.clear();
}

/// Brings what the host knows of the engine's requirements of attribute value text up to date, and hands the engine
/// the text of every value the host withheld under a name something reads now: only those values, rather than every attribute
/// of the document again.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_refresh_attribute_value_text_requirements(host: &DocumentHost, read: &BegunRead) {
    // The host knows where the requirements are where it queued no rule since its last job.
    let selector_requirements = match host.known_selector_attribute_value_text_requirements_version() {
        Some(version) => version,
        None => with_engine(read, host, |engine| {
            engine.selector_attribute_value_text_requirements_version()
        }),
    };
    let withheld_names: Vec<StyleAtomID> = {
        let mut atoms = host.engine_memo().atoms.borrow_mut();
        let requirements = selector_requirements.wrapping_add(atoms.attr_names_read.generation());
        if requirements == atoms.value_text_requirements {
            return;
        }
        atoms.value_text_requirements = requirements;
        atoms.names_with_unread_value_text.clear();
        atoms.withheld_value_texts.keys().copied().collect()
    };
    for name in withheld_names {
        if value_text_is_known_unread(host, name) {
            continue;
        }
        let texts: Vec<(StyleAtomID, RetainedUtf16FlyString)> = {
            let mut atoms = host.engine_memo().atoms.borrow_mut();
            let Some(values) = atoms.withheld_value_texts.remove(&name) else {
                continue;
            };
            values
                .into_iter()
                .filter_map(|(value, raw)| {
                    let (atom, text) = atoms.names.get(&raw)?;
                    debug_assert_eq!(*atom, value, "a withheld value's atom lives as long as its name entry");
                    Some((value, text.clone()))
                })
                .collect()
        };
        let mut projection = host.engine_memo().projection.borrow_mut();
        for (value, text) in texts {
            projection.set_attribute_value_text(value, text);
        }
    }
}
