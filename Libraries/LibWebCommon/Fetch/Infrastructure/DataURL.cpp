/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Karol Kosek <krkk@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWebCommon/Fetch/Infrastructure/DataURL.h>
#include <LibWebCommon/RustFFI.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#data-url-processor
ErrorOr<DataURL> process_data_url(URL::URL const& data_url)
{
    // 1. Assert: dataURL’s scheme is "data".
    VERIFY(data_url.scheme() == "data");

    // 2. Let input be the result of running the URL serializer on dataURL with exclude fragment set to true.
    auto input = data_url.serialize(URL::ExcludeFragment::Yes);

    // NB: The rest of the steps are in Rust/src/fetch/data_url.rs.
    struct Result {
        ByteBuffer body;
        Optional<MimeSniff::MimeType> mime_type;
    } result;
    auto on_body = [](void* ctx, ReadonlyBytes bytes) {
        static_cast<Result*>(ctx)->body = MUST(ByteBuffer::copy(bytes));
    };
    auto on_mime_type = [](void* ctx, FFI::FfiMimeType const* mime_type) {
        static_cast<Result*>(ctx)->mime_type = MimeSniff::MimeType::from_ffi(*mime_type);
    };
    if (!FFI::fetch_process_data_url(input.bytes(), &result, on_body, on_mime_type))
        return Error::from_string_literal("Invalid data: URL");
    return DataURL { result.mime_type.release_value(), move(result.body) };
}

}
