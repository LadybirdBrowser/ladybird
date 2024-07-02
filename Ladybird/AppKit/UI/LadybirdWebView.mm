/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/TemporaryChange.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/SourceHighlighter.h>
#include <LibWebView/URL.h>
#include <UI/LadybirdWebViewBridge.h>

#import <Application/Application.h>
#import <Application/ApplicationDelegate.h>
#import <UI/Event.h>
#import <UI/LadybirdWebView.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <Utilities/Conversions.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr NSInteger CONTEXT_MENU_PLAY_PAUSE_TAG = 1;
static constexpr NSInteger CONTEXT_MENU_MUTE_UNMUTE_TAG = 2;
static constexpr NSInteger CONTEXT_MENU_CONTROLS_TAG = 3;
static constexpr NSInteger CONTEXT_MENU_LOOP_TAG = 4;
static constexpr NSInteger CONTEXT_MENU_SEARCH_SELECTED_TEXT_TAG = 5;
static constexpr NSInteger CONTEXT_MENU_COPY_LINK_TAG = 6;

// Calls to [NSCursor hide] and [NSCursor unhide] must be balanced. We use this struct to ensure
// we only call [NSCursor hide] once and to ensure that we do call [NSCursor unhide].
// https://developer.apple.com/documentation/appkit/nscursor#1651301
struct HideCursor {
    HideCursor()
    {
        [NSCursor hide];
    }

    ~HideCursor()
    {
        [NSCursor unhide];
    }
};

@interface LadybirdWebView ()
{
    OwnPtr<Ladybird::WebViewBridge> m_web_view_bridge;

    URL::URL m_context_menu_url;
    Gfx::ShareableBitmap m_context_menu_bitmap;
    Optional<String> m_context_menu_search_text;

    Optional<HideCursor> m_hidden_cursor;
}

@property (nonatomic, weak) id<LadybirdWebViewObserver> observer;
@property (nonatomic, strong) NSMenu* page_context_menu;
@property (nonatomic, strong) NSMenu* link_context_menu;
@property (nonatomic, strong) NSMenu* image_context_menu;
@property (nonatomic, strong) NSMenu* audio_context_menu;
@property (nonatomic, strong) NSMenu* video_context_menu;
@property (nonatomic, strong) NSMenu* select_dropdown;
@property (nonatomic, strong) NSTextField* status_label;
@property (nonatomic, strong) NSAlert* dialog;

// NSEvent does not provide a way to mark whether it has been handled, nor can we attach user data to the event. So
// when we dispatch the event for a second time after WebContent has had a chance to handle it, we must track that
// event ourselves to prevent indefinitely repeating the event.
@property (nonatomic, strong) NSEvent* event_being_redispatched;

@end

@implementation LadybirdWebView

@synthesize page_context_menu = _page_context_menu;
@synthesize link_context_menu = _link_context_menu;
@synthesize image_context_menu = _image_context_menu;
@synthesize audio_context_menu = _audio_context_menu;
@synthesize video_context_menu = _video_context_menu;
@synthesize status_label = _status_label;

- (instancetype)init:(id<LadybirdWebViewObserver>)observer
{
    if (self = [super init]) {
        self.observer = observer;

        auto* delegate = (ApplicationDelegate*)[NSApp delegate];
        auto* screens = [NSScreen screens];

        Vector<Web::DevicePixelRect> screen_rects;
        screen_rects.ensure_capacity([screens count]);

        for (id screen in screens) {
            auto screen_rect = Ladybird::ns_rect_to_gfx_rect([screen frame]).to_type<Web::DevicePixels>();
            screen_rects.unchecked_append(screen_rect);
        }

        // This returns device pixel ratio of the screen the window is opened in
        auto device_pixel_ratio = [[NSScreen mainScreen] backingScaleFactor];

        m_web_view_bridge = MUST(Ladybird::WebViewBridge::create(move(screen_rects), device_pixel_ratio, [delegate webContentOptions], [delegate webdriverContentIPCPath], [delegate preferredColorScheme], [delegate preferredContrast], [delegate preferredMotion]));
        [self setWebViewCallbacks];

        m_web_view_bridge->initialize_client();

        auto* area = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                                  options:NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect | NSTrackingMouseMoved
                                                    owner:self
                                                 userInfo:nil];
        [self addTrackingArea:area];
    }

    return self;
}

#pragma mark - Public methods

- (void)loadURL:(URL::URL const&)url
{
    m_web_view_bridge->load(url);
}

- (void)loadHTML:(StringView)html
{
    m_web_view_bridge->load_html(html);
}

- (void)navigateBack
{
    m_web_view_bridge->traverse_the_history_by_delta(-1);
}

- (void)navigateForward
{
    m_web_view_bridge->traverse_the_history_by_delta(1);
}

- (void)reload
{
    m_web_view_bridge->reload();
}

- (WebView::ViewImplementation&)view
{
    return *m_web_view_bridge;
}

- (String const&)handle
{
    return m_web_view_bridge->handle();
}

- (void)handleResize
{
    [self updateViewportRect:Ladybird::WebViewBridge::ForResize::Yes];
    [self updateStatusLabelPosition];
}

- (void)handleDevicePixelRatioChange
{
    m_web_view_bridge->set_device_pixel_ratio([[self window] backingScaleFactor]);
    [self updateViewportRect:Ladybird::WebViewBridge::ForResize::Yes];
    [self updateStatusLabelPosition];
}

- (void)handleScroll
{
    [self updateViewportRect:Ladybird::WebViewBridge::ForResize::No];
    [self updateStatusLabelPosition];
}

- (void)handleVisibility:(BOOL)is_visible
{
    m_web_view_bridge->set_system_visibility_state(is_visible);
}

- (void)findInPage:(NSString*)query
    caseSensitivity:(CaseSensitivity)case_sensitivity
{
    m_web_view_bridge->find_in_page(Ladybird::ns_string_to_string(query), case_sensitivity);
}

- (void)findInPageNextMatch
{
    m_web_view_bridge->find_in_page_next_match();
}

- (void)findInPagePreviousMatch
{
    m_web_view_bridge->find_in_page_previous_match();
}

- (void)zoomIn
{
    m_web_view_bridge->zoom_in();
}

- (void)zoomOut
{
    m_web_view_bridge->zoom_out();
}

- (void)resetZoom
{
    m_web_view_bridge->reset_zoom();
}

- (float)zoomLevel
{
    return m_web_view_bridge->zoom_level();
}

- (void)setPreferredColorScheme:(Web::CSS::PreferredColorScheme)color_scheme
{
    m_web_view_bridge->set_preferred_color_scheme(color_scheme);
}

