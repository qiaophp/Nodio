# thirdparty/

Third-party sources that are **not** committed to this repository.

## scintilla

`nodio-ide` edits source through the Scintilla editing component. Scintilla
is fetched separately so the repository stays free of vendored third-party
code:

```
# pinned release: Scintilla 4.4.6 — the last release whose lexers ship
# inside the component itself, so no separate Lexilla checkout is needed
curl -L -o sci.zip \
  https://github.com/mirror/scintilla/archive/refs/tags/rel-4-4-6.zip
mkdir -p thirdparty/scintilla
unzip -q sci.zip -d thirdparty/scintilla
# the archive nests everything one level down; lift it into place
mv thirdparty/scintilla/scintilla-rel-4-4-6/* thirdparty/scintilla/
rmdir thirdparty/scintilla/scintilla-rel-4-4-6
```

Then configure with the IDE enabled (the default):

```
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release
```

`nodio-ide.exe` links Scintilla **statically** (`SCI_LEXER` defined, window
class registered by `Scintilla_RegisterClasses`), so the editor needs no DLL
alongside the executable.

Without `thirdparty/scintilla`, CMake skips the `nodio-ide` target and builds
only `engine` + `nodio` — the engine never depends on third-party code.

License: Scintilla is distributed under its own license (see
`thirdparty/scintilla/License.txt` after fetching).
