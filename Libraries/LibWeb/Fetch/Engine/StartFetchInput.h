/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibWeb/Fetch/Engine/EngineEventSink.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/NetworkLoader.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Engine {

// What main fetch runs its in-parallel steps from, starting at step 12.
struct MainFetchInput {
    NonnullRefPtr<Infrastructure::FetchParams const> fetch_params;
    // The response main fetch's earlier steps set: null, or a network error.
    RefPtr<Infrastructure::Response> response;
};

struct StartFetchInput {
    StartFetchInput(Badge<Infrastructure::FetchParams>, FetchId fetch_id, MainFetchInput main_fetch, NonnullOwnPtr<EngineEventSink> event_sink, NonnullRefPtr<NetworkLoader> network_loader)
        : fetch_id(fetch_id)
        , main_fetch(move(main_fetch))
        , event_sink(move(event_sink))
        , network_loader(move(network_loader))
    {
    }

    // An agent's fetch ids rise in the order it posts their starts.
    FetchId fetch_id;
    MainFetchInput main_fetch;
    NonnullOwnPtr<EngineEventSink> event_sink;
    NonnullRefPtr<NetworkLoader> network_loader;
};

}
