cmake_minimum_required(VERSION 3.26)
project(beta-quest CXX)
set(CMAKE_CXX_STANDARD 23)

# The Dusklight version to build against: a release tag (e.g. "v1.5.0") or a commit SHA.
# To update your mod to a newer Dusklight version, change this line and reconfigure.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "ec51ba298b7086fa1f7f9cf590fb79041a0a7f81")
endif ()

# Fetches the Dusklight sources into ./dusklight and provides DUSKLIGHT_DIR.
# Use -DDUSKLIGHT_DIR= to point at an existing Dusklight checkout.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")

add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)
include_directories("${DUSKLIGHT_DIR}/src")


add_mod(beta-quest
        FEATURES game          # remove for service/asset-only mods; add webgpu for GfxService
        SOURCES src/mod.cpp
        MOD_JSON mod.json
)
