/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ICC/Tags.h>

namespace Gfx::ICC {

Optional<StringView> tag_signature_spec_name(TagSignature tag_signature)
{
    switch (tag_signature) {
#define TAG(name, id) \
    case name:        \
        return #name##sv;
        ENUMERATE_TAG_SIGNATURES(TAG)
#undef TAG
    }
    return {};
}

}
