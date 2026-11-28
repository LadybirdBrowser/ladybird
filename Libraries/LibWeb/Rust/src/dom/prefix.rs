/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The start of each DOM object, as Rust lays it out: one `#[repr(C)]` struct per level of the class hierarchy, which
//! starts with the struct of the level above it, and holds the level's own fields after that. Rust reaches the state
//! of an object as typed fields of the prefix of its class, read in place from the start of its cell, and C++ asserts
//! that each of its classes starts with the prefix of the same name.
//!
//! The fields of a level that C++ owns, and which Rust never reads, sit in its prefix as opaque slots, each the size
//! and alignment of the C++ member it stands for. They are what keeps C++ inside the region Rust lays out, and the
//! list below is meant to shrink. A field that C++ owns but Rust reads, like the pointer to an event target's data, is
//! typed as what C++ keeps there instead. A level whose class has no subclass with a prefix of its own ends with its
//! last Rust field, and the C++ members of the class follow it.
//!
//! | Prefix | Slot | What C++ keeps there | What would let Rust own it |
//! |---|---|---|---|
//! | WrappablePrefix | vtable | GC::Cell's vtable pointer | Rust defining the object, with C++ as a payload |
//! | WrappablePrefix | cell_header | GC::Cell's identity hash state, State and CellKind | the GC API crate |
//! | WrappablePrefix | preservation_flags | GC::Cell's embedder flags, which Bindings::GCAllocatedWrappable keeps its wrapper preservation flags in | wrapper preservation in Rust |
//! | WrappablePrefix | main_world_wrapper | Bindings::Wrappable::m_main_world_wrapper, a GC::Weak<JS::HostObject> | the GC API crate's weak references |
//! | NodePrefix | associated_animation_count_in_subtree | DOM::Node::m_associated_animation_count_in_subtree, a u32 | the animation bookkeeping in Rust |
//! | NodePrefix | node_directory_slot | DOM::Node::m_node_directory_slot, a u32 | the node directory in Rust |
//! | NodePrefix | rare_data | DOM::Node::m_rare_data, an OwnPtr<Node::RareData> | the rare data in Rust, or moved after the prefixes |
//! | TextPrefix | slottable_mixin_vtable | the vtable pointer of DOM::Text's SlottableMixin base | SlottableMixin without virtual functions |
//! | ElementPrefix | slottable_mixin_vtable | the vtable pointer of DOM::Element's SlottableMixin base | SlottableMixin without virtual functions |
//! | ElementPrefix | aria_mixin_vtable | the vtable pointer of DOM::Element's ARIA::ARIAMixin base | ARIAMixin without virtual functions |
//! | ElementPrefix | animatable_vtable | the vtable pointer of DOM::Element's Animations::Animatable base | Animatable without virtual functions |
//! | ElementPrefix | animations | Animations::Animatable::m_animations, a pointer to the element's animations in Rust | the animations kept in a typed field |
//! | ElementPrefix | inline_declarations | DOM::Element::m_inline_declarations, a pointer to the style attribute's declarations | the declarations kept in a typed field |
//! | ElementPrefix | installed_style | DOM::Element::m_installed_style, a CSS::InstalledStyle | the style engine reaching the element's record |
//! | ElementPrefix | animation_style_generation | DOM::Element::m_animation_style_generation, a u64 | the animation bookkeeping in Rust |
//! | ElementPrefix | animation_subtree_style_generation | DOM::Element::m_animation_subtree_style_generation, a u64 | the animation bookkeeping in Rust |
//! | ElementPrefix | published_presentational_hint_properties | DOM::Element::m_published_presentational_hint_properties, a Vector<CSS::StyleProperty> | presentational hints in Rust |
//! | ElementPrefix | style_flags | DOM::Element's one-bit flags, from m_initial_style_features_pending to m_style_attribute_awaits_content_security_policy_check | flags Rust reads, in the core |
//! | ElementPrefix | custom_element_state | DOM::Element::m_custom_element_state, a CustomElementState | the custom element state in the core |
//! | ElementPrefix | inline_style | DOM::Element::m_inline_style, a GC::Member<CSS::CSSStyleProperties> | the GC API crate |
//! | ElementPrefix | lang_value | DOM::Element::m_lang_value, an Optional<Utf16String> | the lang value in the core |
//! | HTMLElementPrefix | global_event_handlers_vtable | the vtable pointer of HTML::HTMLElement's HTML::GlobalEventHandlers base | GlobalEventHandlers without virtual functions |
//! | HTMLElementPrefix | form_associated_element_vtable | the vtable pointer of HTML::HTMLElement's HTML::FormAssociatedElement base | FormAssociatedElement without virtual functions |
//! | HTMLElementPrefix | content_editable_state | HTML::HTMLElement::m_content_editable_state, a ContentEditableState | the editing state in Rust |
//! | HTMLElementPrefix | click_in_progress | HTML::HTMLElement::m_click_in_progress, a bool | HTMLElement's state in Rust |
//! | HTMLElementPrefix | popover_visibility_state | HTML::HTMLElement::m_popover_visibility_state, a PopoverVisibilityState | the popover state in Rust |
//! | HTMLElementPrefix | popover_showing_or_hiding | HTML::HTMLElement::m_popover_showing_or_hiding, a bool | the popover state in Rust |
//! | DocumentPrefix | global_event_handlers_vtable | the vtable pointer of DOM::Document's HTML::GlobalEventHandlers base | GlobalEventHandlers without virtual functions |
//!
//! Each level starts after all of the level above it, padding included, which is where #[repr(C)] places a nested
//! struct's successor, and where every C++ ABI places the members of a derived class. The Itanium ABI could also place
//! them in the padding at the end of the base class, which the MSVC ABI never does, so the C++ classes assert that none
//! of the fields a prefix describes is there. GC::Cell, JS::Cell and Bindings::GCAllocatedWrappable are no levels of
//! their own: WrappablePrefix covers the cell's header, whose last bytes GC::Cell keeps for the embedder, which keeps
//! the wrapper preservation flags and the class id there, and GCAllocatedWrappable has no fields.

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::mem::offset_of;

