/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Forward.h>

namespace WebView {

void disable_site_isolation();
[[nodiscard]] bool is_url_suitable_for_same_process_navigation(URL::URL const& current_url, URL::URL const& target_url);

}
