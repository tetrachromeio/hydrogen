#!/usr/bin/env python3
"""Drives `hy lsp` like an editor would, over stdin/stdout.

Usage: test_lsp.py <path to hy>   (exit 0 on success)
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path


class Client:
    def __init__(self, hy, cwd):
        self.p = subprocess.Popen([hy, "lsp"], cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next_id = 1
        self.notifications = []

    def send(self, msg):
        body = json.dumps(msg).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def read(self):
        length = 0
        while True:
            line = self.p.stdout.readline().decode().strip()
            if not line:
                break
            if line.startswith("Content-Length:"):
                length = int(line.split(":")[1])
        return json.loads(self.p.stdout.read(length))

    def request(self, method, params):
        rid = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            msg = self.read()
            if msg.get("id") == rid:
                return msg.get("result")
            self.notifications.append(msg)

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def diagnostics(self, uri):
        """Reads until diagnostics for `uri` arrive; returns them."""
        while True:
            for i, n in enumerate(self.notifications):
                if n.get("method") == "textDocument/publishDiagnostics" and n["params"]["uri"] == uri:
                    self.notifications.pop(i)
                    return n["params"]["diagnostics"]
            self.notifications.append(self.read())


def check(cond, what):
    if not cond:
        print("FAIL:", what)
        sys.exit(1)


def main():
    hy = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp).resolve() / "app"
        (root / "Shapes").mkdir(parents=True)
        (root / "hy.toml").write_text('[project]\nname = "app"\n')
        shapes = root / "Shapes" / "init.h2"
        shapes.write_text("pub new function area(w: int, h: int) -> int { return w * h }\n")
        main_h2 = root / "main.h2"
        good = (
            "use Elemental\n"
            "use Shapes\n"
            "\n"
            "new function main() -> int {\n"
            "    let size = area(3, 4)\n"
            "    print(size)\n"
            "    return 0\n"
            "}\n"
        )
        main_h2.write_text(good)
        uri = main_h2.as_uri()

        c = Client(hy, root)
        init = c.request("initialize", {"processId": None, "rootUri": root.as_uri(), "capabilities": {}})
        check(init["capabilities"]["definitionProvider"], "initialize advertises definition")
        c.notify("initialized", {})

        # 1. unsaved text with an error is reported
        broken = good.replace("print(size)", "print(sise)")
        c.notify("textDocument/didOpen",
                 {"textDocument": {"uri": uri, "languageId": "hydrogen", "version": 1, "text": broken}})
        diags = c.diagnostics(uri)
        check(len(diags) == 1 and "sise" in diags[0]["message"], f"error for 'sise', got {diags}")
        check(diags[0]["range"]["start"] == {"line": 5, "character": 10}, f"error position, got {diags}")

        # 2. fixing it clears the error
        c.notify("textDocument/didChange",
                 {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": good}]})
        check(c.diagnostics(uri) == [], "errors cleared after the fix")

        # 3. go to definition: `area` is defined in Shapes/init.h2
        loc = c.request("textDocument/definition", {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 16}})
        check(loc and loc["uri"] == shapes.as_uri() and loc["range"]["start"]["line"] == 0, f"definition of area, got {loc}")

        # definition of a local: `size` used on line 6 -> its let on line 5
        loc = c.request("textDocument/definition", {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 11}})
        check(loc and loc["uri"] == uri and loc["range"]["start"]["line"] == 4, f"definition of size, got {loc}")

        # 4. hover shows types and signatures
        h = c.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 11}})
        check(h and "size: int" in h["contents"]["value"], f"hover on size, got {h}")
        h = c.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 16}})
        check(h and "function area(w: int, h: int) -> int" in h["contents"]["value"], f"hover on area, got {h}")

        # nothing under the cursor
        check(c.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 2, "character": 0}}) is None,
              "hover on a blank line")

        c.request("shutdown", None)
        c.notify("exit", None)
        check(c.p.wait(timeout=10) == 0, "server exits cleanly")
    print("lsp ok")


if __name__ == "__main__":
    main()
