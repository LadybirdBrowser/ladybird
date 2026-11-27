/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/NetworkActivity.h>
#include <LibWeb/Fetch/Engine/NetworkInterception.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>

namespace Web::Fetch::Fetching {

[[nodiscard]] WEB_API Engine::Event::ObservedRequest observed_request(Infrastructure::Request const&, Infrastructure::FetchTimingInfo const&);
[[nodiscard]] WEB_API Engine::Event::ObservedResponse observed_response(Infrastructure::Response const&);

void report_response_started(Infrastructure::FetchParams const&, Infrastructure::Response const&);
void report_response_completed(Infrastructure::FetchParams const&, Infrastructure::Response const&);
void report_fetch_error(Infrastructure::FetchParams const&, String reason);

[[nodiscard]] WEB_API bool can_wait_at_network_phase(Infrastructure::FetchParams const&, Engine::NetworkPhase);

void report_network_phase(Infrastructure::FetchParams const&, Engine::NetworkPhase, Infrastructure::Response const*);

// A decision that changes the request changes the original request too.
WEB_API void wait_at_network_phase(Infrastructure::FetchParams const&, Engine::NetworkPhase, RefPtr<Infrastructure::Response>, RefPtr<Infrastructure::Request> original_request, Function<void(Engine::NetworkDecision const&, RefPtr<Infrastructure::Response>)> continuation);

[[nodiscard]] bool webdriver_bidi_bypasses_cache(Infrastructure::FetchParams const&);

void update_request_headers_for_webdriver_bidi(Infrastructure::FetchParams const&);

void apply_generated_header_overrides(Infrastructure::Request&);

void remove_content_length_override(Infrastructure::Request&);

}
