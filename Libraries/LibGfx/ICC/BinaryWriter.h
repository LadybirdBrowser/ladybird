/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Gfx::ICC {

class Profile;

// Serializes a Profile object.
// Ignores the Profile's on_disk_size() and id() and recomputes them instead.
// Also ignores and the offsets and sizes in tag data.
// But if the profile has its tag data in tag order and has a computed id,
// it's a goal that  encode(Profile::try_load_from_externally_owned_memory(bytes) returns `bytes`.
// Unconditionally computes a Profile ID (which is an MD5 hash of most of the contents, see Profile::compute_id()) and writes it to the output.
ErrorOr<ByteBuffer> encode(Profile const&);

}