use libgc_api::Trace;
use libgc_api::layout::CELL_IDENTITY_HASH_STATE_OFFSET;

use super::attr::AttrCore;
use super::element_core::ElementCore;
use super::event_target::EventTargetCore;
use super::event_target::EventTargetDataField;
use super::node_class::NodeClassId;
use super::node_core::CharacterDataCore;
use super::node_core::DocumentCore;
use super::node_core::DocumentFragmentCore;
use super::node_core::NodeCore;
use super::node_core::ParentNodeCore;
use super::node_core::TextCore;
use super::slots::ShadowRootCore;
use super::slots::SlotCore;

/// The prefix `P` of the object whose cell starts at `cell`, read for `'a`.
///
/// # Safety
///
/// `cell` must be the start of a live object whose class starts with `P`, which lives for `'a`, and whose fields outside
/// of cells C++ does not change for `'a`.
#[inline(always)]
pub(super) unsafe fn prefix_of<'a, P, T>(cell: *const T) -> &'a P {
    // SAFETY: Guaranteed by the caller.
    unsafe { &*cell.cast::<P>() }
}

/// A C++ member inside a prefix, as `N` units of `T`, which give it the member's size and alignment. Rust never reads
/// or writes it, and C++ changes it behind shared references to the prefix.
#[repr(transparent)]
pub struct OpaqueCxx<T, const N: usize>(UnsafeCell<MaybeUninit<[T; N]>>);

