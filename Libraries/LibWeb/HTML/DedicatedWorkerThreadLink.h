/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <LibWebCommon/HTML/SerializationRecords.h>

namespace Web::HTML {

class DedicatedWorkerThreadLink : public AtomicRefCounted<DedicatedWorkerThreadLink> {
public:
    virtual ~DedicatedWorkerThreadLink() = default;

    virtual void terminate() = 0;

    virtual void rtc_transform_init(u64 transform_id, SerializedTransferRecord options_record) = 0;
    virtual void rtc_transform_teardown(u64 transform_id) = 0;
    virtual void rtc_transform_encoded_audio_frame(u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number) = 0;
};

}
