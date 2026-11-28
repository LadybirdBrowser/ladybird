/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Dispatching an event: building the event's path from the cores of the targets it passes, walking it in the
//! capturing and bubbling phases, and invoking the listeners of each target, which event_listeners.rs keeps. Only the
//! steps that need state C++ keeps call into it, each through a callback of the table C++ registers (see EventHooks),
//! and only for a target that needs it, such as running the callback of a listener, the "get the parent" of an event
//! target that is no node and has one, and the steps that run activation behavior.
//! https://dom.spec.whatwg.org/#concept-event-dispatch

use core::cell::UnsafeCell;
use core::ops::ControlFlow;

use super::event::EVENT_PHASE_AT_TARGET;
use super::event::EVENT_PHASE_BUBBLING;
use super::event::EVENT_PHASE_CAPTURING;
use super::event::EVENT_PHASE_NONE;
use super::event::EventCell;
use super::event::EventPathItem;
use super::event::EventRef;
use super::event::RawEventRef;
use super::event_listeners::ClonedListeners;
use super::event_listeners::EventListenerRef;
use super::event_listeners::RawEventListenerRef;
use super::event_listeners::remove_an_event_listener;
use super::event_listeners::with_cloned_listeners;
use super::event_target::EVENT_TARGET_HAS_GET_THE_PARENT;
use super::event_target::EventTargetCell;
use super::event_target::EventTargetMember;
use super::event_target::EventTargetRef;
use super::event_target::NullableEventTargetRef;
use super::event_target::RawEventTargetRef;
use super::event_target::RawNullableEventTargetRef;
use super::node_class::NODE_CLASS_HAS_ACTIVATION_BEHAVIOR;
use super::node_core::NodeType;
use super::node_ref::NodeRef;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

// What C++ works out about an event before it dispatches it, a bit each, which only C++ knows how to tell.

/// Whether the event is a MouseEvent object whose type attribute is "click".
pub const DISPATCH_IS_ACTIVATION_EVENT: u8 = 1 << 0;
/// Whether the event's type attribute is "load".
pub const DISPATCH_TYPE_IS_LOAD: u8 = 1 << 1;
/// Whether the event's path ends below a disabled form control, which it does for a trusted mousedown, mouseup, click
/// or dblclick MouseEvent.
pub const DISPATCH_PATH_ENDS_BELOW_DISABLED_FORM_CONTROL: u8 = 1 << 2;
/// Whether the event is a trusted PointerEvent, whose coalesced events have its target.
pub const DISPATCH_IS_TRUSTED_POINTER_EVENT: u8 = 1 << 3;

/// The type of an event that is being dispatched, as C++ works it out before it dispatches it.
#[derive(Clone, Copy)]
pub(crate) struct EventTypes {
    /// The bits of the event's type in the listener type summaries of the targets' cores, and those of its legacy type
    /// if it has one.
    pub(crate) bits: u16,
    /// The raw word of the interned string of the event's type attribute value.
    pub(crate) event_type: usize,
    /// The raw word of the interned string of the event's legacy type, if the event is trusted and has one, or 0.
    /// https://dom.spec.whatwg.org/#concept-event-listener-invoke
    pub(crate) legacy_event_type: usize,
}

/// The phase listeners are invoked for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum InvocationPhase {
    Capturing,
    Bubbling,
}

