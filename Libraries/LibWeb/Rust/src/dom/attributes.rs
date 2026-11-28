/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The attributes of an element: getting, changing, appending and removing them, on the attribute list of the element's
//! core, and handling their changes.
//! https://dom.spec.whatwg.org/#concept-element-attribute
//!
//! C++ allocates the list and the qualified names of its attributes, so appending an attribute calls into C++, which
//! creates the attribute and appends it. Changing an attribute's value, and taking an attribute out of the list, is
//! done in place.
//!
//! Handling an attribute change runs steps that C++ owns: Attr nodes, the editing history, mutation records, custom
//! element reactions and the attribute change steps. Rust calls into C++ for them only when they have something to do,
//! as the core flags tell: an element with no Attr nodes, in a document with no editing history and no one who hears
//! of its attribute changes, that is not custom, and that has no attribute change steps for the attribute's name, has
//! nothing to do in C++. What is left of the attribute change steps is done here: the collection and tree versions,
//! and, for an element the style engine does not see, the steps for its id and its classes, which are core state.

use core::ptr::NonNull;

use super::collection::collection_attributes_changed;
use super::element_core::ELEMENT_HAS_OWN_ATTRIBUTE_CHANGE_STEPS;
use super::element_core::ELEMENT_IS_CUSTOM;
use super::element_core::ELEMENT_IS_HTML_ELEMENT;
use super::element_core::ELEMENT_MAY_HAVE_ATTRIBUTE_NODES;
use super::element_core::ELEMENT_REFLECTION_HAS_CLASS_LIST;
use super::hooks;
use super::hooks::ATTRIBUTE_CHANGE_DETACH_ATTRIBUTE_NODE;
use super::hooks::ATTRIBUTE_CHANGE_ENQUEUE_CUSTOM_ELEMENT_REACTION;
use super::hooks::ATTRIBUTE_CHANGE_NEW_VALUE_IS_NULL;
use super::hooks::ATTRIBUTE_CHANGE_NOTIFY_EDITING_HISTORY;
use super::hooks::ATTRIBUTE_CHANGE_OLD_VALUE_IS_NULL;
use super::hooks::ATTRIBUTE_CHANGE_QUEUE_MUTATION_RECORD;
use super::hooks::ATTRIBUTE_CHANGE_RUN_ATTRIBUTE_CHANGE_STEPS;
use super::hooks::ATTRIBUTE_CHANGE_STEPS;
use super::hooks::ATTRIBUTE_CHANGE_TELL_STYLE_ENGINE;
use super::mutation_observers::attribute_mutation_record_is_heard;
use super::node::NodeCell;
use super::node_core::DOCUMENT_HAS_EDITING_HISTORY;
use super::node_core::DOCUMENT_IS_HTML_DOCUMENT;
use super::node_ref::NodeRef;
use super::qualified_name::QualifiedNameImpl;
use super::strings::BorrowedString;
use super::strings::ascii_lowercase_short_raw;
use super::strings::is_short_raw;
use super::strings::raw_strings_equal;
use super::strings::string_hash;
use super::strings::units_are_empty;
use super::strings::units_contain_ascii_uppercase;
use super::strings::units_equal_ascii;
use super::strings::units_equal_ascii_lowercased;
use super::strings::units_start_with_ascii;
use super::tree_version::bump_dom_tree_version;
use super::tree_version::dom_tree_version;
use crate::stage::MainThread;
use ak::Utf16StringUnits;

crate::stage::main_thread_ffi_entries!(without_host);

/// https://infra.spec.whatwg.org/#html-namespace
const HTML_NAMESPACE: &str = "http://www.w3.org/1999/xhtml";

/// A qualified name to look an attribute up by.
#[derive(Clone, Copy)]
enum LookupName {
    /// The name, whose raw word is this. A short name in ASCII lowercase is one too.
    Exact(usize),
    /// The name in ASCII lowercase, of a name that is not short.
    Lowercased(BorrowedString),
}

impl LookupName {
    /// `qualified_name`, in ASCII lowercase where `lowercase` says so.
    #[inline(always)]
    fn new(qualified_name: BorrowedString, lowercase: bool) -> Self {
        if !lowercase {
            return Self::Exact(qualified_name.raw());
        }
        if is_short_raw(qualified_name.raw()) {
            return Self::Exact(ascii_lowercase_short_raw(qualified_name.raw()));
        }
        if !units_contain_ascii_uppercase(qualified_name.units()) {
            return Self::Exact(qualified_name.raw());
        }
        Self::Lowercased(qualified_name)
    }

    #[inline(always)]
    fn matches(self, attribute_qualified_name: usize) -> bool {
        match self {
            // SAFETY: The name looked up lives for the lookup, and the attribute's name as long as its list, which the
            //         caller borrows.
            Self::Exact(raw) => unsafe { raw_strings_equal(attribute_qualified_name, raw) },
            // SAFETY: The attribute's name lives as long as its list, which the caller borrows.
            Self::Lowercased(name) => units_equal_ascii_lowercased(
                unsafe { ak::utf16_string_units(&attribute_qualified_name) },
                name.units(),
            ),
        }
    }

