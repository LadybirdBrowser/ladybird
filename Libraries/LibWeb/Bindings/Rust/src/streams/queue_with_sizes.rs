/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Queue-with-sizes, https://streams.spec.whatwg.org/#queue-with-sizes

use std::collections::VecDeque;

use core::cell::Cell;

use libjs_rust::api::{GcRefCell, Trace, Value};

use crate::bindings::{Exception, WebIdlResult};
use crate::streams::abstract_operations::is_non_negative_number;

/// https://streams.spec.whatwg.org/#value-with-size
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ValueWithSize {
    pub(crate) value: Value,
    #[gc(untraced)]
    pub(crate) size: f64,
}

/// The [[queue]] and [[queueTotalSize]] internal slots of a container,
/// https://streams.spec.whatwg.org/#queue-with-sizes
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct QueueWithSizes {
    queue: GcRefCell<VecDeque<ValueWithSize>>,
    #[gc(untraced)]
    queue_total_size: Cell<f64>,
}

impl Default for QueueWithSizes {
    fn default() -> Self {
        Self {
            queue: GcRefCell::new(VecDeque::new()),
            queue_total_size: Cell::new(0.0),
        }
    }
}

impl QueueWithSizes {
    pub(crate) fn is_empty(&self) -> bool {
        self.queue.borrow().is_empty()
    }

    pub(crate) fn total_size(&self) -> f64 {
        self.queue_total_size.get()
    }

    // https://streams.spec.whatwg.org/#dequeue-value
    pub(crate) fn dequeue_value(&self) -> Value {
        // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.
        // 2. Assert: container.[[queue]] is not empty.
        // 3. Let valueWithSize be container.[[queue]][0].
        // 4. Remove valueWithSize from container.[[queue]].
        let value_with_size = self
            .queue
            .borrow_mut()
            .pop_front()
            .expect("a container dequeues from a queue that is not empty");

        // 5. Set container.[[queueTotalSize]] to container.[[queueTotalSize]] − valueWithSize’s size.
        self.queue_total_size
            .set(self.queue_total_size.get() - value_with_size.size);

        // 6. If container.[[queueTotalSize]] < 0, set container.[[queueTotalSize]] to 0. (This can occur due to
        //    rounding errors.)
        if self.queue_total_size.get() < 0.0 {
            self.queue_total_size.set(0.0);
        }

        // 7. Return valueWithSize’s value.
        value_with_size.value
    }

    // https://streams.spec.whatwg.org/#enqueue-value-with-size
    pub(crate) fn enqueue_value_with_size(&self, value: Value, size: Value) -> WebIdlResult<()> {
        // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.
        // 2. If ! IsNonNegativeNumber(size) is false, throw a RangeError exception.
        if !is_non_negative_number(size) {
            return Err(Exception::range_error("Chunk has non-positive size"));
        }

        // 3. If size is +∞, throw a RangeError exception.
        let size = size.as_f64();
        if size == f64::INFINITY {
            return Err(Exception::range_error("Chunk has infinite size"));
        }

        // 4. Append a new value-with-size with value value and size size to container.[[queue]].
        self.queue.borrow_mut().push_back(ValueWithSize { value, size });

        // 5. Set container.[[queueTotalSize]] to container.[[queueTotalSize]] + size.
        self.queue_total_size.set(self.queue_total_size.get() + size);
        Ok(())
    }

    // https://streams.spec.whatwg.org/#peek-queue-value
    pub(crate) fn peek_queue_value(&self) -> Value {
        // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.
        // 2. Assert: container.[[queue]] is not empty.
        // 3. Let valueWithSize be container.[[queue]][0].
        // 4. Return valueWithSize’s value.
        self.queue
            .borrow()
            .front()
            .expect("a container peeks at a queue that is not empty")
            .value
    }

    // https://streams.spec.whatwg.org/#reset-queue
    pub(crate) fn reset_queue(&self) {
        // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.
        // 2. Set container.[[queue]] to a new empty list.
        self.queue.borrow_mut().clear();

        // 3. Set container.[[queueTotalSize]] to 0.
        self.queue_total_size.set(0.0);
    }
}
