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
#include <LibWeb/Export.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::HTML {

struct PostedMessageDescriptor {
    SerializedTransferRecord serialize_with_transfer_result;
    Variant<Utf16String, URL::Origin> target_origin;
    URL::Origin source_origin;
    CrossProcessId source_navigable_id;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::PostedMessageDescriptor const&);

template<>
WEB_API ErrorOr<Web::HTML::PostedMessageDescriptor> decode(Decoder&);

}