    #[inline(always)]
    fn is_ascii(self, ascii: &str) -> bool {
        match self {
            // SAFETY: The raw word is that of a live string, or a short string in place.
            Self::Exact(raw) => units_equal_ascii(unsafe { ak::utf16_string_units(&raw) }, ascii),
            Self::Lowercased(name) => {
                units_equal_ascii_lowercased(Utf16StringUnits::Ascii(ascii.as_bytes()), name.units())
            }
        }
    }

    /// The name as a string, which the attribute appended under it is created with.
    fn to_utf16_string(self) -> ak::Utf16String {
        match self {
            // SAFETY: The raw word is that of a live string, or a short string in place, which holds no reference.
            Self::Exact(raw) => unsafe { ak::Utf16String::from_raw_borrowed(raw) },
            Self::Lowercased(name) => name.to_utf16_string().to_ascii_lowercase(),
        }
    }
}

/// Whether the element is in the HTML namespace and its node document is an HTML document, where a qualified name to
/// look an attribute up by is first put in ASCII lowercase.
#[inline(always)]
fn lowercases_qualified_names(element: NodeRef) -> bool {
    if element.document_core().flags() & DOCUMENT_IS_HTML_DOCUMENT == 0 {
        return false;
    }
    let core = element.element_core().expect("only an element has attributes");
    core.flags() & ELEMENT_IS_HTML_ELEMENT != 0
        || core
            .qualified_name()
            .namespace()
            .is_some_and(|namespace| units_equal_ascii(namespace.units(), HTML_NAMESPACE))
}

/// Writes the declarations of the element's inline style to its style attribute, where script changed them since,
/// before the attribute named `is_style_attribute` says is read.
#[inline(always)]
fn synchronize_attribute(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    is_style_attribute: impl FnOnce() -> bool,
) {
    let core = element.element_core().expect("only an element has attributes");
    if core.style_attribute_is_dirty() && is_style_attribute() {
        hooks::synchronize_style_attribute(main_thread, element);
    }
}

/// The index of the first attribute in the element's attribute list whose qualified name is `name`.
#[inline(always)]
fn find_by_qualified_name(main_thread: &mut MainThread<'_>, element: NodeRef, name: LookupName) -> Option<usize> {
    synchronize_attribute(main_thread, element, || name.is_ascii("style"));
    let core = element.element_core().expect("only an element has attributes");
    core.attributes()
        .iter()
        .position(|attribute| name.matches(attribute.name().as_string().raw_identity()))
}

/// https://dom.spec.whatwg.org/#concept-element-attributes-get-by-name
#[inline(always)]
fn get_an_attribute_by_name(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    qualified_name: BorrowedString,
) -> Option<usize> {
    // 1. If element is in the HTML namespace and its node document is an HTML document, then set qualifiedName to
    //    qualifiedName in ASCII lowercase.
    let name = LookupName::new(qualified_name, lowercases_qualified_names(element));

    // 2. Return the first attribute in element's attribute list whose qualified name is qualifiedName; otherwise null.
    find_by_qualified_name(main_thread, element, name)
}

/// https://dom.spec.whatwg.org/#concept-element-attributes-get-by-namespace
/// The namespace and the local name are interned strings.
#[inline(always)]
fn get_an_attribute_by_namespace_and_local_name(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    namespace: Option<BorrowedString>,
    local_name: BorrowedString,
) -> Option<usize> {
    // 1. If namespace is the empty string, then set it to null.
    let namespace = namespace.filter(|namespace| !units_are_empty(namespace.units()));

    synchronize_attribute(main_thread, element, || {
        namespace.is_none() && units_equal_ascii(local_name.units(), "style")
    });

    // 2. Return the attribute in element's attribute list whose namespace is namespace and local name is localName, if
    //    any; otherwise null.
    let core = element.element_core().expect("only an element has attributes");
    core.attributes().iter().position(|attribute| {
        let name = attribute.name();
        // NB: The names are interned, so they are equal exactly when their raw identities are.
        name.local_name().raw_identity() == local_name.raw()
            && match (name.namespace(), namespace) {
                (None, None) => true,
                (Some(attribute_namespace), Some(namespace)) => attribute_namespace.raw_identity() == namespace.raw(),
                _ => false,
            }
    })
}

/// https://dom.spec.whatwg.org/#concept-element-attributes-change
fn change_an_attribute(main_thread: &mut MainThread<'_>, element: NodeRef, index: usize, value: ak::Utf16String) {
    let core = element.element_core().expect("only an element has attributes");
    let new_value = value.clone();

    // 1. Let oldValue be attribute's value.
    // 2. Set attribute's value to value.
    // SAFETY: Nothing borrows the attribute's value.
    let old_value = unsafe { core.attribute_list().replace_value(index, value) };

    // 3. Handle attribute changes for attribute with attribute's element, oldValue, and value.
    // NB: The name is held for the steps, which can take the attribute out of the list.
    let name = hold_qualified_name(core.attributes()[index].name().parts());
    handle_attribute_changes(
        main_thread,
        element,
        name,
        Some(old_value.raw_identity()),
        Some(new_value.raw_identity()),
        IsRemoval::No,
    );
    hooks::release_qualified_name(main_thread, name);
}

