cmake_minimum_required(VERSION 3.20)
project(smooth-ofx VERSION 1.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# --- スモークテスト用のミニホスト (Windows のみ) ---
option(BUILD_TESTS "Build host smoke test" ON)
if(BUILD_TESTS AND WIN32)
    add_executable(host_smoke tests/host_smoke.cpp)
    target_include_directories(host_smoke PRIVATE ${OFX_INCLUDE_DIR})
    set_target_properties(host_smoke PROPERTIES CXX_STANDARD 17)
    if(MSVC)
        target_compile_options(host_smoke PRIVATE /utf-8)
    endif()
endif()
