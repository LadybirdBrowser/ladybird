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
//! For the classes whose objects Rust defines whole (see node_objects.rs and element_objects.rs), Rust writes each slot
//! of a new node as the C++ constructors would, and C++ visits, finalizes and destroys what the slots hold through
//! hooks. For an element, Rust compares the slots with what they start out as, so that it only calls a hook when C++
//! has something to do.
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
//! | ElementPrefix | inline_style | DOM::Element::m_inline_style, a GC::Member<CSS::CSSStyleProperties> | the GC API crate |
//! | ElementPrefix | lang_value | DOM::Element::m_lang_value, an Optional<Utf16String> | the lang value in the core |
//! | HTMLElementPrefix | global_event_handlers_vtable | the vtable pointer of HTML::HTMLElement's HTML::GlobalEventHandlers base | GlobalEventHandlers without virtual functions |
//! | HTMLElementPrefix | form_associated_element_vtable | the vtable pointer of HTML::HTMLElement's HTML::FormAssociatedElement base | FormAssociatedElement without virtual functions |
//! | HTMLElementPrefix | content_editable_state | HTML::HTMLElement::m_content_editable_state, a ContentEditableState | the editing state in Rust |
//! | HTMLElementPrefix | click_in_progress | HTML::HTMLElement::m_click_in_progress, a bool | HTMLElement's state in Rust |
//! | HTMLElementPrefix | popover_visibility_state | HTML::HTMLElement::m_popover_visibility_state, a PopoverVisibilityState | the popover state in Rust |
//! | HTMLElementPrefix | popover_showing_or_hiding | HTML::HTMLElement::m_popover_showing_or_hiding, a bool | the popover state in Rust |
//! | MathMLElementPrefix | global_event_handlers_vtable | the vtable pointer of MathML::MathMLElement's HTML::GlobalEventHandlers base | GlobalEventHandlers without virtual functions |
//! | DocumentPrefix | global_event_handlers_vtable | the vtable pointer of DOM::Document's HTML::GlobalEventHandlers base | GlobalEventHandlers without virtual functions |
//!
//! Each level starts after all of the level above it, padding included, which is where #[repr(C)] places a nested
//! struct's successor, and where every C++ ABI places the members of a derived class. The Itanium ABI could also place
//! them in the padding at the end of the base class, which the MSVC ABI never does, so the C++ classes assert that none
//! of the fields a prefix describes is there. GC::Cell, JS::Cell and Bindings::GCAllocatedWrappable are no levels of
//! their own: WrappablePrefix covers the cell's header, whose last bytes GC::Cell keeps for the embedder, which keeps
//! the wrapper preservation flags and the class id there, and GCAllocatedWrappable has no fields.

use core::cell::Cell;
use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::mem::offset_of;

use libgc_api::Trace;
use libgc_api::cell::{CELL_KIND_OTHER, CELL_STATE_LIVE};
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
use super::node_core::NodePointer;
use super::node_core::NodeType;
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

/// A C++ member inside a prefix, as `N` units of `T`, which give it the member's size and alignment. Rust never writes it
/// once the object exists, and only reads it to compare it with what it starts out as. C++ changes it behind shared
/// references to the prefix.
#[repr(transparent)]
pub struct OpaqueCxx<T, const N: usize>(UnsafeCell<MaybeUninit<[T; N]>>);

