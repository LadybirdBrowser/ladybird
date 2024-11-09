/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HTML {

enum class DocumentReadyState {
    Loading,
    Interactive,
    Complete,
};

}
