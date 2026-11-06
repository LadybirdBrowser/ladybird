/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WindowOrWorkerGlobalScope, https://html.spec.whatwg.org/multipage/webappapis.html#windoworworkerglobalscope

use libjs_rust::api::{Gc, Object, Value};

use crate::bindings::{CallContext, WebIdlResult};
use crate::html::structured_serialize::transfer_record::TransferRecord;
use crate::html::structured_serialize::{structured_deserialize_with_transfer, structured_serialize_with_transfer};

// https://html.spec.whatwg.org/multipage/structured-data.html#dom-structuredclone
pub(crate) fn structured_clone(cx: &CallContext, value: Value, transfer: &[Gc<Object>]) -> WebIdlResult<Value> {
    // 1. Let serialized be ? StructuredSerializeWithTransfer(value, options["transfer"]).
    let serialized = structured_serialize_with_transfer(cx, value, transfer)?;

    // 2. Let deserializeRecord be ? StructuredDeserializeWithTransfer(serialized, this's relevant realm).
    let deserialize_record = structured_deserialize_with_transfer(cx, serialized);
    TransferRecord::discard(serialized);

    // 3. Return deserializeRecord.[[Deserialized]].
    Ok(deserialize_record?.deserialized)
}