- (void)setPreferredContrast:(Web::CSS::PreferredContrast)contrast
{
    m_web_view_bridge->set_preferred_contrast(contrast);
}

- (void)setPreferredMotion:(Web::CSS::PreferredMotion)motion
{
    m_web_view_bridge->set_preferred_motion(motion);
}

- (void)debugRequest:(ByteString const&)request argument:(ByteString const&)argument
{
    m_web_view_bridge->debug_request(request, argument);
}

- (void)viewSource
{
    m_web_view_bridge->get_source();
}

#pragma mark - Private methods

static void copy_data_to_clipboard(StringView data, NSPasteboardType pasteboard_type)
{
    auto* ns_data = Ladybird::string_to_ns_data(data);

    auto* pasteBoard = [NSPasteboard generalPasteboard];
    [pasteBoard clearContents];
    [pasteBoard setData:ns_data forType:pasteboard_type];
}

- (void)updateViewportRect:(Ladybird::WebViewBridge::ForResize)for_resize
{
    auto content_rect = [self frame];
    auto document_rect = [[self documentView] frame];
    auto device_pixel_ratio = m_web_view_bridge->device_pixel_ratio();

    auto position = [&](auto content_size, auto document_size, auto scroll) {
        return max(0, (document_size - content_size) * device_pixel_ratio * scroll);
    };

    auto horizontal_scroll = [[[self scrollView] horizontalScroller] floatValue];
    auto vertical_scroll = [[[self scrollView] verticalScroller] floatValue];

    auto ns_viewport_rect = NSMakeRect(
        position(content_rect.size.width, document_rect.size.width, horizontal_scroll),
        position(content_rect.size.height, document_rect.size.height, vertical_scroll),
        content_rect.size.width,
        content_rect.size.height);

    auto viewport_rect = Ladybird::ns_rect_to_gfx_rect(ns_viewport_rect);
    m_web_view_bridge->set_viewport_rect(viewport_rect, for_resize);
}

- (void)updateStatusLabelPosition
{
    static constexpr CGFloat LABEL_INSET = 10;

    if (_status_label == nil || [[self status_label] isHidden]) {
        return;
    }

    auto visible_rect = [self visibleRect];
    auto status_label_rect = [self.status_label frame];

    auto position = NSMakePoint(LABEL_INSET, visible_rect.origin.y + visible_rect.size.height - status_label_rect.size.height - LABEL_INSET);
    [self.status_label setFrameOrigin:position];
}

