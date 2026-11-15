/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWebView/MediaSessionPlatformBackendMacOS.h>
#include <LibWebView/NowPlayingInfo.h>
#include <LibWebView/StringConversionsMacOS.h>
#include <math.h>

#import <AppKit/AppKit.h>
#import <MediaPlayer/MediaPlayer.h>

@interface LadybirdMediaSessionCommandTarget : NSObject
{
@public
    Function<void(Web::Bindings::MediaSessionAction, Optional<double>, Optional<double>, Optional<bool>)> on_action;
    WebView::NowPlayingPlaybackStatus playback_status;
    bool has_state;
}
- (MPRemoteCommandHandlerStatus)handleCommand:(MPRemoteCommandEvent*)event;
- (MPRemoteCommandHandlerStatus)handleCommandOnMainThread:(MPRemoteCommandEvent*)event;
@end

@implementation LadybirdMediaSessionCommandTarget

- (MPRemoteCommandHandlerStatus)handleCommand:(MPRemoteCommandEvent*)event
{
    if ([NSThread isMainThread])
        return [self handleCommandOnMainThread:event];

    // MediaPlayer can call from a worker queue; the result must reflect the session when the UI thread handles it.
    __block MPRemoteCommandHandlerStatus result;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = [self handleCommandOnMainThread:event];
    });
    return result;
}

- (MPRemoteCommandHandlerStatus)handleCommandOnMainThread:(MPRemoteCommandEvent*)event
{
    if (!has_state || !on_action)
        return MPRemoteCommandHandlerStatusCommandFailed;

    using Web::Bindings::MediaSessionAction;
    auto* center = MPRemoteCommandCenter.sharedCommandCenter;
    MediaSessionAction action;
    Optional<double> seek_time;
    if (event.command == center.playCommand) {
        action = MediaSessionAction::Play;
    } else if (event.command == center.pauseCommand) {
        action = MediaSessionAction::Pause;
    } else if (event.command == center.togglePlayPauseCommand) {
        action = playback_status == WebView::NowPlayingPlaybackStatus::Playing ? MediaSessionAction::Pause : MediaSessionAction::Play;
    } else if (event.command == center.stopCommand) {
        action = MediaSessionAction::Stop;
    } else if (event.command == center.nextTrackCommand) {
        action = MediaSessionAction::Nexttrack;
    } else if (event.command == center.previousTrackCommand) {
        action = MediaSessionAction::Previoustrack;
    } else if (event.command == center.changePlaybackPositionCommand) {
        auto position = ((MPChangePlaybackPositionCommandEvent*)event).positionTime;
        if (!isfinite(position))
            return MPRemoteCommandHandlerStatusCommandFailed;
        action = MediaSessionAction::Seekto;
        seek_time = position;
    } else {
        return MPRemoteCommandHandlerStatusCommandFailed;
    }
    on_action(action, {}, seek_time, {});
    return MPRemoteCommandHandlerStatusSuccess;
}

@end

namespace WebView {

namespace Detail {

struct NowPlayingBridge {
    AK_ALLOC_WITH_KMALLOC;

    LadybirdMediaSessionCommandTarget* target;
    NSArray<MPRemoteCommand*>* commands;
    NSMutableArray* tokens;
    RefPtr<Gfx::Bitmap const> bitmap;
    MPMediaItemArtwork* artwork;
};

}

static NSImage* image_for_bitmap(Gfx::Bitmap const& bitmap)
{
    CGBitmapInfo bitmap_info;
    bool premultiplied = bitmap.alpha_type() == Gfx::AlphaType::Premultiplied;
    switch (bitmap.format()) {
    case Gfx::BitmapFormat::BGRA8888:
        bitmap_info = kCGBitmapByteOrder32Little | static_cast<CGBitmapInfo>(premultiplied ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaFirst);
        break;
    case Gfx::BitmapFormat::BGRx8888:
        bitmap_info = kCGBitmapByteOrder32Little | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst);
        break;
    case Gfx::BitmapFormat::RGBA8888:
        bitmap_info = kCGBitmapByteOrder32Big | static_cast<CGBitmapInfo>(premultiplied ? kCGImageAlphaPremultipliedLast : kCGImageAlphaLast);
        break;
    case Gfx::BitmapFormat::RGBx8888:
        bitmap_info = kCGBitmapByteOrder32Big | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast);
        break;
    case Gfx::BitmapFormat::Invalid:
        VERIFY_NOT_REACHED();
    }

    auto* data = [NSData dataWithBytes:bitmap.scanline_u8(0) length:bitmap.size_in_bytes()];
    auto provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    if (!provider)
        return nil;
    ScopeGuard release_provider = [&] { CGDataProviderRelease(provider); };
    auto color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!color_space)
        return nil;
    ScopeGuard release_color_space = [&] { CGColorSpaceRelease(color_space); };
    auto image = CGImageCreate(bitmap.width(), bitmap.height(), 8, 32, bitmap.pitch(), color_space, bitmap_info, provider, nullptr, false, kCGRenderingIntentDefault);
    if (!image)
        return nil;
    ScopeGuard release_image = [&] { CGImageRelease(image); };
    return [[NSImage alloc] initWithCGImage:image size:NSMakeSize(bitmap.width(), bitmap.height())];
}

