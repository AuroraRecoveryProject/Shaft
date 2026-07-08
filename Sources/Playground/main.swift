import Foundation
import Shaft
import ShaftCodeHighlight

#if os(Android)
    import ShaftRecoverySkia
#else
    import ShaftSetup
#endif

#if os(Android)
    func recoveryScaleArgument() -> Float {
        let arguments = CommandLine.arguments
        for (index, argument) in arguments.enumerated() {
            if argument == "--scale", index + 1 < arguments.count, let value = Float(arguments[index + 1]) {
                return value
            }
            if argument.hasPrefix("--scale="), let value = Float(argument.dropFirst("--scale=".count)) {
                return value
            }
        }
        if let rawValue = ProcessInfo.processInfo.environment["SHAFT_RECOVERY_SCALE"],
           let value = Float(rawValue)
        {
            return value
        }
        return 1.0
    }

    func recoveryRendererArgument() -> RecoverySkiaRendererMode {
        let arguments = CommandLine.arguments
        for (index, argument) in arguments.enumerated() {
            if argument == "--renderer", index + 1 < arguments.count,
               let mode = RecoverySkiaRendererMode(rawValue: arguments[index + 1])
            {
                return mode
            }
            if argument.hasPrefix("--renderer="),
               let mode = RecoverySkiaRendererMode(rawValue: String(argument.dropFirst("--renderer=".count)))
            {
                return mode
            }
        }
        return RecoverySkiaRendererMode.fromEnvironment()
    }

    useRecoverySkiaBackend(
        stopAfterFirstFrame: false,
        scale: recoveryScaleArgument(),
        rendererMode: recoveryRendererArgument()
    )
#else
    ShaftSetup.useDefault()
#endif

#if DEBUG && canImport(SwiftReload) && !os(Android)
    import SwiftReload
    LocalSwiftReloader(onReload: backend.scheduleReassemble).start()
#endif

runApp(Playground())

final class Playground: StatefulWidget {
    func createState() -> PlaygroundState {
        PlaygroundState()
    }
}

final class PlaygroundState: State<Playground> {
    let pageByTitle: [String: Widget] = {
        var pages: [String: Widget] = [
            "Observation": Concept_Observation(),
            "ShaftKit": Concept_ShaftKit(),
            "Backend": Concept_Backend(),
            "Background": Kit_Background(),
            "Button": Kit_Button(),
            "Divider": Kit_Divider(),
            "Image": Kit_Image(),
            "Icons": Kit_Icons(),
            "ListView": Kit_ListView(),
            "Markdown": Kit_Markdown(),
            "NavigationSplitView": Kit_NavigationSplitView(),
            "Resizable": Kit_Resizable(),
            "TextField": Kit_TextField(),
            "Typography": Kit_Typography(),
            "3D Cube": Demo_Cube(),
            "Shader": Demo_Shader(),
        ]
        #if !os(Android)
        pages["Hacker News"] = HackerNewsApp()
        pages["Multi Window"] = Demo_MultiWindow()
        #endif
        return pages
    }()

    lazy var selectedPage = ValueNotifier("Observation")

    override func initState() {
        super.initState()
        updateTitle()
        selectedPage.addListener(self, callback: handleSelectedPageChanged)
    }

    override func dispose() {
        selectedPage.removeListener(self)
        super.dispose()
    }

    private func handleSelectedPageChanged() {
        updateTitle()
    }

    private func updateTitle() {
        View.maybeOf(context)?.title = "Playground - \(selectedPage.wrappedValue)"

    }

    override func build(context: BuildContext) -> Widget {
        return NavigationSplitView {
            FixedListView(selection: selectedPage) {
                Section {
                    Text("Concepts")
                } content: {
                    MenuTile("Observation")
                    MenuTile("ShaftKit")
                    MenuTile("Backend")
                }
                Section {
                    Text("Controls")
                } content: {
                    MenuTile("Background")
                    MenuTile("Button")
                    MenuTile("Divider")
                    MenuTile("Image")
                    MenuTile("Icons")
                    MenuTile("ListView")
                    MenuTile("NavigationSplitView")
                    MenuTile("Resizable")
                    MenuTile("TextField")
                    MenuTile("Typography")
                    MenuTile("Markdown")
                }
                Section {
                    Text("Demos")
                } content: {
                    #if !os(Android)
                    MenuTile("Hacker News")
                    #endif
                    MenuTile("3D Cube")
                    MenuTile("Shader")
                    #if !os(Android)
                    MenuTile("Multi Window")
                    #endif
                    MenuTile("Video Codec")
                }
            }
        } detail: {
            let page = pageByTitle[selectedPage.wrappedValue]
            page ?? Text("Under construction").padding(.all(20))
        }
    }
}

final class MenuTile: StatelessWidget {
    init(_ title: String) {
        self.title = title
    }

    let title: String

    func build(context: any BuildContext) -> any Widget {
        ListTile(title) {
            Text(title)
        }
    }
}
