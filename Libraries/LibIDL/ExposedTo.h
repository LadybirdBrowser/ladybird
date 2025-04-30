/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIDL/Types.h>

namespace IDL {

enum ExposedTo {
    Nobody = 0x0,
    DedicatedWorker = 0x1,
    SharedWorker = 0x2,
    ServiceWorker = 0x4,
    AudioWorklet = 0x8,
    Window = 0x10,
    ShadowRealm = 0x20,
    Worklet = 0x40,
    AllWorkers = DedicatedWorker | SharedWorker | ServiceWorker | AudioWorklet, // FIXME: Is "AudioWorklet" a Worker? We'll assume it is for now (here, and line below)
    All = AllWorkers | Window | ShadowRealm | Worklet,
};
AK_ENUM_BITWISE_OPERATORS(ExposedTo);

ErrorOr<ExposedTo> parse_exposure_set(StringView interface_name, StringView exposed);

}
