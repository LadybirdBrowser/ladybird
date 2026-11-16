/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>

namespace Ladybird {

namespace Detail {

struct SparkleUpdater;

}

class SoftwareUpdater {
    AK_MAKE_NONCOPYABLE(SoftwareUpdater);
    AK_MAKE_NONMOVABLE(SoftwareUpdater);

public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullOwnPtr<SoftwareUpdater> create();
    ~SoftwareUpdater();

    void install_update();
    void install_latest_version();

    Function<void()> on_will_relaunch;

private:
    explicit SoftwareUpdater(NonnullOwnPtr<Detail::SparkleUpdater>);

    NonnullOwnPtr<Detail::SparkleUpdater> m_sparkle;
};

}
