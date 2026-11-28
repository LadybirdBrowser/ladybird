/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <LibWeb/WebRTC/EncodedFrame.h>
#include <LibWebCommon/HTML/SerializationRecords.h>

namespace Web::HTML {

class DedicatedWorkerThreadLink : public AtomicRefCounted<DedicatedWorkerThreadLink> {
public:
    virtual ~DedicatedWorkerThreadLink() = default;

    virtual void terminate() = 0;

    virtual void rtc_transform_init(u64 transform_id, SerializedTransferRecord options_record) = 0;
    virtual void rtc_transform_teardown(u64 transform_id) = 0;
    virtual void rtc_transform_encoded_frame(u64 transform_id, WebRTC::EncodedFrame) = 0;
};

}
