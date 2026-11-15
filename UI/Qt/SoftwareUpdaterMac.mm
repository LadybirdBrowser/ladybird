/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/SoftwareUpdaterMac.h>

#import <Sparkle/Sparkle.h>

namespace Ladybird {

namespace Detail {

struct SparkleUpdater {
    AK_ALLOC_WITH_KMALLOC;

    SPUStandardUpdaterController* controller { nil };
};

}

// Sparkle finds updates in the appcast that the app's Info.plist names. The Info.plist also keeps it from looking for
// them by itself, so it only looks when the user asks.
NonnullOwnPtr<SoftwareUpdater> SoftwareUpdater::create()
{
    auto sparkle = make<Detail::SparkleUpdater>();
    sparkle->controller = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES updaterDelegate:nil userDriverDelegate:nil];
    return adopt_own(*new SoftwareUpdater(move(sparkle)));
}

SoftwareUpdater::SoftwareUpdater(NonnullOwnPtr<Detail::SparkleUpdater> sparkle)
    : m_sparkle(move(sparkle))
{
}

SoftwareUpdater::~SoftwareUpdater() = default;

void SoftwareUpdater::check_for_updates()
{
    [m_sparkle->controller checkForUpdates:nil];
}

}
