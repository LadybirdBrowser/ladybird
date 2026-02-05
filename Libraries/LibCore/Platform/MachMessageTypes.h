/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/Types.h>

#if !defined(AK_OS_MACH)
#    error "This file is only available on Mach platforms"
#endif

#include <mach/mach.h>

namespace Core::Platform {

struct MessageBodyWithSelfTaskPort {
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port_descriptor;
    mach_msg_audit_trailer_t trailer;
};

struct MessageWithSelfTaskPort {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port_descriptor;
};

struct BackingStoreMetadata {
    u64 page_id { 0 };
    i32 back_backing_store_id { 0 };
    i32 front_backing_store_id { 0 };
};

struct MessageBodyWithBackingStores {
    mach_msg_body_t body;
    mach_msg_port_descriptor_t front_descriptor;
    mach_msg_port_descriptor_t back_descriptor;
    BackingStoreMetadata metadata;
    mach_msg_audit_trailer_t trailer;
};

struct MessageWithBackingStores {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t front_descriptor;
    mach_msg_port_descriptor_t back_descriptor;
    BackingStoreMetadata metadata;
};

struct ReceivedMachMessage {
    mach_msg_header_t header;
    union {
        MessageBodyWithSelfTaskPort parent;
        MessageBodyWithBackingStores parent_iosurface;
    } body;
};

static constexpr mach_msg_id_t SELF_TASK_PORT_MESSAGE_ID = 0x1234CAFE;
static constexpr mach_msg_id_t BACKING_STORE_IOSURFACES_MESSAGE_ID = 0x1234CAFF;

}
