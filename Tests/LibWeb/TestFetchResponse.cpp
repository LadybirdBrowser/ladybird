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

namespace {

class TransferTrackingResponse final : public Web::Fetch::Infrastructure::Response {
    GC_CELL(TransferTrackingResponse, Web::Fetch::Infrastructure::Response);
    GC_DECLARE_ALLOCATOR(TransferTrackingResponse);

public:
    static GC::Ref<TransferTrackingResponse> create(JS::VM& vm)
    {
        return vm.heap().allocate<TransferTrackingResponse>();
    }

    bool was_released_for_transfer() const { return m_was_released_for_transfer; }
    virtual void release_request_for_transfer() const override { m_was_released_for_transfer = true; }

private:
    TransferTrackingResponse()
        : Response(HTTP::HeaderList::create())
    {
    }

    mutable bool m_was_released_for_transfer { false };
};

GC_DEFINE_ALLOCATOR(TransferTrackingResponse);

}

TEST_CASE(javascript_bytecode_cache_memory_cache_request_headers_are_cloned)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto request_headers = HTTP::HeaderList::create({
        { "User-Agent"sv, "Ladybird"sv },
    });
    auto response = Web::Fetch::Infrastructure::Response::create(*vm);
    response->set_javascript_bytecode_cache_memory_cache_request_headers(request_headers);

    auto cloned_response = response->clone(realm);
    auto const& cloned_request_headers = cloned_response->javascript_bytecode_cache_memory_cache_request_headers();
    request_headers->set({ "User-Agent"sv, "Changed"sv });

    VERIFY(cloned_request_headers.has_value());
    EXPECT_EQ((*cloned_request_headers)->get("User-Agent"sv), Optional<ByteString> { "Ladybird" });
}

TEST_CASE(http_redirect_fetch_releases_intermediate_response_for_transfer)
{
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    auto request_url = URL::Parser::basic_parse("https://example.test/start"sv);
    VERIFY(request_url.has_value());

    auto request = Web::Fetch::Infrastructure::Request::create(*vm);
    request->set_url_list({ request_url.release_value() });
    request->set_redirect_count(20);

    auto fetch_params = Web::Fetch::Infrastructure::FetchParams::create(*vm, request, Web::Fetch::Infrastructure::FetchTimingInfo::create(*vm));
    auto response = TransferTrackingResponse::create(*vm);
    response->set_status(302);
    response->set_header_list(HTTP::HeaderList::create({ { "Location"sv, "/redirected"sv } }));

    auto pending_response = Web::Fetch::Fetching::http_redirect_fetch(realm, fetch_params, response);

    VERIFY(pending_response);
    EXPECT(response->was_released_for_transfer());
}
