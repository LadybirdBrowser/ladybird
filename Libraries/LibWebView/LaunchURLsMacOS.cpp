/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/LaunchURLsMacOS.h>

#include <Carbon/Carbon.h>

namespace WebView {

static void append_url_from_descriptor(AEDesc const& descriptor, Vector<ByteString>& urls)
{
    auto size = AEGetDescDataSize(&descriptor);
    if (size <= 0)
        return;

    Vector<char> buffer;
    buffer.resize(static_cast<size_t>(size));
    if (AEGetDescData(&descriptor, buffer.data(), size) != noErr)
        return;

    urls.append(ByteString { buffer.data(), buffer.size() });
}

static OSErr handle_get_url_event(AppleEvent const* event, AppleEvent*, SRefCon urls)
{
    // A kAEGetURL event's direct object is the URL, as text.
    AEDesc url {};
    if (auto status = AEGetParamDesc(event, keyDirectObject, typeUTF8Text, &url); status != noErr)
        return status;

    append_url_from_descriptor(url, *static_cast<Vector<ByteString>*>(urls));
    AEDisposeDesc(&url);
    return noErr;
}

static OSErr handle_open_documents_event(AppleEvent const* event, AppleEvent*, SRefCon urls)
{
    // A kAEOpenDocuments event's direct object is a list of the files to open, which coerce to file URLs.
    AEDescList files {};
    if (auto status = AEGetParamDesc(event, keyDirectObject, typeAEList, &files); status != noErr)
        return status;

    long count = 0;
    AECountItems(&files, &count);

    for (long index = 1; index <= count; ++index) {
        AEKeyword keyword {};
        AEDesc file {};
        if (AEGetNthDesc(&files, index, typeFileURL, &keyword, &file) != noErr)
            continue;

        append_url_from_descriptor(file, *static_cast<Vector<ByteString>*>(urls));
        AEDisposeDesc(&file);
    }

    AEDisposeDesc(&files);
    return noErr;
}

Vector<ByteString> take_urls_from_launch_apple_events()
{
    Vector<ByteString> urls;

    auto get_url_handler = NewAEEventHandlerUPP(handle_get_url_event);
    auto open_documents_handler = NewAEEventHandlerUPP(handle_open_documents_event);
    AEInstallEventHandler(kInternetEventClass, kAEGetURL, get_url_handler, &urls, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, open_documents_handler, &urls, false);

    // LaunchServices has already queued the launch events by the time this runs — one kAEGetURL event per URL, back to
    // back — so this only takes what is queued, and never waits for more. This is the same check Chrome makes in
    // ProcessSingleton::WaitForAndForwardOpenURLEvent().
    EventTypeSpec const apple_event_type { kEventClassAppleEvent, kEventAppleEvent };
    EventRef event = nullptr;
    while (ReceiveNextEvent(1, &apple_event_type, kEventDurationNanosecond, true, &event) == noErr) {
        AEProcessEvent(event);
        ReleaseEvent(event);
    }

    AERemoveEventHandler(kInternetEventClass, kAEGetURL, get_url_handler, false);
    AERemoveEventHandler(kCoreEventClass, kAEOpenDocuments, open_documents_handler, false);
    DisposeAEEventHandlerUPP(get_url_handler);
    DisposeAEEventHandlerUPP(open_documents_handler);

    return urls;
}

}
