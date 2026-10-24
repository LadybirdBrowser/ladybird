/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A Rust endpoint that TestRustEchoServer speaks with over the platform's transport, using code that the IPC compiler
//! generated for both languages from RustEchoServer.ipc.

#[path = "../../../../Libraries/RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../../Libraries/RustPanic.rs"]
mod rust_panic;

use std::io::Write;

use libipc_rust::EndpointMessage;
#[cfg(all(unix, not(target_vendor = "apple")))]
use libipc_rust::transport_socket::TransportSocket as Transport;
#[cfg(windows)]
use libipc_rust::transport_windows::TransportWindows as Transport;
#[cfg(target_vendor = "apple")]
use libipc_rust::{
    mach::{MachPort, PortRight},
    transport::TransportMachPort as Transport,
};

// NB: The generated code names the types RustEchoServer.ipc declares as crate::ipc_types::Namespace_Name.
mod ipc_types {
    include!(env!("RUST_ECHO_SERVER_ENDPOINT"));
}

use ipc_types::rust_echo_server;
use rust_echo_server::{
    Message,
    messages::{EchoFilesResponse, EchoResponse, EchoUtf8Response, EchoValueResponse, WriteToFileResponse},
};

#[cfg(test)]
mod tests;

fn reply(transport: &Transport, message: impl EndpointMessage) -> std::io::Result<()> {
    let (bytes, attachments) = message.encode_message().expect("the reply encodes");
    transport.post_message(bytes, attachments)
}

fn serve(transport: Transport) {
    while let Some(received) = transport.receive() {
        let (bytes, attachments) = received.into_parts();
        let message = match Message::decode(bytes.as_ref(), attachments) {
            Ok(message) => message,
            Err(error) => {
                eprintln!("RustEchoServer: Refusing a message: {error:?}");
                break;
            }
        };
        let replied = match message {
            Message::Echo(echo) => reply(
                &transport,
                EchoResponse {
                    number: echo.number,
                    text: echo.text,
                    utf16_text: echo.utf16_text,
                    values: echo.values,
                    bytes: echo.bytes,
                },
            ),
            Message::WriteToFile(request) => {
                let mut file = std::fs::File::from(request.file.0);
                file.write_all(&request.contents.0).expect("the file is writable");
                reply(&transport, WriteToFileResponse)
            }
            Message::EchoFiles(request) => reply(&transport, EchoFilesResponse { files: request.files }),
            Message::EchoUtf8(request) => reply(
                &transport,
                EchoUtf8Response {
                    text: request.text,
                    bytes: request.bytes,
                },
            ),
            Message::EchoValue(request) => reply(&transport, EchoValueResponse { value: request.value }),
            Message::Stop(_) => break,
            Message::EchoResponse(_)
            | Message::WriteToFileResponse(_)
            | Message::EchoFilesResponse(_)
            | Message::EchoUtf8Response(_)
            | Message::EchoValueResponse(_) => {
                eprintln!("RustEchoServer: Refusing a response sent to it");
                break;
            }
        };
        if let Err(error) = replied {
            eprintln!("RustEchoServer: Could not reply: {error}");
            break;
        }
    }
    transport.flush();
}

/// Serves the endpoint on a thread of its own, taking over a receive right and a send right that this task holds.
#[cfg(target_vendor = "apple")]
#[unsafe(no_mangle)]
pub extern "C" fn rust_echo_server_serve(receive_right: u32, send_right: u32) {
    let receive_right = MachPort::adopt(receive_right, PortRight::Receive);
    let send_right = MachPort::adopt(send_right, PortRight::Send);
    let transport = Transport::new(receive_right, send_right).expect("the transport starts");
    std::thread::spawn(move || serve(transport));
}

/// Serves the endpoint on a thread of its own, taking over a connected Unix domain socket.
#[cfg(all(unix, not(target_vendor = "apple")))]
#[unsafe(no_mangle)]
pub extern "C" fn rust_echo_server_serve(socket_fd: std::os::fd::RawFd) {
    use std::os::fd::FromRawFd;
    // NB: The caller gives up its descriptor of a connected socket, which nothing else owns.
    let socket = unsafe { std::os::unix::net::UnixStream::from_raw_fd(socket_fd) };
    let transport = Transport::new(socket).expect("the transport starts");
    std::thread::spawn(move || serve(transport));
}

/// Serves the endpoint on a thread of its own, taking over one end of an overlapped duplex pipe.
///
/// # Safety
///
/// `pipe` must be an open handle of a connected pipe, which the caller gives up and nothing else owns.
#[cfg(windows)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_echo_server_serve(pipe: std::os::windows::io::RawHandle) {
    use std::os::windows::io::FromRawHandle;
    // SAFETY: Guaranteed by the caller.
    let pipe = unsafe { std::os::windows::io::OwnedHandle::from_raw_handle(pipe) };
    let transport = Transport::new(pipe).expect("the transport starts");
    std::thread::spawn(move || serve(transport));
}