/// https://dom.spec.whatwg.org/#concept-element-attributes-append
/// https://dom.spec.whatwg.org/#concept-create-attribute
fn append_an_attribute(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    local_name: usize,
    prefix: usize,
    namespace_uri: usize,
    value: ak::Utf16String,
) {
    let new_value = value.clone();

    // 1. Append attribute to element's attribute list.
    // 2. Set attribute's element to element.
    // 3. Set attribute's node document to element's node document.
    hooks::append_attribute(main_thread, element, local_name, prefix, namespace_uri, value);

    // 4. Handle attribute changes for attribute with element, null, and attribute's value.
    let core = element.element_core().expect("only an element has attributes");
    let name = hold_qualified_name(
        core.attributes()
            .last()
            .expect("the attribute was appended")
            .name()
            .parts(),
    );
    handle_attribute_changes(
        main_thread,
        element,
        name,
        None,
        Some(new_value.raw_identity()),
        IsRemoval::No,
    );
    hooks::release_qualified_name(main_thread, name);
}

/// Step 2.5 of cloning a single node, for the element `node` and its copy `copy`, which has no attributes.
/// https://dom.spec.whatwg.org/#clone-a-single-node
pub(super) fn copy_attributes(main_thread: &mut MainThread<'_>, node: NodeRef, copy: NodeRef) {
    // NB: The style attribute is written out first where script changed its declarations, as reading it does.
    synchronize_attribute(main_thread, node, || true);
    let count = node
        .element_core()
        .expect("only an element has attributes")
        .attributes()
        .len();
    if count == 0 {
        return;
    }
    // NB: The copy takes all of the attributes, so its list is made with room for them at once.
    hooks::reserve_attributes(main_thread, copy, count);

    // 5. For each attribute of node's attribute list:
    // NB: Handling an attribute change can run script that changes node's attributes, so the list is read anew for
    //     each attribute.
    let mut index = 0;
    loop {
        let core = node.element_core().expect("only an element has attributes");
        let Some(attribute) = core.attributes().get(index) else {
            break;
        };
        index += 1;

        // 1. Let copyAttribute be the result of cloning a single node given attribute, document, and null.
        // NB: An attribute that is no Attr node has a namespace, namespace prefix, local name and value, which the copy
        //     shares.
        let name = hold_qualified_name(attribute.name().parts());
        let value = attribute.value().to_utf16_string();

        // 2. Append copyAttribute to copy.
        append_an_attribute_named(main_thread, copy, name, value);
        hooks::release_qualified_name(main_thread, name);
    }
}

/// Appends an attribute named `name`, which the caller holds a reference to, whose value is `value`, to `element`.
/// https://dom.spec.whatwg.org/#concept-element-attributes-append
fn append_an_attribute_named(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    name: NonNull<QualifiedNameImpl>,
    value: ak::Utf16String,
) {
    let new_value = value.raw_identity();
    let core = element.element_core().expect("only an element has attributes");
    // SAFETY: The caller holds a reference to the name's parts.
    let local_name = unsafe { name.as_ref() }.local_name().raw_identity();

    // 1. Append attribute to element's attribute list.
    if core.attribute_list().has_room_for_one_more() {
        add_to_subtree_name_filter(element, name_filter_bit(local_name));
        // SAFETY: The list has room, and nothing borrows its attributes. The list takes a reference to the name.
        unsafe {
            core.attribute_list()
                .append_unchecked(hold_qualified_name(name.as_ref()), value);
        }
    } else {
        // SAFETY: The caller holds a reference to the name's parts.
        let parts = unsafe { name.as_ref() };
        hooks::append_attribute(
            main_thread,
            element,
            local_name,
            parts.prefix().map_or(0, |prefix| prefix.raw_identity()),
            parts.namespace().map_or(0, |namespace| namespace.raw_identity()),
            value,
        );
    }

    // 2. Set attribute's element to element.
    // 3. Set attribute's node document to element's node document.
    // 4. Handle attribute changes for attribute with element, null, and attribute's value.
    // NB: The list holds a reference to the value, which stays alive while the changes are handled.
    handle_attribute_changes(main_thread, element, name, None, Some(new_value), IsRemoval::No);
}

/// https://dom.spec.whatwg.org/#concept-element-attributes-remove
fn remove_an_attribute(main_thread: &mut MainThread<'_>, element: NodeRef, index: usize) {
    // 1. Let element be attribute's element.
    let core = element.element_core().expect("only an element has attributes");

    // 2. Remove attribute from element's attribute list.
    // SAFETY: Nothing borrows an attribute of the list.
    let (name, old_value) = unsafe { core.attribute_list().take(index) };

    // 3. Set attribute's element to null.
    // 4. Handle attribute changes for attribute with element, attribute's value, and null.
    // NB: The attribute_changed hook sets the element of the attribute's Attr node to null, as the first of the steps
    //     it runs.
    handle_attribute_changes(
        main_thread,
        element,
        name,
        Some(old_value.raw_identity()),
        None,
        IsRemoval::Yes,
    );
    hooks::release_qualified_name(main_thread, name);
}

