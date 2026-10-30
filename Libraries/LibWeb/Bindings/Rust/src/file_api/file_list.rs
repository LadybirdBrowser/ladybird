/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FileList, https://w3c.github.io/FileAPI/#filelist-section

use libjs_rust::api::{DeferGc, GcRefCell, MarkedVec, Trace, Vm};

use super::file::File;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct FileList {
    files: GcRefCell<Vec<Ref<File>>>,
}

impl Wrappable for FileList {}

impl FileList {
    pub(crate) fn new(_: &DeferGc<'_>, files: &[Ref<File>]) -> Self {
        Self {
            files: GcRefCell::new(files.to_vec()),
        }
    }

    pub(crate) fn add_file(&self, vm: &Vm, file: Ref<File>) {
        self.files.borrow_mut(vm).push(file);
    }

    pub(crate) fn file_count(&self) -> u32 {
        u32::try_from(self.files.borrow().len()).expect("a list has fewer than 2^32 files")
    }

    pub(crate) fn file(&self, index: u32) -> Option<Ref<File>> {
        self.files.borrow().get(index as usize).copied()
    }

    // https://w3c.github.io/FileAPI/#dfn-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.file_count())
    }

    // https://w3c.github.io/FileAPI/#dfn-item
    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Option<Ref<File>>> {
        Ok(self.file(index))
    }
}

impl Serializable for FileList {
    fn serialization_steps(
        &self,
        cx: &mut CallContext,
        serialized: &mut SerializationWriter,
        for_storage: bool,
    ) -> WebIdlResult<()> {
        // 1. Set serialized.[[Files]] to an empty list.
        // 2. For each file in value, append the sub-serialization of file to serialized.[[Files]].
        // NB: The files are serialized in place, as they were before FileList was implemented in Rust, so that stored
        //     values keep the positions that their objects have in the memory of the serialization.
        let count = self.file_count();
        serialized.write_u64(u64::from(count));
        for index in 0..count {
            let file = self.file(index).expect("the list holds the file");
            serialized.write_object_in_place::<File>(cx, &file, for_storage)?;
        }

        Ok(())
    }

    fn deserialization_steps(cx: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Ref<Self>> {
        // 1. For each file of serialized.[[Files]], add the sub-deserialization of file to value.
        let count = serialized.read_u64()?;
        let files = MarkedVec::new(cx.vm());
        for _ in 0..count {
            files.push(serialized.read_object_in_place::<File>(cx)?);
        }

        Ok(allocate(cx.vm(), |defer_gc| Self::new(defer_gc, &files.values())))
    }
}