// SAFETY: C++ owns what the slot holds, and visits the cells it can hold in its visit_edges().
unsafe impl<T, const N: usize> Trace for OpaqueCxx<T, N> {
    #[inline(always)]
    fn trace(&self, _: &mut libgc_api::Visitor<'_>) {}
}

impl<T, const N: usize> OpaqueCxx<T, N> {
    /// A slot that holds `units`, which is how the C++ member it stands for starts out in an object Rust makes.
    #[inline(always)]
    pub(super) const fn new(units: [T; N]) -> Self {
        Self(UnsafeCell::new(MaybeUninit::new(units)))
    }
}

impl<T: Copy + PartialEq, const N: usize> OpaqueCxx<T, N> {
    /// Whether the slot holds `units`, which is what the C++ member it stands for starts out as, as C++ left it.
    ///
    /// # Safety
    ///
    /// The slot must be one of a live object, whose C++ constructor initialized it, and which C++ does not change during
    /// the call.
    #[inline(always)]
    pub(super) unsafe fn holds(&self, units: [T; N]) -> bool {
        // SAFETY: Guaranteed by the caller. Every member these slots stand for is plain data in every bit pattern C++
        //         leaves in it.
        unsafe { (*self.0.get()).assume_init_read() == units }
    }
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
    /// DOM::Element::m_custom_element_state, a CustomElementState, which C++ keeps as the element's custom element state
    /// changes, and which Rust reads and writes as it clones an element.
    /// https://dom.spec.whatwg.org/#concept-element-custom-element-state
    pub(super) custom_element_state: Cell<u8>,
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

/// The start of every MathML::MathMLElement.
#[derive(Trace)]
#[repr(C)]
pub struct MathMLElementPrefix {
    pub(super) element: ElementPrefix,
    global_event_handlers_vtable: OpaqueCxx<u64, 1>,
}

/// The start of every HTML::HTMLSlotElement.
#[derive(Trace)]
#[repr(C)]
pub struct HTMLSlotElementPrefix {
    pub(super) html_element: HTMLElementPrefix,
    pub(super) core: SlotCore,
}

/// What C++ writes into a node it constructs that Rust has to write into a node it makes: the vtable pointers of the
/// node's class, and the pointer to the shared null GC::WeakImpl that every null GC::Weak holds.
#[derive(Clone, Copy)]
pub(super) struct CxxWords {
    pub(super) vtable: u64,
    pub(super) null_weak_impl: u64,
}

impl NodePrefix {
    /// The prefix of `node`, a new node of the class `class_id` and of `node_type`, that is no document, and whose node
    /// document is `document`, as the constructors of DOM::Node and the classes it derives from make it.
    #[inline(always)]
    pub(super) fn new(
        words: CxxWords,
        class_id: NodeClassId,
        node_type: NodeType,
        node: NodePointer,
        document: NodePointer,
    ) -> Self {
        Self {
            event_target: EventTargetPrefix {
                wrappable: WrappablePrefix {
                    vtable: OpaqueCxx::new([words.vtable]),
                    // NB: A cell whose identity was never hashed, which is live and of GC::CellKind::Other.
                    cell_header: OpaqueCxx::new([0, CELL_STATE_LIVE, CELL_KIND_OTHER]),
                    preservation_flags: OpaqueCxx::new([0]),
                    class_id,
                    // NB: A GC::Weak that is null.
                    main_world_wrapper: OpaqueCxx::new([words.null_weak_impl]),
                },
                core: EventTargetCore::for_new_node(),
                data: EventTargetDataField::null(),
            },
            core: NodeCore::new(node_type, node, document),
            associated_animation_count_in_subtree: OpaqueCxx::new([0]),
            node_directory_slot: OpaqueCxx::new([0]),
            // NB: A null OwnPtr.
            rare_data: OpaqueCxx::new([0]),
        }
    }
}

impl ParentNodePrefix {
    /// The prefix of a new ParentNode that starts with `node`, as the constructor of DOM::ParentNode makes it.
    #[inline(always)]
    pub(super) fn new(node: NodePrefix) -> Self {
        Self {
            node,
            core: ParentNodeCore::new(),
        }
    }
}

/// The custom element state that C++ gives a new element, CustomElementState::Undefined.
pub(super) const CUSTOM_ELEMENT_STATE_UNDEFINED: u8 = 0;
/// CustomElementState::Uncustomized.
pub(super) const CUSTOM_ELEMENT_STATE_UNCUSTOMIZED: u8 = 2;
/// The state that C++ gives the contenteditable attribute of a new HTML element, ContentEditableState::Inherit.
const CONTENT_EDITABLE_STATE_INHERIT: u8 = 3;

impl ElementPrefix {
    /// The prefix of a new element that starts with `parent_node` and has the core `core`, as the constructor of
    /// DOM::Element makes it, whose SlottableMixin, ARIA::ARIAMixin and Animations::Animatable bases C++ gives the
    /// vtable pointers `slottable_mixin_vtable`, `aria_mixin_vtable` and `animatable_vtable`.
    #[inline(always)]
    pub(super) fn new(
        parent_node: ParentNodePrefix,
        slottable_mixin_vtable: u64,
        aria_mixin_vtable: u64,
        animatable_vtable: u64,
        core: ElementCore,
    ) -> Self {
        Self {
            parent_node,
            slottable_mixin_vtable: OpaqueCxx::new([slottable_mixin_vtable]),
            aria_mixin_vtable: OpaqueCxx::new([aria_mixin_vtable]),
            animatable_vtable: OpaqueCxx::new([animatable_vtable]),
            // NB: Null pointers, an empty CSS::InstalledStyle, generations of 0, an empty Vector, clear flags, a null
            //     GC::Member and an empty Optional, as DOM::Element's default member initializers make them, which
            //     holds_new_slots() checks against an element C++ constructs.
            animations: OpaqueCxx::new([0]),
            core,
            inline_declarations: OpaqueCxx::new([0]),
            installed_style: OpaqueCxx::new([0]),
            animation_style_generation: OpaqueCxx::new([0]),
            animation_subtree_style_generation: OpaqueCxx::new([0]),
            published_presentational_hint_properties: OpaqueCxx::new([0; 3]),
            style_flags: OpaqueCxx::new([0; 3]),
            custom_element_state: Cell::new(CUSTOM_ELEMENT_STATE_UNDEFINED),
            inline_style: OpaqueCxx::new([0]),
            lang_value: OpaqueCxx::new([0]),
        }
    }

