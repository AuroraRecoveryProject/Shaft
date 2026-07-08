// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import Foundation
import PackageDescription

let recoverySkia = ProcessInfo.processInfo.environment["SHAFT_RECOVERY_SKIA"] == "1"
let packageRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent().path
let repoRoot = URL(fileURLWithPath: packageRoot).appendingPathComponent("..").standardized.path

func resolvePackagePath(_ value: String) -> String {
    URL(fileURLWithPath: packageRoot).appendingPathComponent(value).standardized.path
}

func resolveRepoPath(_ value: String) -> String {
    if value.hasPrefix("/") {
        return URL(fileURLWithPath: value).standardized.path
    }
    return URL(fileURLWithPath: repoRoot).appendingPathComponent(value).standardized.path
}

func requiredRecoveryEnv(_ name: String) -> String {
    if let value = ProcessInfo.processInfo.environment[name], !value.isEmpty {
        return value
    }
    if recoverySkia {
        fatalError("\(name) must be set when SHAFT_RECOVERY_SKIA=1")
    }
    return ""
}

let vendoredSwiftMath = "third_party/SwiftMath"
let minuitwrpRoot = resolveRepoPath(requiredRecoveryEnv("MINUITWRP_PREBUILT_ROOT"))
let defaultMinuitwrpPrebuiltLib = "\(minuitwrpRoot)/build/libminuitwrp_core.a"
let minuitwrpPrebuiltLib = resolveRepoPath(ProcessInfo.processInfo.environment["MINUITWRP_PREBUILT_LIB"] ?? defaultMinuitwrpPrebuiltLib)
let skiaSourceRoot = recoverySkia ? requiredRecoveryEnv("SHAFT_SKIA_SOURCE_ROOT") : ""
let skiaIcuSourceRoot = recoverySkia
    ? URL(fileURLWithPath: skiaSourceRoot)
        .deletingLastPathComponent()
        .appendingPathComponent("icu/source/common")
        .standardized.path
    : ""
let skiaAndroidLibRoot = resolveRepoPath(requiredRecoveryEnv("SHAFT_SKIA_ANDROID_LIB_ROOT"))
let skiaHarfbuzzLib = "\(skiaAndroidLibRoot)/libharfbuzz.a"
let skiaRecoverySupportLibs = [
    "libskparagraph.a",
    "libskshaper.a",
    "libskunicode_icu.a",
    "libskunicode_core.a",
    "libicu.a",
    "libpng.a",
    "libzlib.a",
    "libskcms.a",
    "libexpat.a",
    "libcpu-features.a",
].map { "\(skiaAndroidLibRoot)/\($0)" } + [
    skiaHarfbuzzLib,
    "\(skiaAndroidLibRoot)/libskia.a",
]
let skiaCIncludeFlags = [skiaSourceRoot, skiaIcuSourceRoot]
    .filter { !$0.isEmpty }
    .map { "-I\($0)" }
let skiaSwiftImportFlags = [skiaSourceRoot, skiaIcuSourceRoot]
    .filter { !$0.isEmpty }
    .flatMap { ["-Xcc", "-I\($0)"] }

