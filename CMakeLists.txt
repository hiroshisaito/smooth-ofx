cmake_minimum_required(VERSION 3.20)
project(smooth-ofx VERSION 1.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- ビルド ID 採取 (UI 上で UAT 中のビルドを判別するため) ---
# git short SHA + (working tree が dirty なら "+dirty") を構成時に取得し、
# プリプロセッサ定義 SMOOTH_OFX_GIT_SHA として smooth.cpp に渡す。
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE  SMOOTH_OFX_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT SMOOTH_OFX_GIT_SHA)
    set(SMOOTH_OFX_GIT_SHA "unknown")
endif()
execute_process(
    COMMAND git diff --quiet HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE  _SMOOTH_OFX_DIRTY
    ERROR_QUIET
)
if(_SMOOTH_OFX_DIRTY EQUAL 1)
    set(SMOOTH_OFX_GIT_SHA "${SMOOTH_OFX_GIT_SHA}+dirty")
endif()
message(STATUS "smooth-ofx build id: ${PROJECT_VERSION}+${SMOOTH_OFX_GIT_SHA}")

# OFX SDK (submodule: include/openfx)
set(OFX_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include/openfx/include")
if(NOT EXISTS "${OFX_INCLUDE_DIR}/ofxImageEffect.h")
    message(FATAL_ERROR
        "OFX SDK headers not found at ${OFX_INCLUDE_DIR}\n"
        "Run: git submodule update --init --recursive"
    )
endif()

add_library(smooth MODULE
    smooth/smooth.cpp
    smooth/upMode.cpp
    smooth/downMode.cpp
    smooth/8link.cpp
    smooth/Lack.cpp
    smooth/util.cpp
)

target_include_directories(smooth PRIVATE
    ${OFX_INCLUDE_DIR}
    smooth/
)

target_compile_definitions(smooth PRIVATE
    SMOOTH_OFX_VERSION="${PROJECT_VERSION}"
    SMOOTH_OFX_GIT_SHA="${SMOOTH_OFX_GIT_SHA}"
)

# MSVC: ソースを UTF-8 として解釈 (ソースファイルに BOM が無いため、
# 既定の ACP (日本語環境では Shift-JIS) だと先頭バイト 0x5C 問題で誤解析される)
if(MSVC)
    target_compile_options(smooth PRIVATE /utf-8)
endif()

# --- プラットフォーム別設定 ---

if(WIN32)
    # Windows: .ofx は DLL
    set_target_properties(smooth PROPERTIES
        OUTPUT_NAME "smooth"
        SUFFIX      ".ofx"
        PREFIX      ""
    )
    # OFXプラグインのインストール先 (DaVinci Resolve / Vegas 等の標準パス)
    set(OFX_INSTALL_DIR "C:/Program Files/Common Files/OFX/Plugins"
        CACHE PATH "OFX plugin install directory")

elseif(APPLE)
    # macOS: smooth.ofx.bundle/Contents/MacOS/smooth.ofx
    # Info.plist は dist/Info.plist.in から CMake が生成する
    # OUTPUT_NAME="smooth.ofx" + BUNDLE_EXTENSION="bundle" で
    #   bundle フォルダ = "smooth.ofx.bundle"
    #   内部実行ファイル = "smooth.ofx"
    set_target_properties(smooth PROPERTIES
        OUTPUT_NAME                         "smooth.ofx"
        SUFFIX                              ""
        PREFIX                              ""
        BUNDLE                              TRUE
        BUNDLE_EXTENSION                    "bundle"
        MACOSX_BUNDLE_INFO_PLIST            "${CMAKE_SOURCE_DIR}/dist/Info.plist.in"
        MACOSX_BUNDLE_EXECUTABLE_NAME       "smooth.ofx"
        MACOSX_BUNDLE_BUNDLE_NAME           "smooth"
        MACOSX_BUNDLE_BUNDLE_VERSION        "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING  "${PROJECT_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER        "jp.loilo.smooth"
    )
    set(OFX_INSTALL_DIR "/Library/OFX/Plugins"
        CACHE PATH "OFX plugin install directory")

else()
    # Linux (将来対応)
    set_target_properties(smooth PROPERTIES
        OUTPUT_NAME "smooth"
        SUFFIX      ".ofx"
        PREFIX      ""
    )
    set(OFX_INSTALL_DIR "/usr/OFX/Plugins"
        CACHE PATH "OFX plugin install directory")
endif()

# インストールターゲット (プラットフォーム別)
if(APPLE)
    # Bundle 丸ごと OFX_INSTALL_DIR 直下に配置
    install(TARGETS smooth
        BUNDLE  DESTINATION "${OFX_INSTALL_DIR}"
        LIBRARY DESTINATION "${OFX_INSTALL_DIR}"
    )
elseif(WIN32)
    install(TARGETS smooth
        LIBRARY DESTINATION "${OFX_INSTALL_DIR}/smooth.ofx.bundle/Contents/Win64"
    )
else()
    install(TARGETS smooth
        LIBRARY DESTINATION "${OFX_INSTALL_DIR}/smooth.ofx.bundle/Contents/Linux-x86-64"
    )
endif()

# --- スモークテスト用のミニホスト (cross-platform) ---
option(BUILD_TESTS "Build host smoke test" ON)
if(BUILD_TESTS)
    add_executable(host_smoke tests/host_smoke.cpp)
    target_include_directories(host_smoke PRIVATE ${OFX_INCLUDE_DIR})
    set_target_properties(host_smoke PROPERTIES CXX_STANDARD 17)
    if(MSVC)
        target_compile_options(host_smoke PRIVATE /utf-8)
    endif()
    if(UNIX AND NOT APPLE)
        # Linux: dlopen needs -ldl
        target_link_libraries(host_smoke PRIVATE dl)
    endif()
endif()

# --- Rust core (smooth_core) integration ---
# 1.6.0 から default ON。Resolve UAT (macOS, 2026-05-06) で 全項目 PASS:
# build-id / range / line weight / transparent / 8/16/16f/32f / 線画黒ピクセル無し /
# 4K (4400x4400) で体感速度向上。
# Rust toolchain 不在の環境では明示的に -DUSE_RUST_CORE=OFF を指定して C++ baseline で
# ビルドできる (cargo が見つからない場合は configure 時に FATAL_ERROR で停止)。
option(USE_RUST_CORE "Build & link smooth_core Rust crate (default ON since 1.6.0)" ON)

if(USE_RUST_CORE)
    set(RUST_MANIFEST "${CMAKE_SOURCE_DIR}/smooth-ae/rust/smooth_core/Cargo.toml")
    set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo-target")

    if(NOT EXISTS "${RUST_MANIFEST}")
        message(FATAL_ERROR
            "smooth_core Cargo.toml not found at ${RUST_MANIFEST}\n"
            "Run: git submodule update --init smooth-ae"
        )
    endif()

    find_program(CARGO_EXECUTABLE cargo)
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR "cargo not found in PATH; install Rust (https://rustup.rs) or disable USE_RUST_CORE")
    endif()

    # CMake arch -> Rust target triple
    set(_RUST_TARGETS "")
    if(APPLE)
        if(CMAKE_OSX_ARCHITECTURES)
            foreach(_ARCH IN LISTS CMAKE_OSX_ARCHITECTURES)
                if(_ARCH STREQUAL "arm64")
                    list(APPEND _RUST_TARGETS "aarch64-apple-darwin")
                elseif(_ARCH STREQUAL "x86_64")
                    list(APPEND _RUST_TARGETS "x86_64-apple-darwin")
                else()
                    message(FATAL_ERROR "Unsupported CMAKE_OSX_ARCHITECTURES entry: ${_ARCH}")
                endif()
            endforeach()
        else()
            # No explicit arch -> use host
            if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
                list(APPEND _RUST_TARGETS "aarch64-apple-darwin")
            else()
                list(APPEND _RUST_TARGETS "x86_64-apple-darwin")
            endif()
        endif()
    elseif(WIN32)
        list(APPEND _RUST_TARGETS "x86_64-pc-windows-msvc")
    else()
        list(APPEND _RUST_TARGETS "x86_64-unknown-linux-gnu")
    endif()

    # Per-arch cargo build commands; collect resulting static libs for lipo merge.
    # Cargo names static libs per platform: MSVC -> `<crate>.lib`, everything
    # else (MinGW, macOS, Linux) -> `lib<crate>.a`.
    set(_RUST_LIB_FILES "")
    foreach(_RUST_TGT IN LISTS _RUST_TARGETS)
        if(_RUST_TGT MATCHES "windows-msvc$")
            set(_LIB_PATH "${RUST_TARGET_DIR}/${_RUST_TGT}/release/smooth_core.lib")
        else()
            set(_LIB_PATH "${RUST_TARGET_DIR}/${_RUST_TGT}/release/libsmooth_core.a")
        endif()
        list(APPEND _RUST_LIB_FILES "${_LIB_PATH}")

        set(_CARGO_ENV "")
        if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
            list(APPEND _CARGO_ENV "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()

        add_custom_command(
            OUTPUT  "${_LIB_PATH}"
            COMMAND ${CMAKE_COMMAND} -E env ${_CARGO_ENV}
                    ${CARGO_EXECUTABLE} build --release
                            --manifest-path "${RUST_MANIFEST}"
                            --target-dir "${RUST_TARGET_DIR}"
                            --target ${_RUST_TGT}
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "cargo build smooth_core (${_RUST_TGT})"
            VERBATIM
        )
        # 個別の cargo target のため、cargo の incremental に任せる
        # (CMake の依存ファイル列挙だと .rs を全部明記する必要が出るため省略)。
        set_source_files_properties("${_LIB_PATH}" PROPERTIES SYMBOLIC TRUE)
    endforeach()

    list(LENGTH _RUST_LIB_FILES _NUM_RUST_LIBS)
    if(APPLE AND _NUM_RUST_LIBS GREATER 1)
        # マルチアーキ: lipo で fat static lib に統合
        set(SMOOTH_CORE_LIB "${RUST_TARGET_DIR}/libsmooth_core_universal.a")
        add_custom_command(
            OUTPUT  "${SMOOTH_CORE_LIB}"
            COMMAND lipo -create -output "${SMOOTH_CORE_LIB}" ${_RUST_LIB_FILES}
            DEPENDS ${_RUST_LIB_FILES}
            COMMENT "lipo merge smooth_core (${_RUST_TARGETS})"
            VERBATIM
        )
    else()
        list(GET _RUST_LIB_FILES 0 SMOOTH_CORE_LIB)
    endif()

    add_custom_target(smooth_core_lib ALL DEPENDS "${SMOOTH_CORE_LIB}")
    add_dependencies(smooth smooth_core_lib)
    target_link_libraries(smooth PRIVATE "${SMOOTH_CORE_LIB}")
    # MSVC: cargo's static lib does not propagate the native deps Rust's std
    # needs. `cargo rustc -- --print native-static-libs` reports the set below
    # for x86_64-pc-windows-msvc on Rust 1.94+; kernel32 is implicit.
    if(MSVC)
        target_link_libraries(smooth PRIVATE ntdll userenv ws2_32 dbghelp)
    endif()
    target_include_directories(smooth PRIVATE
        "${CMAKE_SOURCE_DIR}/smooth-ae/rust/smooth_core/include"
    )
    target_compile_definitions(smooth PRIVATE SMOOTH_OFX_USE_RUST_CORE=1)

    message(STATUS "smooth_core: rust targets = ${_RUST_TARGETS}")
    message(STATUS "smooth_core: linking ${SMOOTH_CORE_LIB}")
endif()
