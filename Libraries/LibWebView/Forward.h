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
class CookieJar;
class DownloadStore;
class ExternalURLHandler;
class FaviconStore;
class FontService;
class FontServiceHost;
class HistoryStore;
class HSTSStore;
class Menu;
class OutOfProcessWebView;
class ProcessManager;
class RequestServerSiteBindings;
class SessionStore;
class Settings;
class SettingsUI;
class StorageJar;
class TraversableSessionHistory;
class ViewImplementation;
class WebContentClient;
class WebContentTestClient;
class WebDriverBrowserConnection;
class WebWorkerClient;
class WebUI;

struct DownloadRecord;
struct BookmarkItem;
struct BrowserOptions;
struct CookieStorageKey;
struct HistoryEntry;
struct SearchEngine;
struct WebContentOptions;
class WebContentPage;

}

namespace AK {

template<>
struct Traits<WebView::CookieStorageKey>;

}
