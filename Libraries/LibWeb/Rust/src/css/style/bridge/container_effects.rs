/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// What a container condition the engine evaluated read of a container or its subject, which the
/// host records as it records its own evaluation's.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiContainerEffectKind {
    /// The node is a container a size or scroll-state query asks about.
    SizeContainerUsage,
    /// The node is a container a style query asks about.
    StyleContainerUsage,
    /// The node is a scroll-state container a query asks about, whose state is snapshotted.
    ScrollStateContainerUsage,
    /// The node has no box yet, and the query is evaluated again after layout.
    NeedsEvaluationAfterLayout,
    /// The subject's style resolved a viewport-relative length in the query.
    SubjectViewportDependency,
    /// A container-relative length resolved against the node, which has no box yet, and is
    /// resolved again after layout, a partial relayout included.
    UnitsNeedEvaluationAfterLayout,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiContainerEffect {
    pub node: u32,
    pub kind: FfiContainerEffectKind,
}

/// What the container conditions of a row the engine answered read of its containers, besides the
/// effects: whether the row's style depends on a size or scroll-state query or a style query.
#[repr(C)]
pub struct FfiContainerEffects {
    pub depends_on_size: bool,
    pub depends_on_style: bool,
}

/// Takes what the container conditions of the row the host is installing for `node` read of its
/// containers, for the host to record as it records its own evaluation's. Each effect is handed to
/// `record` with no engine borrow held, so recording may call the engine.
///
/// # Safety
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_take_container_effects(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    context: *mut c_void,
    record: unsafe extern "C" fn(*mut c_void, FfiContainerEffect),
) -> FfiContainerEffects {
    let verdict = StyleNodeID::from_raw(node)
        .filter(|_| host.container_effects_may_be_held())
        .and_then(|node| with_engine(read, host, |engine| engine.take_container_effects_for_install(node)))
        .unwrap_or_default();
    for &(node, kind) in &verdict.effects {
        unsafe { record(context, FfiContainerEffect { node: node.raw(), kind }) };
    }
    FfiContainerEffects {
        depends_on_size: verdict.depends_on_size,
        depends_on_style: verdict.depends_on_style,
    }
}
