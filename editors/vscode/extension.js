// Starts `hy lsp` for .h2 files and connects it to VS Code.
const vscode = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

function activate(context) {
  const hy = vscode.workspace.getConfiguration("hydrogen").get("hyPath") || "hy";
  const server = { command: hy, args: ["lsp"] };
  client = new LanguageClient(
    "hydrogen",
    "Hydrogen",
    { run: server, debug: server },
    { documentSelector: [{ scheme: "file", language: "hydrogen" }] }
  );
  client.start().catch((err) =>
    vscode.window.showErrorMessage(
      `Hydrogen: could not start '${hy} lsp' (${err.message}). Set "hydrogen.hyPath" to the full path of hy.`
    )
  );
  context.subscriptions.push({ dispose: () => client && client.stop() });
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
