# Vendoring note

This directory is a **source copy**, not a git submodule.

Upstream is the local repository `C:\dev\interception-input`, which has no
git remote yet. Every other dependency under `third-party/` is a submodule;
this one cannot be until upstream is published.

Vendored at upstream commit:

    2838b21  Make examples and tests opt-out for add_subdirectory() consumers

## Re-syncing

Until upstream is published, changes made here do **not** flow back. Copy in
both directions by hand:

    # upstream -> engine
    cp -r <upstream>/{include,src,external} third-party/interception-input/
    cp <upstream>/CMakeLists.txt third-party/interception-input/

`examples/` and `tests/` are carried for reference; the engine build does not
compile them (see `INTERCEPTION_INPUT_BUILD_*` below).

## Converting to a submodule later

Once upstream is on GitHub:

    git rm -r third-party/interception-input
    git submodule add <url> third-party/interception-input

No CMake change is needed — `cmake/compile_definitions/windows.cmake` already
consumes it by path.

## Build options

The engine sets both to `OFF`:

- `INTERCEPTION_INPUT_BUILD_EXAMPLES` — three driver smoke-test executables
- `INTERCEPTION_INPUT_BUILD_TESTS` — unit tests plus a CTest registration
  that would otherwise be injected into the engine's own suite

## Runtime requirement

The library only sends input when the **Interception kernel driver** is
installed on the host (`install-interception.exe /install` plus a reboot).
With no driver, `InterceptionInput::create()` fails with
`ErrorCode::DriverUnavailable` and the engine falls back to `SendInput`.
See `IMPLEMENTATION_CONTRACT.md` for the full API contract.
