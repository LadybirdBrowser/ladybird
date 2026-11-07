/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Bindings/AudioParamMap.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::WebAudio {

class WEB_API AudioParamMap final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AudioParamMap, audio_param_map)

public:
    static GC::Ref<AudioParamMap> create();
    void set_entry(Utf16FlyString key, GC::Ref<AudioParam> value);
    GC::RootVector<GC::Ref<AudioParam>> values() const;
};

}
