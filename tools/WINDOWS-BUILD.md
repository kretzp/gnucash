# Native Windows development build

This checkout uses the official [gnucash-on-windows](https://github.com/Gnucash/gnucash-on-windows)
MSYS2/MinGW-w64 approach. The local support checkout is at
`tools/gnucash-on-windows` (baseline `9e75888`). Build artifacts and the complete
toolchain are isolated in `.windows-build`, excluded using `.git/info/exclude`.
The GnuCash source is this workspace, not the separate WSL clone.

## Commands (PowerShell, from the repository root)

```powershell
./tools/build-windows.ps1 configure
./tools/build-windows.ps1 build
./tools/build-windows.ps1 test
./tools/build-windows.ps1 install
./tools/build-windows.ps1 version
./tools/build-windows.ps1 run
```

The helper selects MINGW64 and sets PATH and the Guile source/compiled search
paths. It explicitly selects the MinGW64 CMake mode and overrides both
pkg-config search variables to prevent paths from the active GCC 16 environment
leaking into the build. Configuration uses a Debug build, Ninja, and eight build jobs. Build
output is in `.windows-build/build-gcc15`; the user-local installation is in
`.windows-build/install`. The run action requires install first and retains the
MSYS2 runtime DLL search path. Close a running native GnuCash before installing
updated binaries.

This is a development installation. It depends on the local MSYS2 DLLs and is
not a portable distribution or an all-in-one setup installer.

The helper supplies valid locale variables derived from the current Windows
culture when launching the application; build/tests use `en_US.UTF-8`.
With the inherited `C.UTF-8` locale, this source revision crashed in
`set_platform_locale` while freeing an environment-owned pointer; the native
GDB trace is in `startup-debug.log`. Valid locale variables allow startup
without a GnuCash source modification. Guile automatic cache compilation is
disabled to avoid concurrent writes to its shared startup cache; explicit
Scheme compilation remains enabled.

## Compatible private toolchain

The currently published GnuCash WebKit package imports the emulated-TLS
`__once_call`/`__once_callable` symbols removed by GCC 16. See the
[MSYS2 ABI change notice](https://www.msys2.org/news/#2026-05-11-native-thread-local-storage-tls-with-gcc-16).
The development build therefore uses a separate `.windows-build/mingw15`
prefix containing GCC 15.2.0-14, Boost 1.90.0-3 and ICU 78.3-1. The corresponding
archive packages and detached signatures are in `.windows-build/downloads`;
GPG verified them against the MSYS2 keyring before extraction. This prefix was
prepared from a copy of the MSYS2 MinGW64 files, with these signed packages
extracted into the copy. The active MSYS2 package database and its GCC 16
installation were preserved.

The helper uses GCC and libraries from this private prefix, while invoking the
original CMake/Ninja executables explicitly. Do not mix the GCC 16 C++ libraries
back into this prefix. It is a deliberately versioned development toolchain,
not a pacman-managed installation. Signatures and preparation are logged in
`portable-signatures.log` and `portable-toolchain.log`.

## Bootstrap and upstream compatibility

The initial environment was created from upstream `setup-mingw64.ps1` with
`-x86_64`, a project-local target directory and the HTTPS MSYS2 mirror. A local
derived script at `.windows-build/setup-development.ps1` omits HTML Help
Workshop and Inno Setup, which concern documentation/installer packaging.
No machine-wide PowerShell execution policy was changed.

The upstream bootstrap's `libmangle-git` and `tools-git` toolchain packages no
longer exist. Available MinGW64 GCC, CMake, Ninja, SWIG, GDB, pkgconf and library
packages were installed explicitly instead.

Use the archive's patched `mingw-w64-x86_64-swig` 4.2.1-2 for this x64 build.
The current MSYS2 SWIG 4.5.1 generated Guile bindings using 32-bit `unsigned long`
for pointers. The GnuCash package uses pointer-sized conversions. After changing
SWIG versions, regenerate both `common/swig-runtime.h` and the generated
`swig-*.c`/`swig-*.cpp` files in the build directory. A general pacman upgrade can
replace this patched package, so recheck its version after upgrading.

The individual 64-bit WebKit URL in that bootstrap returned HTTP 404. The
official replacement dependency archive was downloaded from:

<https://downloads.sourceforge.net/project/gnucash/Dependencies/gnc-mingw64-repo.tar.zst>

Its contents are extracted to `.windows-build/gnc-repo/mingw64` and registered
as the local `gnc-mingw64` pacman repository. Pacman verifies the packages using
the GnuCash signing key imported by the upstream bootstrap; signature checks
remain enabled. The archive supplies native Guile 3, WebKitGTK 2.4, AqBanking,
Gwenhywfar, OFX, DBI and related dependencies. Other dependencies come from MSYS2.

Upstream jhbuild and its patched configuration are retained in
`.windows-build/src`. The application is built directly with CMake, using the
corresponding native environment and upstream CMake options, so edits in this
workspace do not need a second source checkout. The Guile paths are adjusted
for the packaged Guile 3 instead of the upstream jhbuild template's Guile 2.2.

## Logs

Initial setup logs: `.windows-build/bootstrap.log`, `toolchain.log`,
`dependencies.log`, `gnc-repo-download.log`. Build verification logs are stored
in the same directory when commands are run with PowerShell redirection, e.g.
`./tools/build-windows.ps1 build *> .windows-build/build.log`.

Keep Linux shell scripts in LF format. The local Git attribute for
`util/gnc-vcs-info` enforces LF without modifying upstream tracked attributes.
The two XML load/save fixtures also have local LF attributes; their tracked
contents are unchanged.

## Verification on 2026-11-18

Source baseline: `7574033d12` (GnuCash 5.17 development).

- Full native build and local installation completed.
- Installed `gnucash.exe --version` completed successfully through the helper.
- Installed `gnucash-cli.exe --report list` loaded the report catalogue.
- Windows CTest: **101/129 passed, 28 failed** (`tests-us.log`).
- Six tests crash in the original source; the expression-parser debugger trace
  reaches `get_locale_name()` in `libgnucash/core-utils/gnc-locale-utils.c:49`,
  which writes to the buffer returned by `setlocale()`. The `C` locale triggers
  this failure. `test-exp-parser-debug.log` records the trace.
- Most other failures are locale-sensitive report expectations; the timezone
  test also expects English Windows timezone names. The LF load/save fixtures
  pass. The suite is **not green**; passing version/report-list checks do not
  establish correctness of all application workflows.

No upstream GnuCash source changes were made to hide or disable these failures.
The new files are development helpers/documentation. Installer packaging, full
GUI interaction, and finance-data round trips in the installed application
were not verified.