- (void)setWebViewCallbacks
{
    // We need to make sure that these callbacks don't cause reference cycles.
    // By default, capturing self will copy a strong reference to self in ARC.
    __weak LadybirdWebView* weak_self = self;

    m_web_view_bridge->on_did_layout = [weak_self](auto content_size) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        auto inverse_device_pixel_ratio = m_web_view_bridge->inverse_device_pixel_ratio();
        [[self documentView] setFrameSize:NSMakeSize(content_size.width() * inverse_device_pixel_ratio, content_size.height() * inverse_device_pixel_ratio)];
    };

    m_web_view_bridge->on_ready_to_paint = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self setNeedsDisplay:YES];
    };

    m_web_view_bridge->on_new_web_view = [weak_self](auto activate_tab, auto, auto) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return String {};
        }
        // FIXME: Create a child tab that re-uses the ConnectionFromClient of the parent tab
        return [self.observer onCreateNewTab:"about:blank"sv activateTab:activate_tab];
    };

    m_web_view_bridge->on_request_web_content = [weak_self]() {
        Application* application = NSApp;
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            VERIFY_NOT_REACHED();
        }
        return [application launchWebContent:*(self->m_web_view_bridge)].release_value_but_fixme_should_propagate_errors();
    };

    m_web_view_bridge->on_request_worker_agent = []() {
        Application* application = NSApp;
        return [application launchWebWorker].release_value_but_fixme_should_propagate_errors();
    };

    m_web_view_bridge->on_activate_tab = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [[self window] orderFront:nil];
    };

    m_web_view_bridge->on_close = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [[self window] close];
    };

    m_web_view_bridge->on_load_start = [weak_self](auto const& url, bool is_redirect) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.observer onLoadStart:url isRedirect:is_redirect];

        if (_status_label != nil) {
            [self.status_label setHidden:YES];
        }
    };

    m_web_view_bridge->on_load_finish = [weak_self](auto const& url) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.observer onLoadFinish:url];
    };

    m_web_view_bridge->on_url_change = [weak_self](auto const& url) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.observer onURLChange:url];
    };

    m_web_view_bridge->on_navigation_buttons_state_changed = [weak_self](auto back_enabled, auto forward_enabled) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.observer onBackNavigationEnabled:back_enabled
                      forwardNavigationEnabled:forward_enabled];
    };

    m_web_view_bridge->on_title_change = [weak_self](auto const& title) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.observer onTitleChange:title];
    };

    m_web_view_bridge->on_favicon_change = [weak_self](auto const& bitmap) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.observer onFaviconChange:bitmap];
    };

    m_web_view_bridge->on_finish_handling_key_event = [weak_self](auto const& key_event) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        NSEvent* event = Ladybird::key_event_to_ns_event(key_event);

        self.event_being_redispatched = event;
        [NSApp sendEvent:event];
        self.event_being_redispatched = nil;
    };

    m_web_view_bridge->on_cursor_change = [weak_self](auto cursor) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        if (cursor == Gfx::StandardCursor::Hidden) {
            if (!m_hidden_cursor.has_value()) {
                m_hidden_cursor.emplace();
            }

            return;
        }

        m_hidden_cursor.clear();

        switch (cursor) {
        case Gfx::StandardCursor::Arrow:
            [[NSCursor arrowCursor] set];
            break;
        case Gfx::StandardCursor::Crosshair:
            [[NSCursor crosshairCursor] set];
            break;
        case Gfx::StandardCursor::IBeam:
            [[NSCursor IBeamCursor] set];
            break;
        case Gfx::StandardCursor::ResizeHorizontal:
            [[NSCursor resizeLeftRightCursor] set];
            break;
        case Gfx::StandardCursor::ResizeVertical:
            [[NSCursor resizeUpDownCursor] set];
            break;
        case Gfx::StandardCursor::ResizeDiagonalTLBR:
            // FIXME: AppKit does not have a corresponding cursor, so we should make one.
            [[NSCursor arrowCursor] set];
            break;
        case Gfx::StandardCursor::ResizeDiagonalBLTR:
            // FIXME: AppKit does not have a corresponding cursor, so we should make one.
            [[NSCursor arrowCursor] set];
            break;
        case Gfx::StandardCursor::ResizeColumn:
            [[NSCursor resizeLeftRightCursor] set];
            break;
        case Gfx::StandardCursor::ResizeRow:
            [[NSCursor resizeUpDownCursor] set];
            break;
        case Gfx::StandardCursor::Hand:
            [[NSCursor pointingHandCursor] set];
            break;
        case Gfx::StandardCursor::Help:
            // FIXME: AppKit does not have a corresponding cursor, so we should make one.
            [[NSCursor arrowCursor] set];
            break;
        case Gfx::StandardCursor::Drag:
            [[NSCursor closedHandCursor] set];
            break;
        case Gfx::StandardCursor::DragCopy:
            [[NSCursor dragCopyCursor] set];
            break;
        case Gfx::StandardCursor::Move:
            // FIXME: AppKit does not have a corresponding cursor, so we should make one.
            [[NSCursor dragCopyCursor] set];
            break;
        case Gfx::StandardCursor::Wait:
            // FIXME: AppKit does not have a corresponding cursor, so we should make one.
            [[NSCursor arrowCursor] set];
            break;
        case Gfx::StandardCursor::Disallowed:
            // FIXME: AppKit does not have a corresponding cursor, so we should make one.
            [[NSCursor arrowCursor] set];
            break;
        case Gfx::StandardCursor::Eyedropper:
            // FIXME: AppKit does not have a corresponding cursor, so we should make one.
            [[NSCursor arrowCursor] set];
            break;
        case Gfx::StandardCursor::Zoom:
            // FIXME: AppKit does not have a corresponding cursor, so we should make one.
            [[NSCursor arrowCursor] set];
            break;
        default:
            break;
        }
    };

    m_web_view_bridge->on_zoom_level_changed = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self updateViewportRect:Ladybird::WebViewBridge::ForResize::Yes];
    };

    m_web_view_bridge->on_navigate_back = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self navigateBack];
    };

    m_web_view_bridge->on_navigate_forward = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self navigateForward];
    };

    m_web_view_bridge->on_refresh = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self reload];
    };

    m_web_view_bridge->on_request_tooltip_override = [weak_self](auto, auto const& tooltip) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        self.toolTip = Ladybird::string_to_ns_string(tooltip);
    };

    m_web_view_bridge->on_stop_tooltip_override = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        self.toolTip = nil;
    };

    m_web_view_bridge->on_enter_tooltip_area = [weak_self](auto const& tooltip) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        self.toolTip = Ladybird::string_to_ns_string(tooltip);
    };

    m_web_view_bridge->on_leave_tooltip_area = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        self.toolTip = nil;
    };

    m_web_view_bridge->on_link_hover = [weak_self](auto const& url) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        auto* url_string = Ladybird::string_to_ns_string(url.serialize());
        [self.status_label setStringValue:url_string];
        [self.status_label sizeToFit];
        [self.status_label setHidden:NO];

        [self updateStatusLabelPosition];
    };

    m_web_view_bridge->on_link_unhover = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.status_label setHidden:YES];
    };

    m_web_view_bridge->on_link_click = [weak_self](auto const& url, auto const& target, unsigned modifiers) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        if (modifiers == Web::UIEvents::KeyModifier::Mod_Super) {
            [self.observer onCreateNewTab:url activateTab:Web::HTML::ActivateTab::No];
        } else if (target == "_blank"sv) {
            [self.observer onCreateNewTab:url activateTab:Web::HTML::ActivateTab::Yes];
        } else {
            [self.observer loadURL:url];
        }
    };

    m_web_view_bridge->on_link_middle_click = [weak_self](auto url, auto, unsigned) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.observer onCreateNewTab:url activateTab:Web::HTML::ActivateTab::No];
    };

    m_web_view_bridge->on_context_menu_request = [weak_self](auto position) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        auto* search_selected_text_menu_item = [self.page_context_menu itemWithTag:CONTEXT_MENU_SEARCH_SELECTED_TEXT_TAG];

        auto selected_text = self.observer
            ? m_web_view_bridge->selected_text_with_whitespace_collapsed()
            : OptionalNone {};
        TemporaryChange change_url { m_context_menu_search_text, move(selected_text) };

        if (m_context_menu_search_text.has_value()) {
            auto* delegate = (ApplicationDelegate*)[NSApp delegate];
            auto action_text = WebView::format_search_query_for_display([delegate searchEngine].query_url, *m_context_menu_search_text);

            [search_selected_text_menu_item setTitle:Ladybird::string_to_ns_string(action_text)];
            [search_selected_text_menu_item setHidden:NO];
        } else {
            [search_selected_text_menu_item setHidden:YES];
        }

        auto* event = Ladybird::create_context_menu_mouse_event(self, position);
        [NSMenu popUpContextMenu:self.page_context_menu withEvent:event forView:self];
    };

    m_web_view_bridge->on_link_context_menu_request = [weak_self](auto const& url, auto position) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        TemporaryChange change_url { m_context_menu_url, url };

        auto* copy_link_menu_item = [self.link_context_menu itemWithTag:CONTEXT_MENU_COPY_LINK_TAG];

        switch (WebView::url_type(url)) {
        case WebView::URLType::Email:
            [copy_link_menu_item setTitle:@"Copy Email Address"];
            break;
        case WebView::URLType::Telephone:
            [copy_link_menu_item setTitle:@"Copy Phone Number"];
            break;
        case WebView::URLType::Other:
            [copy_link_menu_item setTitle:@"Copy URL"];
            break;
        }

        auto* event = Ladybird::create_context_menu_mouse_event(self, position);
        [NSMenu popUpContextMenu:self.link_context_menu withEvent:event forView:self];
    };

    m_web_view_bridge->on_image_context_menu_request = [weak_self](auto const& url, auto position, auto const& bitmap) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        TemporaryChange change_url { m_context_menu_url, url };
        TemporaryChange change_bitmap { m_context_menu_bitmap, bitmap };

        auto* event = Ladybird::create_context_menu_mouse_event(self, position);
        [NSMenu popUpContextMenu:self.image_context_menu withEvent:event forView:self];
    };

    m_web_view_bridge->on_media_context_menu_request = [weak_self](auto position, auto const& menu) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        TemporaryChange change_url { m_context_menu_url, menu.media_url };

        auto* context_menu = menu.is_video ? self.video_context_menu : self.audio_context_menu;
        auto* play_pause_menu_item = [context_menu itemWithTag:CONTEXT_MENU_PLAY_PAUSE_TAG];
        auto* mute_unmute_menu_item = [context_menu itemWithTag:CONTEXT_MENU_MUTE_UNMUTE_TAG];
        auto* controls_menu_item = [context_menu itemWithTag:CONTEXT_MENU_CONTROLS_TAG];
        auto* loop_menu_item = [context_menu itemWithTag:CONTEXT_MENU_LOOP_TAG];

        if (menu.is_playing) {
            [play_pause_menu_item setTitle:@"Pause"];
        } else {
            [play_pause_menu_item setTitle:@"Play"];
        }

        if (menu.is_muted) {
            [mute_unmute_menu_item setTitle:@"Unmute"];
        } else {
            [mute_unmute_menu_item setTitle:@"Mute"];
        }

        auto controls_state = menu.has_user_agent_controls ? NSControlStateValueOn : NSControlStateValueOff;
        [controls_menu_item setState:controls_state];

        auto loop_state = menu.is_looping ? NSControlStateValueOn : NSControlStateValueOff;
        [loop_menu_item setState:loop_state];

        auto* event = Ladybird::create_context_menu_mouse_event(self, position);
        [NSMenu popUpContextMenu:context_menu withEvent:event forView:self];
    };

    m_web_view_bridge->on_request_alert = [weak_self](auto const& message) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        auto* ns_message = Ladybird::string_to_ns_string(message);

        self.dialog = [[NSAlert alloc] init];
        [self.dialog setMessageText:ns_message];

        [self.dialog beginSheetModalForWindow:[self window]
                            completionHandler:^(NSModalResponse) {
                                m_web_view_bridge->alert_closed();
                                self.dialog = nil;
                            }];
    };

    m_web_view_bridge->on_request_confirm = [weak_self](auto const& message) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        auto* ns_message = Ladybird::string_to_ns_string(message);

        self.dialog = [[NSAlert alloc] init];
        [[self.dialog addButtonWithTitle:@"OK"] setTag:NSModalResponseOK];
        [[self.dialog addButtonWithTitle:@"Cancel"] setTag:NSModalResponseCancel];
        [self.dialog setMessageText:ns_message];

        [self.dialog beginSheetModalForWindow:[self window]
                            completionHandler:^(NSModalResponse response) {
                                m_web_view_bridge->confirm_closed(response == NSModalResponseOK);
                                self.dialog = nil;
                            }];
    };

    m_web_view_bridge->on_request_prompt = [weak_self](auto const& message, auto const& default_) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        auto* ns_message = Ladybird::string_to_ns_string(message);
        auto* ns_default = Ladybird::string_to_ns_string(default_);

        auto* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
        [input setStringValue:ns_default];

        self.dialog = [[NSAlert alloc] init];
        [[self.dialog addButtonWithTitle:@"OK"] setTag:NSModalResponseOK];
        [[self.dialog addButtonWithTitle:@"Cancel"] setTag:NSModalResponseCancel];
        [self.dialog setMessageText:ns_message];
        [self.dialog setAccessoryView:input];

        self.dialog.window.initialFirstResponder = input;

        [self.dialog beginSheetModalForWindow:[self window]
                            completionHandler:^(NSModalResponse response) {
                                Optional<String> text;

                                if (response == NSModalResponseOK) {
                                    text = Ladybird::ns_string_to_string([input stringValue]);
                                }

                                m_web_view_bridge->prompt_closed(move(text));
                                self.dialog = nil;
                            }];
    };

    m_web_view_bridge->on_request_set_prompt_text = [weak_self](auto const& message) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        if (self.dialog == nil || [self.dialog accessoryView] == nil) {
            return;
        }

        auto* ns_message = Ladybird::string_to_ns_string(message);

        auto* input = (NSTextField*)[self.dialog accessoryView];
        [input setStringValue:ns_message];
    };

    m_web_view_bridge->on_request_accept_dialog = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil || self.dialog == nil) {
            return;
        }

        [[self window] endSheet:[[self dialog] window]
                     returnCode:NSModalResponseOK];
    };

    m_web_view_bridge->on_request_dismiss_dialog = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil || self.dialog == nil) {
            return;
        }

        [[self window] endSheet:[[self dialog] window]
                     returnCode:NSModalResponseCancel];
    };

    m_web_view_bridge->on_request_color_picker = [weak_self](Color current_color) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        auto* panel = [NSColorPanel sharedColorPanel];
        [panel setColor:Ladybird::gfx_color_to_ns_color(current_color)];
        [panel setShowsAlpha:NO];
        [panel setTarget:self];
        [panel setAction:@selector(colorPickerUpdate:)];

        NSNotificationCenter* notification_center = [NSNotificationCenter defaultCenter];
        [notification_center addObserver:self
                                selector:@selector(colorPickerClosed:)
                                    name:NSWindowWillCloseNotification
                                  object:panel];

        [panel makeKeyAndOrderFront:nil];
    };

    m_web_view_bridge->on_request_file_picker = [weak_self](auto const& accepted_file_types, auto allow_multiple_files) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        auto* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];

        if (allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes) {
            [panel setAllowsMultipleSelection:YES];
            [panel setMessage:@"Select files"];
        } else {
            [panel setAllowsMultipleSelection:NO];
            [panel setMessage:@"Select file"];
        }

        NSMutableArray<UTType*>* accepted_file_filters = [NSMutableArray array];

        for (auto const& filter : accepted_file_types.filters) {
            filter.visit(
                [&](Web::HTML::FileFilter::FileType type) {
                    switch (type) {
                    case Web::HTML::FileFilter::FileType::Audio:
                        [accepted_file_filters addObject:UTTypeAudio];
                        break;
                    case Web::HTML::FileFilter::FileType::Image:
                        [accepted_file_filters addObject:UTTypeImage];
                        break;
                    case Web::HTML::FileFilter::FileType::Video:
                        [accepted_file_filters addObject:UTTypeVideo];
                        break;
                    }
                },
                [&](Web::HTML::FileFilter::MimeType const& filter) {
                    auto* ns_mime_type = Ladybird::string_to_ns_string(filter.value);

                    if (auto* ut_type = [UTType typeWithMIMEType:ns_mime_type]) {
                        [accepted_file_filters addObject:ut_type];
                    }
                },
                [&](Web::HTML::FileFilter::Extension const& filter) {
                    auto* ns_extension = Ladybird::string_to_ns_string(filter.value);

                    if (auto* ut_type = [UTType typeWithFilenameExtension:ns_extension]) {
                        [accepted_file_filters addObject:ut_type];
                    }
                });
        }

        // FIXME: Create an accessory view to allow selecting the active file filter.
        [panel setAllowedContentTypes:accepted_file_filters];
        [panel setAllowsOtherFileTypes:YES];

        [panel beginSheetModalForWindow:[self window]
                      completionHandler:^(NSInteger result) {
                          Vector<Web::HTML::SelectedFile> selected_files;

                          auto create_selected_file = [&](NSString* ns_file_path) {
                              auto file_path = Ladybird::ns_string_to_byte_string(ns_file_path);

                              if (auto file = Web::HTML::SelectedFile::from_file_path(file_path); file.is_error())
                                  warnln("Unable to open file {}: {}", file_path, file.error());
                              else
                                  selected_files.append(file.release_value());
                          };

                          if (result == NSModalResponseOK) {
                              for (NSURL* url : [panel URLs]) {
                                  create_selected_file([url path]);
                              }
                          }

                          m_web_view_bridge->file_picker_closed(move(selected_files));
                      }];
    };

    self.select_dropdown = [[NSMenu alloc] initWithTitle:@"Select Dropdown"];
    [self.select_dropdown setDelegate:self];

    m_web_view_bridge->on_request_select_dropdown = [weak_self](Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.select_dropdown removeAllItems];
        self.select_dropdown.minimumWidth = minimum_width;

        auto add_menu_item = [self](Web::HTML::SelectItemOption const& item_option, bool in_option_group) {
            NSMenuItem* menuItem = [[NSMenuItem alloc]
                initWithTitle:Ladybird::string_to_ns_string(in_option_group ? MUST(String::formatted("    {}", item_option.label)) : item_option.label)
                       action:item_option.disabled ? nil : @selector(selectDropdownAction:)
                keyEquivalent:@""];
            menuItem.representedObject = [NSNumber numberWithUnsignedInt:item_option.id];
            menuItem.state = item_option.selected ? NSControlStateValueOn : NSControlStateValueOff;
            [self.select_dropdown addItem:menuItem];
        };

        for (auto const& item : items) {
            if (item.has<Web::HTML::SelectItemOptionGroup>()) {
                auto const& item_option_group = item.get<Web::HTML::SelectItemOptionGroup>();
                NSMenuItem* subtitle = [[NSMenuItem alloc]
                    initWithTitle:Ladybird::string_to_ns_string(item_option_group.label)
                           action:nil
                    keyEquivalent:@""];
                [self.select_dropdown addItem:subtitle];

                for (auto const& item_option : item_option_group.items)
                    add_menu_item(item_option, true);
            }

            if (item.has<Web::HTML::SelectItemOption>())
                add_menu_item(item.get<Web::HTML::SelectItemOption>(), false);

            if (item.has<Web::HTML::SelectItemSeparator>())
                [self.select_dropdown addItem:[NSMenuItem separatorItem]];
        }

        auto device_pixel_ratio = m_web_view_bridge->device_pixel_ratio();
        auto* event = Ladybird::create_context_menu_mouse_event(self, Gfx::IntPoint { content_position.x() / device_pixel_ratio, content_position.y() / device_pixel_ratio });
        [NSMenu popUpContextMenu:self.select_dropdown withEvent:event forView:self];
    };

    m_web_view_bridge->on_get_all_cookies = [](auto const& url) {
        auto* delegate = (ApplicationDelegate*)[NSApp delegate];
        return [delegate cookieJar].get_all_cookies(url);
    };

    m_web_view_bridge->on_get_named_cookie = [](auto const& url, auto const& name) {
        auto* delegate = (ApplicationDelegate*)[NSApp delegate];
        return [delegate cookieJar].get_named_cookie(url, name);
    };

    m_web_view_bridge->on_get_cookie = [](auto const& url, auto source) {
        auto* delegate = (ApplicationDelegate*)[NSApp delegate];
        return [delegate cookieJar].get_cookie(url, source);
    };

    m_web_view_bridge->on_set_cookie = [](auto const& url, auto const& cookie, auto source) {
        auto* delegate = (ApplicationDelegate*)[NSApp delegate];
        [delegate cookieJar].set_cookie(url, cookie, source);
    };

    m_web_view_bridge->on_update_cookie = [](auto const& cookie) {
        auto* delegate = (ApplicationDelegate*)[NSApp delegate];
        [delegate cookieJar].update_cookie(cookie);
    };

    m_web_view_bridge->on_restore_window = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [[self window] setIsMiniaturized:NO];
        [[self window] orderFront:nil];
    };

    m_web_view_bridge->on_reposition_window = [weak_self](auto const& position) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return Gfx::IntPoint {};
        }
        auto frame = [[self window] frame];
        frame.origin = Ladybird::gfx_point_to_ns_point(position);
        [[self window] setFrame:frame display:YES];

        return Ladybird::ns_point_to_gfx_point([[self window] frame].origin);
    };

    m_web_view_bridge->on_resize_window = [weak_self](auto const& size) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return Gfx::IntSize {};
        }
        auto frame = [[self window] frame];
        frame.size = Ladybird::gfx_size_to_ns_size(size);
        [[self window] setFrame:frame display:YES];

        return Ladybird::ns_size_to_gfx_size([[self window] frame].size);
    };

    m_web_view_bridge->on_maximize_window = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return Gfx::IntRect {};
        }
        auto frame = [[NSScreen mainScreen] frame];
        [[self window] setFrame:frame display:YES];

        return Ladybird::ns_rect_to_gfx_rect([[self window] frame]);
    };

    m_web_view_bridge->on_minimize_window = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return Gfx::IntRect {};
        }
        [[self window] setIsMiniaturized:YES];

        return Ladybird::ns_rect_to_gfx_rect([[self window] frame]);
    };

    m_web_view_bridge->on_fullscreen_window = [weak_self]() {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return Gfx::IntRect {};
        }
        if (([[self window] styleMask] & NSWindowStyleMaskFullScreen) == 0) {
            [[self window] toggleFullScreen:nil];
        }

        return Ladybird::ns_rect_to_gfx_rect([[self window] frame]);
    };

    m_web_view_bridge->on_received_source = [weak_self](auto const& url, auto const& source) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        auto html = WebView::highlight_source(url, source);

        [self.observer onCreateNewTab:html
                                  url:url
                          activateTab:Web::HTML::ActivateTab::Yes];
    };

    m_web_view_bridge->on_theme_color_change = [weak_self](auto color) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        self.backgroundColor = Ladybird::gfx_color_to_ns_color(color);
    };

    m_web_view_bridge->on_find_in_page = [weak_self](auto current_match_index, auto const& total_match_count) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.observer onFindInPageResult:current_match_index + 1
                          totalMatchCount:total_match_count];
    };

    m_web_view_bridge->on_insert_clipboard_entry = [](auto const& data, auto const&, auto const& mime_type) {
        NSPasteboardType pasteboard_type = nil;

        // https://w3c.github.io/clipboard-apis/#os-specific-well-known-format
        if (mime_type == "text/plain"sv)
            pasteboard_type = NSPasteboardTypeString;
        else if (mime_type == "text/html"sv)
            pasteboard_type = NSPasteboardTypeHTML;
        else if (mime_type == "text/png"sv)
            pasteboard_type = NSPasteboardTypePNG;

        if (pasteboard_type)
            copy_data_to_clipboard(data, pasteboard_type);
    };

    m_web_view_bridge->on_audio_play_state_changed = [weak_self](auto play_state) {
        LadybirdWebView* self = weak_self;
        if (self == nil) {
            return;
        }
        [self.observer onAudioPlayStateChange:play_state];
    };
}

