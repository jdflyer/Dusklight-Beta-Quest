cmake_minimum_required(VERSION 3.26)
project(my_mod CXX)

# The Dusklight version to build against: a release tag (e.g. "v1.5.0") or a commit SHA.
# To update your mod to a newer Dusklight version, change this line and reconfigure.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "76b56cd8b81809fce0a5c2a44e2f6d437591132f")
endif ()

# Fetches the Dusklight sources into ./dusklight and provides DUSKLIGHT_DIR.
# Use -DDUSKLIGHT_DIR= to point at an existing Dusklight checkout.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")

add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

add_mod(my_mod
        FEATURES game          # remove for service/asset-only mods; add webgpu for GfxService
        SOURCES src/mod.cpp
        MOD_JSON mod.json
        RES_DIR res            # mod resources, including icon.png and banner.png
        OVERLAY_DIR overlay    # game file overlays; remove if unused
        TEXTURES_DIR textures  # texture replacements; remove if unused
)
