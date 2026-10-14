/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/2.0/#5.1
//!
//! A buffer holds indices or other data, and which of the two is decided by the first target that it is bound to. A
//! target that an operation does not bind buffers to has no kind, so that a bind that fails for its target cannot
//! decide the kind of a buffer.

use core::cell::Cell;

use super::constants::{GL_ELEMENT_ARRAY_BUFFER, GL_TRANSFORM_FEEDBACK_BUFFER, GL_UNIFORM_BUFFER};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum BufferKind {
    ElementArray,
    OtherData,
}

impl BufferKind {
    pub(crate) fn of_buffers_at_indexed_target(target: u32) -> Option<Self> {
        matches!(target, GL_TRANSFORM_FEEDBACK_BUFFER | GL_UNIFORM_BUFFER).then_some(Self::OtherData)
    }
}

#[derive(Default)]
pub(crate) struct KindOfFirstBinding(Cell<Option<BufferKind>>);

impl KindOfFirstBinding {
    /// Operations ask before they look at the target, so it can be one that they do not bind buffers to. A buffer of
    /// the wrong kind for it is refused all the same, and only a buffer that is not refused goes on to fail for it.
    pub(crate) fn allows_binding_to(&self, target: u32) -> bool {
        match self.0.get() {
            None => true,
            Some(BufferKind::ElementArray) => target == GL_ELEMENT_ARRAY_BUFFER,
            Some(BufferKind::OtherData) => target != GL_ELEMENT_ARRAY_BUFFER,
        }
    }

    pub(crate) fn record_binding_as(&self, kind: BufferKind) {
        if self.0.get().is_none() {
            self.0.set(Some(kind));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::web_gl::constants::GL_ARRAY_BUFFER;

    const TARGET_THAT_NO_OPERATION_BINDS_BUFFERS_TO: u32 = 0x1234;

    #[test]
    fn asking_whether_a_buffer_can_be_bound_does_not_decide_its_kind() {
        let kind = KindOfFirstBinding::default();
        assert!(kind.allows_binding_to(GL_ARRAY_BUFFER));
        assert!(kind.allows_binding_to(GL_ELEMENT_ARRAY_BUFFER));
        assert!(kind.allows_binding_to(TARGET_THAT_NO_OPERATION_BINDS_BUFFERS_TO));
        assert!(kind.allows_binding_to(GL_ARRAY_BUFFER));
    }

    #[test]
    fn a_buffer_keeps_the_kind_of_its_first_binding() {
        let kind = KindOfFirstBinding::default();
        kind.record_binding_as(BufferKind::ElementArray);
        assert!(kind.allows_binding_to(GL_ELEMENT_ARRAY_BUFFER));
        assert!(!kind.allows_binding_to(GL_ARRAY_BUFFER));
        assert!(!kind.allows_binding_to(GL_UNIFORM_BUFFER));

        kind.record_binding_as(BufferKind::OtherData);
        assert!(kind.allows_binding_to(GL_ELEMENT_ARRAY_BUFFER));
        assert!(!kind.allows_binding_to(GL_ARRAY_BUFFER));

        let kind = KindOfFirstBinding::default();
        kind.record_binding_as(BufferKind::OtherData);
        assert!(kind.allows_binding_to(GL_ARRAY_BUFFER));
        assert!(kind.allows_binding_to(GL_UNIFORM_BUFFER));
        assert!(!kind.allows_binding_to(GL_ELEMENT_ARRAY_BUFFER));
    }

    #[test]
    fn a_target_that_binds_no_buffers_refuses_only_a_buffer_of_indices() {
        let kind = KindOfFirstBinding::default();
        kind.record_binding_as(BufferKind::ElementArray);
        assert!(!kind.allows_binding_to(TARGET_THAT_NO_OPERATION_BINDS_BUFFERS_TO));

        let kind = KindOfFirstBinding::default();
        kind.record_binding_as(BufferKind::OtherData);
        assert!(kind.allows_binding_to(TARGET_THAT_NO_OPERATION_BINDS_BUFFERS_TO));
    }

    #[test]
    fn bind_buffer_base_and_range_give_a_kind_only_at_their_indexed_targets() {
        assert_eq!(
            BufferKind::of_buffers_at_indexed_target(GL_TRANSFORM_FEEDBACK_BUFFER),
            Some(BufferKind::OtherData)
        );
        assert_eq!(
            BufferKind::of_buffers_at_indexed_target(GL_UNIFORM_BUFFER),
            Some(BufferKind::OtherData)
        );
        assert_eq!(BufferKind::of_buffers_at_indexed_target(GL_ARRAY_BUFFER), None);
        assert_eq!(BufferKind::of_buffers_at_indexed_target(GL_ELEMENT_ARRAY_BUFFER), None);
        assert_eq!(
            BufferKind::of_buffers_at_indexed_target(TARGET_THAT_NO_OPERATION_BINDS_BUFFERS_TO),
            None
        );
    }
}