- (void)selectDropdownAction:(NSMenuItem*)menuItem
{
    NSNumber* data = [menuItem representedObject];
    m_web_view_bridge->select_dropdown_closed([data unsignedIntValue]);
}

- (void)menuDidClose:(NSMenu*)menu
{
    if (!menu.highlightedItem)
        m_web_view_bridge->select_dropdown_closed({});
}

- (void)colorPickerUpdate:(NSColorPanel*)colorPanel
{
    m_web_view_bridge->color_picker_update(Ladybird::ns_color_to_gfx_color(colorPanel.color), Web::HTML::ColorPickerUpdateState::Update);
}

- (void)colorPickerClosed:(NSNotification*)notification
{
    m_web_view_bridge->color_picker_update(Ladybird::ns_color_to_gfx_color([NSColorPanel sharedColorPanel].color), Web::HTML::ColorPickerUpdateState::Closed);
}

- (NSScrollView*)scrollView
{
    return (NSScrollView*)[self superview];
}

- (void)copy:(id)sender
{
    copy_data_to_clipboard(m_web_view_bridge->selected_text(), NSPasteboardTypeString);
}

- (void)paste:(id)sender
{
    auto* paste_board = [NSPasteboard generalPasteboard];

    if (auto* contents = [paste_board stringForType:NSPasteboardTypeString]) {
        m_web_view_bridge->paste(Ladybird::ns_string_to_string(contents));
    }
}

