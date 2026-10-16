/*
 * Copyright (c) 2022, The SerenityOS developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/Traits.h>
#include <LibWebCommon/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

class Action;
class Application;
class ApplyHistoryStep;
class AudioServerControlClient;
class Autocomplete;
class AutocompleteService;
class BlobURLStore;
class BrowsingSession;
class BookmarkStore;
class CanonicalBrowsingContext;
class CanonicalBrowsingContextGroup;
class CanonicalDocument;
class CanonicalDocumentState;
class CanonicalEnvironmentSettingsObject;
class CanonicalWindowEnvironmentSettingsObject;
class CanonicalWorkerEnvironmentSettingsObject;
class CanonicalSessionHistoryEntry;
class CanonicalNavigable;
class CanonicalSimilarOriginWindowAgent;
class CanonicalTraversable;
class CanonicalWindow;
class CompositorClient;
class CompositorPresentationClient;
class CookieJar;
class DownloadStore;
class ExternalURLHandler;
class FaviconStore;
class FontService;
class FontServiceHost;
class HistoryStore;
class HSTSStore;
class Menu;
class NavigationLoader;
class OutOfProcessWebView;
class PictureInPictureManager;
class PictureInPictureWindow;
class ProcessManager;
class RequestServerInstance;
class RequestServerManager;
class RequestServerSiteBindings;
class SessionStore;
class Settings;
class SettingsUI;
class SpellChecker;
class StorageJar;
class TraversableSessionHistory;
class VideoDecoderServerControlClient;
class ViewImplementation;
class WebContentClient;
class WebContentTestClient;
class WebDriverBrowserConnection;
class WebWorkerClient;
class WebUI;

struct DownloadRecord;
struct BookmarkItem;
struct BrowserOptions;
struct ChangingNavigable;
struct CookieStorageKey;
struct HistoryEntry;
struct HistoryOperation;
struct SearchEngine;
struct WebContentOptions;
class WebContentPage;

enum class SessionLaunchPolicy : u8;

}

namespace AK {

template<>
struct Traits<WebView::CookieStorageKey>;

}
