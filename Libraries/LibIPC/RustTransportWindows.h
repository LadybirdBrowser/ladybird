/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace IPC {

struct RustWindowsTransport;
struct RustWindowsMessage;

}

extern "C" {
IPC::RustWindowsTransport* ipc_windows_transport_adopt(int);
IPC::RustWindowsTransport* ipc_windows_transport_take_over();
bool ipc_windows_transport_pair(IPC::RustWindowsTransport**, int*);
void ipc_windows_transport_destroy(IPC::RustWindowsTransport*);
void ipc_windows_transport_close(IPC::RustWindowsTransport*);
bool ipc_windows_transport_is_open(IPC::RustWindowsTransport const*);
bool ipc_windows_transport_send(IPC::RustWindowsTransport*, u8 const*, size_t, int const*, size_t);
IPC::RustWindowsMessage* ipc_windows_transport_receive(IPC::RustWindowsTransport*);
u8 const* ipc_windows_message_bytes(IPC::RustWindowsMessage const*, size_t*);
size_t ipc_windows_message_attachment_count(IPC::RustWindowsMessage const*);
int ipc_windows_message_take_attachment(IPC::RustWindowsMessage*);
void ipc_windows_message_destroy(IPC::RustWindowsMessage*);
int ipc_windows_descriptor_clone(int);
void ipc_windows_descriptor_close(int);
}
