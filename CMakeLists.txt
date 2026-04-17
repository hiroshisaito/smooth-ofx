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
    # macOS: .ofx.bundle (将来対応)
    set_target_properties(smooth PROPERTIES
        OUTPUT_NAME "smooth"
        SUFFIX      ".ofx"
        PREFIX      ""
        BUNDLE      TRUE
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

# インストールターゲット
install(TARGETS smooth
    LIBRARY DESTINATION "${OFX_INSTALL_DIR}/smooth.ofx.bundle/Contents/Win64"
)

# --- スモークテスト用のミニホスト (Windows のみ) ---
option(BUILD_TESTS "Build host smoke test" ON)
if(BUILD_TESTS AND WIN32)
    add_executable(host_smoke tests/host_smoke.cpp)
    target_include_directories(host_smoke PRIVATE ${OFX_INCLUDE_DIR})
    set_target_properties(host_smoke PROPERTIES CXX_STANDARD 17)
endif()
