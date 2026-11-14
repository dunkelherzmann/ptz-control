// swift-tools-version:5.10
// macOS host: Swift + AppKit over the shared C++20 core in core/.
import PackageDescription

let package = Package(
    name: "PTZControl",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "PTZControl", targets: ["PTZControl"]),
    ],
    targets: [
        .target(
            name: "PTZCore",
            path: "core",
            sources: ["src"],
            publicHeadersPath: "include",
            cxxSettings: [.unsafeFlags(["-Wall", "-Wextra", "-Wno-unused-parameter"])],
            linkerSettings: [.linkedLibrary("curl")]
        ),
        .executableTarget(
            name: "PTZControl",
            dependencies: ["PTZCore"],
            path: "macos/Sources/PTZControl",
            linkerSettings: [.linkedFramework("AppKit"), .linkedFramework("CoreMedia"), .linkedFramework("VideoToolbox")]
        ),
    ],
    cxxLanguageStandard: .cxx20
)
