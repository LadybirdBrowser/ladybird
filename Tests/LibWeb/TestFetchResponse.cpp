/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Loader/LoaderConfig.h>

namespace {

class TransferTrackingResponse final : public Web::Fetch::Infrastructure::Response {
public:
    static NonnullRefPtr<TransferTrackingResponse> create()
    {
        return adopt_ref(*new TransferTrackingResponse);
    }

    bool transfer_lease_was_released() const { return m_transfer_lease_was_released; }
    virtual void release_request_transfer_lease() const override { m_transfer_lease_was_released = true; }

private:
    TransferTrackingResponse()
        : Response(HTTP::HeaderList::create())
    {
    }

    mutable bool m_transfer_lease_was_released { false };
};

}

TEST_CASE(javascript_bytecode_cache_memory_cache_request_headers_are_cloned)
{
    auto request_headers = HTTP::HeaderList::create({
        { "User-Agent"sv, "Ladybird"sv },
    });
    auto response = Web::Fetch::Infrastructure::Response::create();
    response->set_javascript_bytecode_cache_memory_cache_request_headers(request_headers);

    auto cloned_response = response->clone();
    auto const& cloned_request_headers = cloned_response->javascript_bytecode_cache_memory_cache_request_headers();
    request_headers->set({ "User-Agent"sv, "Changed"sv });

    VERIFY(cloned_request_headers.has_value());
    EXPECT_EQ((*cloned_request_headers)->get("User-Agent"sv), Optional<ByteString> { "Ladybird" });
}

TEST_CASE(http_redirect_fetch_releases_intermediate_response_transfer_lease)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto request_url = URL::Parser::basic_parse("https://example.test/start"sv);
    VERIFY(request_url.has_value());

    auto request = Web::Fetch::Infrastructure::Request::create();
    request->set_url_list({ request_url.release_value() });
    request->set_redirect_count(20);

    auto algorithms = Web::Fetch::Infrastructure::FetchAlgorithms::create(Web::Fetch::Infrastructure::FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent });
    Web::Fetch::Infrastructure::FetchControllerRegistry controller_registry;
    auto controller = GC::make_root(Web::Fetch::Infrastructure::FetchController::create(controller_registry));
    auto fetch_params = Web::Fetch::Infrastructure::FetchParams::create(*controller, request, algorithms, Web::Fetch::Infrastructure::FetchTimingInfo::create(), Web::current_loader_config());
    auto response = TransferTrackingResponse::create();
    response->set_status(302);
    response->set_header_list(HTTP::HeaderList::create({ { "Location"sv, "/redirected"sv } }));

    auto pending_response = Web::Fetch::Fetching::http_redirect_fetch(realm, fetch_params, response);

    VERIFY(pending_response);
    EXPECT(response->transfer_lease_was_released());
}