    /// Whether the slots of DOM::Element in the prefix hold what new() writes into them, for C++ to check that new()
    /// makes an element as its constructor does.
    ///
    /// # Safety
    ///
    /// The prefix must be that of an element C++ constructed, which it does not change during the call.
    pub(super) unsafe fn holds_new_slots(
        &self,
        slottable_mixin_vtable: u64,
        aria_mixin_vtable: u64,
        animatable_vtable: u64,
    ) -> bool {
        // SAFETY: Guaranteed by the caller.
        unsafe {
            self.slottable_mixin_vtable.holds([slottable_mixin_vtable])
                && self.aria_mixin_vtable.holds([aria_mixin_vtable])
                && self.animatable_vtable.holds([animatable_vtable])
                && self.animations.holds([0])
                && self.inline_declarations.holds([0])
                && self.installed_style.holds([0])
                && self.animation_style_generation.holds([0])
                && self.animation_subtree_style_generation.holds([0])
                && self.published_presentational_hint_properties.holds([0; 3])
                && self.style_flags.holds([0; 3])
                && self.custom_element_state.get() == CUSTOM_ELEMENT_STATE_UNDEFINED
                && self.inline_style.holds([0])
                && self.lang_value.holds([0])
        }
    }

    /// Whether C++ keeps cells in the slots of the element, which only it can visit: a preserved wrapper, the event
    /// handlers of its event target data, its rare data, its animations or the CSSOM object of its style attribute.
    ///
    /// # Safety
    ///
    /// The prefix must be that of a live element, which C++ does not change during the call.
    #[inline(always)]
    pub(super) unsafe fn has_cxx_edges(&self) -> bool {
        let node = &self.parent_node.node;
        let event_target = &node.event_target;
        // SAFETY: Guaranteed by the caller.
        unsafe {
            !event_target.wrappable.preservation_flags.holds([0])
                || !event_target.data.is_null()
                || !node.rare_data.holds([0])
                || !self.animations.holds([0])
                || !self.inline_style.holds([0])
        }
    }

    /// Whether C++ keeps memory outside the heap in the slots of the element, which only it can measure: its event
    /// target data, its rare data or the declarations of its style attribute.
    ///
    /// # Safety
    ///
    /// As for has_cxx_edges().
    #[inline(always)]
    pub(super) unsafe fn has_cxx_external_memory(&self) -> bool {
        let node = &self.parent_node.node;
        // SAFETY: Guaranteed by the caller.
        unsafe {
            !node.event_target.data.is_null() || !node.rare_data.holds([0]) || !self.inline_declarations.holds([0])
        }
    }

    /// Whether DOM::Element::finalize() has anything to do for the dead element: preserved wrappers outside the main
    /// world to forget, a node directory slot to free, the declarations of its style attribute or its animations to
    /// destroy.
    ///
    /// # Safety
    ///
    /// As for has_cxx_edges().
    #[inline(always)]
    pub(super) unsafe fn needs_cxx_finalize(&self) -> bool {
        let node = &self.parent_node.node;
        // SAFETY: Guaranteed by the caller.
        unsafe {
            !node.event_target.wrappable.preservation_flags.holds([0])
                || !node.node_directory_slot.holds([0])
                || !self.inline_declarations.holds([0])
                || !self.animations.holds([0])
        }
    }

    /// Whether a C++ member in the slots of the finalized element holds anything its destructor has to release: a weak
    /// reference to its wrapper, its event target data, its rare data, its animations, its installed style, its
    /// presentational hints or its lang value. `null_weak_impl` is what a null GC::Weak holds.
    ///
    /// # Safety
    ///
    /// As for has_cxx_edges().
    #[inline(always)]
    pub(super) unsafe fn needs_cxx_destroy(&self, null_weak_impl: u64) -> bool {
        let node = &self.parent_node.node;
        // SAFETY: Guaranteed by the caller.
        unsafe {
            !node.event_target.wrappable.main_world_wrapper.holds([null_weak_impl])
                || !node.event_target.data.is_null()
                || !node.rare_data.holds([0])
                || !self.animations.holds([0])
                || !self.installed_style.holds([0])
                || !self.published_presentational_hint_properties.holds([0; 3])
                || !self.lang_value.holds([0])
        }
    }

    /// Whether the element has rare data, which C++ keeps.
    ///
    /// # Safety
    ///
    /// As for has_cxx_edges().
    #[inline(always)]
    pub(super) unsafe fn has_rare_data(&self) -> bool {
        // SAFETY: Guaranteed by the caller.
        unsafe { !self.parent_node.node.rare_data.holds([0]) }
    }

