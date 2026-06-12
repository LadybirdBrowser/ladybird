/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibDatabase/Export.h>

namespace Database {

class Database;

using StatementID = size_t;

enum class MigrationOutcome : u8 {
    Success,        // The store's schema is now at the latest known version.
    DatabaseTooNew, // The recorded version is newer than this build understands; the database was left untouched.
};

enum class MigrationMode : u8 {
    Apply,     // Run pending migrations and record the new version.
    CheckOnly, // Determine the outcome, then roll back unconditionally without committing anything.
};

}
