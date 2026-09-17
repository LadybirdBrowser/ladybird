/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Fetch/Engine/BodyIntent.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWebCommon/FileAPI/SerializedBlobURLEntry.h>

namespace Web::Fetch::Engine {

struct StartFetchInput {
    FetchId fetch_id;
    BodyIntent body_intent;

    // Resolved by the agent that starts the fetch, at fetch start.
    Optional<FileAPI::SerializedBlobURLEntry> blob_url_entry;
};

}
