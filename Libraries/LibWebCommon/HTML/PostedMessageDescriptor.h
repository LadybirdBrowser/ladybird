/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/SerializationRecords.h>

namespace Web::HTML {

struct PostedMessageDescriptor {
    SerializedTransferRecord serialize_with_transfer_result;
    Variant<Utf16String, URL::Origin> target_origin;
    URL::Origin source_origin;
    Optional<CrossProcessId> source_navigable_id;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::PostedMessageDescriptor const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::PostedMessageDescriptor> decode(Decoder&);

}
