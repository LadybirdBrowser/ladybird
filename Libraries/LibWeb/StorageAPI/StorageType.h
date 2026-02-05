/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#storage-type
// A storage type is "local" or "session".
enum class StorageType {
    Local,
    Session,
};

}
