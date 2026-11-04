/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/StorageAPI/StorageAreaId.h>
#include <LibWebCommon/StorageAPI/StorageEndpoint.h>

namespace Web::StorageAPI {

// What the process that owns the storage areas answers when this process connects to one: the area's id and
// everything the bottle holds. No id means the area couldn't be resolved, and the mirror stays detached.
struct StorageAreaConnection {
    Optional<StorageAreaId> id;
    Vector<Utf16String> keys;
    Vector<Utf16String> values;
    u64 usage { 0 };
};

// The process's one link to the storage areas' owner, for the mirrors of the storage bottles, which are implemented
// in Rust (Libraries/LibWeb/Bindings/Rust/src/storage_api/storage_area_mirror.rs). WebContent connects it to the
// browser process; a process without one leaves every mirror detached.
class WEB_API StorageAreaMirrorClient {
public:
    virtual ~StorageAreaMirrorClient() = default;

    virtual StorageAreaConnection connect_to_storage_area(PageId, StorageEndpointType, HTML::EnvironmentId const&) = 0;
    virtual void disconnect_from_storage_area(StorageAreaId) = 0;
    virtual void set_storage_item(StorageAreaId, Utf16String const& key, Utf16String const& value, String const& document_url) = 0;
    virtual void remove_storage_item(StorageAreaId, Utf16String const& key, String const& document_url) = 0;
    virtual void clear_storage_area(StorageAreaId, String const& document_url) = 0;
};

WEB_API void set_storage_area_mirror_client(StorageAreaMirrorClient*);

// What the owner relays, found by area id. A change for an area this process no longer mirrors is dropped.
WEB_API void storage_area_did_change(StorageAreaId, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url);
WEB_API void storage_area_did_acknowledge_write(StorageAreaId);
WEB_API void storage_area_did_refuse_write(StorageAreaId, Vector<Utf16String> keys, Vector<Utf16String> values);
WEB_API void storage_area_did_reset(StorageAreaId, Vector<Utf16String> keys, Vector<Utf16String> values);

}