/// Takes a reference to the parts of a qualified name, which hooks::release_qualified_name() drops.
#[inline(always)]
fn hold_qualified_name(parts: &QualifiedNameImpl) -> NonNull<QualifiedNameImpl> {
    // SAFETY: The parts are live, and only the main thread counts the references to them.
    unsafe { parts.add_reference() };
    NonNull::from(parts)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum IsRemoval {
    No,
    Yes,
}

/// https://dom.spec.whatwg.org/#handle-attribute-changes
fn handle_attribute_changes(
    main_thread: &mut MainThread<'_>,
    element: NodeRef,
    name: NonNull<QualifiedNameImpl>,
    old_value: Option<usize>,
    new_value: Option<usize>,
    is_removal: IsRemoval,
) {
    let value_changed = match (old_value, new_value) {
        // SAFETY: The caller holds a reference to the values.
        (Some(old_value), Some(new_value)) => !unsafe { raw_strings_equal(old_value, new_value) },
        (None, None) => false,
        _ => true,
    };
    let element_flags = element.element_core().expect("only an element has attributes").flags();

    let mut steps = 0;
    if old_value.is_none() {
        steps |= ATTRIBUTE_CHANGE_OLD_VALUE_IS_NULL;
    }
    if new_value.is_none() {
        steps |= ATTRIBUTE_CHANGE_NEW_VALUE_IS_NULL;
    }
    if is_removal == IsRemoval::Yes && element_flags & ELEMENT_MAY_HAVE_ATTRIBUTE_NODES != 0 {
        steps |= ATTRIBUTE_CHANGE_DETACH_ATTRIBUTE_NODE;
    }
    // NB: Mutations during a recorded editing command must go through the Editing proxy functions.
    if element.document_core().flags() & DOCUMENT_HAS_EDITING_HISTORY != 0 {
        steps |= ATTRIBUTE_CHANGE_NOTIFY_EDITING_HISTORY;
    }

    // 1. Queue a mutation record of "attributes" for element with attribute's local name, attribute's namespace,
    //    oldValue, « », « », null, and null.
    // OPTIMIZATION: A record no one hears is not queued, see mutation_observers.rs.
    if attribute_mutation_record_is_heard(element) {
        steps |= ATTRIBUTE_CHANGE_QUEUE_MUTATION_RECORD;
    }

    // 2. If element is custom, then enqueue a custom element callback reaction with element, callback name
    //    "attributeChangedCallback", and « attribute's local name, oldValue, newValue, attribute's namespace ».
    if element_flags & ELEMENT_IS_CUSTOM != 0 {
        steps |= ATTRIBUTE_CHANGE_ENQUEUE_CUSTOM_ELEMENT_REACTION;
    }

    // 3. Run the attribute change steps with element, attribute's local name, oldValue, newValue, and attribute's
    //    namespace.
    // SAFETY: The caller holds a reference to the name's parts.
    let parts = unsafe { name.as_ref() };
    // NB: The style engine hears of a change to an attribute of an element that is connected or that it styles, which
    //     the attribute change steps tell it of where they run.
    let style_engine_hears = element.is_connected() || element.core().style_node_id().is_some();
    if !style_engine_hears
        && element_flags & ELEMENT_HAS_OWN_ATTRIBUTE_CHANGE_STEPS == 0
        && parts.namespace().is_none()
        && run_core_attribute_change_steps(element, parts.local_name().units(), new_value, value_changed)
    {
        // NB: These were all the attribute change steps for the change.
    } else if has_attribute_change_steps(element_flags, parts, value_changed) {
        steps |= ATTRIBUTE_CHANGE_RUN_ATTRIBUTE_CHANGE_STEPS;
    } else if value_changed && style_engine_hears {
        steps |= ATTRIBUTE_CHANGE_TELL_STYLE_ENGINE;
    }
    if steps & ATTRIBUTE_CHANGE_STEPS != 0 {
        hooks::attribute_changed(
            main_thread,
            element,
            name,
            old_value.unwrap_or(0),
            new_value.unwrap_or(0),
            steps,
        );
    }

    // NB: The attribute change steps of every element that C++ does not run: the collections whose filters read the
    //     attribute, and the DOM tree version, see below.
    if value_changed {
        if parts.namespace().is_none() {
            let attribute_types = collection_attribute_types(parts.local_name().units());
            if attribute_types != 0 {
                collection_attributes_changed(main_thread, element, attribute_types);
            }
        }
        bump_dom_tree_version_for_attribute_change(main_thread, element);
    }
}

/// Runs the attribute change steps of every element for a change to its attribute without a namespace whose local name
/// is `local_name` to the value `new_value`, which changes the value where `value_changed` says so, where they change
/// only the element's core, and answers whether they do. They do for the element's id, and for its classes where no
/// DOMTokenList reflects them.
#[inline(always)]
fn run_core_attribute_change_steps(
    element: NodeRef,
    local_name: Utf16StringUnits<'_>,
    new_value: Option<usize>,
    value_changed: bool,
) -> bool {
    let Utf16StringUnits::Ascii(ascii) = local_name else {
        return false;
    };
    let core = element.element_core().expect("only an element has attributes");
    match ascii {
        b"id" => {
            if value_changed {
                set_id(element, new_value);
            }
            true
        }
        b"class" if core.reflection_flags() & ELEMENT_REFLECTION_HAS_CLASS_LIST == 0 => {
            if value_changed {
                set_classes(element, new_value);
            }
            true
        }
        _ => false,
    }
}

/// The interned string with the code units of the string `string`.
#[inline(always)]
fn fly_string_of(string: BorrowedString) -> ak::Utf16FlyString {
    if is_short_raw(string.raw()) {
        // SAFETY: A short string is its own interned string, and owns no storage.
        return unsafe { ak::Utf16FlyString::from_raw_owned(string.raw()) };
    }
    ak::Utf16FlyString::from_utf16_string(&string.to_utf16_string())
}

/// Keeps the element's ID for the value `value` of its id attribute, as the attribute change steps of every element do
/// for an element the style engine does not see.
/// https://dom.spec.whatwg.org/#concept-id
fn set_id(element: NodeRef, value: Option<usize>) {
    // SAFETY: The caller borrows the value for the call.
    let value = value.map(|value| unsafe { BorrowedString::from_raw(value) });
    // An element's unique identifier (ID) is its id attribute's value, if it has one and its value is not the empty
    // string; otherwise the element has no ID.
    let id = value.filter(|value| !units_are_empty(value.units())).map(fly_string_of);
    let core = element.element_core().expect("only an element has an ID");
    // SAFETY: Nothing borrows the element's ID across the change.
    drop(unsafe { core.id_field().replace(id) });
}

/// Sets the element's classes to the tokens of the value `value`, of its class attribute, and adds them to the subtree
/// name filters.
/// https://dom.spec.whatwg.org/#concept-class
fn set_classes(element: NodeRef, value: Option<usize>) {
    let classes = element
        .element_core()
        .expect("only an element has classes")
        .class_names();
    // SAFETY: Nothing borrows the element's classes across the change.
    unsafe { classes.clear() };
    let Some(value) = value else {
        return;
    };
    // SAFETY: The caller borrows the value for the call.
    let value = unsafe { BorrowedString::from_raw(value) };
    let units = value.units();
    let length = match units {
        Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => units.len(),
        Utf16StringUnits::Utf16(units) => units.len(),
    };
    let unit_at = |index: usize| match units {
        Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => u16::from(units[index]),
        Utf16StringUnits::Utf16(units) => units[index],
    };
    // https://infra.spec.whatwg.org/#ascii-whitespace
    let is_ascii_whitespace = |unit: u16| matches!(unit, 0x09 | 0x0a | 0x0c | 0x0d | 0x20);

    let mut filter_bits = 0u64;
    let mut index = 0;
    while index < length {
        if is_ascii_whitespace(unit_at(index)) {
            index += 1;
            continue;
        }
        let start = index;
        while index < length && !is_ascii_whitespace(unit_at(index)) {
            index += 1;
        }
        let class = match units {
            _ if start == 0 && index == length => fly_string_of(value),
            Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) if units[start..index].is_ascii() => {
                // SAFETY: ASCII is UTF-8.
                ak::Utf16FlyString::from_utf8(unsafe { core::str::from_utf8_unchecked(&units[start..index]) })
            }
            Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => {
                let code_units: Vec<u16> = units[start..index].iter().map(|&unit| u16::from(unit)).collect();
                ak::Utf16FlyString::from_utf16(&code_units)
            }
            Utf16StringUnits::Utf16(units) => ak::Utf16FlyString::from_utf16(&units[start..index]),
        };
        // SAFETY: The class is a live string.
        filter_bits |= unsafe { name_filter_bit(class.raw_identity()) };
        // SAFETY: Nothing borrows the element's classes across the change.
        unsafe { classes.append(class) };
    }
    add_to_subtree_name_filter(element, filter_bits);
}

