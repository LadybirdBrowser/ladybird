/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::WebDriver {

template<typename T>
using Promise = Core::Promise<T, Web::WebDriver::Error>;

}
