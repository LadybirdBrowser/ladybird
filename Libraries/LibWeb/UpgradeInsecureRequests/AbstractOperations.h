/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::UpgradeInsecureRequests {

WEB_API void upgrade_request_to_a_potentially_trustworthy_url_if_appropriate(Fetch::Infrastructure::Request&);

}
