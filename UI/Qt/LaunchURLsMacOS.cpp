/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/LaunchURLsMacOS.h>

#include <Carbon/Carbon.h>

namespace Ladybird {

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

    // LaunchServices has already queued the launch events by the time this runs, one kAEGetURL event per URL, back to
    // back. So this only takes what is queued, and never waits for more. The first call only lets the event source turn
    // the queued Apple events into events, and takes none of them.
    EventTypeSpec const apple_event_type { kEventClassAppleEvent, kEventAppleEvent };
    EventRef first_event = nullptr;
    (void)ReceiveNextEvent(1, &apple_event_type, kEventDurationNanosecond, false, &first_event);

    // NB: Only the URL events are taken. The other launch events, kAEOpenApplication above all, stay queued for AppKit,
    //     which does not order any window in that is shown later on until it has handled them.
    auto is_url_event = [](EventRef event, void*) -> Boolean {
        if (GetEventClass(event) != kEventClassAppleEvent || GetEventKind(event) != kEventAppleEvent)
            return false;
        OSType event_class {};
        OSType event_id {};
        GetEventParameter(event, kEventParamAEEventClass, typeType, nullptr, sizeof(event_class), nullptr, &event_class);
        GetEventParameter(event, kEventParamAEEventID, typeType, nullptr, sizeof(event_id), nullptr, &event_id);
        return (event_class == kInternetEventClass && event_id == kAEGetURL) || (event_class == kCoreEventClass && event_id == kAEOpenDocuments);
    };
    while (auto* event = FindSpecificEventInQueue(GetMainEventQueue(), is_url_event, nullptr)) {
        RetainEvent(event);
        RemoveEventFromQueue(GetMainEventQueue(), event);
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
