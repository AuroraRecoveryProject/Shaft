import Foundation
import Shaft

#if os(Android)
    import ShaftSkia
#endif

final class Demo_Shader: StatelessWidget {
    func build(context: BuildContext) -> Widget {
        #if os(Android)
            return Column(crossAxisAlignment: .start, spacing: 14) {
                Text("Shader")
                    .textStyle(.playgroundTitle)

                Text("Drawing a SkSL shader using Skia.")
                    .textStyle(.playgroundAbstract)

                HorizontalDivider()

                Text("Recovery Skia")
                    .textStyle(.playgroundHeading)

                Text(
                    """
                    This Android recovery variant runs a single-pass SkSL shader through Shaft's Skia canvas. \
                    Touch points are passed into the shader as uniforms, based on shader_graph/example/shaders/touch_simple.frag.
                    """
                )
                .textStyle(.playgroundBody)

                RecoverySkiaShaderView()
            }
            .padding(.all(20))
            .textStyle(.init(height: 1.5))
        #else
            return PageContent {
                Text("Shader")
                    .textStyle(.playgroundTitle)

                Text("Drawing a SkSL shader using Skia.")
                    .textStyle(.playgroundAbstract)

                HorizontalDivider()

                Background {
                    Text("This demo is currently available on Android recovery.")
                        .textStyle(.playgroundBody)
                }
            }
        #endif
    }
}

#if os(Android)
    private final class RecoverySkiaShaderView: StatefulWidget {
        func createState() -> some State<RecoverySkiaShaderView> {
            RecoverySkiaShaderViewState()
        }
    }

    private final class RecoverySkiaShaderViewState: State<RecoverySkiaShaderView> {
        private let animationStart = ContinuousClock.now
        private let shaderState = RecoverySkiaShaderState()
        private var active = false
        private var scheduled = false

        override func initState() {
            super.initState()
            active = true
            scheduleTick()
        }

        override func dispose() {
            active = false
            super.dispose()
        }

        override func build(context: BuildContext) -> Widget {
            Listener(
                onPointerDown: handlePointerDown,
                onPointerMove: handlePointerMove,
                onPointerUp: handlePointerUp,
                onPointerCancel: handlePointerCancel,
                behavior: .opaque
            ) {
                RecoverySkiaShaderRenderWidget(state: shaderState)
                .constrained(width: .infinity, height: 620)
            }
            .decoration(
                .box(
                    color: .init(0xFF_101418),
                    border: .all(.init(color: .init(0xFF_303842), width: 1, style: .solid)),
                    borderRadius: .circular(12)
                )
            )
            .horizontalExpand()
        }

        private func handlePointerDown(_ event: PointerDownEvent) {
            updateTouch(event.position, active: true)
        }

        private func handlePointerMove(_ event: PointerMoveEvent) {
            updateTouch(event.position, active: true)
        }

        private func handlePointerUp(_ event: PointerUpEvent) {
            updateTouch(event.position, active: false)
        }

        private func handlePointerCancel(_ event: PointerCancelEvent) {
            updateTouch(event.position, active: false)
        }

        private func updateTouch(_ globalPosition: Offset, active: Bool) {
            guard let box = context.findRenderObject() as? RenderBox else {
                return
            }
            let local = box.globalToLocal(globalPosition)
            shaderState.updateTouch(local, active: active)
        }

        private func scheduleTick() {
            guard !scheduled else {
                return
            }
            scheduled = true
            _ = SchedulerBinding.shared.scheduleFrameCallback { [weak self] _ in
                guard let self else {
                    return
                }
                scheduled = false
                guard active else {
                    return
                }
                let elapsed = animationStart.duration(to: ContinuousClock.now)
                let nextTime = Float(Double(elapsed.inMicroseconds) / Double(Duration.microsecondsPerSecond))
                shaderState.updateTime(nextTime)
                scheduleTick()
            }
        }
    }

    private final class RecoverySkiaShaderState: ChangeNotifier {
        private(set) var time: Float = 0.0
        private(set) var touchPosition = Offset(-1, -1)
        private(set) var touchActive: Float = 0.0

        func updateTime(_ nextTime: Float) {
            guard abs(nextTime - time) >= 0.001 else {
                return
            }
            time = nextTime
            notifyListeners()
        }

        func updateTouch(_ position: Offset, active: Bool) {
            touchPosition = position
            touchActive = active ? 1.0 : 0.0
            notifyListeners()
        }
    }

    private final class RecoverySkiaShaderRenderWidget: LeafRenderObjectWidget {
        init(state: RecoverySkiaShaderState) {
            self.state = state
        }

        private let state: RecoverySkiaShaderState

        func createRenderObject(context: BuildContext) -> RenderRecoverySkiaShader {
            RenderRecoverySkiaShader(state: state)
        }

        func updateRenderObject(context: BuildContext, renderObject: RenderRecoverySkiaShader) {
            renderObject.state = state
        }
    }

    private final class RenderRecoverySkiaShader: RenderBox {
        init(state: RecoverySkiaShaderState) {
            self.state = state
            super.init()
            state.addListener(self, callback: markNeedsPaint)
        }

        override var isRepaintBoundary: Bool {
            true
        }

        var state: RecoverySkiaShaderState {
            didSet {
                if state !== oldValue {
                    oldValue.removeListener(self)
                    state.addListener(self, callback: markNeedsPaint)
                    markNeedsPaint()
                }
            }
        }

        private let shaderLayer = RecoverySkiaShaderLayer()

        deinit {
            state.removeListener(self)
        }

        override func performLayout() {
            size = boxConstraint.constrain(Size(boxConstraint.maxWidth, boxConstraint.maxHeight))
        }

        override func paint(context: PaintingContext, offset: Offset) {
            shaderLayer.offset = offset
            shaderLayer.size = size
            shaderLayer.time = state.time
            shaderLayer.touchPosition = state.touchPosition
            shaderLayer.touchActive = state.touchActive
            context.addLayer(shaderLayer)
        }
    }

    private final class RecoverySkiaShaderLayer: Layer {
        var offset = Offset.zero
        var size = Size.zero
        var time: Float = 0.0
        var touchPosition = Offset(-1, -1)
        var touchActive: Float = 0.0

        func paint(context: LayerPaintContext) {
            guard let canvas = context.canvas as? SkiaCanvas else {
                return
            }
            canvas.save()
            canvas.translate(offset.dx, offset.dy)
            canvas.drawRuntimeShaderDemo(
                width: size.width,
                height: size.height,
                time: time,
                touchX: touchPosition.dx,
                touchY: touchPosition.dy,
                touchActive: touchActive
            )
            canvas.restore()
        }
    }
#endif
