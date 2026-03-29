# TBD-16 Pico Audio
# Drop-in audio driver for the RP2350 on DaDa Machines TBD-16.
#
# Provides two CMake targets:
#
#   tbd16_audio_core  — Simple API + callback API only
#   tbd16_audio       — Full driver with pico_audio_i2s compatibility
#
# Usage in your CMakeLists.txt:
#
#   add_subdirectory(tbd16-audio)
#   target_link_libraries(your_target tbd16_audio)
#
# For projects that don't use the pico_audio_i2s API:
#
#   target_link_libraries(your_target tbd16_audio_core)

# Core driver (simple API + callback API)
add_library(tbd16_audio_core INTERFACE)
target_sources(tbd16_audio_core INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/tbd16_audio.c
)
target_include_directories(tbd16_audio_core INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Full driver with pico_audio_i2s compatibility
add_library(tbd16_audio INTERFACE)
target_sources(tbd16_audio INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/tbd16_audio.c
    ${CMAKE_CURRENT_LIST_DIR}/pico_audio_i2s_compat.c
)
target_include_directories(tbd16_audio INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)