/// The bit of the subtree name filters for the interned string whose raw word is `name`, as DOM::Element's
/// name_filter_bit() works it out.
#[inline(always)]
///
/// # Safety
///
/// `name` must be the raw word of a live string, which lives for the call.
unsafe fn name_filter_bit(name: usize) -> u64 {
    // SAFETY: Guaranteed by the caller.
    1 << (unsafe { string_hash(name) } % 64)
}

/// Adds `bits` to the subtree name filter of the element and of its inclusive ancestor elements, as DOM::Element's
/// add_to_subtree_name_filter() does.
fn add_to_subtree_name_filter(element: NodeRef, bits: u64) {
    // NB: An element's filter holds those of its children, so the walk up ends at the first that has the bits.
    let mut current = Some(element);
    while let Some(node) = current {
        let Some(core) = node.element_core() else {
            break;
        };
        let filter = core.subtree_name_filter_cell();
        if filter.get() & bits == bits {
            break;
        }
        filter.set(filter.get() | bits);
        current = node.parent();
    }
}

/// The attribute types of the collections whose filters read an attribute without a namespace whose local name is
/// `local_name`, as a mask of the bits of the HTMLCollection::AttributeInvalidationType values but None, less one.
#[inline(always)]
fn collection_attribute_types(local_name: Utf16StringUnits<'_>) -> u8 {
    let Utf16StringUnits::Ascii(ascii) = local_name else {
        return 0;
    };
    match ascii {
        b"class" => 1 << 0,
        b"name" => (1 << 1) | (1 << 2),
        b"id" => 1 << 2,
        b"href" => 1 << 3,
        b"type" => 1 << 4,
        _ => 0,
    }
}

