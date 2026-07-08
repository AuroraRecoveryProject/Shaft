import CRecoverySkia
import CSkia
import Foundation
import Shaft
import ShaftSkia
import SwiftMath

public typealias RecoverySkiaTimer = Shaft.Timer
public typealias ShaftLocale = Shaft.Locale
public typealias ShaftRect = Shaft.Rect
public typealias ShaftSize = Shaft.Size

@_silgen_name("usleep")
private func c_usleep(_ useconds: UInt32) -> Int32

private func recoveryLog(_ message: StaticString) {
    message.withUTF8Buffer { buffer in
        buffer.withMemoryRebound(to: CChar.self) { chars in
            recovery_skia_log_marker(chars.baseAddress!)
        }
    }
}

private func recoveryLog(_ message: String) {
    message.withCString { chars in
        recovery_skia_log_marker(chars)
    }
}

public func recoverySkiaDebugLog(_ message: StaticString) {
    recoveryLog(message)
}

private enum RecoveryTouchCoordinateSpace {
    case logical
    case physical

    static func fromEnvironment() -> Self {
        switch ProcessInfo.processInfo.environment["SHAFT_RECOVERY_TOUCH_COORDINATES"] {
        case "logical":
            return .logical
        default:
            return .physical
        }
    }
}

public enum RecoverySkiaRendererMode: String {
    case software
    case vulkan

    public static func fromEnvironment() -> Self {
        if let rawValue = ProcessInfo.processInfo.environment["SHAFT_RECOVERY_RENDERER"],
           let mode = Self(rawValue: rawValue)
        {
            return mode
        }
        return .software
    }
}

public final class RecoverySkiaBackend: Backend {
    public init(
        renderer: SkiaRenderer = SkiaRenderer(),
        quitOnLastWindowClose: Bool = false,
        stopAfterFirstFrame: Bool = false,
        devicePixelRatio: Float = 1.0,
        rendererMode: RecoverySkiaRendererMode = .software
    ) {
        self.renderer = renderer
        self.skiaRenderer = renderer
        self.quitOnLastWindowClose = quitOnLastWindowClose
        self.stopAfterFirstFrame = stopAfterFirstFrame
        self.devicePixelRatio = max(0.25, devicePixelRatio)
        self.rendererMode = rendererMode
    }

    public let renderer: Renderer
    private let skiaRenderer: SkiaRenderer
    public var quitOnLastWindowClose: Bool
    public var stopAfterFirstFrame: Bool

    private var views: [Int: RecoverySkiaView] = [:]
    private var nextViewID = 1
    private var tasks: [() -> Void] = []
    private var stopped = false
    private var frameScheduled = false
    private var frameCount = 0
    private let start = ContinuousClock.now
    private let devicePixelRatio: Float
    private let rendererMode: RecoverySkiaRendererMode
    private let touchCoordinateSpace = RecoveryTouchCoordinateSpace.fromEnvironment()
    private var pointerIdentifier = 0
    private var pointerIdentifiers: [Int: Int] = [:]
    private var pointerPositions: [Int: (x: Int, y: Int)] = [:]

    public var onPointerData: PointerDataCallback?
    public var onKeyEvent: KeyEventCallback?
    public var onMetricsChanged: MetricsChangedCallback?
    public var onBeginFrame: FrameCallback?
    public var onDrawFrame: VoidCallback?
    public var onReassemble: VoidCallback?
    public var onAppLifecycleStateChanged: AppLifecycleStateCallback?
    public private(set) var lifecycleState: AppLifecycleState = .detached

    public func createView() -> NativeView? {
        let initResult = recovery_skia_init()
        if initResult != 0 {
            recoveryLog("[shaft-recovery-skia] createView init failed")
            return nil
        }
        guard let pixels = recovery_skia_pixels() else {
            recoveryLog("[shaft-recovery-skia] createView pixels failed")
            recovery_skia_shutdown()
            return nil
        }

        let view = RecoverySkiaView(
            viewID: nextViewID,
            backend: self,
            renderer: skiaRenderer,
            pixels: pixels,
            rowBytes: Int(recovery_skia_row_bytes()),
            width: Int(recovery_skia_width()),
            height: Int(recovery_skia_height()),
            devicePixelRatio: devicePixelRatio,
            rendererMode: rendererMode
        )
        if rendererMode == .software {
            recoveryLog("[shaft-recovery-skia] renderer=software")
        }
        nextViewID += 1
        views[view.viewID] = view
        lifecycleState = .resumed
        onAppLifecycleStateChanged?(.resumed)
        return view
    }

    public func destroyView(_ view: NativeView) {
        views.removeValue(forKey: view.viewID)?.isDestroyed = true
        if views.isEmpty {
            recovery_skia_shutdown()
            lifecycleState = .detached
            onAppLifecycleStateChanged?(.detached)
            if quitOnLastWindowClose {
                stop()
            }
        }
    }

