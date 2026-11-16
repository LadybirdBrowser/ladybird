/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/PNGWriter.h>

#import <UI/Qt/Conversions.h>

namespace Ladybird {

NSURL* url_to_ns_url(URL::URL const& url)
{
    auto serialized_url = url.serialize();
    return [NSURL URLWithDataRepresentation:string_to_ns_data(serialized_url.bytes_as_string_view()) relativeToURL:nil];
}

}
