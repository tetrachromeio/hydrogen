// Boron's macOS backend: a native NSWindow showing a WKWebView.
//
// The Hydrogen program owns the loop: bn_wait() runs the AppKit event loop only
// until the page reports an event (or the window closes), then returns it. So
// there are no callbacks into Hydrogen and no threads.
//
// Built with -fobjc-arc (see Boron's hy.toml). With BORON_HEADLESS=1 every call
// goes to the headless backend instead, so tests run the same on a Mac.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <stdlib.h>
#include <string.h>

#include "boron.h"
#include "boron_page.h"

static bool headless(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("BORON_HEADLESS");
        cached = v && *v && strcmp(v, "0") != 0;
    }
    return cached;
}

static NSString* toNSString(const uint8_t* bytes, uint64_t len) {
    NSString* s = [[NSString alloc] initWithBytes:bytes length:(NSUInteger)len encoding:NSUTF8StringEncoding];
    return s ? s : @"";
}

// A JavaScript string literal for `s` (JSON-style escaping).
static NSString* jsString(NSString* s) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:@[ s ] options:0 error:nil];
    NSString* arr = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]; // ["..."]
    return [arr substringWithRange:NSMakeRange(1, arr.length - 2)];
}

static void wakeEventLoop(void) {
    NSEvent* e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:0
                                     context:nil
                                     subtype:0
                                       data1:0
                                       data2:0];
    [NSApp postEvent:e atStart:NO];
}

@interface BNWindow : NSObject <WKScriptMessageHandler, WKNavigationDelegate, NSWindowDelegate>
@property(strong) NSWindow* window;
@property(strong) WKWebView* web;
@property(strong) NSMutableArray<NSString*>* queue; // "kind\x1fid\x1fvalue"
@property(copy) NSString* html;                     // last render, re-applied if the page reloads
@property(copy) NSString* css;
@property BOOL open;
@property BOOL ready;
- (instancetype)initWithTitle:(NSString*)title width:(CGFloat)w height:(CGFloat)h;
- (void)run:(NSString*)script;
- (void)markClosed;
- (void)closeWindow;
@end

static NSMutableArray<BNWindow*>* gWindows; // keeps open windows alive; used by Quit

@interface BNAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation BNAppDelegate
// Quit (Cmd+Q, Dock menu) closes the windows; the program's loop sees the close
// events and ends normally, so its cleanup (drop) still runs.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    for (BNWindow* w in [gWindows copy]) [w closeWindow];
    return NSTerminateCancel;
}
@end

static BNAppDelegate* gDelegate;

static void startApp(NSString* name) {
    if (NSApp && gDelegate) return;
    [NSApplication sharedApplication];
    gDelegate = [BNAppDelegate new];
    gWindows = [NSMutableArray new];
    NSApp.delegate = gDelegate;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSMenu* bar = [NSMenu new];
    NSMenuItem* appItem = [NSMenuItem new];
    NSMenu* appMenu = [NSMenu new];
    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [bar addItem:appItem];

    // Text fields in the page need these for Cmd+C / Cmd+V / Cmd+Z to work.
    NSMenuItem* editItem = [NSMenuItem new];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem* redo = [edit addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"z"];
    redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = edit;
    [bar addItem:editItem];

    NSMenuItem* windowItem = [NSMenuItem new];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    windowItem.submenu = windowMenu;
    [bar addItem:windowItem];
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = windowMenu;

    [NSApp finishLaunching];
}

@implementation BNWindow

- (instancetype)initWithTitle:(NSString*)title width:(CGFloat)w height:(CGFloat)h {
    if (!(self = [super init])) return nil;
    _queue = [NSMutableArray new];
    _open = YES;

    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.releasedWhenClosed = NO; // ARC owns it
    _window.title = title;
    _window.delegate = self;
    _window.minSize = NSMakeSize(200, 120);

    WKWebViewConfiguration* config = [WKWebViewConfiguration new];
    [config.userContentController addScriptMessageHandler:self name:@"boron"];
    _web = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, w, h) configuration:config];
    _web.navigationDelegate = self;
    _web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [_web setValue:@NO forKey:@"drawsBackground"]; // no white flash in dark mode
#if defined(__MAC_13_3) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_13_3
    if (@available(macOS 13.3, *)) _web.inspectable = YES; // Safari > Develop > <app>
#endif
    _window.contentView = _web;
    [_web loadHTMLString:@(BN_PAGE) baseURL:nil];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    return self;
}

- (void)run:(NSString*)script {
    if (_ready) [_web evaluateJavaScript:script completionHandler:nil];
}

- (void)applyAll {
    if (_css) [self run:[NSString stringWithFormat:@"boron.css(%@)", jsString(_css)]];
    if (_html) [self run:[NSString stringWithFormat:@"boron.patch(%@)", jsString(_html)]];
}

- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
    if (![message.body isKindOfClass:[NSString class]]) return;
    NSString* msg = message.body;
    if ([msg hasPrefix:@"ready\x1f"]) {
        _ready = YES;
        [self applyAll]; // first load, or the page was reloaded
        return;
    }
    [_queue addObject:msg];
    wakeEventLoop();
}

// Links open in the browser; the page itself never navigates away.
- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decision {
    if (action.navigationType == WKNavigationTypeLinkActivated) {
        if (action.request.URL) [[NSWorkspace sharedWorkspace] openURL:action.request.URL];
        decision(WKNavigationActionPolicyCancel);
        return;
    }
    decision(WKNavigationActionPolicyAllow);
}

- (void)windowWillClose:(NSNotification*)note {
    [self markClosed];
}

- (void)markClosed {
    if (!_open) return;
    _open = NO;
    [_web.configuration.userContentController removeScriptMessageHandlerForName:@"boron"]; // breaks the cycle
    [gWindows removeObject:self];
    wakeEventLoop();
}

- (void)closeWindow {
    if (_open) [_window close]; // -> windowWillClose
}

@end

void* bn_open(const uint8_t* title, uint64_t title_len, int64_t width, int64_t height) {
    if (headless()) return bnh_open(title, title_len, width, height);
    @autoreleasepool {
        NSString* t = toNSString(title, title_len);
        startApp(t);
        BNWindow* w = [[BNWindow alloc] initWithTitle:t width:(CGFloat)width height:(CGFloat)height];
        [gWindows addObject:w];
        return (__bridge_retained void*)w;
    }
}

void bn_render(void* window, const uint8_t* html, uint64_t len) {
    if (headless()) return bnh_render(window, html, len);
    @autoreleasepool {
        BNWindow* w = (__bridge BNWindow*)window;
        w.html = toNSString(html, len);
        [w run:[NSString stringWithFormat:@"boron.patch(%@)", jsString(w.html)]];
    }
}

void bn_css(void* window, const uint8_t* css, uint64_t len) {
    if (headless()) return bnh_css(window, css, len);
    @autoreleasepool {
        BNWindow* w = (__bridge BNWindow*)window;
        w.css = toNSString(css, len);
        [w run:[NSString stringWithFormat:@"boron.css(%@)", jsString(w.css)]];
    }
}

void bn_title(void* window, const uint8_t* title, uint64_t len) {
    if (headless()) return bnh_title(window, title, len);
    @autoreleasepool {
        BNWindow* w = (__bridge BNWindow*)window;
        w.window.title = toNSString(title, len);
    }
}

static void setString(BnString* out, NSString* s) {
    const char* utf8 = s.UTF8String;
    bn_set_string(out, utf8, strlen(utf8));
}

// Moves the oldest queued page message into the three strings.
static void popEvent(BNWindow* w, BnString* kind, BnString* id, BnString* value) {
    @autoreleasepool {
        NSString* msg = w.queue.firstObject;
        [w.queue removeObjectAtIndex:0];
        NSArray<NSString*>* parts = [msg componentsSeparatedByString:@"\x1f"];
        setString(kind, parts.count > 0 ? parts[0] : @"");
        setString(id, parts.count > 1 ? parts[1] : @"");
        // The value may itself contain the separator only if the user typed it; keep it whole.
        NSString* rest = @"";
        if (parts.count > 2)
            rest = [[parts subarrayWithRange:NSMakeRange(2, parts.count - 2)] componentsJoinedByString:@"\x1f"];
        setString(value, rest);
    }
}

bool bn_wait(void* window, BnString* kind, BnString* id, BnString* value) {
    if (headless()) return bnh_wait(window, kind, id, value);
    BNWindow* w = (__bridge BNWindow*)window;
    while (w.open && w.queue.count == 0) {
        @autoreleasepool {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantFuture]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (e && e.type != NSEventTypeApplicationDefined) [NSApp sendEvent:e];
            [NSApp updateWindows];
        }
    }
    if (w.queue.count == 0) return false; // closed
    popEvent(w, kind, id, value);
    return true;
}

int64_t bn_poll(void* window, BnString* kind, BnString* id, BnString* value) {
    if (headless()) return bnh_poll(window, kind, id, value);
    BNWindow* w = (__bridge BNWindow*)window;
    @autoreleasepool {
        // Everything already pending, without blocking: input, page messages, and the
        // run loop work that sends renders to the page and puts them on screen.
        NSEvent* e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES])) {
            if (e.type != NSEventTypeApplicationDefined) [NSApp sendEvent:e];
        }
        [NSApp updateWindows];
    }
    if (w.queue.count > 0) {
        popEvent(w, kind, id, value);
        return 1;
    }
    return w.open ? 0 : -1;
}

void bn_close(void* window) {
    if (headless()) return bnh_close(window);
    @autoreleasepool {
        BNWindow* w = (__bridge_transfer BNWindow*)window;
        [w closeWindow];
        [w markClosed];
    }
}
