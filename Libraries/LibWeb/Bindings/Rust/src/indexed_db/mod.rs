/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Indexed Database API, https://w3c.github.io/IndexedDB/

pub(crate) mod cpp;
pub(crate) mod exports;
pub(crate) mod idb_cursor;
pub(crate) mod idb_cursor_with_value;
pub(crate) mod idb_database;
pub(crate) mod idb_factory;
pub(crate) mod idb_index;
pub(crate) mod idb_key_range;
pub(crate) mod idb_object_store;
pub(crate) mod idb_open_db_request;
pub(crate) mod idb_record;
pub(crate) mod idb_request;
pub(crate) mod idb_transaction;
pub(crate) mod idb_version_change_event;
pub(crate) mod internal;
