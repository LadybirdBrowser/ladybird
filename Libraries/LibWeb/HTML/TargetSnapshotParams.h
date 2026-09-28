/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/TargetSnapshotParams.h>

namespace Web::HTML {

TargetSnapshotParams snapshot_target_snapshot_params(LocalNavigable& target_navigable);

}
