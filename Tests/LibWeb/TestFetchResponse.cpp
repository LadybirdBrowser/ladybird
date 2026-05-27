/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

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
