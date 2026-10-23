/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! File, https://w3c.github.io/FileAPI/#file-section

use std::time::{SystemTime, UNIX_EPOCH};

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, Trace};

use super::blob::{Blob, BlobPart, BlobPropertyBag, process_blob_parts, type_from_options};
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Inherits, Wrappable};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};

/// The dictionary FilePropertyBag of FileAPI/File.idl, whose members the bindings convert.
pub(crate) struct FilePropertyBag {
    pub(crate) parent: BlobPropertyBag,
    pub(crate) last_modified: Option<i64>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct File {
    blob: Blob,
    // https://w3c.github.io/FileAPI/#dfn-name
    #[gc(untraced)]
    name: Utf16String,
    // https://w3c.github.io/FileAPI/#dfn-lastModified
    #[gc(untraced)]
    last_modified: i64,
}

impl Wrappable for File {}

impl Inherits<Blob> for File {
    fn parent(&self) -> &Blob {
        &self.blob
    }
}

impl File {
    // https://w3c.github.io/FileAPI/#ref-for-dom-file-file
    pub(crate) fn construct(
        _: &mut CallContext,
        file_bits: MarkedVec<'_, BlobPart>,
        file_name: UsvString,
        options: FilePropertyBag,
    ) -> WebIdlResult<Self> {
        // 1. Let bytes be the result of processing blob parts given fileBits and options.
        let bytes = process_blob_parts(&file_bits, &options.parent);

        Ok(Self::create(bytes, file_name.into_utf16_string(), &options))
    }

    /// Steps 2 to 4 of the File constructor, given the bytes that step 1 results in.
    pub(crate) fn create(bytes: Vec<u8>, file_name: Utf16String, options: &FilePropertyBag) -> Self {
        // 2. Let n be the fileName argument to the constructor.
        //    NOTE: Underlying OS filesystems use differing conventions for file name; with constructed files, mandating
        //          UTF-16 lessens ambiquity when file names are converted to byte sequences.
        let name = file_name;

        // 3. Process FilePropertyBag dictionary argument by running the following substeps:
        let r#type = type_from_options(&options.parent.r#type);

        // 3. If the lastModified member is provided, let d be set to the lastModified dictionary member. If it is not
        //    provided, set d to the current date and time represented as the number of milliseconds since the Unix
        //    Epoch (which is the equivalent of Date.now() [ECMA-262]).
        //    Note: Since ECMA-262 Date objects convert to long long values representing the number of milliseconds
        //          since the Unix Epoch, the lastModified member could be a Date object [ECMA-262].
        let last_modified = options.last_modified.unwrap_or_else(|| {
            let since_epoch = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
            i64::try_from(since_epoch.as_millis()).unwrap_or(i64::MAX)
        });

        // 4. Return a new File object F such that:
        //    2. F refers to the bytes byte sequence.
        //       NOTE: Spec started at 2 therefore keeping the same number sequence here.
        //    3. F.size is set to the number of total bytes in bytes.
        //    4. F.name is set to n.
        //    5. F.type is set to t.
        //    6. F.lastModified is set to d.
        Self {
            blob: Blob::new(bytes, r#type),
            name,
            last_modified,
        }
    }

    pub(crate) fn name_string(&self) -> &Utf16String {
        &self.name
    }

    pub(crate) fn last_modified_value(&self) -> i64 {
        self.last_modified
    }

    // https://w3c.github.io/FileAPI/#dfn-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.name.clone())
    }

    // https://w3c.github.io/FileAPI/#dfn-lastModified
    pub(crate) fn last_modified(&self, _: &mut CallContext) -> WebIdlResult<i64> {
        Ok(self.last_modified)
    }
}

impl Serializable for File {
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        // 1. Set serialized.[[SnapshotState]] to value’s snapshot state.
        // 2. Set serialized.[[ByteSequence]] to value’s underlying byte sequence.
        self.blob.serialize(serialized);

        // 3. Set serialized.[[Name]] to the value of value’s name attribute.
        serialized.write_utf16_string(&self.name);

        // 4. Set serialized.[[LastModified]] to the value of value’s lastModified attribute.
        serialized.write_i64(self.last_modified);

        Ok(())
    }

    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        // 1. Set value’s snapshot state to serialized.[[SnapshotState]].
        // 2. Set value’s underlying byte sequence to serialized.[[ByteSequence]].
        let blob = Blob::deserialize(serialized)?;

        // 3. Initialize the value of value’s name attribute to serialized.[[Name]].
        let name = serialized.read_utf16_string()?;

        // 4. Initialize the value of value’s lastModified attribute to serialized.[[LastModified]].
        let last_modified = serialized.read_i64()?;

        Ok(Self {
            blob,
            name,
            last_modified,
        })
    }
}