/// Bumps the DOM tree version for a change to an attribute of the element, which moves no element, so the tree
/// positions of the elements of the document taken at the version before stand.
#[inline(always)]
fn bump_dom_tree_version_for_attribute_change(main_thread: &mut MainThread<'_>, element: NodeRef) {
    let version_before = dom_tree_version(element);
    bump_dom_tree_version(main_thread, element);
    let positions_version = element.document_core().element_tree_positions_dom_tree_version();
    if positions_version.get() == version_before {
        positions_version.set(dom_tree_version(element.document()));
    }
}

/// Whether the attribute change steps for a change to the attribute named `name` of `element`, which changes its
/// value where `value_changed` says so, do anything, which they do in C++: those of the element's class, and those
/// that every element and every HTML element has for the names they act on.
#[inline(always)]
fn has_attribute_change_steps(element_flags: u8, name: &QualifiedNameImpl, value_changed: bool) -> bool {
    if element_flags & ELEMENT_HAS_OWN_ATTRIBUTE_CHANGE_STEPS != 0 {
        return true;
    }
    // NB: Every namespaced attribute's change runs in C++, which xml:lang's needs.
    if name.namespace().is_some() {
        return true;
    }
    has_base_attribute_change_steps(
        name.local_name().units(),
        element_flags & ELEMENT_IS_HTML_ELEMENT != 0,
        value_changed,
    )
}

/// Whether the attribute change steps of every element, or of every HTML element where `is_html_element` says so, do
/// something for a change to the attribute without a namespace whose local name is `local_name`, which changes its
/// value where `value_changed` says so.
fn has_base_attribute_change_steps(
    local_name: Utf16StringUnits<'_>,
    is_html_element: bool,
    value_changed: bool,
) -> bool {
    // NB: No attribute change steps act on a name that is not ASCII.
    let Utf16StringUnits::Ascii(ascii) = local_name else {
        return false;
    };
    match ascii {
        // NB: The steps for an element's id and its slot do nothing where the value stays the same.
        b"id" | b"slot" => value_changed,
        // The names DOM::Element::attribute_changed() acts on.
        b"blocking" | b"name" | b"class" | b"style" | b"dir" | b"lang" | b"part" | b"exportparts" => true,
        // The ARIA attributes that reflect elements, whose explicitly set attr-elements their changes reset.
        b"aria-activedescendant"
        | b"aria-controls"
        | b"aria-describedby"
        | b"aria-details"
        | b"aria-errormessage"
        | b"aria-flowto"
        | b"aria-labelledby"
        | b"aria-owns" => true,
        // The names DOM::Element::run_attribute_change_steps() acts on: the spell checking state, and table spans.
        b"spellcheck" | b"readonly" | b"disabled" | b"type" | b"contenteditable" | b"colspan" | b"rowspan"
        | b"span" => value_changed,
        // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#nonce-attributes
        b"nonce" => true,
        // The names HTML::HTMLElement::attribute_changed() acts on, and its event handler content attributes.
        b"inert" | b"popover" => is_html_element,
        _ => is_html_element && units_start_with_ascii(local_name, "on"),
    }
}

/// The index of the attribute the element has whose qualified name is `qualified_name`, or -1 for none, for the C++ of
/// "get an attribute by name".
///
/// # Safety
///
/// The element must be live, `qualified_name` the raw word of a live string, and the call must be made on the main
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_get_an_attribute_by_name(element: &NodeCell, qualified_name: usize) -> isize {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    // SAFETY: The string is live for the call.
    let qualified_name = unsafe { BorrowedString::from_raw(qualified_name) };
    get_an_attribute_by_name(&mut main_thread, element, qualified_name).map_or(-1, |index| index as isize)
}

