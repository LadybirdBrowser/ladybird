/*
 * Copyright (c) 2023-2026, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FileReader, https://w3c.github.io/FileAPI/#dfn-filereader

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use std::time::{Duration, Instant};

use ak::{Base64Alphabet, Utf16String, encode_base64};
use encoding_rs::{Encoding, UTF_8};
use libjs_rust::api::{
    ArrayBuffer, ArrayBufferShared, EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcWeak, Must as _, PropertyKey,
    Realm, Trace, TypedArrayBase, TypedArrayKind, Value, Visitor, Vm,
};
use libtextcodec_rust::encoding_for_label;
use liburl_rust::url::ExcludeFragment;
use libwebcommon_rust::mime_sniff::mime_type::MimeType;

use super::blob::AnyBlob;
use crate::bindings::conversions::{BufferSource, CppInterface};
use crate::bindings::event_target::{CppEvent, CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate, with_temporary_execution_context_and_callbacks};
use crate::bindings::promise::{
    TaskId, TaskSource, WebIdlPromise, currently_running_task, queue_a_global_task, react_to_promise, remove_tasks,
};
use crate::bindings::realm_data::{WrapperWorld, wrapper_world_of};
use crate::bindings::{CallContext, Exception, MustNotThrow as _, WebIdlResult};
use crate::streams::readable_stream::ReadableStream;
use crate::streams::readable_stream_default_reader::ReadableStreamDefaultReader;

/// The union (DOMString or ArrayBuffer) of FileAPI/FileReader.idl.
pub(crate) enum DOMStringOrArrayBuffer {
    DOMString(Utf16String),
    ArrayBuffer(Gc<ArrayBuffer>),
}

// https://w3c.github.io/FileAPI/#dom-filereader-readystate
#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    // The FileReader object has been constructed, and there are no pending reads. None of the read methods have been called.
    // This is the default state of a newly minted FileReader object, until one of the read methods have been called on it.
    Empty = 0,

    // A File or Blob is being read. One of the read methods is being processed, and no error has occurred during the read.
    Loading = 1,

    // The entire File or Blob has been read into memory, OR a file read error occurred, OR the read was aborted using abort().
    // The FileReader is no longer reading a File or Blob.
    // If readyState is set to DONE it means at least one of the read methods have been called on this FileReader.
    Done = 2,
}

/// The type that a read operation packages its bytes as.
#[derive(Clone, Copy)]
pub(crate) enum Type {
    ArrayBuffer,
    BinaryString,
    Text,
    DataURL,
}

/// What package data returns.
pub(crate) enum PackagedData {
    String(Utf16String),
    Bytes(Vec<u8>),
}

// https://w3c.github.io/FileAPI/#dfn-filereader
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct FileReader {
    event_target: CppEventTarget,
    realm: Gc<Realm>,

    // Internal state to handle aborting the FileReader.
    #[gc(untraced)]
    pending_tasks: RefCell<Vec<TaskId>>,
    // The read operation being processed, which abort() terminates, and which a later read replaces.
    current_read: Cell<Option<ReadOperationRef>>,

    // A FileReader has an associated state, that is "empty", "loading", or "done". It is initially "empty".
    // https://w3c.github.io/FileAPI/#filereader-state
    #[gc(untraced)]
    state: Cell<State>,

    // A FileReader has an associated result (null, a DOMString or an ArrayBuffer). It is initially null.
    // https://w3c.github.io/FileAPI/#filereader-result
    #[gc(untraced)]
    result: RefCell<Option<PackagedData>>,

    // The result ArrayBuffer must keep a stable identity across accesses within a
    // world, so it is created lazily and cached per wrapper world. The cache holds
    // weak references (a dropped ArrayBuffer is unobservable) and is invalidated
    // whenever the result changes. See set_result().
    #[gc(untraced)]
    result_array_buffers: RefCell<Vec<(WrapperWorld, GcWeak<ArrayBuffer>)>>,
}

impl Wrappable for FileReader {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for FileReader {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

/// The steps of a task that FileReader::queue_a_task() queues, with what they capture.
#[derive(Clone, Copy)]
struct QueuedSteps<C> {
    captures: C,
    steps: fn(&Vm, C),
}

// SAFETY: The steps are a plain function, which holds no cells.
unsafe impl<C: Trace> Trace for QueuedSteps<C> {
    fn trace(&self, visitor: &mut Visitor) {
        self.captures.trace(visitor);
    }
}

/// The state of a read operation, https://w3c.github.io/FileAPI/#readOperation, which its steps keep across chunks.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct ReadOperation {
    reader: Ref<ReadableStreamDefaultReader>,
    #[gc(untraced)]
    bytes: RefCell<Vec<u8>>,
    is_first_chunk: Cell<bool>,
    #[gc(untraced)]
    progress_timer: Cell<Option<Instant>>,
    #[gc(untraced)]
    r#type: Type,
    #[gc(untraced)]
    encoding_name: Option<Utf16String>,
    #[gc(untraced)]
    blobs_type: Utf16String,
}

static READ_OPERATION_CLASS: EmbedderClass<ReadOperation> = EmbedderClass::new("FileReaderReadOperation");

impl EmbedderValue for ReadOperation {
    const CLASS: &'static EmbedderClass<Self> = &READ_OPERATION_CLASS;
}

type ReadOperationRef = Gc<EmbedderCell<ReadOperation>>;

fn to_utf8_string(string: &Utf16String) -> String {
    String::from_utf16_lossy(&string.to_utf16())
}

// https://w3c.github.io/FileAPI/#blob-package-data
pub(crate) fn package_data(
    bytes: Vec<u8>,
    r#type: Type,
    mime_type: Option<&Utf16String>,
    encoding_name: Option<&Utf16String>,
) -> PackagedData {
    // A Blob has an associated package data algorithm, given bytes, a type, a optional mimeType, and a optional encodingName, which switches on type and runs the associated steps:
    match r#type {
        Type::DataURL => {
            // Return bytes as a DataURL [RFC2397] subject to the considerations below:
            // Use mimeType as part of the Data URL if it is available in keeping with the Data URL specification [RFC2397].
            // If mimeType is not available return a Data URL without a media-type. [RFC2397].
            // https://github.com/w3c/FileAPI/issues/104
            let mime_string = match mime_type {
                Some(mime_type) if !mime_type.is_empty() => to_utf8_string(mime_type),
                _ => String::from("application/octet-stream"),
            };
            let payload = to_utf8_string(&encode_base64(&bytes, Base64Alphabet::Base64, false));
            let url = liburl_rust::create_with_data(&mime_string, &payload, true);
            PackagedData::String(Utf16String::from_utf8(url.serialize(ExcludeFragment::No)))
        }
        Type::Text => {
            // 1. Let encoding be failure.
            let mut encoding: Option<&'static Encoding> = None;

            // 2. If the encodingName is present, set encoding to the result of getting an encoding from encodingName.
            if let Some(encoding_name) = encoding_name {
                encoding = encoding_for_label(encoding_name.as_units());
            }

            // 3. If encoding is failure, and mimeType is present:
            if encoding.is_none()
                && let Some(mime_type) = mime_type
            {
                // 1. Let type be the result of parse a MIME type given mimeType.
                let code_points: Vec<char> = char::decode_utf16(mime_type.to_utf16().iter().copied())
                    .map(|code_point| code_point.unwrap_or(char::REPLACEMENT_CHARACTER))
                    .collect();
                let maybe_type = MimeType::parse(&code_points);

                // 2. If type is not failure, set encoding to the result of getting an encoding from type’s parameters["charset"].
                if let Some(r#type) = maybe_type
                    && let Some((_, charset)) = r#type.parameters().find(|(name, _)| *name == "charset")
                {
                    encoding = Encoding::for_label(charset.as_bytes());
                }
            }

            // 4. If encoding is failure, then set encoding to UTF-8.
            // 5. Decode bytes using fallback encoding encoding, and return the result.
            let (decoded, _, _) = encoding.unwrap_or(UTF_8).decode(&bytes);
            PackagedData::String(Utf16String::from_utf8(&decoded))
        }
        // Return bytes. The binding wrapper performs the ArrayBuffer allocation
        // for the caller's realm.
        Type::ArrayBuffer => PackagedData::Bytes(bytes),
        Type::BinaryString => {
            // Return bytes as a binary string, in which every byte is represented by a code unit of equal value [0..255].
            let code_units: Vec<u16> = bytes.iter().map(|&byte| u16::from(byte)).collect();
            PackagedData::String(Utf16String::from_utf16(&code_units))
        }
    }
}

impl FileReader {
    fn new(realm: Gc<Realm>) -> Self {
        Self {
            event_target: CppEventTarget::new(),
            realm,
            pending_tasks: RefCell::new(Vec::new()),
            current_read: Cell::new(None),
            state: Cell::new(State::Empty),
            result: RefCell::new(None),
            result_array_buffers: RefCell::new(Vec::new()),
        }
    }

    // https://w3c.github.io/FileAPI/#filereaderConstrctr
    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Ref<Self>> {
        let reader = allocate(cx.vm(), |_| Self::new(cx.realm()));
        reader.event_target.set_owner(reader);
        Ok(reader)
    }

    // https://w3c.github.io/FileAPI/#dom-filereader-readystate
    pub(crate) fn ready_state(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        Ok(self.state.get() as u16)
    }

    // https://w3c.github.io/FileAPI/#dom-filereader-result
    pub(crate) fn result(&self, cx: &mut CallContext) -> WebIdlResult<Option<DOMStringOrArrayBuffer>> {
        let result = self.result.borrow();
        let Some(result) = &*result else {
            return Ok(None);
        };
        match result {
            PackagedData::String(string) => Ok(Some(DOMStringOrArrayBuffer::DOMString(string.clone()))),
            PackagedData::Bytes(bytes) => {
                // Return the same ArrayBuffer for repeated accesses in this world; the ArrayBuffer
                // is created in the receiver's realm and its identity must be stable.
                let wrapper_world = wrapper_world_of(cx.vm(), cx.realm());
                let cached = self
                    .result_array_buffers
                    .borrow()
                    .iter()
                    .find(|(world, _)| *world == wrapper_world)
                    .and_then(|(_, array_buffer)| array_buffer.get());
                if let Some(cached) = cached {
                    return Ok(Some(DOMStringOrArrayBuffer::ArrayBuffer(cached)));
                }
                let array_buffer = ArrayBuffer::create_from_bytes(cx.vm(), cx.realm(), bytes, ArrayBufferShared::No);
                let mut result_array_buffers = self.result_array_buffers.borrow_mut();
                result_array_buffers.retain(|(world, _)| *world != wrapper_world);
                result_array_buffers.push((wrapper_world, GcWeak::new(cx.vm().heap(), array_buffer)));
                Ok(Some(DOMStringOrArrayBuffer::ArrayBuffer(array_buffer)))
            }
        }
    }

    fn set_result(&self, result: Option<PackagedData>) {
        *self.result.borrow_mut() = result;

        // Any previously handed-out ArrayBuffers belong to the old result.
        self.result_array_buffers.borrow_mut().clear();
    }

    // https://w3c.github.io/FileAPI/#dom-filereader-error
    pub(crate) fn error(&self, _: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // A FileReader has an associated error (null or a DOMException). It is initially null.
        // https://w3c.github.io/FileAPI/#filereader-error
        // NB: Nothing sets the error yet, so it stays null.
        Ok(None)
    }

    fn fire_event(&self, event_name: &str) {
        let event = CppEvent::create(self.realm, event_name, false, false, false);
        self.event_target.dispatch_event(&event);
    }

    fn queue_a_task<C: Trace + Copy + 'static>(this: Ref<Self>, vm: &Vm, captures: C, steps: fn(&Vm, C)) {
        // To implement the requirement of removing queued tasks on an abort we keep track of a list of
        // task IDs which are pending evaluation. This allows an abort to go through the task queue to
        // remove those pending tasks.

        let id = queue_a_global_task(
            vm,
            this.realm,
            TaskSource::FileReading,
            (this, QueuedSteps { captures, steps }),
            |vm, (this, task)| {
                let current_task = currently_running_task(this.realm);

                (task.steps)(vm, task.captures);

                this.pending_tasks.borrow_mut().retain(|id| *id != current_task);
            },
        );
        this.pending_tasks.borrow_mut().push(id);
    }

    // https://w3c.github.io/FileAPI/#readOperation
    fn read_operation(
        this: &Ref<Self>,
        cx: &mut CallContext,
        blob: &AnyBlob,
        r#type: Type,
        encoding_name: Option<Utf16String>,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();
        let realm = this.realm;
        let blobs_type = blob.type_string().clone();

        // 1. If fr’s state is "loading", throw an InvalidStateError DOMException.
        if this.state.get() == State::Loading {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("Read already in progress"),
            ));
        }

        // 2. Set fr’s state to "loading".
        this.state.set(State::Loading);

        // 3. Set fr’s result to null.
        this.set_result(None);

        // 4. Set fr’s error to null.
        // NB: The error is always null, see error().

        // 5. Let stream be the result of calling get stream on blob.
        let stream = blob.stream(&mut CallContext::new(vm, realm))?;

        // 6. Let reader be the result of getting a reader from stream.
        let reader = ReadableStream::get_a_reader(stream, vm)?;

        // 7. Let bytes be an empty byte sequence.
        let bytes = Vec::new();

        // 8. Let chunkPromise be the result of reading a chunk from stream with reader.
        let chunk_promise = ReadableStreamDefaultReader::read(&reader, &mut CallContext::new(vm, realm))?;

        // 9. Let isFirstChunk be true.
        let is_first_chunk = true;

        // 10. In parallel, while true:
        let state = READ_OPERATION_CLASS.allocate(vm, |_| ReadOperation {
            reader,
            bytes: RefCell::new(bytes),
            is_first_chunk: Cell::new(is_first_chunk),
            progress_timer: Cell::new(None),
            r#type,
            encoding_name,
            blobs_type,
        });
        this.current_read.set(Some(state));

        queue_a_global_task(
            vm,
            realm,
            TaskSource::FileReading,
            (*this, state, chunk_promise),
            |vm, (this, state, chunk_promise)| {
                with_temporary_execution_context_and_callbacks(this.realm, || {
                    Self::process_next_chunk(vm, this, state, chunk_promise);
                });
            },
        );

        Ok(())
    }

    fn process_next_chunk(vm: &Vm, this: Ref<Self>, state: ReadOperationRef, chunk_promise: WebIdlPromise) {
        // 1. Wait for chunkPromise to be fulfilled or rejected.
        react_to_promise(
            vm,
            chunk_promise,
            (this, state),
            Some(|cx, (this, state), promise_value| {
                with_temporary_execution_context_and_callbacks(this.realm, || {
                    Self::chunk_promise_fulfilled(cx.vm(), this, state, promise_value);
                });
                Ok(Value::UNDEFINED)
            }),
            // 6. Otherwise, if chunkPromise is rejected with an error error, queue a task to run the following steps and abort this algorithm:
            Some(|cx, (this, state), _| {
                with_temporary_execution_context_and_callbacks(this.realm, || {
                    if this.current_read.get() != Some(state) {
                        return;
                    }

                    Self::queue_a_task(this, cx.vm(), this, |_, this| {
                        // 1. Set fr’s state to "done".
                        this.state.set(State::Done);

                        // FIXME: 2. Set fr’s error to error.

                        // 3. Fire a progress event called error at fr.
                        this.fire_event("error");

                        // 4. If fr’s state is not "loading", fire a progress event called loadend at fr.
                        if this.state.get() != State::Loading {
                            this.fire_event("loadend");
                        }

                        // NOTE: Event handler for the error event could have started another load, if that happens the
                        //       loadend event for this load is not fired.
                    });
                });
                Ok(Value::UNDEFINED)
            }),
        );
    }

    fn chunk_promise_fulfilled(vm: &Vm, this: Ref<Self>, state: ReadOperationRef, promise_value: Value) {
        if this.current_read.get() != Some(state) {
            return;
        }

        // 2. If chunkPromise is fulfilled, and isFirstChunk is true, queue a task to fire a progress event called loadstart at fr.
        // NOTE: ISSUE 2 We might change loadstart to be dispatched synchronously, to align with XMLHttpRequest behavior. [Issue #119]
        if state.is_first_chunk.get() {
            Self::queue_a_task(this, vm, this, |_, this| this.fire_event("loadstart"));
        }

        // 3. Set isFirstChunk to false.
        state.is_first_chunk.set(false);

        assert!(promise_value.is_object());
        let result = promise_value.as_object();

        let value = result.get(vm, &PropertyKey::from_utf8("value")).must();
        let done = result.get(vm, &PropertyKey::from_utf8("done")).must();

        let byte_sequence = if !done.as_bool() && value.is_object() {
            value
                .as_object()
                .downcast::<TypedArrayBase>()
                .filter(|typed_array| typed_array.kind() == TypedArrayKind::Uint8Array)
        } else {
            None
        };

        // 4. If chunkPromise is fulfilled with an object whose done property is false and whose value property is a Uint8Array object, run these steps:
        if let Some(byte_sequence) = byte_sequence {
            // 1. Let bs be the byte sequence represented by the Uint8Array object.
            // 2. Append bs to bytes.
            // SAFETY: Appending the bytes neither runs script nor allocates a cell.
            unsafe {
                BufferSource::TypedArray(byte_sequence)
                    .with_bytes(|bytes| state.bytes.borrow_mut().extend_from_slice(bytes));
            }

            // 3. If roughly 50ms have passed since these steps were last invoked, queue a task to fire a progress event called progress at fr.
            let now = Instant::now();
            let enough_time_passed = state
                .progress_timer
                .get()
                .is_none_or(|progress_timer| now - progress_timer >= Duration::from_millis(50));
            // WPT tests for this and expects no progress event to fire when there isn't any data.
            // See http://wpt.live/FileAPI/reading-data-section/filereader_events.any.html
            let contained_data = byte_sequence.array_length().length() > 0;
            if enough_time_passed && contained_data {
                Self::queue_a_task(this, vm, this, |_, this| this.fire_event("progress"));
                state.progress_timer.set(Some(now));
            }

            // 4. Set chunkPromise to the result of reading a chunk from stream with reader.
            let next_chunk_promise =
                ReadableStreamDefaultReader::read(&state.reader, &mut CallContext::new(vm, this.realm)).must();
            Self::process_next_chunk(vm, this, state, next_chunk_promise);
        }
        // 5. Otherwise, if chunkPromise is fulfilled with an object whose done property is true, queue a task to run the following steps and abort this algorithm:
        else if done.as_bool() {
            Self::queue_a_task(this, vm, (this, state), |_, (this, state)| {
                // 1. Set fr’s state to "done".
                this.state.set(State::Done);

                // 2. Let result be the result of package data given bytes, type, blob’s type, and encodingName.
                let bytes = core::mem::take(&mut *state.bytes.borrow_mut());
                let result = package_data(
                    bytes,
                    state.r#type,
                    Some(&state.blobs_type),
                    state.encoding_name.as_ref(),
                );

                // 3. If package data threw an exception error:
                // NB: Package data does not throw.

                // 4. Else:
                // 1. Set fr’s result to result.
                this.set_result(Some(result));

                // 2. Fire a progress event called load at the fr.
                this.fire_event("load");

                // 5. If fr’s state is not "loading", fire a progress event called loadend at the fr.
                if this.state.get() != State::Loading {
                    this.fire_event("loadend");
                }

                // NOTE: Event handler for the load or error events could have started another load, if that happens
                //       the loadend event for this load is not fired.
            });
        }
    }

    // https://w3c.github.io/FileAPI/#dfn-readAsDataURL
    pub(crate) fn read_as_data_url(this: &Ref<Self>, cx: &mut CallContext, blob: AnyBlob) -> WebIdlResult<()> {
        // The readAsDataURL(blob) method, when invoked, must initiate a read operation for blob with DataURL.
        Self::read_operation(this, cx, &blob, Type::DataURL, None)
    }

    // https://w3c.github.io/FileAPI/#dfn-readAsText
    pub(crate) fn read_as_text(
        this: &Ref<Self>,
        cx: &mut CallContext,
        blob: AnyBlob,
        encoding: Option<Utf16String>,
    ) -> WebIdlResult<()> {
        // The readAsText(blob, encoding) method, when invoked, must initiate a read operation for blob with Text and encoding.
        Self::read_operation(this, cx, &blob, Type::Text, encoding)
    }

    // https://w3c.github.io/FileAPI/#dfn-readAsArrayBuffer
    pub(crate) fn read_as_array_buffer(this: &Ref<Self>, cx: &mut CallContext, blob: AnyBlob) -> WebIdlResult<()> {
        // The readAsArrayBuffer(blob) method, when invoked, must initiate a read operation for blob with ArrayBuffer.
        Self::read_operation(this, cx, &blob, Type::ArrayBuffer, None)
    }

    // https://w3c.github.io/FileAPI/#dfn-readAsBinaryString
    pub(crate) fn read_as_binary_string(this: &Ref<Self>, cx: &mut CallContext, blob: AnyBlob) -> WebIdlResult<()> {
        // The readAsBinaryString(blob) method, when invoked, must initiate a read operation for blob with BinaryString.
        // NOTE: The use of readAsArrayBuffer() is preferred over readAsBinaryString(), which is provided for backwards compatibility.
        Self::read_operation(this, cx, &blob, Type::BinaryString, None)
    }

    // https://w3c.github.io/FileAPI/#dfn-abort
    pub(crate) fn abort(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // 1. If this's state is "empty" or if this's state is "done" set this's result to null and terminate this algorithm.
        if self.state.get() == State::Empty || self.state.get() == State::Done {
            self.set_result(None);
            return Ok(());
        }

        // 2. If this's state is "loading" set this's state to "done" and set this's result to null.
        if self.state.get() == State::Loading {
            self.state.set(State::Done);
            self.set_result(None);
        }

        // 3. If there are any tasks from this on the file reading task source in an affiliated task queue, then remove those tasks from that task queue.
        let pending_tasks = self.pending_tasks.take();
        remove_tasks(self.realm, &pending_tasks);

        // 4. Terminate the algorithm for the read method being processed.
        self.current_read.set(None);

        // 5. Fire a progress event called abort at this.
        self.fire_event("abort");

        // 6. If this's state is not "loading", fire a progress event called loadend at this.
        if self.state.get() != State::Loading {
            self.fire_event("loadend");
        }

        Ok(())
    }
}