/// The callbacks of dispatching an event, as DOM/EventDispatcher.cpp registers them. Each takes the event and the
/// targets it acts on as DOM::Event and DOM::EventTarget pointers.
#[repr(C)]
pub struct EventHooks {
    /// See [`run_listener`].
    pub run_listener: unsafe extern "C" fn(
        event: RawEventRef,
        listener: RawEventListenerRef,
        invocation_target_in_shadow_tree: bool,
        legacy_output_did_listeners_throw: &mut bool,
    ),
    /// See [`run_event_listener_removal_steps`].
    pub run_event_listener_removal_steps:
        unsafe extern "C" fn(target: RawEventTargetRef, listener: RawEventListenerRef),
    /// See [`set_event_type`].
    pub set_event_type: unsafe extern "C" fn(event: RawEventRef, event_type: usize),
    /// See [`get_the_parent_of_object`].
    pub get_the_parent:
        unsafe extern "C" fn(target: RawEventTargetRef, event: RawEventRef) -> RawNullableEventTargetRef,
    /// See [`has_activation_behavior`].
    pub has_activation_behavior: unsafe extern "C" fn(target: RawEventTargetRef) -> bool,
    /// See [`is_disabled_form_control`].
    pub is_disabled_form_control: unsafe extern "C" fn(target: RawEventTargetRef) -> bool,
    /// See [`set_touch_target_list`].
    pub set_touch_target_list: unsafe extern "C" fn(event: RawEventRef, touch_targets: ak::Span<'_, EventTargetMember>),
    /// See [`set_target_of_coalesced_events`].
    pub set_target_of_coalesced_events: unsafe extern "C" fn(event: RawEventRef),
    /// See [`run_legacy_pre_activation_behavior`].
    pub run_legacy_pre_activation_behavior:
        unsafe extern "C" fn(activation_target: RawEventTargetRef, activation_event_target: RawEventTargetRef) -> bool,
    /// See [`activation_target_after_listeners`].
    pub activation_target_after_listeners:
        unsafe extern "C" fn(event: RawEventRef, activation_target: RawEventTargetRef) -> RawEventTargetRef,
    /// See [`run_activation_behavior`].
    pub run_activation_behavior: unsafe extern "C" fn(
        event: RawEventRef,
        activation_target: RawEventTargetRef,
        is_interactive_label_descendant: bool,
    ),
    /// See [`run_legacy_canceled_activation_behavior`].
    pub run_legacy_canceled_activation_behavior: unsafe extern "C" fn(activation_target: RawEventTargetRef),
}

/// The registered table, which C++ sets once, before any event exists, and never changes after that.
#[repr(transparent)]
struct RegisteredHooks(UnsafeCell<Option<&'static EventHooks>>);

// SAFETY: The table is set once, before any event exists, and only the main thread, which dispatches every event, reads
//         it.
unsafe impl Sync for RegisteredHooks {}

static HOOKS: RegisteredHooks = RegisteredHooks(UnsafeCell::new(None));

fn hooks(_main_thread: &mut MainThread<'_>) -> &'static EventHooks {
    // SAFETY: Nothing writes the table after the first event exists, see RegisteredHooks.
    unsafe { *HOOKS.0.get() }.expect("the event hooks are registered before any event exists")
}

/// Registers the callbacks of dispatching an event.
///
/// # Safety
///
/// No event may exist yet, and the call must be made on the main thread, before any other thread starts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_register_event_hooks(hooks: &'static EventHooks) {
    // SAFETY: Guaranteed by the caller.
    unsafe { *HOOKS.0.get() = Some(hooks) };
}

/// Steps 6 to 15 of inner invoke for `listener`, which C++ runs as they need state it keeps: works out the global
/// objects of the event's currentTarget and of the listener's callback, skips the listener if either is a Window whose
/// associated Document is not fully active, and otherwise calls the listener's callback with `event`, reporting what
/// it throws and setting `legacy_output_did_listeners_throw` if it does.
/// https://dom.spec.whatwg.org/#concept-event-listener-inner-invoke
///
/// Can run script, allocate and change the tree.
fn run_listener(
    main_thread: &mut MainThread<'_>,
    event: EventRef,
    listener: EventListenerRef,
    invocation_target_in_shadow_tree: bool,
    legacy_output_did_listeners_throw: &mut bool,
) {
    // SAFETY: The callback is called on the main thread with a live event, and a listener a clone of a list keeps alive.
    unsafe {
        (hooks(main_thread).run_listener)(
            event,
            listener,
            invocation_target_in_shadow_tree,
            legacy_output_did_listeners_throw,
        );
    }
}

/// The steps that follow from removing `listener` from `target`'s event listener list, which C++ owns: telling the
/// page and the target's class of the change.
///
/// Runs no script.
fn run_event_listener_removal_steps(
    main_thread: &mut MainThread<'_>,
    target: EventTargetRef,
    listener: EventListenerRef,
) {
    // SAFETY: The callback is called on the main thread with a live target and listener.
    unsafe { (hooks(main_thread).run_event_listener_removal_steps)(target, listener) };
}

/// Sets `event`'s type attribute value to the interned string whose raw word is `event_type`, which C++ keeps.
///
/// Runs no script.
fn set_event_type(main_thread: &mut MainThread<'_>, event: EventRef, event_type: usize) {
    // SAFETY: The callback is called on the main thread with a live event and a live interned string.
    unsafe { (hooks(main_thread).set_event_type)(event, event_type) };
}

/// The "get the parent" of `target`, an object that is no node, given `event`, which C++ answers for an object whose
/// class has one.
/// https://dom.spec.whatwg.org/#get-the-parent
///
/// Runs no script, and leaves the tree as it is.
fn get_the_parent_of_object(
    main_thread: &mut MainThread<'_>,
    target: EventTargetRef,
    event: EventRef,
) -> Option<EventTargetRef> {
    // SAFETY: The callback is called on the main thread with a live target and event.
    unsafe { (hooks(main_thread).get_the_parent)(target, event) }.into()
}

/// Whether `target`, a node whose class can have activation behavior, has it.
/// https://dom.spec.whatwg.org/#eventtarget-activation-behavior
///
/// Runs no script, and leaves the tree as it is.
fn has_activation_behavior(main_thread: &mut MainThread<'_>, target: EventTargetRef) -> bool {
    // SAFETY: The callback is called on the main thread with a live target.
    unsafe { (hooks(main_thread).has_activation_behavior)(target) }
}

/// Whether `target`, an element, is a disabled form control.
/// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fe-disabled
///
/// Runs no script, and leaves the tree as it is.
fn is_disabled_form_control(main_thread: &mut MainThread<'_>, target: EventTargetRef) -> bool {
    // SAFETY: The callback is called on the main thread with a live target.
    unsafe { (hooks(main_thread).is_disabled_form_control)(target) }
}

/// Sets `event`'s touch target list to `touch_targets`, and has a TouchEvent's touch lists follow it.
/// https://dom.spec.whatwg.org/#event-touch-target-list
///
/// Allocates. Runs no script, and leaves the tree as it is.
fn set_touch_target_list(main_thread: &mut MainThread<'_>, event: EventRef, touch_targets: &[EventTargetMember]) {
    // SAFETY: The callback is called on the main thread with a live event, and targets the event's path keeps alive.
    unsafe { (hooks(main_thread).set_touch_target_list)(event, ak::Span::new(touch_targets)) }
}

/// Gives the coalesced events of `event`, a trusted PointerEvent, its target.
///
/// Runs no script, and leaves the tree as it is.
fn set_target_of_coalesced_events(main_thread: &mut MainThread<'_>, event: EventRef) {
    // SAFETY: The callback is called on the main thread with a live event.
    unsafe { (hooks(main_thread).set_target_of_coalesced_events)(event) }
}

/// Step 6.12 of dispatching `event`: runs `activation_target`'s legacy-pre-activation behavior, if it has one. Before
/// that, answers whether the clicked target, `activation_event_target`, is an interactive descendant of an activation
/// target that is a label, which the label's activation behavior needs after listeners may have moved it.
///
/// Can run script, allocate and change the tree.
fn run_legacy_pre_activation_behavior(
    main_thread: &mut MainThread<'_>,
    activation_target: EventTargetRef,
    activation_event_target: EventTargetRef,
) -> bool {
    // SAFETY: The callback is called on the main thread with live targets.
    unsafe { (hooks(main_thread).run_legacy_pre_activation_behavior)(activation_target, activation_event_target) }
}

/// The activation target of `event` once its listeners have run, for `activation_target`: a link that contains a
/// button that neither submits nor resets a form as the activation target is, as other browsers have it.
///
/// Runs no script, and leaves the tree as it is.
fn activation_target_after_listeners(
    main_thread: &mut MainThread<'_>,
    event: EventRef,
    activation_target: EventTargetRef,
) -> EventTargetRef {
    // SAFETY: The callback is called on the main thread with a live event and target.
    unsafe { (hooks(main_thread).activation_target_after_listeners)(event, activation_target) }
}

/// Step 12.1 of dispatching `event`: runs `activation_target`'s activation behavior with `event`.
///
/// Can run script, allocate and change the tree.
fn run_activation_behavior(
    main_thread: &mut MainThread<'_>,
    event: EventRef,
    activation_target: EventTargetRef,
    is_interactive_label_descendant: bool,
) {
    // SAFETY: The callback is called on the main thread with a live event and target.
    unsafe {
        (hooks(main_thread).run_activation_behavior)(event, activation_target, is_interactive_label_descendant);
    }
}

/// Step 12.2 of dispatching an event: runs `activation_target`'s legacy-canceled-activation behavior, if it has one.
///
/// Can run script, allocate and change the tree.
fn run_legacy_canceled_activation_behavior(main_thread: &mut MainThread<'_>, activation_target: EventTargetRef) {
    // SAFETY: The callback is called on the main thread with a live target.
    unsafe { (hooks(main_thread).run_legacy_canceled_activation_behavior)(activation_target) }
}

/// What dispatch reads of the event that does not change while its path is built.
#[derive(Clone, Copy)]
struct PathFacts {
    composed: bool,
    type_is_load: bool,
}

/// https://dom.spec.whatwg.org/#get-the-parent
///
/// `first_invocation_target` is the invocation target of the first item of the event's path.
fn get_the_parent(
    main_thread: &mut MainThread<'_>,
    event: EventRef,
    facts: PathFacts,
    target: EventTargetRef,
    first_invocation_target: EventTargetRef,
) -> Option<EventTargetRef> {
    let Some(node) = target.as_node() else {
        // NB: Only an object whose class has a "get the parent" of its own can answer anything but null.
        if target.core().flags() & EVENT_TARGET_HAS_GET_THE_PARENT == 0 {
            return None;
        }
        return get_the_parent_of_object(main_thread, target, event);
    };

    if node.node_type() == NodeType::DOCUMENT_NODE {
        // A document's get the parent algorithm, given an event, returns null if event's type attribute value is
        // "load" or document does not have a browsing context; otherwise the document's relevant global object.
        // NB: The document's Window is its relevant global object, and it has none without a browsing context.
        if facts.type_is_load {
            return None;
        }
        return node.window();
    }

    if node.is_shadow_root() {
        // A shadow root's get the parent algorithm, given an event, returns null if event's composed flag is unset and
        // shadow root is the root of event's path's first event path item's invocation target; otherwise shadow root's
        // host.
        if !facts.composed
            && first_invocation_target
                .as_node()
                .is_some_and(|first| first.root() == node)
        {
            return None;
        }
        return node.host().map(EventTargetRef::from);
    }

    // A node's get the parent algorithm, given an event, returns the node's assigned slot, if node is assigned;
    // otherwise node's parent.
    node.assigned_slot().or_else(|| node.parent()).map(EventTargetRef::from)
}

/// Whether `root`, the root of a tree, is a shadow-including inclusive ancestor of `node`.
/// https://dom.spec.whatwg.org/#concept-shadow-including-inclusive-ancestor
fn root_is_shadow_including_inclusive_ancestor_of(root: NodeRef, node: NodeRef) -> bool {
    // NB: A root is an inclusive ancestor of exactly the nodes of its tree. Beyond that, the ancestors of a shadow root's
    //     host are its shadow-including ancestors.
    let mut node_root = node.root();
    loop {
        if node_root == root {
            return true;
        }
        if !node_root.is_shadow_root() {
            return false;
        }
        let Some(host) = node_root.host() else {
            return false;
        };
        node_root = host.root();
    }
}

/// https://dom.spec.whatwg.org/#retarget
fn retarget(a: Option<EventTargetRef>, b: EventTargetRef) -> Option<EventTargetRef> {
    let mut a = a?;
    // To retarget an object A against an object B, repeat these steps until they return an object:
    loop {
        // 1. If one of the following is true
        //    - A is not a node
        //    - A's root is not a shadow root
        //    - B is a node and A's root is a shadow-including inclusive ancestor of B
        //    then return A.
        let Some(a_node) = a.as_node() else {
            return Some(a);
        };
        let a_root = a_node.root();
        if !a_root.is_shadow_root() {
            return Some(a);
        }
        if b.as_node()
            .is_some_and(|b_node| root_is_shadow_including_inclusive_ancestor_of(a_root, b_node))
        {
            return Some(a);
        }

        // 2. Set A to A's root's host.
        a = EventTargetRef::from(a_root.host()?);
    }
}

/// Whether `target` is a node whose root is a shadow root.
fn is_node_in_shadow_tree(target: Option<EventTargetRef>) -> bool {
    target
        .and_then(EventTargetRef::as_node)
        .is_some_and(|node| node.root().is_shadow_root())
}

/// https://dom.spec.whatwg.org/#concept-event-path-append
fn append_to_an_event_path(
    event: EventRef,
    invocation_target: EventTargetRef,
    shadow_adjusted_target: Option<EventTargetRef>,
    related_target: Option<EventTargetRef>,
    touch_targets: impl ExactSizeIterator<Item = Option<EventTargetRef>>,
    slot_in_closed_tree: bool,
) {
    // 1. Let invocationTargetInShadowTree be false.
    // 2. If invocationTarget is a node and its root is a shadow root, then set invocationTargetInShadowTree to true.
    let invocation_target_in_shadow_tree = is_node_in_shadow_tree(Some(invocation_target));

    // 3. Let rootOfClosedTree be false.
    // 4. If invocationTarget is a shadow root whose mode is "closed", then set rootOfClosedTree to true.
    let root_of_closed_tree = invocation_target.as_node().is_some_and(NodeRef::is_closed_shadow_root);

    // 5. Append a new event path item to event's path whose invocation target is invocationTarget,
    //    invocation-target-in-shadow-tree is invocationTargetInShadowTree, shadow-adjusted target is
    //    shadowAdjustedTarget, relatedTarget is relatedTarget, touch target list is touchTargets, root-of-closed-tree is
    //    rootOfClosedTree, and slot-in-closed-tree is slotInClosedTree.
    event.core().path().append(
        EventPathItem::new(
            invocation_target,
            shadow_adjusted_target,
            related_target,
            invocation_target_in_shadow_tree,
            root_of_closed_tree,
            slot_in_closed_tree,
        ),
        touch_targets,
    );
}

/// Whether `target` has activation behavior, asking C++ only for a node whose class can have it.
fn target_has_activation_behavior(main_thread: &mut MainThread<'_>, target: EventTargetRef) -> bool {
    target.class_id().steps() & NODE_CLASS_HAS_ACTIVATION_BEHAVIOR != 0 && has_activation_behavior(main_thread, target)
}

/// Whether `target` is an element that is a disabled form control.
fn target_is_disabled_form_control(main_thread: &mut MainThread<'_>, target: EventTargetRef) -> bool {
    target.as_node().is_some_and(NodeRef::is_element) && is_disabled_form_control(main_thread, target)
}

/// Dispatches `event` to `target`, with `target_override`, which is `target` unless the legacy target override flag is
/// given. `touch_target_list` is the event's touch target list, which C++ replaces as listeners run, so it is read only
/// before any of them does. `types` are the event's type and legacy type, and their bits in the listener type
/// summaries of the targets' cores, and `flags` are DISPATCH_* flags. Returns the event's target as its listeners last
/// saw it, before dispatch may clear it.
/// https://dom.spec.whatwg.org/#concept-event-dispatch
#[allow(clippy::too_many_arguments)]
pub(crate) fn dispatch(
    main_thread: &mut MainThread<'_>,
    event: EventRef,
    mut target: EventTargetRef,
    target_override: EventTargetRef,
    touch_target_list: *const [Option<EventTargetRef>],
    types: EventTypes,
    flags: u8,
    legacy_output_did_listeners_throw: &mut bool,
) -> EventTargetRef {
    // 1. Set event's dispatch flag.
    event.core().set_dispatch(true);

    // 2. Let targetOverride be target, if legacy target override flag is not given, and target's associated Document
    //    otherwise.
    // NB: C++ works out targetOverride.

    // 3. Let activationTarget be null.
    let mut activation_target: Option<EventTargetRef> = None;
    // NB: Labels need the click target relative to their tree, even after dispatch retargets or clears event.target.
    let mut activation_event_target = target;
    let mut is_interactive_label_descendant = false;

    // 4. Let relatedTarget be the result of retargeting event's relatedTarget against target.
    // SAFETY: The caller keeps the event alive, and so its targets.
    let event_related_target = unsafe { event.core().related_target() };
    let mut related_target = retarget(event_related_target, target);

    // 5. Let clearTargets be false.
    let mut clear_targets = false;

    let facts = PathFacts {
        composed: event.core().composed(),
        type_is_load: flags & DISPATCH_TYPE_IS_LOAD != 0,
    };
    let is_trusted = event.core().is_trusted();
    let had_touch_targets = touch_target_list.len() != 0;
    let is_activation_event = flags & DISPATCH_IS_ACTIVATION_EVENT != 0;
    let path_ends_below_disabled_form_control = flags & DISPATCH_PATH_ENDS_BELOW_DISABLED_FORM_CONTROL != 0;

    // OPTIMIZATION: Only dispatch events if there is at least one listener on the node or its ancestors. Activation
    //               events are always dispatched. This saves us from going through the path building and dispatch
    //               phases for events that will be dropped on the floor anyway.
    let mut should_dispatch = is_activation_event;
    if !should_dispatch {
        match target.as_node() {
            None => should_dispatch = true,
            Some(node) => {
                should_dispatch = has_listener_on_event_path(main_thread, event, facts, target, types.bits);

                // NB: The script that dispatched an untrusted event can read the target that dispatch leaves on it.
                //     For a node outside any shadow tree, with no related target or touch targets, that's
                //     targetOverride itself, so set it, and skip the dispatch. Anywhere else, retargeting decides
                //     what's left, so dispatch.
                if !should_dispatch && !is_trusted {
                    if node.root().is_shadow_root() || event_related_target.is_some() || had_touch_targets {
                        should_dispatch = true;
                    } else {
                        event.core().set_target(Some(target_override));
                    }
                }
            }
        }
    }

    // AD-HOC: The spec only has a disabled form control prevent the click events queued on the user interaction task
    //         source from being dispatched on it:
    //         https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#enabling-and-disabling-form-controls:-the-disabled-attribute
    //         But no mousedown, mouseup, click, or dblclick from the user reaches a disabled form control, or anything
    //         above it: The path of such an event ends below the control, so a press on the control's own box
    //         dispatches nothing. Every other event (pointer events, mouseover, mousemove, wheel, ...) reaches it as
    //         usual. See https://github.com/whatwg/html/issues/2368.
    if path_ends_below_disabled_form_control && target_is_disabled_form_control(main_thread, target) {
        should_dispatch = false;
    }

    // 6. If target is not relatedTarget or target is event's relatedTarget, then:
    if should_dispatch && (related_target != Some(target) || event_related_target == Some(target)) {
        // 1. Let touchTargets be a new list.
        // 2. For each touchTarget of event's touch target list, append the result of retargeting touchTarget against
        //    target to touchTargets.
        // SAFETY: The list is live and unchanged until a listener runs, after the last use of this view of it.
        let touch_target_list = unsafe { &*touch_target_list };
        event.core().path().begin(touch_target_list.len());
        let touch_targets_against = |against: EventTargetRef| {
            touch_target_list
                .iter()
                .map(move |&touch_target| retarget(touch_target, against))
        };

        // 3. Append to an event path with event, target, targetOverride, relatedTarget, touchTargets, and false.
        append_to_an_event_path(
            event,
            target,
            Some(target_override),
            related_target,
            touch_targets_against(target),
            false,
        );
        let first_invocation_target = target;

        // 4. Let isActivationEvent be true, if event is a MouseEvent object and event's type attribute is "click";
        //    otherwise false.
        // NB: C++ tells whether it is, see DISPATCH_IS_ACTIVATION_EVENT.

        // 5. If isActivationEvent is true and target has activation behavior, then set activationTarget to target.
        if is_activation_event && target_has_activation_behavior(main_thread, target) {
            activation_target = Some(target);
            activation_event_target = target;
        }

        // 6. Let slottable be target, if target is a slottable and is assigned, and null otherwise.
        let mut slottable = target.as_node().is_some_and(|node| node.assigned_slot().is_some());

        // 7. Let slotInClosedTree be false.
        let mut slot_in_closed_tree = false;

        // 8. Let parent be the result of invoking target's get the parent with event.
        let mut parent = get_the_parent(main_thread, event, facts, target, first_invocation_target);

        // 9. While parent is non-null:
        while let Some(current_parent) = parent {
            if path_ends_below_disabled_form_control && target_is_disabled_form_control(main_thread, current_parent) {
                break;
            }

            // 1. If slottable is non-null:
            if slottable {
                // 1. Assert: parent is a slot.
                // NB: An assigned slottable's get the parent is its assigned slot.

                // 2. Set slottable to null.
                slottable = false;

                // 3. If parent's root is a shadow root whose mode is "closed", then set slotInClosedTree to true.
                if current_parent
                    .as_node()
                    .is_some_and(|slot| slot.root().is_closed_shadow_root())
                {
                    slot_in_closed_tree = true;
                }
            }

            // 2. If parent is a slottable and is assigned, then set slottable to parent.
            if current_parent
                .as_node()
                .is_some_and(|node| node.assigned_slot().is_some())
            {
                slottable = true;
            }

            // 3. Let relatedTarget be the result of retargeting event's relatedTarget against parent.
            related_target = retarget(event_related_target, current_parent);

            // 4. Let touchTargets be a new list.
            // 5. For each touchTarget of event's touch target list, append the result of retargeting touchTarget
            //    against parent to touchTargets.
            let touch_targets = touch_targets_against(current_parent);

            // 6. If parent is a Window object, or parent is a node and target's root is a shadow-including inclusive
            //    ancestor of parent, then:
            // INTEROP: A parent that is neither a Window object nor a node, such as the transaction and the connection
            //          an IndexedDB request propagates its events to, is handled like a Window object too. This keeps
            //          the event's target as it propagates, as other browsers do.
            let parent_is_in_scope = match (current_parent.as_node(), target.as_node()) {
                (Some(parent_node), Some(target_node)) => {
                    root_is_shadow_including_inclusive_ancestor_of(target_node.root(), parent_node)
                }
                _ => true,
            };
            if parent_is_in_scope {
                // 1. If isActivationEvent is true, event's bubbles attribute is true, activationTarget is null, and
                //    parent has activation behavior, then set activationTarget to parent.
                if is_activation_event
                    && event.core().bubbles()
                    && activation_target.is_none()
                    && target_has_activation_behavior(main_thread, current_parent)
                {
                    activation_target = Some(current_parent);
                    activation_event_target = target;
                }

                // 2. Append to an event path with event, parent, null, relatedTarget, touchTargets, and
                //    slotInClosedTree.
                append_to_an_event_path(
                    event,
                    current_parent,
                    None,
                    related_target,
                    touch_targets,
                    slot_in_closed_tree,
                );
            }
            // 7. Otherwise, if parent is relatedTarget, then set parent to null.
            else if related_target == Some(current_parent) {
                parent = None;
            }
            // 8. Otherwise:
            else {
                // 1. Set target to parent.
                target = current_parent;

                // 2. If isActivationEvent is true, activationTarget is null, and target has activation behavior, then
                //    set activationTarget to target.
                if is_activation_event
                    && activation_target.is_none()
                    && target_has_activation_behavior(main_thread, target)
                {
                    activation_target = Some(target);
                    activation_event_target = target;
                }

                // 3. Append to an event path with event, parent, target, relatedTarget, touchTargets, and
                //    slotInClosedTree.
                append_to_an_event_path(
                    event,
                    current_parent,
                    Some(target),
                    related_target,
                    touch_targets,
                    slot_in_closed_tree,
                );
            }

            // 9. If parent is non-null, then set parent to the result of invoking parent's get the parent with event.
            if let Some(current_parent) = parent {
                parent = get_the_parent(main_thread, event, facts, current_parent, first_invocation_target);
            }

            // 10. Set slotInClosedTree to false.
            slot_in_closed_tree = false;
        }

        // 10. Let clearTargetsItem be the last event path item in event's path whose shadow-adjusted target is
        //     non-null.
        // 11. If clearTargetsItem's shadow-adjusted target, clearTargetsItem's relatedTarget, or an EventTarget object
        //     in clearTargetsItem's touch target list is a node whose root is a shadow root: set clearTargets to true.
        {
            let path = event.core().path();
            let (clear_targets_index, clear_targets_item) = path
                .items()
                .iter()
                .enumerate()
                .rev()
                .find(|(_, item)| item.has_shadow_adjusted_target())
                .expect("the first event path item has a shadow-adjusted target");
            // SAFETY: The event's path keeps the targets of its items alive.
            unsafe {
                clear_targets = is_node_in_shadow_tree(clear_targets_item.shadow_adjusted_target())
                    || is_node_in_shadow_tree(clear_targets_item.related_target())
                    || path
                        .touch_targets(clear_targets_index)
                        .iter()
                        .any(|touch_target| is_node_in_shadow_tree(EventTargetRef::from_member(touch_target)));
            }
        }

        // 12. If activationTarget is non-null and activationTarget has legacy-pre-activation behavior, then run
        //     activationTarget's legacy-pre-activation behavior.
        if let Some(activation_target) = activation_target {
            is_interactive_label_descendant =
                run_legacy_pre_activation_behavior(main_thread, activation_target, activation_event_target);
        }

        // 13. For each item of event's path, in reverse order:
        // NB: The path holds its items for as long as the dispatch takes, and only the dispatch writes them, but each is
        //     read anew after listeners ran, as is the event's state.
        // NB: The target item of an item is the last item at or before it whose shadow-adjusted target is non-null,
        //     which is never after the target item of a later item, so the loops find each by looking at every item
        //     at most once.
        let mut item_index = event.core().path().items().len();
        let mut target_item_index: Option<usize> = None;
        while item_index > 0 {
            item_index -= 1;
            let Some(item) = event.core().path().item(item_index) else {
                break;
            };
            // 1. If item's shadow-adjusted target is non-null, then set event's eventPhase attribute to AT_TARGET.
            // 2. Otherwise, set event's eventPhase attribute to CAPTURING_PHASE.
            event.core().set_event_phase(match item.has_shadow_adjusted_target() {
                true => EVENT_PHASE_AT_TARGET,
                false => EVENT_PHASE_CAPTURING,
            });

            // 3. Invoke with item, event, "capturing", and legacyOutputDidListenersThrowFlag if given.
            if target_item_index.is_none_or(|index| index > item_index) {
                let items = event.core().path().items();
                target_item_index = (0..=item_index)
                    .rev()
                    .find(|&index| items[index].has_shadow_adjusted_target());
            }
            let target_item = target_item_index.expect("the first event path item has a shadow-adjusted target");
            invoke(
                main_thread,
                event,
                item_index,
                target_item,
                InvocationPhase::Capturing,
                types,
                flags,
                legacy_output_did_listeners_throw,
            );
        }

        // 14. For each item of event's path:
        let mut item_index = 0;
        let mut target_item = 0;
        while let Some(item) = event.core().path().item(item_index) {
            // 1. If item's shadow-adjusted target is non-null, then set event's eventPhase attribute to AT_TARGET.
            if item.has_shadow_adjusted_target() {
                target_item = item_index;
                event.core().set_event_phase(EVENT_PHASE_AT_TARGET);
            }
            // 2. Otherwise:
            else {
                // 1. If event's bubbles attribute is false, then continue.
                if !event.core().bubbles() {
                    item_index += 1;
                    continue;
                }

                // 2. Set event's eventPhase attribute to BUBBLING_PHASE.
                event.core().set_event_phase(EVENT_PHASE_BUBBLING);
            }

            // 3. Invoke with item, event, "bubbling", and legacyOutputDidListenersThrowFlag if given.
            invoke(
                main_thread,
                event,
                item_index,
                target_item,
                InvocationPhase::Bubbling,
                types,
                flags,
                legacy_output_did_listeners_throw,
            );
            item_index += 1;
        }
    }

    // INTEROP: Buttons inside links that neither submit nor reset a form allow the link's default action. Check after
    //          event listeners have run, since a click listener can give the button a submit, reset, command, or
    //          popover action.
    if let Some(current_activation_target) = activation_target {
        activation_target = Some(activation_target_after_listeners(
            main_thread,
            event,
            current_activation_target,
        ));
    }

    // NB: The last target of the event, which finalizing event timing uses, is cleared by the next steps.
    // SAFETY: The caller keeps the event alive, and so its targets.
    let last_target = unsafe { event.core().target() }.unwrap_or(target);

    // 7. Set event's eventPhase attribute to NONE.
    event.core().set_event_phase(EVENT_PHASE_NONE);

    // 8. Set event's currentTarget attribute to null.
    event.core().set_current_target(None);

    // 9. Set event's path to the empty list.
    event.core().path().clear();

    // 10. Unset event's dispatch flag, stop propagation flag, and stop immediate propagation flag.
    event.core().set_dispatch(false);
    event.core().unset_propagation_flags();

    // 11. If clearTargets is true:
    if clear_targets {
        // 1. Set event's target to null.
        event.core().set_target(None);

        // 2. Set event's relatedTarget to null.
        event.core().set_related_target(None);

        // 3. Set event's touch target list to the empty list.
        if had_touch_targets {
            set_touch_target_list(main_thread, event, &[]);
        }
    }

    // AD-HOC: The coalesced events of a trusted pointer event keep its final target, also when script dispatches the
    //         event again.
    if flags & DISPATCH_IS_TRUSTED_POINTER_EVENT != 0 {
        set_target_of_coalesced_events(main_thread, event);
    }

    // 12. If activationTarget is non-null:
    if let Some(activation_target) = activation_target {
        // 1. If event's canceled flag is unset, then run activationTarget's activation behavior with event.
        if !event.core().canceled() {
            run_activation_behavior(main_thread, event, activation_target, is_interactive_label_descendant);
        }
        // 2. Otherwise, if activationTarget has legacy-canceled-activation behavior, then run activationTarget's
        //    legacy-canceled-activation behavior.
        else {
            run_legacy_canceled_activation_behavior(main_thread, activation_target);
        }
    }

    // 13. Return false if event's canceled flag is set; otherwise true.
    // NB: C++ reads the canceled flag.
    last_target
}

/// Whether a target on the path `event` takes from `target` may have a listener for the event, see
/// DOM::EventDispatcher::dispatch(). The path is not built yet.
fn has_listener_on_event_path(
    main_thread: &mut MainThread<'_>,
    event: EventRef,
    facts: PathFacts,
    target: EventTargetRef,
    type_bits: u16,
) -> bool {
    let mut ancestor = Some(target);
    while let Some(current) = ancestor {
        if current.core().may_have_any_listeners(type_bits) {
            return true;
        }
        ancestor = get_the_parent(main_thread, event, facts, current, target);
    }
    false
}

/// https://dom.spec.whatwg.org/#concept-event-listener-invoke
#[allow(clippy::too_many_arguments)]
fn invoke(
    main_thread: &mut MainThread<'_>,
    event: EventRef,
    item_index: usize,
    target_item_index: usize,
    phase: InvocationPhase,
    types: EventTypes,
    flags: u8,
    legacy_output_did_listeners_throw: &mut bool,
) {
    let core = event.core();
    let path = core.path();
    let item = &path.items()[item_index];

    // 1. Let targetItem be pathItem.
    // 2. While targetItem's shadow-adjusted target is null: set targetItem to the event path item preceding
    //    targetItem in event's path.
    let target_item = &path.items()[target_item_index];

    // 3. Set event's target to targetItem's shadow-adjusted target.
    // SAFETY: The event's path keeps the targets of its items alive.
    let target = unsafe { target_item.shadow_adjusted_target() };
    // AD-HOC: Coalesced events have the same target as the trusted pointer event they were coalesced into.
    // SAFETY: The caller keeps the event alive, and so its targets.
    let target_changed = unsafe { core.target() } != target;
    core.set_target(target);

    // 4. Set event's relatedTarget to pathItem's relatedTarget.
    // SAFETY: The event's path keeps the targets of its items alive.
    core.set_related_target(unsafe { item.related_target() });

    // 5. Set event's touch target list to pathItem's touch target list.
    // OPTIMIZATION: Every path item has the same touch target count. Ordinary events keep their empty list.
    let has_touch_targets = path.touch_target_count() != 0;

    // 6. If event's stop propagation flag is set, then return.
    let stop_propagation = core.stop_propagation();

    // 7. Initialize event's currentTarget attribute to pathItem's invocation target.
    // SAFETY: The event's path keeps the targets of its items alive.
    let invocation_target = unsafe { item.invocation_target() };
    let invocation_target_in_shadow_tree = item.invocation_target_in_shadow_tree();

    if target_changed && flags & DISPATCH_IS_TRUSTED_POINTER_EVENT != 0 {
        set_target_of_coalesced_events(main_thread, event);
    }
    if has_touch_targets {
        let path = event.core().path();
        set_touch_target_list(main_thread, event, path.touch_targets(item_index));
    }
    if stop_propagation {
        return;
    }
    event.core().set_current_target(Some(invocation_target));

    // OPTIMIZATION: Inner invoke runs no listener of another type, so a target whose core says it has no listener of the
    //               event's type, or of its legacy type, for this phase has nothing to run.
    if !invocation_target
        .core()
        .may_have_listeners(types.bits, phase == InvocationPhase::Capturing)
    {
        return;
    }

    // 8. Let listeners be a clone of event's currentTarget attribute value's event listener list.
    // NOTE: This avoids event listeners added after this point from being run. Note that removal still has an effect
    //       due to the removed field.
    // OPTIMIZATION: Inner invoke runs no listener registered for another type, so leave those out of the clone. If the
    //               event is trusted, step 11 renames the event and runs inner invoke again, so the clone includes the
    //               listeners of the legacy event type as well.
    with_cloned_listeners(
        invocation_target,
        types.event_type,
        types.legacy_event_type,
        None,
        0,
        &mut |listeners| {
            // 9. Let invocationTargetInShadowTree be pathItem's invocation-target-in-shadow-tree.
            // 10. Let found be the result of running inner invoke with event, listeners, phase,
            //     invocationTargetInShadowTree, and legacyOutputDidListenersThrowFlag if given.
            let found = inner_invoke(
                main_thread,
                event,
                invocation_target,
                listeners,
                types.event_type,
                phase,
                invocation_target_in_shadow_tree,
                legacy_output_did_listeners_throw,
            );

            // 11. If found is false and event's isTrusted attribute is true:
            if !found && event.core().is_trusted() {
                // 1. Let originalEventType be event's type attribute value.
                let original_event_type = types.event_type;

                // 2. If event's type attribute value is a match for any of the strings in the first column in the
                //    following table, set event's type attribute value to the string in the second column on the same
                //    row as the matching string, and return otherwise.
                if types.legacy_event_type == 0 {
                    return;
                }
                set_event_type(main_thread, event, types.legacy_event_type);

                // 3. Inner invoke with event, listeners, phase, invocationTargetInShadowTree, and
                //    legacyOutputDidListenersThrowFlag if given.
                inner_invoke(
                    main_thread,
                    event,
                    invocation_target,
                    listeners,
                    types.legacy_event_type,
                    phase,
                    invocation_target_in_shadow_tree,
                    legacy_output_did_listeners_throw,
                );

                // 4. Set event's type attribute value to originalEventType.
                set_event_type(main_thread, event, original_event_type);
            }
        },
    );
}

/// Inner invoke with `event`, whose type attribute value is the interned string whose raw word is `event_type` and
/// whose currentTarget is `current_target`, `listeners`, `phase`, `invocation_target_in_shadow_tree`, and
/// `legacy_output_did_listeners_throw`. Answers found.
/// https://dom.spec.whatwg.org/#concept-event-listener-inner-invoke
#[allow(clippy::too_many_arguments)]
fn inner_invoke(
    main_thread: &mut MainThread<'_>,
    event: EventRef,
    current_target: EventTargetRef,
    listeners: &ClonedListeners<'_>,
    event_type: usize,
    phase: InvocationPhase,
    invocation_target_in_shadow_tree: bool,
    legacy_output_did_listeners_throw: &mut bool,
) -> bool {
    // 1. Let found be false.
    let mut found = false;

    // 2. For each listener of listeners, whose removed is false:
    let _ = listeners.try_for_each(&mut |listener| {
        let fields = listener.fields();
        if fields.removed() {
            return ControlFlow::Continue(());
        }

        // 1. If event's type attribute value is not listener's type, then continue.
        if fields.type_raw() != event_type {
            return ControlFlow::Continue(());
        }

        // 2. Set found to true.
        found = true;

        // 3. If phase is "capturing" and listener's capture is false, then continue.
        if phase == InvocationPhase::Capturing && !fields.capture() {
            return ControlFlow::Continue(());
        }

        // 4. If phase is "bubbling" and listener's capture is true, then continue.
        if phase == InvocationPhase::Bubbling && fields.capture() {
            return ControlFlow::Continue(());
        }

        // 5. If listener's once is true, then remove an event listener given event's currentTarget attribute value and
        //    listener.
        if fields.once() && remove_an_event_listener(current_target, listener) {
            run_event_listener_removal_steps(main_thread, current_target, listener);
        }

        // 6. Let targetGlobal be event's currentTarget's node document's relevant global object if event's
        //    currentTarget is a node; otherwise event's currentTarget's relevant global object.
        // ...
        // 15. If listenerGlobal is a Window object, then set listenerGlobal's current event to currentEvent.
        run_listener(
            main_thread,
            event,
            listener,
            invocation_target_in_shadow_tree,
            legacy_output_did_listeners_throw,
        );

        // 16. If event's stop immediate propagation flag is set, then break.
        if event.core().stop_immediate_propagation() {
            return ControlFlow::Break(());
        }
        ControlFlow::Continue(())
    });

    // 3. Return found.
    found
}

/// Dispatches `event` to `target`, for DOM::EventDispatcher, see [`dispatch`].
///
/// # Safety
///
/// The event and the targets must be live, which the caller keeps them for as long as the call takes, and the call
/// must be made on the main thread.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rust_dom_dispatch(
    event: &EventCell,
    target: &EventTargetCell,
    target_override: &EventTargetCell,
    touch_target_list: ak::Span<'_, NullableEventTargetRef>,
    type_bits: u16,
    event_type: usize,
    legacy_event_type: usize,
    flags: u8,
    legacy_output_did_listeners_throw: &mut bool,
) -> *const EventTargetCell {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The event and the targets are live.
    let (event, target, target_override) = unsafe {
        (
            EventRef::from_cell(event, &main_thread),
            EventTargetRef::from_cell(target, &main_thread),
            EventTargetRef::from_cell(target_override, &main_thread),
        )
    };
    // NB: A NullableEventTargetRef is laid out as an Option<EventTargetRef>. The list is read only while the path is
    //     built, before anything C++ could change it runs, so dispatch() gets no reference to it that outlives that.
    let touch_target_list = core::ptr::slice_from_raw_parts(
        touch_target_list.as_slice().as_ptr().cast::<Option<EventTargetRef>>(),
        touch_target_list.as_slice().len(),
    );
    dispatch(
        &mut main_thread,
        event,
        target,
        target_override,
        touch_target_list,
        EventTypes {
            bits: type_bits,
            event_type,
            legacy_event_type,
        },
        flags,
        legacy_output_did_listeners_throw,
    )
    .as_ptr()
}

/// The "get the parent" of `target` given an event that is not being dispatched, whose path's first invocation target
/// would be `origin`, for the C++ code that walks the path an event would take before it dispatches it.
///
/// # Safety
///
/// The event and the targets must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_get_the_parent(
    event: &EventCell,
    target: &EventTargetCell,
    origin: &EventTargetCell,
    type_is_load: bool,
) -> *const EventTargetCell {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: The event and the targets are live.
    let (event, target, origin) = unsafe {
        (
            EventRef::from_cell(event, &main_thread),
            EventTargetRef::from_cell(target, &main_thread),
            EventTargetRef::from_cell(origin, &main_thread),
        )
    };
    let facts = PathFacts {
        composed: event.core().composed(),
        type_is_load,
    };
    get_the_parent(&mut main_thread, event, facts, target, origin).map_or(core::ptr::null(), EventTargetRef::as_ptr)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dom::event::EventCore;
    use crate::dom::test_tree;

    /// An event's cell, with its core where DOM::Event has it.
    #[repr(C, align(8))]
    struct TestEvent {
        header: [u8; crate::dom::event::EVENT_CORE_OFFSET],
        core: EventCore,
    }

    fn test_event(composed: bool) -> Box<TestEvent> {
        Box::new(TestEvent {
            header: [0; crate::dom::event::EVENT_CORE_OFFSET],
            core: EventCore::for_test(true, composed),
        })
    }

    fn event_ref(event: &TestEvent, main_thread: &MainThread<'_>) -> EventRef {
        // SAFETY: A test event starts with its cell, and outlives the reference.
        unsafe { EventRef::from_cell(&*(event as *const TestEvent as *const EventCell), main_thread) }
    }

    fn target(node: crate::dom::node::Node<'_>) -> EventTargetRef {
        EventTargetRef::from(NodeRef::from(node))
    }

    const FACTS: PathFacts = PathFacts {
        composed: false,
        type_is_load: false,
    };

    #[test]
    fn a_node_in_a_shadow_tree_is_retargeted_to_the_host_outside_it() {
        let main_thread = MainThread::for_test();
        let document_element = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        let host = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        let outside = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        test_tree::append_child(document_element, host);
        test_tree::append_child(document_element, outside);
        let shadow_root = test_tree::shadow_root(&main_thread, host);
        let inside = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        let inside_sibling = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        test_tree::append_child(shadow_root, inside);
        test_tree::append_child(shadow_root, inside_sibling);

        assert!(retarget(Some(target(inside)), target(outside)) == Some(target(host)));
        assert!(retarget(Some(target(inside)), target(inside_sibling)) == Some(target(inside)));
        assert!(retarget(Some(target(outside)), target(inside)) == Some(target(outside)));
        assert!(retarget(None, target(inside)).is_none());
    }

    #[test]
    fn a_root_is_a_shadow_including_ancestor_of_the_nodes_of_the_shadow_trees_below_it() {
        let main_thread = MainThread::for_test();
        let root = test_tree::node(&main_thread, NodeType::DOCUMENT_FRAGMENT_NODE);
        let host = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        test_tree::append_child(root, host);
        let shadow_root = test_tree::shadow_root(&main_thread, host);
        let inner_host = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        test_tree::append_child(shadow_root, inner_host);
        let inner_shadow_root = test_tree::closed_shadow_root(&main_thread, inner_host);
        let deepest = test_tree::node(&main_thread, NodeType::TEXT_NODE);
        test_tree::append_child(inner_shadow_root, deepest);

        let (root, shadow_root, deepest) = (NodeRef::from(root), NodeRef::from(shadow_root), NodeRef::from(deepest));
        assert!(root_is_shadow_including_inclusive_ancestor_of(root, deepest));
        assert!(root_is_shadow_including_inclusive_ancestor_of(shadow_root, deepest));
        assert!(!root_is_shadow_including_inclusive_ancestor_of(shadow_root, root));
        assert!(NodeRef::from(inner_shadow_root).is_closed_shadow_root());
        assert!(!shadow_root.is_closed_shadow_root());
    }

    #[test]
    fn the_parent_of_an_assigned_slottable_is_its_slot() {
        let main_thread = MainThread::for_test();
        let host = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        let text = test_tree::node(&main_thread, NodeType::TEXT_NODE);
        let child = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        test_tree::append_child(host, text);
        test_tree::append_child(host, child);
        let shadow_root = test_tree::shadow_root(&main_thread, host);
        let slot = test_tree::node(&main_thread, NodeType::ELEMENT_NODE);
        test_tree::append_child(shadow_root, slot);
        test_tree::set_assigned_slot(text, slot);
        test_tree::set_assigned_slot(child, slot);

        let event = test_event(false);
        let (host, text, child, shadow_root, slot) = (
            target(host),
            target(text),
            target(child),
            target(shadow_root),
            target(slot),
        );
        let mut main_thread = main_thread;
        let event = event_ref(&event, &main_thread);
        assert!(get_the_parent(&mut main_thread, event, FACTS, text, text) == Some(slot));
        assert!(get_the_parent(&mut main_thread, event, FACTS, child, child) == Some(slot));
        assert!(get_the_parent(&mut main_thread, event, FACTS, slot, text) == Some(shadow_root));
        // A shadow root is the end of the path of an event that is not composed and was dispatched inside it.
        assert!(get_the_parent(&mut main_thread, event, FACTS, shadow_root, slot).is_none());
        assert!(get_the_parent(&mut main_thread, event, FACTS, shadow_root, text) == Some(host));
        let composed = PathFacts {
            composed: true,
            ..FACTS
        };
        assert!(get_the_parent(&mut main_thread, event, composed, shadow_root, slot) == Some(host));
    }

    #[test]
    fn a_document_without_a_window_is_the_end_of_an_event_path() {
        let main_thread = MainThread::for_test();
        let document = test_tree::node(&main_thread, NodeType::DOCUMENT_NODE);
        let event = test_event(false);
        let document = target(document);
        let mut main_thread = main_thread;
        let event = event_ref(&event, &main_thread);
        assert!(get_the_parent(&mut main_thread, event, FACTS, document, document).is_none());
    }

    #[test]
    fn an_event_path_holds_its_items_and_their_touch_targets_until_it_is_emptied() {
        let main_thread = MainThread::for_test();
        let nodes: Vec<EventTargetRef> = (0..40)
            .map(|_| target(test_tree::node(&main_thread, NodeType::ELEMENT_NODE)))
            .collect();
        let event = test_event(false);
        let event = event_ref(&event, &main_thread);
        let path = event.core().path();
        path.begin(2);
        for (index, &node) in nodes.iter().enumerate() {
            let shadow_adjusted_target = (index == 0).then_some(node);
            let touch_targets = [Some(node), None];
            append_to_an_event_path(
                event,
                node,
                shadow_adjusted_target,
                None,
                touch_targets.into_iter(),
                false,
            );
        }

        assert_eq!(path.items().len(), nodes.len());
        for (index, &node) in nodes.iter().enumerate() {
            let item = path.item(index).unwrap();
            // SAFETY: The test keeps every node alive.
            unsafe {
                assert!(item.invocation_target() == node);
                assert_eq!(item.has_shadow_adjusted_target(), index == 0);
                assert!(item.related_target().is_none());
                let touch_targets = path.touch_targets(index);
                assert_eq!(touch_targets.len(), 2);
                assert!(EventTargetRef::from_member(&touch_targets[0]) == Some(node));
                assert!(EventTargetRef::from_member(&touch_targets[1]).is_none());
            }
            assert!(!item.invocation_target_in_shadow_tree());
        }

        path.clear();
        assert!(path.items().is_empty());
        assert!(path.touch_targets(0).is_empty());
    }
}
