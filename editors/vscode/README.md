# Hydrogen for VS Code

Syntax highlighting for `.h2` files, plus live errors, go to definition and
hover from Hydrogen's language server (`hy lsp`).

## Install (from this folder)

You need Node.js (for `npm`) and the `hy` tool on your `PATH` (or set
`hydrogen.hyPath` in VS Code settings to its full path).

```bash
cd editors/vscode
npm install
npx @vscode/vsce package --allow-missing-repository
code --install-extension hydrogen-language-0.1.0.vsix
```

To uninstall: `code --uninstall-extension tetrachrome.hydrogen-language`.

## Other editors

`hy lsp` is a standard Language Server Protocol server on stdin/stdout, so any
LSP-capable editor works. For example, in Neovim (0.10+):

```lua
vim.filetype.add({ extension = { h2 = "hydrogen" } })
vim.api.nvim_create_autocmd("FileType", {
  pattern = "hydrogen",
  callback = function() vim.lsp.start({ name = "hy", cmd = { "hy", "lsp" } }) end,
})
```
