/*
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Forward.h>

#pragma once

namespace Web::HTML {

using DeserializationMemory = GC::RootVector<JS::Value>;
using SerializationRecord = Vector<u32>;
using SerializationMemory = HashMap<GC::Root<JS::Value>, u32>;

}
