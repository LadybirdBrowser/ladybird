/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <UI/Qt/SoftwareUpdaterMac.h>

#import <Sparkle/Sparkle.h>

// While it offers the latest version as an update, the running version counts as older than itself, so that Sparkle
// offers to install the newest version of the appcast even when it is the one running. Sparkle's installer refuses
// only downgrades, so installing it again goes through. Sparkle tells versions that look the same apart by their build
// numbers, which are the same as well, so the versions show as they are.
@interface LadybirdUpdaterDelegate : NSObject <SPUUpdaterDelegate, SPUStandardUserDriverDelegate, SUVersionComparison, SUVersionDisplay>

@property (atomic) BOOL offersLatestVersion;
@property (nonatomic, copy) void (^willRelaunch)(void);

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

- (void)updaterWillRelaunchApplication:(SPUUpdater*)updater
{
    if (self.willRelaunch)
        self.willRelaunch();
}

@end

// Sparkle only runs once the user chose to install an update in Ladybird's own notice, so it installs the update and
// relaunches without asking again. Its progress and its alerts show in Sparkle's own windows. The window that says
// Sparkle is checking for updates is left out: Sparkle's update window would replace it, but that window is skipped.
@interface LadybirdUserDriver : NSObject <SPUUserDriver>

- (instancetype)initWithStandardUserDriver:(SPUStandardUserDriver*)standardUserDriver;

@end

@implementation LadybirdUserDriver
{
    SPUStandardUserDriver* _standard;
}

- (instancetype)initWithStandardUserDriver:(SPUStandardUserDriver*)standardUserDriver
{
    if (self = [super init])
        _standard = standardUserDriver;
    return self;
}

- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest*)request reply:(void (^)(SUUpdatePermissionResponse*))reply
{
    [_standard showUpdatePermissionRequest:request reply:reply];
}

- (void)showUserInitiatedUpdateCheckWithCancellation:(void (^)(void))cancellation
{
}

- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem*)appcastItem state:(SPUUserUpdateState*)state reply:(void (^)(SPUUserUpdateChoice))reply
{
    // Sparkle cannot install an update that only informs, so the user sees what it says instead. A check that the user did
    // not start, such as one that a user default scheduled, asks the user as Sparkle normally does.
    if (appcastItem.informationOnlyUpdate || !state.userInitiated) {
        [_standard showUpdateFoundWithAppcastItem:appcastItem state:state reply:reply];
        return;
    }
    reply(SPUUserUpdateChoiceInstall);
}

- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData*)downloadData
{
    [_standard showUpdateReleaseNotesWithDownloadData:downloadData];
}

- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError*)error
{
    [_standard showUpdateReleaseNotesFailedToDownloadWithError:error];
}

- (void)showUpdateNotFoundWithError:(NSError*)error acknowledgement:(void (^)(void))acknowledgement
{
    [_standard showUpdateNotFoundWithError:error acknowledgement:acknowledgement];
}

- (void)showUpdaterError:(NSError*)error acknowledgement:(void (^)(void))acknowledgement
{
    [_standard showUpdaterError:error acknowledgement:acknowledgement];
}

- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation
{
    [_standard showDownloadInitiatedWithCancellation:cancellation];
}

- (void)showDownloadDidReceiveExpectedContentLength:(uint64_t)expectedContentLength
{
    [_standard showDownloadDidReceiveExpectedContentLength:expectedContentLength];
}

- (void)showDownloadDidReceiveDataOfLength:(uint64_t)length
{
    [_standard showDownloadDidReceiveDataOfLength:length];
}

- (void)showDownloadDidStartExtractingUpdate
{
    [_standard showDownloadDidStartExtractingUpdate];
}

- (void)showExtractionReceivedProgress:(double)progress
{
    [_standard showExtractionReceivedProgress:progress];
}

- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply
{
    reply(SPUUserUpdateChoiceInstall);
}

- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)applicationTerminated retryTerminatingApplication:(void (^)(void))retryTerminatingApplication
{
    [_standard showInstallingUpdateWithApplicationTerminated:applicationTerminated retryTerminatingApplication:retryTerminatingApplication];
}

- (void)showUpdateInstalledAndRelaunched:(BOOL)relaunched acknowledgement:(void (^)(void))acknowledgement
{
    [_standard showUpdateInstalledAndRelaunched:relaunched acknowledgement:acknowledgement];
}

- (void)showUpdateInFocus
{
    [_standard showUpdateInFocus];
}

- (void)dismissUpdateInstallation
{
    [_standard dismissUpdateInstallation];
}

@end

namespace Ladybird {

namespace Detail {

struct SparkleUpdater {
    AK_ALLOC_WITH_KMALLOC;

    // Sparkle holds its delegates weakly.
    LadybirdUpdaterDelegate* delegate { nil };
    LadybirdUserDriver* user_driver { nil };
    SPUUpdater* updater { nil };
};

}

// Sparkle finds updates in the appcast that the app's Info.plist names. The Info.plist also keeps it from looking for
// them by itself, so it only looks when the user asks.
NonnullOwnPtr<SoftwareUpdater> SoftwareUpdater::create()
{
    auto sparkle = make<Detail::SparkleUpdater>();
    sparkle->delegate = [[LadybirdUpdaterDelegate alloc] init];
    auto* standard_user_driver = [[SPUStandardUserDriver alloc] initWithHostBundle:NSBundle.mainBundle delegate:sparkle->delegate];
    sparkle->user_driver = [[LadybirdUserDriver alloc] initWithStandardUserDriver:standard_user_driver];
    sparkle->updater = [[SPUUpdater alloc] initWithHostBundle:NSBundle.mainBundle applicationBundle:NSBundle.mainBundle userDriver:sparkle->user_driver delegate:sparkle->delegate];

    NSError* error = nil;
    if (![sparkle->updater startUpdater:&error])
        warnln("Unable to start Sparkle: {}", error.localizedDescription.UTF8String);
    return adopt_own(*new SoftwareUpdater(move(sparkle)));
}

SoftwareUpdater::SoftwareUpdater(NonnullOwnPtr<Detail::SparkleUpdater> sparkle)
    : m_sparkle(move(sparkle))
{
    m_sparkle->delegate.willRelaunch = ^{
        if (on_will_relaunch)
            on_will_relaunch();
    };
}

SoftwareUpdater::~SoftwareUpdater() = default;

void SoftwareUpdater::install_update()
{
    [m_sparkle->updater checkForUpdates];
}

void SoftwareUpdater::install_latest_version()
{
    m_sparkle->delegate.offersLatestVersion = YES;
    [m_sparkle->updater checkForUpdates];
}

}