let package = Package(
    name: "Shaft",

    platforms: [
        .macOS(.v14),
        .iOS(.v13),
        .tvOS(.v13),
    ],

    products: [
        // Shaft playground app
        .executable(name: "Playground", targets: ["Playground"]),

        // .executable(name: "WebDemo", targets: ["WebDemo"]),

        // The Shaft framework, is platform-independent and requires a backend
        // to run.
        .library(name: "Shaft", targets: ["Shaft"]),

        // The helper library for setting up the default backend and renderer.
        .library(name: "ShaftSetup", targets: ["ShaftSetup"]),

        // Code highlighting library for Shaft
        .library(name: "ShaftCodeHighlight", targets: ["ShaftCodeHighlight"]),

        // The Lucide icons for Shaft
        .library(name: "ShaftLucide", targets: ["ShaftLucide"]),

        // The SDL3 backend for Shaft
        .library(name: "ShaftSDL3", targets: ["ShaftSDL3"]),

        // The Skia renderer for Shaft
        .library(name: "ShaftSkia", targets: ["ShaftSkia"]),

        // The Markdown support for Shaft
        .library(name: "ShaftMarkdown", targets: ["ShaftMarkdown"]),

        // The Android recovery Skia backend.
        .library(name: "ShaftRecoverySkia", targets: ["ShaftRecoverySkia"]),

        // (experimental) Tool to build application bundles
        .plugin(name: "BuilderPlugin", targets: ["BuilderPlugin"]),
    ],

    dependencies: [
        recoverySkia
            ? .package(name: "SwiftMath", path: vendoredSwiftMath)
            : .package(
                url: "https://github.com/ShaftUI/SwiftMath",
                .upToNextMajor(from: "3.4.0")
            ),
        .package(
            url: "https://github.com/ShaftUI/SwiftSDL3",
            exact: "0.1.6"
        ),
        // .package(
        //     name: "SwiftSDL3",
        //     path: "../SwiftSDL3"
        // ),
        .package(
            url: "https://github.com/onevcat/Rainbow",
            .upToNextMajor(from: "4.0.0")
        ),
        .package(
            url: "https://github.com/ShaftUI/swift-collections",
            .upToNextMinor(from: "1.3.0")
        ),
        .package(
            url: "https://github.com/ShaftUI/Splash",
            branch: "master"
        ),
        .package(
            url: "https://github.com/ShaftUI/SwiftReload.git",
            from: "0.0.1"
        ),
        .package(
            url: "https://github.com/swiftlang/swift-markdown.git",
            branch: "main"
        ),
        .package(
            url: "https://github.com/apple/swift-docc-plugin",
            from: "1.0.0"
        ),
    ],

    targets: [
        recoverySkia
            ? .executableTarget(
                name: "Playground",
                dependencies: [
                    "Fetch",
                    "Shaft",
                    "ShaftMarkdown",
                    "ShaftSkia",
                    "ShaftRecoverySkia",
                    "ShaftLucide",
                    "ShaftCodeHighlight",
                ],
                path: "Sources/Playground",
                exclude: [
                    "HackerNews",
                    "Metal",
                    "Pages/Demo_MultiWindow.swift",
                ],
                swiftSettings: [
                    .interoperabilityMode(.Cxx),
                ],
                linkerSettings: [
                    .unsafeFlags([
                        minuitwrpPrebuiltLib,
                        "../build/embedded-assets/icudtl.o",
                        "../build/embedded-assets/roboto_regular.o",
                    ] + skiaRecoverySupportLibs),
                    .linkedLibrary("log", .when(platforms: [.android])),
                    .linkedLibrary("z"),
                    .linkedLibrary("dl"),
                    .linkedLibrary("m"),
                    .linkedLibrary("c++_static"),
                ]
            )
            : .executableTarget(
                name: "Playground",
                dependencies: [
                    "Fetch",
                    "SwiftMath",
                    "Shaft",
                    "ShaftMarkdown",
                    "ShaftSetup",
                    "ShaftLucide",
                    "ShaftCodeHighlight",
                    .product(
                        name: "SwiftReload",
                        package: "SwiftReload",
                        condition: .when(platforms: [.linux, .macOS])
                    ),
                ],
                swiftSettings: [
                    .interoperabilityMode(.Cxx, .when(platforms: [.windows, .linux, .macOS])),
                    .unsafeFlags(["-Xfrontend", "-enable-private-imports"]),
                    .unsafeFlags(["-Xfrontend", "-enable-implicit-dynamic"]),
                ],
                linkerSettings: [
                    .unsafeFlags(
                        ["-Xlinker", "--export-dynamic"],
                        .when(platforms: [.linux, .android])
                    )
                ]
            ),

        .target(
            name: "CSkia",
            dependencies: recoverySkia ? [] : [
                "skia"
            ],
            sources: [
                "utils.cpp",
                "recovery_vulkan.cpp",
                "runtime_shader_demo.cpp"
            ],
            publicHeadersPath: recoverySkia ? "Public" : ".",
            cSettings: recoverySkia ? [
                .unsafeFlags(skiaCIncludeFlags)
            ] : [],
            cxxSettings: (recoverySkia ? [
                .unsafeFlags(skiaCIncludeFlags)
            ] : []) + [
                .define("SK_FONTMGR_FONTCONFIG_AVAILABLE", .when(platforms: [.linux]))
            ],
            swiftSettings: [
                .unsafeFlags(["-Xfrontend", "-enable-private-imports"]),
                .unsafeFlags(["-Xfrontend", "-enable-implicit-dynamic"]),
            ],
            linkerSettings: [
                .linkedLibrary("d3d12", .when(platforms: [.windows])),
                .linkedLibrary("d3dcompiler", .when(platforms: [.windows])),
                .linkedLibrary("openGL32", .when(platforms: [.windows])),
                .linkedLibrary("stdc++", .when(platforms: [.windows])),
                .linkedLibrary("dxgi", .when(platforms: [.windows])),

                .linkedFramework("CoreGraphics", .when(platforms: [.macOS])),
                .linkedFramework("CoreText", .when(platforms: [.macOS])),
                .linkedFramework("CoreFoundation", .when(platforms: [.macOS])),
                .linkedFramework("Metal", .when(platforms: [.macOS])),

                .linkedLibrary("fontconfig", .when(platforms: [.linux])),
                .linkedLibrary("freetype", .when(platforms: [.linux])),
                .linkedLibrary("GL", .when(platforms: [.linux])),
                .linkedLibrary("GLX", .when(platforms: [.linux])),
                .linkedLibrary("wayland-client", .when(platforms: [.linux])),

                // .unsafeFlags(["-L.shaft/skia"]),
            ]
        ),

        .binaryTarget(
            name: "skia",
            url:
                "https://github.com/ShaftUI/skia-bundle/releases/download/build-126-3/skia-m126-6bfb13368b.artifactbundle.zip",
            checksum: "d21b5ab4d3c084cf931ff19c4a9d0a34984db4750c76eaab5e8aff785ba7c30e"
        ),

        .target(
            name: "CSkiaResource",
            resources: [
                .copy("icudtl.dat")
            ]
        ),

        .target(
            name: "CRecoverySkia",
            path: "Sources/CRecoverySkia",
            publicHeadersPath: ".",
            cxxSettings: [
                .unsafeFlags([
                    "-std=c++20",
                ])
            ]
        ),

        .target(
            name: "ShaftRecoverySkia",
            dependencies: [
                "Shaft",
                "ShaftSkia",
                "CRecoverySkia",
            ],
            swiftSettings: [.interoperabilityMode(.Cxx)]
        ),

        .plugin(
            name: "BuilderPlugin",
            capability: .command(
                intent: .custom(verb: "build", description: "Build application bundle"),
                permissions: [
                    .allowNetworkConnections(
                        scope: .all(),
                        reason: "To retrieve additional resources"
                    ),
                    .writeToPackageDirectory(reason: "To read configuration files"),
                ]
            )
        ),

        .target(
            name: "Fetch"
        ),

        .target(
            name: "Shaft",
            dependencies: [
                "SwiftMath",
                "Fetch",
                .product(name: "Collections", package: "swift-collections"),
            ] + (recoverySkia ? [] : [
                "Rainbow",
            ])
        ),

        .target(
            name: "ShaftSetup",
            dependencies: [
                "Shaft",
                .target(
                    name: "ShaftSkia",
                    condition: .when(platforms: [.linux, .windows, .macOS])
                ),
                .target(
                    name: "ShaftSDL3",
                    condition: .when(platforms: [.linux, .windows, .macOS])
                ),
            ],
            swiftSettings: [
                .interoperabilityMode(.Cxx, .when(platforms: [.linux, .windows, .macOS]))
            ]
        ),

        .target(
            name: "ShaftSDL3",
            dependencies: [
                "SwiftSDL3",
                "SwiftMath",
                "Shaft",
            ],
            swiftSettings: [.interoperabilityMode(.Cxx)]
        ),

        .target(
            name: "ShaftSkia",
            dependencies: [
                "CSkia",
                "CSkiaResource",
                "SwiftMath",
                "Shaft",
            ],
            swiftSettings: [.interoperabilityMode(.Cxx)] + (recoverySkia ? [
                .unsafeFlags(skiaSwiftImportFlags)
            ] : [])
        ),

        .target(
            name: "ShaftCodeHighlight",
            dependencies: [
                "Splash",
                "Shaft",
            ]
        ),

        .target(
            name: "ShaftLucide",
            dependencies: [
                "Shaft"
            ],
            exclude: [
                "Resource/lucide.woff2"
            ],
            resources: [
                .embedInCode("Resource/lucide.json"),
                .embedInCode("Resource/lucide.ttf"),
            ]
        ),

        .target(
            name: "ShaftMarkdown",
            dependencies: [
                "Shaft",
                .product(name: "Markdown", package: "swift-markdown"),
            ]
        ),

        .testTarget(
            name: "ShaftTests",
            dependencies: [
                "Shaft",
                "ShaftSetup",
            ],
            swiftSettings: [
                .interoperabilityMode(.Cxx)
            ]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
