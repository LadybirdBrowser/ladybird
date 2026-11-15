/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

    void check_for_updates();
    void show_update_dialog_for_latest_version();

private:
    explicit SoftwareUpdater(NonnullOwnPtr<Detail::SparkleUpdater>);

    NonnullOwnPtr<Detail::SparkleUpdater> m_sparkle;
};

}