MediaSessionPlatformBackendMacOS::MediaSessionPlatformBackendMacOS()
    : m_bridge(make<Detail::NowPlayingBridge>())
{
    VERIFY([NSThread isMainThread]);
    auto* target = [[LadybirdMediaSessionCommandTarget alloc] init];
    target->on_action = [this](auto action, auto seek_offset, auto seek_time, auto fast_seek) {
        if (on_action)
            on_action(action, seek_offset, seek_time, fast_seek);
    };
    m_bridge->target = target;
    auto* center = MPRemoteCommandCenter.sharedCommandCenter;
    m_bridge->commands = @[ center.playCommand, center.pauseCommand, center.togglePlayPauseCommand, center.stopCommand,
        center.nextTrackCommand, center.previousTrackCommand, center.changePlaybackPositionCommand ];
    m_bridge->tokens = [[NSMutableArray alloc] init];
    for (MPRemoteCommand* command in m_bridge->commands) {
        id token = [command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
            return [target handleCommand:event];
        }];
        [m_bridge->tokens addObject:token];
    }
    update({});
}

MediaSessionPlatformBackendMacOS::~MediaSessionPlatformBackendMacOS()
{
    VERIFY([NSThread isMainThread]);
    m_bridge->target->on_action = nullptr;
    update({});
    for (NSUInteger i = 0; i < m_bridge->commands.count; ++i)
        [m_bridge->commands[i] removeTarget:m_bridge->tokens[i]];
}

void MediaSessionPlatformBackendMacOS::update(Optional<NowPlayingState> const& state)
{
    VERIFY([NSThread isMainThread]);
    auto info = now_playing_info_for(state, MonotonicTime::now());
    m_bridge->target->has_state = state.has_value();
    m_bridge->target->playback_status = info.playback_status;

    auto* commands = MPRemoteCommandCenter.sharedCommandCenter;
    commands.playCommand.enabled = info.commands.play;
    commands.pauseCommand.enabled = info.commands.pause;
    commands.togglePlayPauseCommand.enabled = info.commands.toggle_play_pause;
    commands.stopCommand.enabled = info.commands.stop;
    commands.nextTrackCommand.enabled = info.commands.next_track;
    commands.previousTrackCommand.enabled = info.commands.previous_track;
    commands.changePlaybackPositionCommand.enabled = info.commands.change_playback_position;
    commands.skipForwardCommand.enabled = false;
    commands.skipBackwardCommand.enabled = false;
    commands.seekForwardCommand.enabled = false;
    commands.seekBackwardCommand.enabled = false;

    RefPtr<Gfx::Bitmap const> bitmap = state.has_value() ? state->artwork : nullptr;
    if (!same_artwork(bitmap, m_bridge->bitmap)) {
        m_bridge->bitmap = bitmap;
        m_bridge->artwork = nil;
        if (bitmap) {
            if (auto* image = image_for_bitmap(*bitmap)) {
                m_bridge->artwork = [[MPMediaItemArtwork alloc] initWithBoundsSize:image.size
                                                                    requestHandler:^NSImage*(CGSize) { return image; }];
            }
        }
    }

    auto* center = MPNowPlayingInfoCenter.defaultCenter;
    if (state.has_value()) {
        auto* dictionary = [NSMutableDictionary dictionary];
        dictionary[MPMediaItemPropertyTitle] = string_to_ns_string(info.title);
        dictionary[MPMediaItemPropertyArtist] = string_to_ns_string(info.artist);
        dictionary[MPMediaItemPropertyAlbumTitle] = string_to_ns_string(info.album);
        if (info.duration.has_value())
            dictionary[MPMediaItemPropertyPlaybackDuration] = @(*info.duration);
        if (info.elapsed_time.has_value())
            dictionary[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(*info.elapsed_time);
        if (info.playback_rate.has_value())
            dictionary[MPNowPlayingInfoPropertyPlaybackRate] = @(*info.playback_rate);
        if (m_bridge->artwork)
            dictionary[MPMediaItemPropertyArtwork] = m_bridge->artwork;
        center.nowPlayingInfo = dictionary;
    } else {
        center.nowPlayingInfo = nil;
    }

    switch (info.playback_status) {
    case NowPlayingPlaybackStatus::Playing:
        center.playbackState = MPNowPlayingPlaybackStatePlaying;
        break;
    case NowPlayingPlaybackStatus::Paused:
        center.playbackState = MPNowPlayingPlaybackStatePaused;
        break;
    case NowPlayingPlaybackStatus::Stopped:
        center.playbackState = MPNowPlayingPlaybackStateStopped;
        break;
    }
}

}
