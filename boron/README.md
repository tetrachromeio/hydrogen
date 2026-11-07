# Boron

Native desktop apps in Hydrogen. Boron is element 5: built on Hydrogen, for UI.

```
use Elemental
use Boron

new function main() -> int {
    var app = App("Counter", 320, 180)
    var count = 0
    while app.is_open() {
        var ui = Ui()
        ui.heading("Counter")
        ui.text("You clicked {count} times.")
        ui.accent_button("up", "+1")
        app.show(ui)

        let ev = app.wait()
        if ev.clicked("up") { count += 1 }
    }
    return 0
}
```

## How it works

- **Your program owns the loop.** You build the screen, `show` it, then `wait` for
  the next event, update your variables, and go around again. Boron has no
  callbacks, no retained widget objects and no hidden threads. The screen is just
  a function of your variables.
- **On macOS the window is native.** It is a real `NSWindow` with a `WKWebView`
  (Safari's engine, built into macOS, so nothing is bundled) inside. The `Ui`
  becomes HTML styled to look like macOS controls, in light and dark mode, using
  the system accent color.
- **Re-rendering is cheap and keeps state.** Each `show` sends new HTML. The page
  updates in place rather than rebuilding, so text fields keep focus, caret and
  half-typed text, and scroll position stays put.
- **No ports or servers.** Events come back through WebKit's message channel, in
  process. `wait()` runs the AppKit event loop only until the next event arrives.
- **Standard Mac behavior.** The app has a proper menu bar: Cmd+Q, Cmd+W, Cmd+M,
  and Cmd+C/V/X/Z/A in text fields. Links open in your browser. Quitting closes
  the window, so your loop ends normally and cleanup (`drop`) still runs.

```
 your loop ── Ui (HTML) ──▶ bn_render ──▶ WKWebView: boron.patch(html)
     ▲                                          │
     └──── Event ◀── bn_wait ◀── postMessage ◀──┘  click / input / change / submit
```

## Use it

Add Boron to your app's `hy.toml`. The path is wherever your Hydrogen checkout is:

```toml
[dependencies]
Boron = { path = "../hydrogen/boron" }
```

Then `hy run`. Boron's native code (Objective-C on macOS) is compiled and linked
automatically through its `[native]` section in `hy.toml`.

Examples: `examples/counter` and `examples/todo`.

```bash
cd boron/examples/todo && hy run
```

## API

### `App`
| | |
|---|---|
| `App(title, width, height)` | opens a window |
| `app.is_open()` | false once it is closed |
| `app.show(ui)` | replaces what the window shows |
| `app.wait()` | the next `Event`, blocking; a `"close"` event when the window closes |
| `app.style(css)` | your own CSS, added to Boron's |
| `app.set_title(title)` | |

### `Ui`: build top to bottom
| | |
|---|---|
| `heading(t)` `subheading(t)` `text(t)` `caption(t)` `selectable(t)` | text (escaped) |
| `button(id, label)` `accent_button(id, label)` `disabled_button(id, label)` | send `click` |
| `input(id, value, placeholder)` `wide_input(...)` `password(...)` | send `input` while typing, `submit` on Enter |
| `checkbox(id, label, checked)` | sends `change` (`"true"`/`"false"`) |
| `slider(id, value, min, max)` | sends `input` |
| `progress(fraction)` `divider()` `spacer()` | |
| `begin_row()` `begin_column()` `begin_card()` `begin(css_class)` … `end()` | layout |
| `html(raw)` | raw HTML, not escaped; `data-bn="id" data-bn-click` makes anything clickable |

Fields are **controlled**: you pass the value in, and you get changes as events.
Store the value (`draft = string(ev.text())`) and pass it back next time. A field
with focus keeps what the user is typing.

### `Event`
| | |
|---|---|
| `ev.clicked(id)` `ev.changed(id)` `ev.submitted(id)` `ev.is_close()` | what happened |
| `ev.text()` `ev.number()` `ev.checked()` | the value |
| `ev.index("delete")` | for ids like `"delete:{i}"`: `Some(i)` |
| `ev.kind` `ev.id` `ev.value` | the raw strings |

## Platforms

| | Backend | Status |
|---|---|---|
| macOS 11+ | AppKit + WKWebView (`native/boron_macos.m`) | written; not yet run on a Mac |
| Linux, Windows | headless (`native/boron_headless.c`) | works; tested |

The **headless backend** opens no window. It prints each render's HTML and reads
events from stdin, one per line: `click up`, `input draft some text`, `change done:1 true`,
`close`. That makes any Boron app testable as a command line program (see
`tests/boron/*.in`). On a Mac, `BORON_HEADLESS=1 hy run` uses it too.

The page layer (`native/page/boron.js`, `boron.css`) is tested in Chromium on
every test run (`tests/boron/test_page.js`). After editing either file, run
`native/page/gen_page.py` to regenerate `native/boron_page.h`.

## Roadmap

1. Run on a Mac and fix what the first build finds.
2. More controls: select, multi-line text, tables and lists with selection, images (local files).
3. Waiting with a timeout (`wait_for(seconds)`) for clocks, animation, and polling a `Channel`,
   so background threads (e.g. Carbon streaming tokens) can update the UI.
4. Several windows, native dialogs (open/save panels, alerts), and app bundles (`hy bundle` → `.app`).
5. Keyed lists, so reordering keeps each row's state.
6. Linux (GTK + WebKitGTK) and Windows (WebView2) backends behind the same C interface (`native/boron.h`).
