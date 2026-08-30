add_library(fc_core STATIC
    timecode.cpp
    memory_pool.cpp
    timeline_model.cpp)

target_include_directories(fc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
