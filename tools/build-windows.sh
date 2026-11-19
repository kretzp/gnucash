#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$project_root/.windows-build/build-gcc15"
install_dir="$project_root/.windows-build/install"
export MINGW_PREFIX="$project_root/.windows-build/mingw15"
cmake_cmd="$project_root/.windows-build/msys2/mingw64/bin/cmake.exe"
export PATH="$install_dir/bin:$MINGW_PREFIX/bin:/usr/bin:$PATH"
export PKG_CONFIG_LIBDIR="$MINGW_PREFIX/lib/pkgconfig:$MINGW_PREFIX/share/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
export GNC_DBD_DIR="$MINGW_PREFIX/lib/dbd"
export GUILE_LOAD_PATH="$MINGW_PREFIX/share/guile/3.0"
export GUILE_LOAD_COMPILED_PATH="$MINGW_PREFIX/lib/guile/3.0/ccache"
export GUILE_AUTO_COMPILE=0
case "${1:-build}" in
    run|mcp-run|version) export LANG="${GNC_WINDOWS_LOCALE:-en_US.UTF-8}" ;;
    *) export LANG=en_US.UTF-8 ;;
esac
export LC_ALL="$LANG" LC_MESSAGES="$LANG" LC_CTYPE="$LANG"
case "${1:-build}" in
    configure)
        "$cmake_cmd" -S "$project_root" -B "$build_dir" -G Ninja \
            -DCMAKE_C_COMPILER="$MINGW_PREFIX/bin/gcc.exe" \
            -DCMAKE_CXX_COMPILER="$MINGW_PREFIX/bin/g++.exe" \
            -DMINGW64=ON \
            -DWITH_MCP=ON \
            -DCMAKE_MAKE_PROGRAM="$project_root/.windows-build/msys2/mingw64/bin/ninja.exe" \
            -DBoost_DIR="$MINGW_PREFIX/lib/cmake/Boost-1.90.0" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_INSTALL_PREFIX="$install_dir" \
            -DCMAKE_PREFIX_PATH="$MINGW_PREFIX" \
            -DPERL_EXECUTABLE=/usr/bin/perl \
            -DGNC_DBD_DIR="$MINGW_PREFIX/lib/dbd"
        ;;
    build) "$cmake_cmd" --build "$build_dir" -j 8 ;;
    test) "$cmake_cmd" --build "$build_dir" --target check -j 8 ;;
    test-mcp)
        "$cmake_cmd" --build "$build_dir" --target test-live-mcp -j 8
        "$project_root/.windows-build/mcp-venv/Scripts/python.exe" -m pytest "$project_root/tools/mcp/test_gnucash_mcp.py" -q
        ;;
    install) "$cmake_cmd" --install "$build_dir" ;;
    version) "$install_dir/bin/gnucash.exe" --version ;;
    run) exec "$install_dir/bin/gnucash.exe" ;;
    mcp-run)
        connection_file="$(cygpath -m "$LOCALAPPDATA")/GnuCash/mcp/live.json"
        exec "$install_dir/bin/gnucash.exe" --mcp-file "$connection_file"
        ;;
    *) echo 'Unknown build action' >&2; exit 2 ;;
esac