- (void)selectAll:(id)sender
{
    m_web_view_bridge->select_all();
}

- (void)searchSelectedText:(id)sender
{
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];

    auto url = MUST(String::formatted([delegate searchEngine].query_url, URL::percent_encode(*m_context_menu_search_text)));
    [self.observer onCreateNewTab:url activateTab:Web::HTML::ActivateTab::Yes];
}

- (void)takeVisibleScreenshot:(id)sender
{
    [self takeScreenshot:WebView::ViewImplementation::ScreenshotType::Visible];
}

- (void)takeFullScreenshot:(id)sender
{
    [self takeScreenshot:WebView::ViewImplementation::ScreenshotType::Full];
}

- (void)takeScreenshot:(WebView::ViewImplementation::ScreenshotType)type
{
    m_web_view_bridge->take_screenshot(type)
        ->when_resolved([self](auto const& path) {
            auto message = MUST(String::formatted("Screenshot saved to: {}", path));

            auto* dialog = [[NSAlert alloc] init];
            [dialog setMessageText:Ladybird::string_to_ns_string(message)];
            [[dialog addButtonWithTitle:@"OK"] setTag:NSModalResponseOK];
            [[dialog addButtonWithTitle:@"Open folder"] setTag:NSModalResponseContinue];

            __block auto* ns_path = Ladybird::string_to_ns_string(path.string());

            [dialog beginSheetModalForWindow:[self window]
                           completionHandler:^(NSModalResponse response) {
                               if (response == NSModalResponseContinue) {
                                   [[NSWorkspace sharedWorkspace] selectFile:ns_path inFileViewerRootedAtPath:@""];
                               }
                           }];
        })
        .when_rejected([self](auto const& error) {
            auto error_message = MUST(String::formatted("{}", error));

            auto* dialog = [[NSAlert alloc] init];
            [dialog setMessageText:Ladybird::string_to_ns_string(error_message)];

            [dialog beginSheetModalForWindow:[self window]
                           completionHandler:nil];
        });
}

