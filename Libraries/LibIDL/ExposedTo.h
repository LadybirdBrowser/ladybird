/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Error.h>
#include <AK/StringView.h>

namespace IDL {

enum class ExposedTo {
    Nobody = 0x0,
    DedicatedWorker = 0x1,
    SharedWorker = 0x2,
    ServiceWorker = 0x4,
    AudioWorklet = 0x8,
    Window = 0x10,
    ShadowRealm = 0x20,
    Worklet = 0x40,
    PaintWorklet = 0x80,
    LayoutWorklet = 0x100,
    // FIXME: Categorize PaintWorklet and LayoutWorklet once we have them and know what they are.
    AllWorkers = DedicatedWorker | SharedWorker | ServiceWorker | AudioWorklet, // FIXME: Is "AudioWorklet" a Worker? We'll assume it is for now (here, and line below)
    All = AllWorkers | Window | ShadowRealm | Worklet,
};
AK_ENUM_BITWISE_OPERATORS(ExposedTo);

ErrorOr<ExposedTo> parse_exposure_set(StringView interface_name, StringView exposed);

}
