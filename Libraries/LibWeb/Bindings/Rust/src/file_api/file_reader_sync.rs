/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FileReaderSync, https://w3c.github.io/FileAPI/#FileReaderSync

use ak::Utf16String;
use libjs_rust::api::{ArrayBuffer, ArrayBufferShared, Gc, Trace};

use super::blob::AnyBlob;
use super::file_reader::{PackagedData, Type, package_data};
use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, WebIdlResult};

// https://w3c.github.io/FileAPI/#FileReaderSync
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct FileReaderSync;

impl Wrappable for FileReaderSync {}

impl FileReaderSync {
    pub(crate) fn construct(_: &mut CallContext) -> WebIdlResult<Self> {
        Ok(Self)
    }

    // https://w3c.github.io/FileAPI/#dfn-readAsArrayBufferSync
    pub(crate) fn read_as_array_buffer(&self, cx: &mut CallContext, blob: AnyBlob) -> WebIdlResult<Gc<ArrayBuffer>> {
        Ok(ArrayBuffer::create_from_bytes(
            cx.vm(),
            cx.realm(),
            &blob.raw_bytes(),
            ArrayBufferShared::No,
        ))
    }

    // https://w3c.github.io/FileAPI/#dfn-readAsBinaryStringSync
    pub(crate) fn read_as_binary_string(&self, _: &mut CallContext, blob: AnyBlob) -> WebIdlResult<Utf16String> {
        Ok(read_as(&blob, Type::BinaryString, None))
    }

    // https://w3c.github.io/FileAPI/#dfn-readAsTextSync
    pub(crate) fn read_as_text(
        &self,
        _: &mut CallContext,
        blob: AnyBlob,
        encoding: Option<Utf16String>,
    ) -> WebIdlResult<Utf16String> {
        Ok(read_as(&blob, Type::Text, encoding.as_ref()))
    }

    // https://w3c.github.io/FileAPI/#dfn-readAsDataURLSync
    pub(crate) fn read_as_data_url(&self, _: &mut CallContext, blob: AnyBlob) -> WebIdlResult<Utf16String> {
        Ok(read_as(&blob, Type::DataURL, None))
    }
}

fn read_as(blob: &AnyBlob, r#type: Type, encoding: Option<&Utf16String>) -> Utf16String {
    let blob_type = blob.type_string();
    match package_data(blob.raw_bytes().to_vec(), r#type, Some(blob_type), encoding) {
        PackagedData::String(string) => string,
        PackagedData::Bytes(_) => unreachable!("package data returns a string for a type other than ArrayBuffer"),
    }
}
