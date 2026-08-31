# Sources/Core/CMakeLists.txt
#
# The Akaizer S DSP core, as a static library with no dependency on any
# Apple framework -- see AkaizerCore.h for the rationale. Consumed by the
# Xcode app target directly, and by Tests/CoreTests here for standalone
# validation.

add_library(AkaizerCore STATIC
    include/AkaizerCore.h
    MachineProfile.h
    MachineProfile.cpp
    StretchEngine.h
    StretchEngine.cpp
    RealtimeStretchPlayer.h
    RealtimeStretchPlayer.cpp
    Interpolator.h
    Interpolator.cpp
    ConverterModel.h
    ConverterModel.cpp
    FilterModel.h
    FilterModel.cpp
    RateModel.h
    RateModel.cpp
)

find_package(Threads REQUIRED)
target_link_libraries(AkaizerCore PUBLIC Threads::Threads)

target_include_directories(AkaizerCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(AkaizerCore PUBLIC cxx_std_17)
