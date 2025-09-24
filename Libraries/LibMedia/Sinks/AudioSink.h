/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Track.h>

namespace Media {

class MEDIA_API AudioSink : public AtomicRefCounted<AudioSink> {
public:
    virtual ~AudioSink() = default;

    virtual void set_provider(Track const&, RefPtr<AudioDataProvider> const&) = 0;
    virtual RefPtr<AudioDataProvider> provider(Track const&) const = 0;
};

}
