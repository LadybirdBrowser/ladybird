/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Types.h>
#include <AK/Windows.h>
#include <LibMedia/Audio/ChannelMap.h>

struct IMMDevice;

namespace AudioServer {

class ScopedComInitialization {
public:
    static ErrorOr<ScopedComInitialization> create();

    ScopedComInitialization(ScopedComInitialization const&) = delete;
    ScopedComInitialization& operator=(ScopedComInitialization const&) = delete;

    ScopedComInitialization(ScopedComInitialization&& other);
    ScopedComInitialization& operator=(ScopedComInitialization&& other);

    ~ScopedComInitialization();

private:
    explicit ScopedComInitialization(bool initialized);

    bool m_initialized { false };
};

ByteString wide_string_to_utf8(wchar_t const* wide_string);
ErrorOr<ByteString> endpoint_id_for_device(IMMDevice& device);
u64 backend_handle_for_endpoint_id(ByteString const& endpoint_id);
ErrorOr<Audio::ChannelMap> convert_ksmedia_channel_bitmask_to_channel_map(u32 channel_bitmask);

}
