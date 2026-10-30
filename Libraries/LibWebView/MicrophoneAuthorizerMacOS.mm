/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/MicrophoneAuthorizerMacOS.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreServices/CoreServices.h>

namespace WebView {

namespace Detail {

// The answer comes back on an arbitrary queue. The callbacks that take it stay on the main thread, so only this
// atomically counted holder crosses threads.
struct PendingMicrophoneRequests : public AtomicRefCounted<PendingMicrophoneRequests> {
    Vector<Function<void(MicrophoneAuthorization)>> callbacks;
};

}

MicrophoneAuthorizerMacOS::MicrophoneAuthorizerMacOS()
    : m_pending_requests(adopt_ref(*new Detail::PendingMicrophoneRequests))
{
}

MicrophoneAuthorizerMacOS::~MicrophoneAuthorizerMacOS() = default;

MicrophoneAuthorization MicrophoneAuthorizerMacOS::authorization() const
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusNotDetermined:
        return MicrophoneAuthorization::NotDetermined;
    case AVAuthorizationStatusAuthorized:
        return MicrophoneAuthorization::Authorized;
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted:
        return MicrophoneAuthorization::Denied;
    }
    return MicrophoneAuthorization::Denied;
}

void MicrophoneAuthorizerMacOS::request_authorization(Function<void(MicrophoneAuthorization)> on_complete)
{
    // Requests made while the system is asking share its answer.
    m_pending_requests->callbacks.append(move(on_complete));
    if (m_pending_requests->callbacks.size() > 1)
        return;

    RefPtr<Detail::PendingMicrophoneRequests> pending_requests = m_pending_requests;
    RefPtr<Core::WeakEventLoopReference> event_loop = Core::EventLoop::current_weak();
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
                                 auto strong_event_loop = event_loop->take();
                                 if (!strong_event_loop)
                                     return;
                                 auto authorization = granted ? MicrophoneAuthorization::Authorized : MicrophoneAuthorization::Denied;
                                 strong_event_loop->deferred_invoke([pending_requests, authorization] {
                                     auto callbacks = move(pending_requests->callbacks);
                                     for (auto& callback : callbacks)
                                         callback(authorization);
                                 });
                             }];
}

void MicrophoneAuthorizerMacOS::open_system_settings()
{
    auto* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"];
    LSOpenCFURLRef((__bridge CFURLRef)url, nullptr);
}

}