    /// Whether the element's style attribute has declarations, or a CSSOM object of them, which C++ keeps.
    ///
    /// # Safety
    ///
    /// As for has_cxx_edges().
    #[inline(always)]
    pub(super) unsafe fn has_inline_style(&self) -> bool {
        // SAFETY: Guaranteed by the caller.
        unsafe { !self.inline_declarations.holds([0]) || !self.inline_style.holds([0]) }
    }
}

impl HTMLElementPrefix {
    /// The prefix of a new HTML element that starts with `element`, as the constructor of HTML::HTMLElement makes it,
    /// whose HTML::GlobalEventHandlers and HTML::FormAssociatedElement bases C++ gives the vtable pointers
    /// `global_event_handlers_vtable` and `form_associated_element_vtable`.
    #[inline(always)]
    pub(super) fn new(
        element: ElementPrefix,
        global_event_handlers_vtable: u64,
        form_associated_element_vtable: u64,
    ) -> Self {
        Self {
            element,
            global_event_handlers_vtable: OpaqueCxx::new([global_event_handlers_vtable]),
            form_associated_element_vtable: OpaqueCxx::new([form_associated_element_vtable]),
            content_editable_state: OpaqueCxx::new([CONTENT_EDITABLE_STATE_INHERIT]),
            click_in_progress: OpaqueCxx::new([0]),
            popover_visibility_state: OpaqueCxx::new([0]),
            popover_showing_or_hiding: OpaqueCxx::new([0]),
        }
    }

    /// Whether the slots of HTML::HTMLElement in the prefix hold what new() writes into them, see
    /// ElementPrefix::holds_new_slots().
    ///
    /// # Safety
    ///
    /// The prefix must be that of an HTML element C++ constructed, which it does not change during the call.
    pub(super) unsafe fn holds_new_slots(
        &self,
        global_event_handlers_vtable: u64,
        form_associated_element_vtable: u64,
    ) -> bool {
        // SAFETY: Guaranteed by the caller.
        unsafe {
            self.global_event_handlers_vtable.holds([global_event_handlers_vtable])
                && self
                    .form_associated_element_vtable
                    .holds([form_associated_element_vtable])
                && self.content_editable_state.holds([CONTENT_EDITABLE_STATE_INHERIT])
                && self.click_in_progress.holds([0])
                && self.popover_visibility_state.holds([0])
                && self.popover_showing_or_hiding.holds([0])
        }
    }
}

impl MathMLElementPrefix {
    /// The prefix of a new MathML element that starts with `element`, as the constructor of MathML::MathMLElement makes
    /// it, whose HTML::GlobalEventHandlers base C++ gives the vtable pointer `global_event_handlers_vtable`.
    #[inline(always)]
    pub(super) fn new(element: ElementPrefix, global_event_handlers_vtable: u64) -> Self {
        Self {
            element,
            global_event_handlers_vtable: OpaqueCxx::new([global_event_handlers_vtable]),
        }
    }

    /// Whether the slot of MathML::MathMLElement in the prefix holds what new() writes into it, see
    /// ElementPrefix::holds_new_slots().
    ///
    /// # Safety
    ///
    /// The prefix must be that of a MathML element C++ constructed, which it does not change during the call.
    pub(super) unsafe fn holds_new_slots(&self, global_event_handlers_vtable: u64) -> bool {
        // SAFETY: Guaranteed by the caller.
        unsafe { self.global_event_handlers_vtable.holds([global_event_handlers_vtable]) }
    }
}

impl DocumentPrefix {
    /// The prefix of a new document that starts with `parent_node`, as the constructor of DOM::Document makes it, which
    /// writes the vtable pointer of its HTML::GlobalEventHandlers base.
    #[inline(always)]
    pub(super) fn new(parent_node: ParentNodePrefix) -> Self {
        Self {
            parent_node,
            global_event_handlers_vtable: OpaqueCxx::new([0]),
            core: DocumentCore::new(),
        }
    }
}

impl TextPrefix {
    /// The prefix of a new Text node, whose SlottableMixin base C++ gives the vtable pointer `slottable_mixin_vtable`.
    #[inline(always)]
    pub(super) fn new(character_data: CharacterDataPrefix, slottable_mixin_vtable: u64) -> Self {
        Self {
            character_data,
            slottable_mixin_vtable: OpaqueCxx::new([slottable_mixin_vtable]),
            core: TextCore::new(),
        }
    }
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
            core: EventTargetCore::for_new_node(),
            data: EventTargetDataField::null(),
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
            custom_element_state: Cell::new(0),
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
const _: () = assert!(
    size_of::<MathMLElementPrefix>() == 304 && offset_of!(MathMLElementPrefix, global_event_handlers_vtable) == 296
);