- (void)openLink:(id)sender
{
    m_web_view_bridge->on_link_click(m_context_menu_url, {}, 0);
}

- (void)openLinkInNewTab:(id)sender
{
    m_web_view_bridge->on_link_middle_click(m_context_menu_url, {}, 0);
}

- (void)copyLink:(id)sender
{
    auto link = WebView::url_text_to_copy(m_context_menu_url);
    copy_data_to_clipboard(link, NSPasteboardTypeString);
}

- (void)copyImage:(id)sender
{
    auto* bitmap = m_context_menu_bitmap.bitmap();
    if (bitmap == nullptr) {
        return;
    }

    auto png = Gfx::PNGWriter::encode(*bitmap);
    if (png.is_error()) {
        return;
    }

    auto* data = [NSData dataWithBytes:png.value().data() length:png.value().size()];

    auto* pasteBoard = [NSPasteboard generalPasteboard];
    [pasteBoard clearContents];
    [pasteBoard setData:data forType:NSPasteboardTypePNG];
}

- (void)toggleMediaPlayState:(id)sender
{
    m_web_view_bridge->toggle_media_play_state();
}

- (void)toggleMediaMuteState:(id)sender
{
    m_web_view_bridge->toggle_media_mute_state();
}

- (void)toggleMediaControlsState:(id)sender
{
    m_web_view_bridge->toggle_media_controls_state();
}

- (void)toggleMediaLoopState:(id)sender
{
    m_web_view_bridge->toggle_media_loop_state();
}

#pragma mark - Properties

- (NSMenu*)page_context_menu
{
    if (!_page_context_menu) {
        _page_context_menu = [[NSMenu alloc] initWithTitle:@"Page Context Menu"];

        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Go Back"
                                                               action:@selector(navigateBack:)
                                                        keyEquivalent:@""]];
        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Go Forward"
                                                               action:@selector(navigateForward:)
                                                        keyEquivalent:@""]];
        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Reload"
                                                               action:@selector(reload:)
                                                        keyEquivalent:@""]];
        [_page_context_menu addItem:[NSMenuItem separatorItem]];

        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy"
                                                               action:@selector(copy:)
                                                        keyEquivalent:@""]];
        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Paste"
                                                               action:@selector(paste:)
                                                        keyEquivalent:@""]];
        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Select All"
                                                               action:@selector(selectAll:)
                                                        keyEquivalent:@""]];
        [_page_context_menu addItem:[NSMenuItem separatorItem]];

        auto* search_selected_text_menu_item = [[NSMenuItem alloc] initWithTitle:@"Search for <query>"
                                                                          action:@selector(searchSelectedText:)
                                                                   keyEquivalent:@""];
        [search_selected_text_menu_item setTag:CONTEXT_MENU_SEARCH_SELECTED_TEXT_TAG];
        [_page_context_menu addItem:search_selected_text_menu_item];
        [_page_context_menu addItem:[NSMenuItem separatorItem]];

        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Take Visible Screenshot"
                                                               action:@selector(takeVisibleScreenshot:)
                                                        keyEquivalent:@""]];
        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Take Full Screenshot"
                                                               action:@selector(takeFullScreenshot:)
                                                        keyEquivalent:@""]];
        [_page_context_menu addItem:[NSMenuItem separatorItem]];

        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"View Source"
                                                               action:@selector(viewSource:)
                                                        keyEquivalent:@""]];
        [_page_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Inspect Element"
                                                               action:@selector(inspectElement:)
                                                        keyEquivalent:@""]];
    }

    return _page_context_menu;
}