// SAFETY: C++ owns what the slot holds, and visits the cells it can hold in its visit_edges().
unsafe impl<T, const N: usize> Trace for OpaqueCxx<T, N> {
    #[inline(always)]
    fn trace(&self, _: &mut libgc_api::Visitor<'_>) {}
}

#[cfg(test)]
impl<T, const N: usize> OpaqueCxx<T, N> {
    pub(crate) const fn zeroed_for_test() -> Self {
        Self(UnsafeCell::new(MaybeUninit::zeroed()))
    }
}

/// The start of every Bindings::Wrappable: the header of its GC::Cell, with the wrapper preservation flags and the
/// class id in the embedder's bytes of it, and the fields of Wrappable.
#[derive(Trace)]
#[repr(C)]
pub struct WrappablePrefix {
    vtable: OpaqueCxx<u64, 1>,
    cell_header: OpaqueCxx<u8, 3>,
    preservation_flags: OpaqueCxx<u8, 1>,
    /// The class of the node this is, or NodeClassId::NotANode, which C++ writes as it constructs the object.
    class_id: NodeClassId,
    main_world_wrapper: OpaqueCxx<u64, 1>,
}

impl WrappablePrefix {
    #[inline(always)]
    pub fn class_id(&self) -> NodeClassId {
        self.class_id
    }
}

/// The start of every DOM::EventTarget.
#[derive(Trace)]
#[repr(C)]
pub struct EventTargetPrefix {
    pub(super) wrappable: WrappablePrefix,
    pub(super) core: EventTargetCore,
    /// The target's DOM::EventTarget::Data, which C++ allocates as the target first needs it, and frees.
    pub(super) data: EventTargetDataField,
}

/// The start of every DOM::Node.
#[derive(Trace)]
#[repr(C)]
pub struct NodePrefix {
    pub(super) event_target: EventTargetPrefix,
    pub(super) core: NodeCore,
    associated_animation_count_in_subtree: OpaqueCxx<u32, 1>,
    node_directory_slot: OpaqueCxx<u32, 1>,
    rare_data: OpaqueCxx<u64, 1>,
}

/// The start of every DOM::ParentNode: an element, a document or a DocumentFragment.
#[derive(Trace)]
#[repr(C)]
pub struct ParentNodePrefix {
    pub(super) node: NodePrefix,
    pub(super) core: ParentNodeCore,
}

/// The start of every DOM::CharacterData node.
#[derive(Trace)]
#[repr(C)]
pub struct CharacterDataPrefix {
    pub(super) node: NodePrefix,
    pub(super) core: CharacterDataCore,
}

/// The start of every DOM::Text node.
#[derive(Trace)]
#[repr(C)]
pub struct TextPrefix {
    pub(super) character_data: CharacterDataPrefix,
    slottable_mixin_vtable: OpaqueCxx<u64, 1>,
    pub(super) core: TextCore,
}

/// The start of every DOM::Attr node.
#[derive(Trace)]
#[repr(C)]
pub struct AttrPrefix {
    pub(super) node: NodePrefix,
    pub(super) core: AttrCore,
}

/// The start of every DOM::DocumentFragment.
#[derive(Trace)]
#[repr(C)]
pub struct DocumentFragmentPrefix {
    pub(super) parent_node: ParentNodePrefix,
    pub(super) core: DocumentFragmentCore,
}

/// The start of every DOM::ShadowRoot.
#[derive(Trace)]
#[repr(C)]
pub struct ShadowRootPrefix {
    pub(super) document_fragment: DocumentFragmentPrefix,
    pub(super) core: ShadowRootCore,
}

/// The start of every DOM::Document.
#[derive(Trace)]
#[repr(C)]
pub struct DocumentPrefix {
    pub(super) parent_node: ParentNodePrefix,
    global_event_handlers_vtable: OpaqueCxx<u64, 1>,
    pub(super) core: DocumentCore,
}

/// The start of every DOM::Element. HTMLElementPrefix starts with it, so it covers all of DOM::Element's fields.
#[derive(Trace)]
#[repr(C)]
pub struct ElementPrefix {
    pub(super) parent_node: ParentNodePrefix,
    slottable_mixin_vtable: OpaqueCxx<u64, 1>,
    aria_mixin_vtable: OpaqueCxx<u64, 1>,
    animatable_vtable: OpaqueCxx<u64, 1>,
    animations: OpaqueCxx<u64, 1>,
    pub(super) core: ElementCore,
    inline_declarations: OpaqueCxx<u64, 1>,
    installed_style: OpaqueCxx<u64, 1>,
    animation_style_generation: OpaqueCxx<u64, 1>,
    animation_subtree_style_generation: OpaqueCxx<u64, 1>,
    published_presentational_hint_properties: OpaqueCxx<u64, 3>,
    style_flags: OpaqueCxx<u8, 3>,
    custom_element_state: OpaqueCxx<u8, 1>,
    inline_style: OpaqueCxx<u32, 1>,
    lang_value: OpaqueCxx<u64, 1>,
}

/// The start of every HTML::HTMLElement, which only HTMLSlotElementPrefix starts with. Rust reads none of it.
#[derive(Trace)]
#[repr(C)]
pub struct HTMLElementPrefix {
    pub(super) element: ElementPrefix,
    global_event_handlers_vtable: OpaqueCxx<u64, 1>,
    form_associated_element_vtable: OpaqueCxx<u64, 1>,
    content_editable_state: OpaqueCxx<u8, 1>,
    click_in_progress: OpaqueCxx<u8, 1>,
    popover_visibility_state: OpaqueCxx<u8, 1>,
    popover_showing_or_hiding: OpaqueCxx<u8, 1>,
}

/// The start of every HTML::HTMLSlotElement.
#[derive(Trace)]
#[repr(C)]
pub struct HTMLSlotElementPrefix {
    pub(super) html_element: HTMLElementPrefix,
    pub(super) core: SlotCore,
}

#[cfg(test)]
impl WrappablePrefix {
    pub(crate) const fn for_test(class_id: NodeClassId) -> Self {
        Self {
            vtable: OpaqueCxx::zeroed_for_test(),
            cell_header: OpaqueCxx::zeroed_for_test(),
            preservation_flags: OpaqueCxx::zeroed_for_test(),
            class_id,
            main_world_wrapper: OpaqueCxx::zeroed_for_test(),
        }
    }
}

#[cfg(test)]
impl EventTargetPrefix {
    pub(crate) fn for_test(class_id: NodeClassId) -> Self {
        Self {
            wrappable: WrappablePrefix::for_test(class_id),
            core: EventTargetCore::null_for_test(),
            data: EventTargetDataField::null_for_test(),
        }
    }
}

#[cfg(test)]
impl NodePrefix {
    pub(crate) fn for_test(class_id: NodeClassId, core: NodeCore) -> Self {
        Self {
            event_target: EventTargetPrefix::for_test(class_id),
            core,
            associated_animation_count_in_subtree: OpaqueCxx::zeroed_for_test(),
            node_directory_slot: OpaqueCxx::zeroed_for_test(),
            rare_data: OpaqueCxx::zeroed_for_test(),
        }
    }
}

#[cfg(test)]
impl TextPrefix {
    pub(crate) fn for_test(character_data: CharacterDataPrefix, core: TextCore) -> Self {
        Self {
            character_data,
            slottable_mixin_vtable: OpaqueCxx::zeroed_for_test(),
            core,
        }
    }
}

#[cfg(test)]
impl DocumentPrefix {
    pub(crate) fn for_test(parent_node: ParentNodePrefix, core: DocumentCore) -> Self {
        Self {
            parent_node,
            global_event_handlers_vtable: OpaqueCxx::zeroed_for_test(),
            core,
        }
    }
}

#[cfg(test)]
impl ElementPrefix {
    pub(crate) fn for_test(parent_node: ParentNodePrefix, core: ElementCore) -> Self {
        Self {
            parent_node,
            slottable_mixin_vtable: OpaqueCxx::zeroed_for_test(),
            aria_mixin_vtable: OpaqueCxx::zeroed_for_test(),
            animatable_vtable: OpaqueCxx::zeroed_for_test(),
            animations: OpaqueCxx::zeroed_for_test(),
            core,
            inline_declarations: OpaqueCxx::zeroed_for_test(),
            installed_style: OpaqueCxx::zeroed_for_test(),
            animation_style_generation: OpaqueCxx::zeroed_for_test(),
            animation_subtree_style_generation: OpaqueCxx::zeroed_for_test(),
            published_presentational_hint_properties: OpaqueCxx::zeroed_for_test(),
            style_flags: OpaqueCxx::zeroed_for_test(),
            custom_element_state: OpaqueCxx::zeroed_for_test(),
            inline_style: OpaqueCxx::zeroed_for_test(),
            lang_value: OpaqueCxx::zeroed_for_test(),
        }
    }
}

// NB: The C++ classes assert that they start with the prefixes of the same names.
const _: () = assert!(
    size_of::<WrappablePrefix>() == 24
        && offset_of!(WrappablePrefix, cell_header) == CELL_IDENTITY_HASH_STATE_OFFSET
        && offset_of!(WrappablePrefix, preservation_flags) == 11
        && offset_of!(WrappablePrefix, class_id) == 12
        && offset_of!(WrappablePrefix, main_world_wrapper) == 16
);
const _: () = assert!(
    size_of::<EventTargetPrefix>() == 40
        && offset_of!(EventTargetPrefix, core) == 24
        && offset_of!(EventTargetPrefix, data) == 32
);
const _: () = assert!(
    size_of::<NodePrefix>() == 96
        && offset_of!(NodePrefix, core) == 40
        && offset_of!(NodePrefix, associated_animation_count_in_subtree) == 80
        && offset_of!(NodePrefix, node_directory_slot) == 84
        && offset_of!(NodePrefix, rare_data) == 88
);
const _: () = assert!(size_of::<ParentNodePrefix>() == 112 && offset_of!(ParentNodePrefix, core) == 96);
const _: () = assert!(size_of::<CharacterDataPrefix>() == 104 && offset_of!(CharacterDataPrefix, core) == 96);
const _: () = assert!(
    size_of::<TextPrefix>() == 128
        && offset_of!(TextPrefix, slottable_mixin_vtable) == 104
        && offset_of!(TextPrefix, core) == 112
);
const _: () = assert!(size_of::<AttrPrefix>() == 120 && offset_of!(AttrPrefix, core) == 96);
const _: () = assert!(size_of::<DocumentFragmentPrefix>() == 120 && offset_of!(DocumentFragmentPrefix, core) == 112);
const _: () = assert!(size_of::<ShadowRootPrefix>() == 168 && offset_of!(ShadowRootPrefix, core) == 120);
const _: () = assert!(
    size_of::<DocumentPrefix>() == 216
        && offset_of!(DocumentPrefix, global_event_handlers_vtable) == 112
        && offset_of!(DocumentPrefix, core) == 120
);
const _: () = assert!(
    size_of::<ElementPrefix>() == 296
        && offset_of!(ElementPrefix, slottable_mixin_vtable) == 112
        && offset_of!(ElementPrefix, aria_mixin_vtable) == 120
        && offset_of!(ElementPrefix, animatable_vtable) == 128
        && offset_of!(ElementPrefix, animations) == 136
        && offset_of!(ElementPrefix, core) == 144
        && offset_of!(ElementPrefix, inline_declarations) == 224
        && offset_of!(ElementPrefix, installed_style) == 232
        && offset_of!(ElementPrefix, animation_style_generation) == 240
        && offset_of!(ElementPrefix, animation_subtree_style_generation) == 248
        && offset_of!(ElementPrefix, published_presentational_hint_properties) == 256
        && offset_of!(ElementPrefix, style_flags) == 280
        && offset_of!(ElementPrefix, custom_element_state) == 283
        && offset_of!(ElementPrefix, inline_style) == 284
        && offset_of!(ElementPrefix, lang_value) == 288
);
const _: () = assert!(
    size_of::<HTMLElementPrefix>() == 320
        && offset_of!(HTMLElementPrefix, global_event_handlers_vtable) == 296
        && offset_of!(HTMLElementPrefix, form_associated_element_vtable) == 304
        && offset_of!(HTMLElementPrefix, content_editable_state) == 312
        && offset_of!(HTMLElementPrefix, click_in_progress) == 313
        && offset_of!(HTMLElementPrefix, popover_visibility_state) == 314
        && offset_of!(HTMLElementPrefix, popover_showing_or_hiding) == 315
);
const _: () = assert!(size_of::<HTMLSlotElementPrefix>() == 368 && offset_of!(HTMLSlotElementPrefix, core) == 320);
