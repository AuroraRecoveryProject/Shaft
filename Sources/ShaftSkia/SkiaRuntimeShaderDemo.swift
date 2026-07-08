// Copyright 2024 The Shaft Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if os(Android)
import CSkia

extension SkiaCanvas {
    public func drawRuntimeShaderDemo(
        width: Float,
        height: Float,
        time: Float,
        touchX: Float,
        touchY: Float,
        touchActive: Float
    ) {
        sk_canvas_draw_runtime_shader_demo(skCanvas, width, height, time, touchX, touchY, touchActive)
    }
}
#endif
