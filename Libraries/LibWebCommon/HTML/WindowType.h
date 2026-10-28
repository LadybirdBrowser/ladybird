/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HTML {

enum class WindowType {
    ExistingOrNone,
    NewAndUnrestricted,
    NewWithNoOpener,
};

}
