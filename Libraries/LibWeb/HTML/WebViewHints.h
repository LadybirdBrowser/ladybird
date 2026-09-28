/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/TokenizedFeatures.h>
#include <LibWebCommon/HTML/WebViewHints.h>

namespace Web::HTML {

WebViewHints web_view_hints_from_tokenised_features(TokenizedFeature::Map const&, Page const&);

}
