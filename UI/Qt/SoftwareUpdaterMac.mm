/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/SoftwareUpdaterMac.h>

#import <Sparkle/Sparkle.h>

// While it offers the latest version as an update, the running version counts as older than itself, so that Sparkle
// offers to install the newest version of the appcast even when it is the one running. Sparkle's installer refuses
// only downgrades, so installing it again goes through. Sparkle tells versions that look the same apart by their build
// numbers, which are the same as well, so the versions show as they are.
@interface LadybirdUpdaterDelegate : NSObject <SPUUpdaterDelegate, SPUStandardUserDriverDelegate, SUVersionComparison, SUVersionDisplay>

@property (atomic) BOOL offersLatestVersion;

@end

@implementation LadybirdUpdaterDelegate

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-implementations"
- (id<SUVersionComparison>)versionComparatorForUpdater:(SPUUpdater*)updater
{
    return self.offersLatestVersion ? self : nil;
}
#pragma clang diagnostic pop

// NB: Sparkle may call this off the main thread.
- (NSComparisonResult)compareVersion:(NSString*)versionA toVersion:(NSString*)versionB
{
    auto result = [[SUStandardVersionComparator defaultComparator] compareVersion:versionA toVersion:versionB];
    if (result == NSOrderedSame && [versionA isEqualToString:NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"]])
        return NSOrderedAscending;
    return result;
}

- (id<SUVersionDisplay>)standardUserDriverRequestsVersionDisplayer
{
    return self.offersLatestVersion ? self : nil;
}

- (NSString*)formatUpdateDisplayVersionFromUpdate:(SUAppcastItem*)update andBundleDisplayVersion:(NSString* __autoreleasing*)inOutBundleDisplayVersion withBundleVersion:(NSString*)bundleVersion
{
    return update.displayVersionString;
}

- (void)updater:(SPUUpdater*)updater didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)updateCheck error:(NSError*)error
{
    self.offersLatestVersion = NO;
}

@end

namespace Ladybird {

namespace Detail {

struct SparkleUpdater {
    AK_ALLOC_WITH_KMALLOC;

    // NB: Sparkle holds its delegate weakly.
    LadybirdUpdaterDelegate* delegate { nil };
    SPUStandardUpdaterController* controller { nil };
};

}

// Sparkle finds updates in the appcast that the app's Info.plist names. The Info.plist also keeps it from looking for
// them by itself, so it only looks when the user asks.
NonnullOwnPtr<SoftwareUpdater> SoftwareUpdater::create()
{
    auto sparkle = make<Detail::SparkleUpdater>();
    sparkle->delegate = [[LadybirdUpdaterDelegate alloc] init];
    sparkle->controller = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES updaterDelegate:sparkle->delegate userDriverDelegate:sparkle->delegate];
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

void SoftwareUpdater::show_update_dialog_for_latest_version()
{
    m_sparkle->delegate.offersLatestVersion = YES;
    [m_sparkle->controller checkForUpdates:nil];
}

}