/// The index of the attribute the element has whose namespace and local name are `namespace_uri` (0 for null) and
/// `local_name`, or -1 for none, for the C++ of "get an attribute by namespace and local name".
///
/// # Safety
///
/// The element must be live, `namespace_uri` the raw word of a live interned string or 0, `local_name` that of a live
/// interned string, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_get_an_attribute_by_namespace_and_local_name(
    element: &NodeCell,
    namespace_uri: usize,
    local_name: usize,
) -> isize {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    // SAFETY: The strings are live for the call.
    let (namespace_uri, local_name) = unsafe {
        (
            (namespace_uri != 0).then(|| BorrowedString::from_raw(namespace_uri)),
            BorrowedString::from_raw(local_name),
        )
    };
    get_an_attribute_by_namespace_and_local_name(&mut main_thread, element, namespace_uri, local_name)
        .map_or(-1, |index| index as isize)
}

/// Steps 2 and 4 to 7 of setAttribute(qualifiedName, value), with the verified value, whose reference passes to the
/// call. C++ runs steps 1 and 3 before.
/// https://dom.spec.whatwg.org/#dom-element-setattribute
///
/// # Safety
///
/// The element must be live, `qualified_name` the raw word of a live string, `value` a reference to a string, and the
/// call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_set_attribute(element: &NodeCell, qualified_name: usize, value: usize) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    // SAFETY: The string is live for the call, and the caller passes its reference to the value.
    let (qualified_name, verified_value) = unsafe {
        (
            BorrowedString::from_raw(qualified_name),
            ak::Utf16String::from_raw_owned(value),
        )
    };

    // 2. If this is in the HTML namespace and its node document is an HTML document, then set qualifiedName to
    //    qualifiedName in ASCII lowercase.
    let qualified_name = LookupName::new(qualified_name, lowercases_qualified_names(element));

    // 4. Let attribute be the first attribute in this's attribute list whose qualified name is qualifiedName, and
    //    null otherwise.
    let attribute = find_by_qualified_name(&mut main_thread, element, qualified_name);

    // 5. If attribute is non-null, then change attribute to verifiedValue and return.
    if let Some(index) = attribute {
        change_an_attribute(&mut main_thread, element, index, verified_value);
        return;
    }

    // 6. Set attribute to the result of creating an attribute given this's node document, qualifiedName, null, null,
    //    and verifiedValue.
    // 7. Append attribute to this.
    let local_name = qualified_name.to_utf16_string();
    append_an_attribute(
        &mut main_thread,
        element,
        local_name.raw_identity(),
        0,
        0,
        verified_value,
    );
}

/// "Set an attribute value", with the value, whose reference passes to the call.
/// https://dom.spec.whatwg.org/#concept-element-attributes-set-value
///
/// # Safety
///
/// The element must be live, `local_name` the raw word of a live interned string, `prefix` and `namespace_uri` those of
/// live interned strings or 0, `value` a reference to a string, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_set_an_attribute_value(
    element: &NodeCell,
    local_name: usize,
    value: usize,
    prefix: usize,
    namespace_uri: usize,
) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    // SAFETY: The strings are live for the call, and the caller passes its reference to the value.
    let (namespace_string, local_name_string, value) = unsafe {
        (
            (namespace_uri != 0).then(|| BorrowedString::from_raw(namespace_uri)),
            BorrowedString::from_raw(local_name),
            ak::Utf16String::from_raw_owned(value),
        )
    };

    // 1. Let attribute be the result of getting an attribute given namespace, localName, and element.
    let attribute =
        get_an_attribute_by_namespace_and_local_name(&mut main_thread, element, namespace_string, local_name_string);

    // 2. If attribute is null, create an attribute whose namespace is namespace, namespace prefix is prefix, local
    //    name is localName, value is value, and node document is element's node document, then append this attribute
    //    to element, and then return.
    let Some(index) = attribute else {
        append_an_attribute(&mut main_thread, element, local_name, prefix, namespace_uri, value);
        return;
    };

    // 3. Change attribute to value.
    change_an_attribute(&mut main_thread, element, index, value);
}

/// "Change an attribute", for the attribute at `index` in the element's attribute list, to the value whose reference
/// passes to the call.
/// https://dom.spec.whatwg.org/#concept-element-attributes-change
///
/// # Safety
///
/// The element must be live and have an attribute at `index`, `value` must be a reference to a string, and the call
/// must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_change_an_attribute(element: &NodeCell, index: usize, value: usize) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    // SAFETY: The caller passes its reference to the value.
    let value = unsafe { ak::Utf16String::from_raw_owned(value) };
    change_an_attribute(&mut main_thread, element, index, value);
}

/// "Remove an attribute", for the attribute at `index` in the element's attribute list.
/// https://dom.spec.whatwg.org/#concept-element-attributes-remove
///
/// # Safety
///
/// The element must be live and have an attribute at `index`, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_remove_an_attribute(element: &NodeCell, index: usize) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    remove_an_attribute(&mut main_thread, element, index);
}

/// "Remove an attribute by name", answering whether there was an attribute to remove.
/// https://dom.spec.whatwg.org/#concept-element-attributes-remove-by-name
///
/// # Safety
///
/// The element must be live, `qualified_name` the raw word of a live string, and the call must be made on the main
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_remove_an_attribute_by_name(element: &NodeCell, qualified_name: usize) -> bool {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    // SAFETY: The string is live for the call.
    let qualified_name = unsafe { BorrowedString::from_raw(qualified_name) };

    // 1. Let attr be the result of getting an attribute given qualifiedName and element.
    let attribute = get_an_attribute_by_name(&mut main_thread, element, qualified_name);

    // 2. If attr is non-null, then remove attr.
    if let Some(index) = attribute {
        remove_an_attribute(&mut main_thread, element, index);
    }

    // 3. Return attr.
    attribute.is_some()
}

