/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::RefCell;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, GcWeak, MarkedVec, Vm};

use super::canvas::Canvas;
use super::exports::create_remote_context;
use super::state::{ContextRef, RenderingContext, WebGL1, WebGL2};
use super::web_gl_context_event::{WebGLContextEvent, WebGLContextEventInit};
use crate::bindings::interface::{Wrapped, with_temporary_execution_context_and_callbacks};
use crate::bindings::promise::{TaskSource, queue_a_task};
use crate::dom::event::EventInit;

#[derive(Clone)]
enum WeakContext {
    WebGL1(GcWeak<EmbedderCell<Wrapped<RenderingContext<WebGL1>>>>),
    WebGL2(GcWeak<EmbedderCell<Wrapped<RenderingContext<WebGL2>>>>),
}

impl WeakContext {
    fn get(&self) -> Option<ContextRef> {
        match self {
            Self::WebGL1(context) => context.get().map(ContextRef::WebGL1),
            Self::WebGL2(context) => context.get().map(ContextRef::WebGL2),
        }
    }
}

struct ContextsOfThisThread {
    contexts: Vec<WeakContext>,
    live_after_pruning: usize,
}

impl ContextsOfThisThread {
    const CONTEXTS_BEFORE_FIRST_PRUNING: usize = 128;

    fn add(&mut self, vm: &Vm, context: ContextRef) {
        if self.contexts.len() >= (2 * self.live_after_pruning).max(Self::CONTEXTS_BEFORE_FIRST_PRUNING) {
            self.contexts.retain(|context| context.get().is_some());
            self.live_after_pruning = self.contexts.len();
        }
        self.contexts.push(match context {
            ContextRef::WebGL1(context) => WeakContext::WebGL1(GcWeak::new(vm.heap(), context)),
            ContextRef::WebGL2(context) => WeakContext::WebGL2(GcWeak::new(vm.heap(), context)),
        });
    }
}

thread_local! {
    static CONTEXTS_OF_THIS_THREAD: RefCell<ContextsOfThisThread> = const {
        RefCell::new(ContextsOfThisThread {
            contexts: Vec::new(),
            live_after_pruning: 0,
        })
    };
}

pub(crate) fn register_context(vm: &Vm, context: ContextRef) {
    CONTEXTS_OF_THIS_THREAD.with_borrow_mut(|contexts| contexts.add(vm, context));
}

pub(crate) fn forget_contexts_of_agent() {
    CONTEXTS_OF_THIS_THREAD.with_borrow_mut(|contexts| {
        contexts.contexts.clear();
        contexts.live_after_pruning = 0;
    });
}

fn live_contexts_of_this_thread(vm: &Vm) -> MarkedVec<'_, ContextRef> {
    let live_contexts = MarkedVec::new(vm);
    CONTEXTS_OF_THIS_THREAD.with_borrow(|contexts| {
        for context in contexts.contexts.iter().filter_map(WeakContext::get) {
            live_contexts.push(context);
        }
    });
    live_contexts
}

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-event
pub(crate) fn fire_webgl_context_event(vm: &Vm, canvas: Canvas, event_type: &str) -> bool {
    let Some(realm) = canvas.relevant_realm() else {
        return true;
    };
    // To fire a WebGL context event named e means that an event using the WebGLContextEvent interface, with its type attribute [DOM4] initialized to e, its cancelable attribute initialized to true, and its isTrusted attribute [DOM4] initialized to true, is to be dispatched at the given object.
    // FIXME: Consider setting a status message.
    let event_init = WebGLContextEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: true,
            composed: false,
        },
        status_message: Utf16String::from_utf8(""),
    };
    let event = WebGLContextEvent::create(vm, realm, &Utf16String::from_utf8(event_type), &event_init);
    canvas.event_target().dispatch_event(event.event())
}

pub(crate) fn lose_contexts_of_this_thread(vm: &Vm) {
    let contexts = live_contexts_of_this_thread(vm);
    for index in 0..contexts.len() {
        let context = contexts.get(index).expect("the index is in bounds");
        let state = context.state();
        if !state.mark_lost() {
            continue;
        }
        queue_a_task(vm, TaskSource::WebGL, context, |vm, context| {
            with_temporary_execution_context_and_callbacks(context.state().realm(), || {
                let state = context.state();
                let page_wants_context_restored = !fire_webgl_context_event(vm, state.canvas(), "webglcontextlost");
                state.set_context_restore_requested(page_wants_context_restored);
                // NB: A Compositor that came back before the page asked for the context is not announced again.
                restore_context_if_wanted(vm, context);
            });
        });
    }
}

fn restore_context_if_wanted(vm: &Vm, context: ContextRef) {
    let state = context.state();
    if !state.wants_restoring() {
        return;
    }
    let Some((remote_context, supported_opengl_extensions)) =
        create_remote_context(state.canvas(), state.is_webgl2(), &state.actual_context_parameters)
    else {
        return;
    };
    state.restore(remote_context, supported_opengl_extensions);
    state.canvas().did_create_backing_storage();
    queue_a_task(vm, TaskSource::WebGL, context, |vm, context| {
        with_temporary_execution_context_and_callbacks(context.state().realm(), || {
            fire_webgl_context_event(vm, context.state().canvas(), "webglcontextrestored");
        });
    });
}

pub(crate) fn restore_contexts_of_this_thread(vm: &Vm) {
    let contexts = live_contexts_of_this_thread(vm);
    for index in 0..contexts.len() {
        restore_context_if_wanted(vm, contexts.get(index).expect("the index is in bounds"));
    }
}