- (NSMenu*)link_context_menu
{
    if (!_link_context_menu) {
        _link_context_menu = [[NSMenu alloc] initWithTitle:@"Link Context Menu"];

        [_link_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Open"
                                                               action:@selector(openLink:)
                                                        keyEquivalent:@""]];
        [_link_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Open in New Tab"
                                                               action:@selector(openLinkInNewTab:)
                                                        keyEquivalent:@""]];
        [_link_context_menu addItem:[NSMenuItem separatorItem]];

        auto* copy_link_menu_item = [[NSMenuItem alloc] initWithTitle:@"Copy URL"
                                                               action:@selector(copyLink:)
                                                        keyEquivalent:@""];
        [copy_link_menu_item setTag:CONTEXT_MENU_COPY_LINK_TAG];
        [_link_context_menu addItem:copy_link_menu_item];
        [_link_context_menu addItem:[NSMenuItem separatorItem]];

        [_link_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Inspect Element"
                                                               action:@selector(inspectElement:)
                                                        keyEquivalent:@""]];
    }

    return _link_context_menu;
}

- (NSMenu*)image_context_menu
{
    if (!_image_context_menu) {
        _image_context_menu = [[NSMenu alloc] initWithTitle:@"Image Context Menu"];

        [_image_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Open Image"
                                                                action:@selector(openLink:)
                                                         keyEquivalent:@""]];
        [_image_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Open Image in New Tab"
                                                                action:@selector(openLinkInNewTab:)
                                                         keyEquivalent:@""]];
        [_image_context_menu addItem:[NSMenuItem separatorItem]];

        [_image_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy Image"
                                                                action:@selector(copyImage:)
                                                         keyEquivalent:@""]];
        [_image_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy Image URL"
                                                                action:@selector(copyLink:)
                                                         keyEquivalent:@""]];
        [_image_context_menu addItem:[NSMenuItem separatorItem]];

        [_image_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Inspect Element"
                                                                action:@selector(inspectElement:)
                                                         keyEquivalent:@""]];
    }

    return _image_context_menu;
}

- (NSMenu*)audio_context_menu
{
    if (!_audio_context_menu) {
        _audio_context_menu = [[NSMenu alloc] initWithTitle:@"Audio Context Menu"];

        auto* play_pause_menu_item = [[NSMenuItem alloc] initWithTitle:@"Play"
                                                                action:@selector(toggleMediaPlayState:)
                                                         keyEquivalent:@""];
        [play_pause_menu_item setTag:CONTEXT_MENU_PLAY_PAUSE_TAG];

        auto* mute_unmute_menu_item = [[NSMenuItem alloc] initWithTitle:@"Mute"
                                                                 action:@selector(toggleMediaMuteState:)
                                                          keyEquivalent:@""];
        [mute_unmute_menu_item setTag:CONTEXT_MENU_MUTE_UNMUTE_TAG];

        auto* controls_menu_item = [[NSMenuItem alloc] initWithTitle:@"Controls"
                                                              action:@selector(toggleMediaControlsState:)
                                                       keyEquivalent:@""];
        [controls_menu_item setTag:CONTEXT_MENU_CONTROLS_TAG];

        auto* loop_menu_item = [[NSMenuItem alloc] initWithTitle:@"Loop"
                                                          action:@selector(toggleMediaLoopState:)
                                                   keyEquivalent:@""];
        [loop_menu_item setTag:CONTEXT_MENU_LOOP_TAG];

        [_audio_context_menu addItem:play_pause_menu_item];
        [_audio_context_menu addItem:mute_unmute_menu_item];
        [_audio_context_menu addItem:controls_menu_item];
        [_audio_context_menu addItem:loop_menu_item];
        [_audio_context_menu addItem:[NSMenuItem separatorItem]];

        [_audio_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Open Audio"
                                                                action:@selector(openLink:)
                                                         keyEquivalent:@""]];
        [_audio_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Open Audio in New Tab"
                                                                action:@selector(openLinkInNewTab:)
                                                         keyEquivalent:@""]];
        [_audio_context_menu addItem:[NSMenuItem separatorItem]];

        [_audio_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy Audio URL"
                                                                action:@selector(copyLink:)
                                                         keyEquivalent:@""]];
        [_audio_context_menu addItem:[NSMenuItem separatorItem]];

        [_audio_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Inspect Element"
                                                                action:@selector(inspectElement:)
                                                         keyEquivalent:@""]];
    }

    return _audio_context_menu;
}

- (NSMenu*)video_context_menu
{
    if (!_video_context_menu) {
        _video_context_menu = [[NSMenu alloc] initWithTitle:@"Video Context Menu"];

        auto* play_pause_menu_item = [[NSMenuItem alloc] initWithTitle:@"Play"
                                                                action:@selector(toggleMediaPlayState:)
                                                         keyEquivalent:@""];
        [play_pause_menu_item setTag:CONTEXT_MENU_PLAY_PAUSE_TAG];

        auto* mute_unmute_menu_item = [[NSMenuItem alloc] initWithTitle:@"Mute"
                                                                 action:@selector(toggleMediaMuteState:)
                                                          keyEquivalent:@""];
        [mute_unmute_menu_item setTag:CONTEXT_MENU_MUTE_UNMUTE_TAG];

        auto* controls_menu_item = [[NSMenuItem alloc] initWithTitle:@"Controls"
                                                              action:@selector(toggleMediaControlsState:)
                                                       keyEquivalent:@""];
        [controls_menu_item setTag:CONTEXT_MENU_CONTROLS_TAG];

        auto* loop_menu_item = [[NSMenuItem alloc] initWithTitle:@"Loop"
                                                          action:@selector(toggleMediaLoopState:)
                                                   keyEquivalent:@""];
        [loop_menu_item setTag:CONTEXT_MENU_LOOP_TAG];

        [_video_context_menu addItem:play_pause_menu_item];
        [_video_context_menu addItem:mute_unmute_menu_item];
        [_video_context_menu addItem:controls_menu_item];
        [_video_context_menu addItem:loop_menu_item];
        [_video_context_menu addItem:[NSMenuItem separatorItem]];

        [_video_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Open Video"
                                                                action:@selector(openLink:)
                                                         keyEquivalent:@""]];
        [_video_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Open Video in New Tab"
                                                                action:@selector(openLinkInNewTab:)
                                                         keyEquivalent:@""]];
        [_video_context_menu addItem:[NSMenuItem separatorItem]];

        [_video_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy Video URL"
                                                                action:@selector(copyLink:)
                                                         keyEquivalent:@""]];
        [_video_context_menu addItem:[NSMenuItem separatorItem]];

        [_video_context_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Inspect Element"
                                                                action:@selector(inspectElement:)
                                                         keyEquivalent:@""]];
    }

    return _video_context_menu;
}

