/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{ArrayBuffer, Gc, Object, Realm, Value};

use crate::bindings::structured_data::{SerializationReader, SerializationWriter};
use crate::bindings::{Exception, WebIdlResult};

#[repr(C)]
#[derive(Default)]
struct OptionalField<T> {
    present: bool,
    value: T,
}

impl<T: Default> From<Option<T>> for OptionalField<T> {
    fn from(value: Option<T>) -> Self {
        Self {
            present: value.is_some(),
            value: value.unwrap_or_default(),
        }
    }
}

// Matches RTCEncodedAudioFrameFields in WebRTC/RTCEncodedAudioFrame.cpp. Serialization takes ownership of
// the string reference and borrows the span; deserialization lends both.
#[repr(C)]
struct RawFields<'a> {
    synchronization_source: OptionalField<u32>,
    payload_type: OptionalField<u8>,
    contributing_sources: OptionalField<ak::Span<'a, u32>>,
    rtp_timestamp: OptionalField<u32>,
    receive_time: OptionalField<f64>,
    capture_time: OptionalField<f64>,
    sender_capture_time_offset: OptionalField<f64>,
    mime_type: OptionalField<usize>,
    sequence_number: OptionalField<i16>,
    audio_level: OptionalField<f64>,
}

unsafe extern "C" {
    fn web_rtc_encoded_audio_frame_with_fields(
        frame: *mut c_void,
        state: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *const RawFields<'_>),
    ) -> *mut c_void;
    fn web_rtc_encoded_audio_frame_create(realm: *mut c_void, fields: &RawFields<'_>, data: *mut c_void)
    -> *mut c_void;
}

fn data_clone_error(message: &[u8]) -> Exception {
    Exception::Dom("DataCloneError", Utf16String::from_ascii(message))
}

// https://w3c.github.io/webrtc-encoded-transform/#RTCEncodedAudioFrame-serialization
pub(crate) fn serialization_steps(
    frame: Gc<Object>,
    serialized: &mut SerializationWriter,
    for_storage: bool,
) -> WebIdlResult<()> {
    // 1. If forStorage is true, then throw a DataCloneError.
    if for_storage {
        return Err(data_clone_error(b"RTCEncodedAudioFrame is not storable"));
    }

    unsafe extern "C" fn write_metadata(state: *mut c_void, fields: *const RawFields<'_>) {
        // SAFETY: The caller lends the writer and the frame's fields for this call.
        let (serialized, fields) = unsafe { (&mut *state.cast::<SerializationWriter>(), &*fields) };
        serialized.write_bool(fields.synchronization_source.present);
        if fields.synchronization_source.present {
            serialized.write_u32(fields.synchronization_source.value);
        }
        serialized.write_bool(fields.payload_type.present);
        if fields.payload_type.present {
            serialized.write_u8(fields.payload_type.value);
        }
        serialized.write_bool(fields.contributing_sources.present);
        if fields.contributing_sources.present {
            let sources = fields.contributing_sources.value.as_slice();
            serialized.write_u64(sources.len() as u64);
            for source in sources {
                serialized.write_u32(*source);
            }
        }
        serialized.write_bool(fields.rtp_timestamp.present);
        if fields.rtp_timestamp.present {
            serialized.write_u32(fields.rtp_timestamp.value);
        }
        serialized.write_bool(fields.receive_time.present);
        if fields.receive_time.present {
            serialized.write_f64(fields.receive_time.value);
        }
        serialized.write_bool(fields.capture_time.present);
        if fields.capture_time.present {
            serialized.write_f64(fields.capture_time.value);
        }
        serialized.write_bool(fields.sender_capture_time_offset.present);
        if fields.sender_capture_time_offset.present {
            serialized.write_f64(fields.sender_capture_time_offset.value);
        }
        serialized.write_bool(fields.mime_type.present);
        if fields.mime_type.present {
            // SAFETY: C++ hands over a reference to the string.
            let mime_type = unsafe { Utf16String::from_raw_owned(fields.mime_type.value) };
            serialized.write_utf16_string(&mime_type);
        }
        serialized.write_bool(fields.sequence_number.present);
        if fields.sequence_number.present {
            serialized.write_i32(i32::from(fields.sequence_number.value));
        }
        serialized.write_bool(fields.audio_level.present);
        if fields.audio_level.present {
            serialized.write_f64(fields.audio_level.value);
        }
    }

    // 2. Set serialized.[[metadata]] to an internal representation of value's metadata.
    // SAFETY: The frame is live, and the callback only writes its borrowed metadata.
    let data = unsafe {
        web_rtc_encoded_audio_frame_with_fields(
            frame.as_ptr().cast(),
            core::ptr::from_mut(serialized).cast(),
            write_metadata,
        )
    };
    // SAFETY: The frame owns the live ArrayBuffer returned by C++.
    let data = unsafe { Gc::<Object>::from_non_null(NonNull::new(data.cast()).expect("a frame has data")) };
    // 3. Set serialized.[[data]] to the sub-serialization of value.[[data]].
    serialized.write_value(Value::from_object(data))
}

fn read_optional<T>(
    serialized: &mut SerializationReader,
    read: impl FnOnce(&mut SerializationReader) -> WebIdlResult<T>,
) -> WebIdlResult<Option<T>> {
    if serialized.read_bool()? {
        Ok(Some(read(serialized)?))
    } else {
        Ok(None)
    }
}

// https://w3c.github.io/webrtc-encoded-transform/#RTCEncodedAudioFrame-serialization
pub(crate) fn deserialization_steps(
    realm: Gc<Realm>,
    serialized: &mut SerializationReader,
) -> WebIdlResult<Gc<Object>> {
    // 1. Set value's metadata to the platform object representation of serialized.[[metadata]].
    let synchronization_source = read_optional(serialized, |s| s.read_u32())?;
    let payload_type = read_optional(serialized, |s| s.read_u8())?;
    let contributing_sources = read_optional(serialized, |serialized| {
        let count = serialized.read_u64()?;
        let mut sources = Vec::new();
        for _ in 0..count {
            sources.push(serialized.read_u32()?);
        }
        Ok(sources)
    })?;
    let rtp_timestamp = read_optional(serialized, |s| s.read_u32())?;
    let receive_time = read_optional(serialized, |s| s.read_f64())?;
    let capture_time = read_optional(serialized, |s| s.read_f64())?;
    let sender_capture_time_offset = read_optional(serialized, |s| s.read_f64())?;
    let mime_type = read_optional(serialized, |s| s.read_utf16_string())?;
    let sequence_number = read_optional(serialized, |s| {
        i16::try_from(s.read_i32()?).map_err(|_| data_clone_error(b"Invalid RTCEncodedAudioFrame sequence number"))
    })?;
    let audio_level = read_optional(serialized, |s| s.read_f64())?;
    let fields = RawFields {
        synchronization_source: synchronization_source.into(),
        payload_type: payload_type.into(),
        contributing_sources: contributing_sources.as_deref().map(ak::Span::new).into(),
        rtp_timestamp: rtp_timestamp.into(),
        receive_time: receive_time.into(),
        capture_time: capture_time.into(),
        sender_capture_time_offset: sender_capture_time_offset.into(),
        mime_type: mime_type.as_ref().map(Utf16String::raw_identity).into(),
        sequence_number: sequence_number.into(),
        audio_level: audio_level.into(),
    };

    // 2. Set value.[[data]] to the sub-deserialization of serialized.[[data]].
    let data = serialized.read_value()?;
    let data = data
        .is_object()
        .then(|| data.as_object())
        .and_then(|object| object.downcast::<ArrayBuffer>())
        .ok_or_else(|| data_clone_error(b"RTCEncodedAudioFrame.[[data]] did not deserialize to an ArrayBuffer"))?;
    // SAFETY: The realm and buffer are live; C++ borrows the metadata and copies it into the new frame.
    let frame = unsafe { web_rtc_encoded_audio_frame_create(realm.as_ptr().cast(), &fields, data.as_ptr().cast()) };
    // SAFETY: C++ returns the live wrapper of the new frame.
    Ok(unsafe { Gc::from_non_null(NonNull::new(frame.cast()).expect("LibWeb creates the frame")) })
}
