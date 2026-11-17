/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::Painting {

struct DisplayListDumpOptions {
    bool owners { true };
    bool text { true };
    bool fonts { false };
    bool record_ids { false };
    bool chunks { false };
    bool raw_node_indices { false };
    bool jsonl { false };
};

}
