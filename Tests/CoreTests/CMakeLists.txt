# Tests/CoreTests/CMakeLists.txt
#
# Zero-dependency test binary for the C++ DSP core -- see TestFramework.h.

add_executable(AkaizerCoreTests
    TestFramework.h
    main.cpp
    MachineProfileTests.cpp
    StretchEngineTests.cpp
    ArtifactTests.cpp
    RealtimeStretchPlayerTests.cpp
    InterpolatorTests.cpp
    ConverterModelTests.cpp
    FilterModelTests.cpp
    IntelligentModeTests.cpp
)

target_link_libraries(AkaizerCoreTests PRIVATE AkaizerCore)
target_compile_features(AkaizerCoreTests PRIVATE cxx_std_17)

add_test(NAME AkaizerCoreTests COMMAND AkaizerCoreTests)
