// swift-tools-version:5.10
// Package.swift
//
// Builds the whole app: the C++ DSP core (Sources/Core, same files the
// standalone CMake build in the repo root tests independently) plus the
// SwiftUI app shell (Sources/App) and the AVFoundation/bridging layer
// (Sources/Audio). Xcode can open this file directly (File > Open) for
// the full editing/debugging experience, so no separate .xcodeproj is
// maintained.
//
// AkaizerCore's public surface is the plain-C header at
// Sources/Core/include/AkaizerCore.h -- Swift imports it as a normal C
// module (`import AkaizerCore`), no bridging header or C++/Swift
// interop mode needed, because the C++ implementation is entirely hidden
// behind that C API by design (see AkaizerCore.h's header comment).

import PackageDescription

let package = Package(
    name: "AkaizerS",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .executable(name: "AkaizerS", targets: ["AkaizerSApp"])
    ],
    targets: [
        .target(
            name: "AkaizerCore",
            path: "Sources/Core",
            exclude: ["CMakeLists.txt"],
            sources: ["MachineProfile.cpp", "StretchEngine.cpp", "RealtimeStretchPlayer.cpp", "Interpolator.cpp", "ConverterModel.cpp", "FilterModel.cpp"],
            publicHeadersPath: "include",
            cxxSettings: [.unsafeFlags(["-std=c++17"])]
        ),
        .target(
            name: "AkaizerAudio",
            dependencies: ["AkaizerCore"],
            path: "Sources/Audio"
        ),
        .executableTarget(
            name: "AkaizerSApp",
            dependencies: ["AkaizerAudio", "AkaizerCore"],
            path: "Sources/App"
        ),
        .testTarget(
            name: "AkaizerAudioTests",
            dependencies: ["AkaizerAudio", "AkaizerCore"],
            path: "Tests/AudioTests"
        )
    ],
    cxxLanguageStandard: .cxx17
)
