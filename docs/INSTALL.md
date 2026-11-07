# Installing Hydrogen

Hydrogen builds from source in a few minutes. Everything lives in one folder,
so removing it is a single `rm -rf`.

> Status: built and tested on Linux. The macOS steps below follow what macOS
> provides, but haven't been run on a Mac yet. If a step fails, the error text
> is what's needed to fix it.

## macOS

### 1. Apple's command line tools (clang, git, python3)

```bash
xcode-select --install
```

A window asks to install the tools; accept and wait for it to finish. If it says
they're already installed, that's fine. Check:

```bash
clang --version
```

### 2. CMake and Ninja

With [Homebrew](https://brew.sh) (install it first if `brew` isn't found):

```bash
brew install cmake ninja
```

Without Homebrew: install CMake from cmake.org (the `.dmg`), run
`sudo "/Applications/CMake.app/Contents/bin/cmake-gui" --install`, and leave out
`-G Ninja` in step 4.

### 3. Get the code

```bash
git clone --branch claude/admiring-ramanujan-3lph5v https://github.com/tetrachromeio/hydrogen.git ~/hydrogen
```

If git asks for a password, use a GitHub personal access token (GitHub →
Settings → Developer settings → Personal access tokens), not your password.

### 4. Build

```bash
cd ~/hydrogen && cmake -S . -B build -G Ninja && cmake --build build
```

### 5. Check it works

```bash
ctest --test-dir build --output-on-failure
```

(valgrind doesn't run on Apple Silicon; the test runner skips its leak checks
there. The Linux-only TCP test is skipped too.)

### 6. Put `hy` on your PATH

For this terminal only:

```bash
export PATH="$HOME/hydrogen/build/compiler:$PATH"
```

To keep it, add that line to `~/.zshrc` (macOS's default shell), then open a new
terminal.

### 7. First program

```bash
cd ~ && hy new playground && cd playground && hy run
```

You should see `Hello from playground!`. Edit `main.h2` and `hy run` again.

### 8. Debugging (optional)

macOS has `lldb` instead of `gdb`:

```bash
lldb -o run -o bt build/playground
```

`breakpoint set --file main.h2 --line 5` and `next` work on Hydrogen lines.

### 9. VS Code (optional)

Needs Node.js (`brew install node`) and VS Code's `code` command (in VS Code:
Cmd+Shift+P → "Shell Command: Install 'code' command in PATH").

```bash
cd ~/hydrogen/editors/vscode && npm install && npx @vscode/vsce package --allow-missing-repository
```

```bash
code --install-extension hydrogen-language-0.1.0.vsix
```

If VS Code can't find `hy`, set **Settings → Hydrogen: Hy Path** to the full path,
e.g. `/Users/<you>/hydrogen/build/compiler/hy`.

### Updating

```bash
cd ~/hydrogen && git pull && cmake --build build
```

### Removing

```bash
rm -rf ~/hydrogen ~/playground
```

Remove the `export PATH=...` line from `~/.zshrc` if you added it, and
`code --uninstall-extension tetrachrome.hydrogen-language` for the extension.
Homebrew's cmake/ninja can stay, or `brew uninstall cmake ninja`.

## Linux

Install `clang`, `cmake`, `ninja-build`, `python3` (and optionally `valgrind`,
`gdb`) from your package manager, then follow steps 3–7 above.

## Windows

Use WSL (Windows Subsystem for Linux) and follow the Linux steps inside it.