- (NSTextField*)status_label
{
    if (!_status_label) {
        _status_label = [NSTextField labelWithString:@""];
        [_status_label setDrawsBackground:YES];
        [_status_label setBordered:YES];
        [_status_label setHidden:YES];

        [self addSubview:_status_label];
    }

    return _status_label;
}

#pragma mark - NSView

- (void)drawRect:(NSRect)rect
{
    auto paintable = m_web_view_bridge->paintable();
    if (!paintable.has_value()) {
        [super drawRect:rect];
        return;
    }

    auto [bitmap, bitmap_size] = *paintable;
    VERIFY(bitmap.format() == Gfx::BitmapFormat::BGRA8888);

    static constexpr size_t BITS_PER_COMPONENT = 8;
    static constexpr size_t BITS_PER_PIXEL = 32;

    auto* context = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(context);

    auto device_pixel_ratio = m_web_view_bridge->device_pixel_ratio();
    auto inverse_device_pixel_ratio = m_web_view_bridge->inverse_device_pixel_ratio();

    CGContextScaleCTM(context, inverse_device_pixel_ratio, inverse_device_pixel_ratio);

    auto* provider = CGDataProviderCreateWithData(nil, bitmap.scanline_u8(0), bitmap.size_in_bytes(), nil);
    auto image_rect = CGRectMake(rect.origin.x * device_pixel_ratio, rect.origin.y * device_pixel_ratio, bitmap_size.width(), bitmap_size.height());

    // Ideally, this would be NSBitmapImageRep, but the equivalent factory initWithBitmapDataPlanes: does
    // not seem to actually respect endianness. We need NSBitmapFormatThirtyTwoBitLittleEndian, but the
    // resulting image is always big endian. CGImageCreate actually does respect the endianness.
    auto* bitmap_image = CGImageCreate(
        bitmap_size.width(),
        bitmap_size.height(),
        BITS_PER_COMPONENT,
        BITS_PER_PIXEL,
        bitmap.pitch(),
        CGColorSpaceCreateDeviceRGB(),
        kCGBitmapByteOrder32Little | kCGImageAlphaFirst,
        provider,
        nil,
        NO,
        kCGRenderingIntentDefault);

    auto* image = [[NSImage alloc] initWithCGImage:bitmap_image size:NSZeroSize];
    [image drawInRect:image_rect];

    CGContextRestoreGState(context);
    CGDataProviderRelease(provider);
    CGImageRelease(bitmap_image);

    [super drawRect:rect];
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [self handleResize];
}

- (void)viewDidEndLiveResize
{
    [super viewDidEndLiveResize];
    [self handleResize];
}

- (void)viewDidChangeEffectiveAppearance
{
    m_web_view_bridge->update_palette();
}

- (BOOL)isFlipped
{
    // The origin of a NSScrollView is the lower-left corner, with the y-axis extending upwards. Instead,
    // we want the origin to be the top-left corner, with the y-axis extending downward.
    return YES;
}

- (void)mouseMoved:(NSEvent*)event
{
    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseMove, event, self, [self scrollView], Web::UIEvents::MouseButton::None);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)scrollWheel:(NSEvent*)event
{
    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseWheel, event, self, [self scrollView], Web::UIEvents::MouseButton::Middle);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)mouseDown:(NSEvent*)event
{
    [[self window] makeFirstResponder:self];

    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseDown, event, self, [self scrollView], Web::UIEvents::MouseButton::Primary);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)mouseUp:(NSEvent*)event
{
    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseUp, event, self, [self scrollView], Web::UIEvents::MouseButton::Primary);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)mouseDragged:(NSEvent*)event
{
    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseMove, event, self, [self scrollView], Web::UIEvents::MouseButton::Primary);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)rightMouseDown:(NSEvent*)event
{
    [[self window] makeFirstResponder:self];

    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseDown, event, self, [self scrollView], Web::UIEvents::MouseButton::Secondary);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)rightMouseUp:(NSEvent*)event
{
    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseUp, event, self, [self scrollView], Web::UIEvents::MouseButton::Secondary);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)rightMouseDragged:(NSEvent*)event
{
    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseMove, event, self, [self scrollView], Web::UIEvents::MouseButton::Secondary);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)otherMouseDown:(NSEvent*)event
{
    if (event.buttonNumber != 2)
        return;

    [[self window] makeFirstResponder:self];

    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseDown, event, self, [self scrollView], Web::UIEvents::MouseButton::Middle);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)otherMouseUp:(NSEvent*)event
{
    if (event.buttonNumber != 2)
        return;

    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseUp, event, self, [self scrollView], Web::UIEvents::MouseButton::Middle);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (void)otherMouseDragged:(NSEvent*)event
{
    if (event.buttonNumber != 2)
        return;

    auto mouse_event = Ladybird::ns_event_to_mouse_event(Web::MouseEvent::Type::MouseMove, event, self, [self scrollView], Web::UIEvents::MouseButton::Middle);
    m_web_view_bridge->enqueue_input_event(move(mouse_event));
}

- (BOOL)performKeyEquivalent:(NSEvent*)event
{
    if ([event window] != [self window]) {
        return NO;
    }
    if ([[self window] firstResponder] != self) {
        return NO;
    }
    if (self.event_being_redispatched == event) {
        return NO;
    }

    [self keyDown:event];
    return YES;
}

- (void)keyDown:(NSEvent*)event
{
    if (self.event_being_redispatched == event) {
        return;
    }

    auto key_event = Ladybird::ns_event_to_key_event(Web::KeyEvent::Type::KeyDown, event);
    m_web_view_bridge->enqueue_input_event(move(key_event));
}

- (void)keyUp:(NSEvent*)event
{
    if (self.event_being_redispatched == event) {
        return;
    }

    auto key_event = Ladybird::ns_event_to_key_event(Web::KeyEvent::Type::KeyUp, event);
    m_web_view_bridge->enqueue_input_event(move(key_event));
}

@end
