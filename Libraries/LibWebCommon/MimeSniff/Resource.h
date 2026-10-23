/*
 * Copyright (c) 2023, Kemal Zebari <kemalzebra@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebCommon/Export.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::MimeSniff {

enum class SniffingContext {
    None,
    Browsing,
    Image,
    AudioOrVideo,
    Font,

    // Non-standard but used in cases where the spec expects us to only run
    // https://mimesniff.spec.whatwg.org/#sniffing-a-mislabeled-binary-resource
    TextOrBinary,
};

struct SniffingConfiguration {
    SniffingContext sniffing_context { SniffingContext::None };
    StringView scheme { ""sv };
    Optional<MimeType> supplied_type = {};
    bool no_sniff { false };
};

// https://mimesniff.spec.whatwg.org/#resource
class WEBCOMMON_API Resource {
public:
    static MimeType sniff(ReadonlyBytes data, SniffingConfiguration configuration = {});
};

}
