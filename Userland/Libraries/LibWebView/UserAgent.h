/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/StringView.h>

namespace WebView {

extern OrderedHashMap<StringView, StringView> const user_agents;

}
