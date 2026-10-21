/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use libipc_rust::DecodeError;
use rust_echo_server::messages::Stop;

#[test]
fn generated_messages_check_the_endpoint_and_message_number() {
    let (bytes, _) = Stop.encode_message().unwrap();
    assert!(matches!(Message::decode(&bytes, vec![]), Ok(Message::Stop(_))));
    for range in [0..4, 4..8] {
        let mut invalid = bytes.clone();
        invalid[range].fill(0);
        assert!(matches!(
            Message::decode(&invalid, vec![]),
            Err(DecodeError::InvalidValue(_))
        ));
    }
}

#[test]
fn generated_messages_refuse_truncated_bytes_and_accept_trailing_bytes_like_cpp() {
    let (mut bytes, _) = Stop.encode_message().unwrap();
    for length in 0..bytes.len() {
        assert!(Message::decode(&bytes[..length], vec![]).is_err());
    }
    bytes.push(0);
    assert!(matches!(Message::decode(&bytes, vec![]), Ok(Message::Stop(_))));
}

#[test]
fn generated_messages_accept_unused_attachments_like_cpp() {
    let (bytes, _) = Stop.encode_message().unwrap();
    let (mut reader, writer) = std::io::pipe().unwrap();
    let attachment = libipc_rust::Attachment::from_fd(writer.into()).unwrap();
    assert!(matches!(
        Message::decode(&bytes, vec![attachment]),
        Ok(Message::Stop(_))
    ));
    assert_eq!(std::io::Read::read(&mut reader, &mut [0]).unwrap(), 0);
}

#[test]
fn generated_utf8_validation_matches_cpp() {
    use libipc_rust::wire::ByteString;
    use libipc_rust::{Encode, EncodeError, Encoder};
    use rust_echo_server::messages::{EchoUtf8, EchoUtf8Response};

    for bytes in [
        b"".as_slice(),
        "héllo".as_bytes(),
        &[0xed, 0xa0, 0x80, 0xed, 0xbf, 0xbf],
    ] {
        let (encoded, attachments) = EchoUtf8 {
            text: ByteString(bytes.to_vec()),
            bytes: bytes.to_vec(),
        }
        .encode_message()
        .unwrap();
        let Message::EchoUtf8(decoded) = Message::decode(&encoded, attachments).unwrap() else {
            panic!("expected EchoUtf8");
        };
        assert_eq!(decoded.text.0, bytes);
        assert_eq!(decoded.bytes, bytes);
        let (encoded, attachments) = EchoUtf8Response {
            text: decoded.text,
            bytes: decoded.bytes,
        }
        .encode_message()
        .unwrap();
        let Message::EchoUtf8Response(decoded) = Message::decode(&encoded, attachments).unwrap() else {
            panic!("expected EchoUtf8Response");
        };
        assert_eq!(decoded.text.0, bytes);
        assert_eq!(decoded.bytes, bytes);
    }
    for bytes in [
        b"\xff".as_slice(),
        b"\xc0\x80",
        b"\xe2\x82",
        b"\xed\xa0\x80\xff",
        b"\xf4\x90\x80\x80",
    ] {
        for (text, bytes) in [(bytes.to_vec(), vec![]), (vec![], bytes.to_vec())] {
            for response in [false, true] {
                let mut encoded = Encoder::new();
                encoded.encode(&rust_echo_server::ENDPOINT_MAGIC).unwrap();
                let id = if response {
                    EchoUtf8Response::MESSAGE_ID
                } else {
                    EchoUtf8::MESSAGE_ID
                };
                encoded.encode(&id).unwrap();
                encoded.encode(&ByteString(text.clone())).unwrap();
                encoded.encode(&bytes).unwrap();
                assert!(matches!(
                    Message::decode(&encoded.finish().0, vec![]),
                    Err(DecodeError::InvalidUtf8)
                ));
                let mut encoder = Encoder::new();
                let (borrowed, owned) = if response {
                    let message = EchoUtf8Response {
                        text: ByteString(text.clone()),
                        bytes: bytes.clone(),
                    };
                    (message.encode(&mut encoder), message.encode_message())
                } else {
                    let message = EchoUtf8 {
                        text: ByteString(text.clone()),
                        bytes: bytes.clone(),
                    };
                    (message.encode(&mut encoder), message.encode_message())
                };
                assert!(matches!(borrowed, Err(EncodeError::InvalidValue("UTF-8"))));
                assert!(matches!(owned, Err(EncodeError::InvalidValue("UTF-8"))));
            }
        }
    }
}

