// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "openflow",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "openflow", targets: ["OpenFlow"])
    ],
    targets: [
        .executableTarget(
            name: "OpenFlow",
            path: "Sources/OpenFlow",
            resources: [
                .process("Resources")
            ]
        )
    ]
)
