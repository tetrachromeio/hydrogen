// Boron's page side: applies HTML from the app without rebuilding the page
// (so focus, caret and scroll survive), and reports events back.
//
// Messages to the app are "kind\x1fid\x1fvalue":
//   click   a [data-bn-click] element was clicked
//   input   a text field or slider changed (value: its contents)
//   change  a checkbox (value: "true"/"false") or select changed
//   submit  Enter in a text field (value: its contents)
//   ready   the page loaded (handled by the native side)
(function () {
    "use strict";
    var root = document.getElementById("bn-root");

    function send(kind, id, value) {
        var msg = kind + "\x1f" + id + "\x1f" + (value == null ? "" : String(value));
        var wk = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.boron;
        if (wk) wk.postMessage(msg);
        else if (window.boronSend) window.boronSend(msg);
    }

    // Values this page sent for a field and the app may still echo back. While the
    // field has focus, an echo of one of them must not overwrite newer typing.
    function remember(el, value) {
        var sent = el.__bnSent || (el.__bnSent = []);
        sent.push(value);
        if (sent.length > 64) sent.shift();
    }

    function sameKind(a, b) {
        if (a.nodeType !== b.nodeType || a.nodeName !== b.nodeName) return false;
        if (a.nodeType !== 1) return true;
        return (a.getAttribute("data-bn") || "") === (b.getAttribute("data-bn") || "") &&
            (a.getAttribute("type") || "") === (b.getAttribute("type") || "");
    }

    function syncAttributes(el, src) {
        var i, a;
        for (i = el.attributes.length - 1; i >= 0; i--) {
            a = el.attributes[i];
            if (!src.hasAttribute(a.name)) el.removeAttribute(a.name);
        }
        for (i = 0; i < src.attributes.length; i++) {
            a = src.attributes[i];
            if (el.getAttribute(a.name) !== a.value) el.setAttribute(a.name, a.value);
        }
        if (el.nodeName === "INPUT") {
            if (el.type === "checkbox") {
                el.checked = src.hasAttribute("checked");
            } else {
                var want = src.getAttribute("value") || "";
                var echo = document.activeElement === el && el.__bnSent && el.__bnSent.indexOf(want) >= 0;
                if (el.value !== want && !echo) el.value = want;
            }
        }
    }

    function morph(parent, src) {
        var want = Array.prototype.slice.call(src.childNodes);
        for (var i = 0; i < want.length; i++) {
            var s = want[i], cur = parent.childNodes[i];
            if (!cur) { parent.appendChild(s); continue; }
            if (!sameKind(cur, s)) { parent.replaceChild(s, cur); continue; }
            if (cur.nodeType !== 1) {
                if (cur.nodeValue !== s.nodeValue) cur.nodeValue = s.nodeValue;
                continue;
            }
            syncAttributes(cur, s);
            morph(cur, s);
        }
        while (parent.childNodes.length > want.length) parent.removeChild(parent.lastChild);
    }

    // Content that grows at the bottom (chats, logs) stays in view: if the page was
    // scrolled to the bottom before an update, it is scrolled to the bottom after.
    function atBottom() {
        var el = document.scrollingElement || document.documentElement;
        return el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
    }

    function patch(html) {
        var follow = atBottom();
        var t = document.createElement("template");
        t.innerHTML = html;
        morph(root, t.content);
        if (follow) {
            var el = document.scrollingElement || document.documentElement;
            el.scrollTop = el.scrollHeight;
        }
    }

    function css(text) {
        document.getElementById("bn-user").textContent = text;
    }

    function idOf(el) { return el.getAttribute("data-bn") || ""; }

    document.addEventListener("click", function (e) {
        var el = e.target.closest && e.target.closest("[data-bn-click]");
        if (!el || el.disabled) return;
        e.preventDefault();
        send("click", idOf(el), "");
    });
    document.addEventListener("input", function (e) {
        var el = e.target;
        if (!el.hasAttribute || !el.hasAttribute("data-bn") || el.type === "checkbox") return;
        if (el.nodeName === "SELECT") return;
        remember(el, el.value);
        send("input", idOf(el), el.value);
    });
    document.addEventListener("change", function (e) {
        var el = e.target;
        if (!el.hasAttribute || !el.hasAttribute("data-bn")) return;
        if (el.type === "checkbox") send("change", idOf(el), el.checked ? "true" : "false");
        else if (el.nodeName === "SELECT") send("change", idOf(el), el.value);
    });
    document.addEventListener("keydown", function (e) {
        var el = e.target;
        if (e.key !== "Enter" || e.isComposing || el.nodeName !== "INPUT" || !el.hasAttribute("data-bn")) return;
        e.preventDefault();
        send("submit", idOf(el), el.value);
    });
    // No "Reload" / "Back" context menu outside text fields: it would throw away the UI.
    document.addEventListener("contextmenu", function (e) {
        if (!(e.target.closest && e.target.closest("input, textarea, .bn-selectable"))) e.preventDefault();
    });

    window.boron = { patch: patch, css: css };
    send("ready", "", "");
})();
