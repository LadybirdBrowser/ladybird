/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBCursorWithValue, https://w3c.github.io/IndexedDB/#idbcursorwithvalue

use libjs_rust::api::{Trace, Value};

use crate::bindings::interface::{Inherits, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};
use crate::indexed_db::idb_cursor::IDBCursor;

// https://w3c.github.io/IndexedDB/#idbcursorwithvalue
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBCursorWithValue {
    cursor: IDBCursor,
}

impl Inherits<IDBCursor> for IDBCursorWithValue {
    fn parent(&self) -> &IDBCursor {
        &self.cursor
    }
}

impl Wrappable for IDBCursorWithValue {}

impl IDBCursorWithValue {
    pub(crate) fn new(cursor: IDBCursor) -> Self {
        Self { cursor }
    }

    // https://w3c.github.io/IndexedDB/#dom-idbcursorwithvalue-value
    pub(crate) fn value(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        // The value getter steps are to return this’s current value.
        Ok(self.cursor.current_value().unwrap_or(Value::UNDEFINED))
    }
}
