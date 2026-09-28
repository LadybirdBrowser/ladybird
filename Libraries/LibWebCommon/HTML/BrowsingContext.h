/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>

namespace Web::HTML {

WEBCOMMON_API URL::Origin determine_the_origin(Optional<URL::URL const&>, SandboxingFlagSet, Optional<URL::Origin> source_origin);

WEBCOMMON_API bool url_matches_about_blank(URL::URL const& url);
WEBCOMMON_API bool url_matches_about_srcdoc(URL::URL const& url);

}