    public func view(_ viewId: Int) -> NativeView? {
        views[viewId]
    }

    public func getKeyboardState() -> [PhysicalKeyboardKey: LogicalKeyboardKey]? {
        nil
    }

    public func launchUrl(_ url: String) -> Bool {
        false
    }

    public func scheduleFrame() {
        guard !frameScheduled else { return }
        frameScheduled = true
        postTask { [weak self] in
            self?.produceFrame()
        }
    }

    private func produceFrame() {
        frameScheduled = false
        frameCount += 1
        let elapsed = start.duration(to: ContinuousClock.now)
        onBeginFrame?(elapsed)
        onDrawFrame?()
        if stopAfterFirstFrame && frameCount >= 1 {
            stop()
        }
    }

    public func scheduleReassemble() {
        postTask { [weak self] in self?.onReassemble?() }
    }

    public func run() {
        stopped = false
        while !stopped {
            pumpPointerEvents()
            var didWork = false
            while !tasks.isEmpty {
                let task = tasks.removeFirst()
                task()
                didWork = true
                pumpPointerEvents()
            }
            if !didWork {
                _ = c_usleep(1_000)
            }
        }
    }

    public func stop() {
        stopped = true
    }

    public var isMainThread: Bool { true }

    public func postTask(_ f: @escaping () -> Void) {
        tasks.append(f)
    }

    private func pumpPointerEvents() {
        var event = RecoverySkiaTouchEvent()
        while recovery_skia_poll_touch(&event) != 0 {
            guard let view = views.values.first else { continue }
            let slot = Int(event.slot)
            let eventX = Int(event.x)
            let eventY = Int(event.y)
            let x: Int
            let y: Int
            switch touchCoordinateSpace {
            case .logical:
                x = Int((Float(eventX) * view.devicePixelRatio).rounded())
                y = Int((Float(eventY) * view.devicePixelRatio).rounded())
            case .physical:
                x = eventX
                y = eventY
            }
            let previous = pointerPositions[slot] ?? (x: x, y: y)
            let change: PointerChange
            let buttons: PointerButtons

            switch event.phase {
            case 1:
                pointerIdentifier += 1
                pointerIdentifiers[slot] = pointerIdentifier
                pointerPositions[slot] = (x, y)
                change = .down
                buttons = .primaryButton
            case 2:
                pointerPositions[slot] = (x, y)
                change = .move
                buttons = .primaryButton
            case 3:
                pointerPositions.removeValue(forKey: slot)
                change = .up
                buttons = []
            default:
                continue
            }

            let packet = PointerData(
                viewId: view.viewID,
                timeStamp: start.duration(to: ContinuousClock.now),
                change: change,
                kind: .touch,
                device: slot,
                pointerIdentifier: pointerIdentifiers[slot] ?? pointerIdentifier,
                physicalX: x,
                physicalY: y,
                physicalDeltaX: x - previous.x,
                physicalDeltaY: y - previous.y,
                buttons: buttons
            )
            recovery_skia_log_touch_packet(
                Int32(event.phase),
                Int32(slot),
                Int32(eventX),
                Int32(eventY),
                Int32(x),
                Int32(y),
                view.devicePixelRatio,
                Float(x) / view.devicePixelRatio,
                Float(y) / view.devicePixelRatio
            )
            onPointerData?(packet)

            if event.phase == 3 {
                pointerIdentifiers.removeValue(forKey: slot)
            }
        }
    }

    public func createTimer(
        _ delay: Duration,
        repeat shouldRepeat: Bool,
        callback: @escaping () -> Void
    ) -> RecoverySkiaTimer {
        let timer = RecoverySkiaBackendTimer()
        func scheduleOnce() {
            Task.detached { [weak self, weak timer] in
                let micros = max(0, delay.inMicroseconds)
                try? await Task.sleep(nanoseconds: UInt64(micros) * 1_000)
                guard let timer, timer.isActive else { return }
                self?.postTask {
                    guard timer.isActive else { return }
                    callback()
                    if shouldRepeat && timer.isActive {
                        scheduleOnce()
                    } else {
                        timer.cancel()
                    }
                }
            }
        }
        scheduleOnce()
        return timer
    }

    public var targetPlatform: TargetPlatform? { .android }

    public func createCursor(_ cursor: SystemMouseCursor) -> NativeMouseCursor? { nil }

    public var locales: [ShaftLocale] { [ShaftLocale("zh", countryCode: "CN")] }
}

public final class RecoverySkiaBackendTimer: RecoverySkiaTimer {
    public private(set) var isActive = true
    public func cancel() { isActive = false }
}