#[test]
fn generated_owned_messages_move_files() {
    use libipc_rust::wire::File;
    use std::os::fd::AsRawFd;

    let (_reader, writer) = std::io::pipe().unwrap();
    let descriptor = writer.as_raw_fd();
    let files = vec![File(writer.into())];
    let (bytes, attachments) = EchoFilesResponse { files }.encode_message().unwrap();
    let Message::EchoFilesResponse(decoded) = Message::decode(&bytes, attachments).unwrap() else {
        panic!("expected EchoFilesResponse");
    };
    assert_eq!(decoded.files.len(), 1);
    #[cfg(not(target_vendor = "apple"))]
    assert_eq!(decoded.files[0].0.as_raw_fd(), descriptor);
    #[cfg(target_vendor = "apple")]
    let _ = descriptor;
}

#[test]
fn server_flushes_posted_replies_before_returning_on_errors() {
    use libipc_rust::wire::ByteString;
    use rust_echo_server::messages::{EchoUtf8, EchoUtf8Response};

    for invalid in [
        vec![0],
        EchoUtf8Response {
            text: ByteString(vec![]),
            bytes: vec![],
        }
        .encode_message()
        .unwrap()
        .0,
    ] {
        let (server, handle) = Transport::create_paired().unwrap();
        #[cfg(target_vendor = "apple")]
        let client = Transport::from_handle(handle).unwrap();
        #[cfg(not(target_vendor = "apple"))]
        let client = Transport::new(handle).unwrap();
        let thread = std::thread::spawn(move || serve(server));
        for number in 0..100u32 {
            reply(
                &client,
                EchoUtf8 {
                    text: ByteString(number.to_string().into_bytes()),
                    bytes: vec![],
                },
            )
            .unwrap();
        }
        client.post_message(invalid, vec![]).unwrap();
        for number in 0..100u32 {
            let received = client.receive().unwrap();
            let (bytes, attachments) = received.into_parts();
            let Message::EchoUtf8Response(response) = Message::decode(bytes.as_ref(), attachments).unwrap() else {
                panic!("expected EchoUtf8Response");
            };
            assert_eq!(response.text.0, number.to_string().as_bytes());
        }
        thread.join().unwrap();
        assert!(client.receive().is_none());
    }
}

#[test]
fn generated_encoders_refuse_what_decoders_refuse() {
    use crate::ipc_types::{RustEcho_Record, RustEcho_Shape};
    use libipc_rust::wire::{ByteString, Utf8String};
    use libipc_rust::{EncodeError, Encoder};

    let record = || RustEcho_Record {
        id: 7,
        label: Utf8String::from("label"),
        name: ByteString(b"name".to_vec()),
        bytes: b"bytes".to_vec(),
        values: vec![1, 2, 3],
        scale: 1.5,
        shape: RustEcho_Shape::Square,
    };
    assert!(Encoder::new().encode(&record()).is_ok());
    let invalidations: [fn(&mut RustEcho_Record); 7] = [
        |record| record.id = 0,
        |record| record.label = Utf8String::from("123456789"),
        |record| record.name = ByteString(b"123456789".to_vec()),
        |record| record.bytes = b"123456789".to_vec(),
        |record| record.values.push(4),
        |record| record.scale = f64::NAN,
        |record| record.scale = f64::INFINITY,
    ];
    for invalidate in invalidations {
        let mut invalid = record();
        invalidate(&mut invalid);
        assert!(matches!(
            Encoder::new().encode(&invalid),
            Err(EncodeError::InvalidValue(_))
        ));
        assert!(matches!(
            Encoder::new().encode_owned(invalid),
            Err(EncodeError::InvalidValue(_))
        ));
    }
}