/// "Remove an attribute by namespace and local name", answering whether there was an attribute to remove.
/// https://dom.spec.whatwg.org/#concept-element-attributes-remove-by-namespace
///
/// # Safety
///
/// The element must be live, `namespace_uri` the raw word of a live interned string or 0, `local_name` that of a live
/// interned string, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_remove_an_attribute_by_namespace_and_local_name(
    element: &NodeCell,
    namespace_uri: usize,
    local_name: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    // SAFETY: The strings are live for the call.
    let (namespace_uri, local_name) = unsafe {
        (
            (namespace_uri != 0).then(|| BorrowedString::from_raw(namespace_uri)),
            BorrowedString::from_raw(local_name),
        )
    };

    // 1. Let attr be the result of getting an attribute given namespace, localName, and element.
    let attribute = get_an_attribute_by_namespace_and_local_name(&mut main_thread, element, namespace_uri, local_name);

    // 2. If attr is non-null, then remove attr.
    if let Some(index) = attribute {
        remove_an_attribute(&mut main_thread, element, index);
    }

    // 3. Return attr.
    attribute.is_some()
}

/// What toggleAttribute() is given for force.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ToggleAttributeForce {
    NotGiven,
    False,
    True,
}

/// Steps 2 to 6 of toggleAttribute(qualifiedName, force). C++ runs step 1 before.
/// https://dom.spec.whatwg.org/#dom-element-toggleattribute
///
/// # Safety
///
/// The element must be live, `qualified_name` the raw word of a live string, and the call must be made on the main
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_toggle_attribute(
    element: &NodeCell,
    qualified_name: usize,
    force: ToggleAttributeForce,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    // SAFETY: The string is live for the call.
    let qualified_name = unsafe { BorrowedString::from_raw(qualified_name) };

    // 2. If this is in the HTML namespace and its node document is an HTML document, then set qualifiedName to
    //    qualifiedName in ASCII lowercase.
    let qualified_name = LookupName::new(qualified_name, lowercases_qualified_names(element));

    // 3. Let attribute be the first attribute in this's attribute list whose qualified name is qualifiedName, and
    //    null otherwise.
    let attribute = find_by_qualified_name(&mut main_thread, element, qualified_name);

    // 4. If attribute is null:
    let Some(index) = attribute else {
        // 1. If force is not given or is true, then append the result of creating an attribute given this's node
        //    document and qualifiedName to this, and then return true.
        if force != ToggleAttributeForce::False {
            let local_name = qualified_name.to_utf16_string();
            append_an_attribute(
                &mut main_thread,
                element,
                local_name.raw_identity(),
                0,
                0,
                ak::Utf16String::default(),
            );
            return true;
        }

        // 2. Return false.
        return false;
    };

    // 5. If force is not given or is false, remove an attribute given qualifiedName and this, and then return false.
    if force != ToggleAttributeForce::True {
        remove_an_attribute(&mut main_thread, element, index);
        return false;
    }

    // 6. Return true.
    true
}

/// "Handle attribute changes", for the attributes C++ changes in its list itself: those of Attr nodes it appends and
/// replaces.
/// https://dom.spec.whatwg.org/#handle-attribute-changes
///
/// # Safety
///
/// The element must be live, `name` the parts of a live qualified name, `old_value` and `new_value` raw words of live
/// strings, null where `old_value_is_null` and `new_value_is_null` say so, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_handle_attribute_changes(
    element: &NodeCell,
    name: NonNull<QualifiedNameImpl>,
    old_value: usize,
    old_value_is_null: bool,
    new_value: usize,
    new_value_is_null: bool,
) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    handle_attribute_changes(
        &mut main_thread,
        element,
        name,
        (!old_value_is_null).then_some(old_value),
        (!new_value_is_null).then_some(new_value),
        IsRemoval::No,
    );
}

/// Sets the element's classes to the tokens of the value of its class attribute, `value` or null where `value_is_null`
/// says so, for the attribute change steps that C++ runs.
///
/// # Safety
///
/// The element must be live, nothing may borrow its classes, `value` must be the raw word of a live string unless
/// `value_is_null`, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_set_classes(element: &NodeCell, value: usize, value_is_null: bool) {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    set_classes(element, (!value_is_null).then_some(value));
}

/// Bumps the DOM tree version for a change to an attribute of the element that C++ makes itself.
///
/// # Safety
///
/// The element must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_bump_dom_tree_version_for_attribute_change(element: &NodeCell) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The element is live.
    let element = unsafe { NodeRef::from_cell(element, &main_thread) };
    bump_dom_tree_version_for_attribute_change(&mut main_thread, element);
}