public final class RecoverySkiaView: NativeView {
    init(
        viewID: Int,
        backend: RecoverySkiaBackend,
        renderer: SkiaRenderer,
        pixels: UnsafeMutableRawPointer,
        rowBytes: Int,
        width: Int,
        height: Int,
        devicePixelRatio: Float,
        rendererMode: RecoverySkiaRendererMode
    ) {
        self.viewID = viewID
        self.backend = backend
        self.renderer = renderer
        self.pixels = pixels
        self.rowBytes = rowBytes
        self.physicalSize = ISize(width, height)
        self.devicePixelRatio = devicePixelRatio
        self.rendererMode = rendererMode
    }

    public let viewID: Int
    public let physicalSize: ISize
    public let devicePixelRatio: Float
    weak var backend: RecoverySkiaBackend?
    public var isDestroyed = false

    private let renderer: SkiaRenderer
    private let pixels: UnsafeMutableRawPointer
    private let rowBytes: Int
    private let rendererMode: RecoverySkiaRendererMode
    private let profileEnabled = ProcessInfo.processInfo.environment["SHAFT_RECOVERY_PROFILE"] != nil
    private lazy var refreshRateHz = max(1, Int(recovery_skia_refresh_rate()))
    private var lastRenderStart: ContinuousClock.Instant?
    private lazy var vulkanSurface: UnsafeMutableRawPointer? = {
        guard rendererMode == .vulkan else { return nil }
        guard let surface = sk_recovery_vulkan_surface_create(Int32(physicalSize.width), Int32(physicalSize.height)) else {
            recoveryLog("[shaft-recovery-skia] renderer=vulkan unavailable; fallback=software")
            return nil
        }
        recoveryLog("[shaft-recovery-skia] renderer=vulkan")
        return surface
    }()

    deinit {
        if let vulkanSurface {
            sk_recovery_vulkan_surface_destroy(vulkanSurface)
        }
    }

    public func render(_ layerTree: LayerTree) {
        guard !isDestroyed else { return }
        let renderStart = ContinuousClock.now
        let canvas: SkiaCanvas
        if let vulkanSurface,
           let vulkanCanvas = SkiaCanvas(
            recoveryVulkanSurface: vulkanSurface,
            pixels: pixels,
            rowBytes: rowBytes,
            size: physicalSize,
            onFlush: { recovery_skia_present() },
            onPresentExternal: { [physicalSize] externalPixels, externalRowBytes in
                recovery_skia_present_external_rgba(
                    externalPixels,
                    Int32(externalRowBytes),
                    Int32(physicalSize.width),
                    Int32(physicalSize.height)
                )
               return true
            }
           )
        {
            canvas = vulkanCanvas
        } else {
            canvas = SkiaCanvas(
                rasterPixels: pixels,
                rowBytes: rowBytes,
                size: physicalSize,
                onFlush: { recovery_skia_present() }
            )
        }
        canvas.clear(color: .init(0xFF00_0000))
        layerTree.paint(context: LayerPaintContext(canvas: canvas))
        canvas.flush()
        if profileEnabled {
            let renderEnd = ContinuousClock.now
            let renderUs = renderStart.duration(to: renderEnd).inMicroseconds
            let intervalUs = lastRenderStart.map { $0.duration(to: renderStart).inMicroseconds } ?? 0
            lastRenderStart = renderStart
            let rawFps = intervalUs > 0 ? 1_000_000.0 / Double(intervalUs) : 0
            let fps = min(rawFps, Double(refreshRateHz))
            recoveryLog("[shaft-recovery-skia:profile] frame interval_us=\(intervalUs) fps=\(String(format: "%.2f", fps)) refresh_hz=\(refreshRateHz) render_us=\(renderUs)")
        }
    }

    public func startTextInput() {}
    public func stopTextInput() {}
    public func setComposingRect(_ rect: ShaftRect) {}
    public func setEditableSizeAndTransform(_ size: ShaftSize, _ transform: Matrix4x4f) {}
    public var textInputActive: Bool { false }
    public var onTextEditing: TextEditingCallback?
    public var onTextComposed: TextComposedCallback?
    public var onTextInputClosed: VoidCallback?
    public var title: String = "Shaft Recovery Skia"
    public var rawView: UnsafeMutableRawPointer? { pixels }
}

public func useRecoverySkiaBackend(
    stopAfterFirstFrame: Bool = false,
    scale: Float = 1.0,
    rendererMode: RecoverySkiaRendererMode = .software
) {
    guard !backendInitialized else { return }
    Shaft.backend = RecoverySkiaBackend(
        stopAfterFirstFrame: stopAfterFirstFrame,
        devicePixelRatio: scale,
        rendererMode: rendererMode
    )
}
