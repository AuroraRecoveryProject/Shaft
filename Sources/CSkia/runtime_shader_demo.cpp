#include "Public/utils.h"

#include <cstdio>
#include <utility>

void sk_canvas_draw_runtime_shader_demo(SkCanvas *canvas, float width, float height, float time, float touchX, float touchY, float touchActive)
{
    static sk_sp<SkRuntimeEffect> effect;
    static bool attempted = false;

    if (!attempted) {
        attempted = true;
        const char *sksl = R"(
            uniform float2 iResolution;
            uniform float iTime;
            uniform float3 iTouch;

            half4 main(float2 fragCoord) {
                float2 resolution = max(iResolution, float2(1.0, 1.0));
                float2 uv = fragCoord / resolution;
                float aspect = resolution.x / resolution.y;
                uv.x *= aspect;

                bool hasTouch = iTouch.x >= 0.0 && iTouch.y >= 0.0;
                float2 center = float2(0.5 * aspect, 0.5) +
                    0.18 * float2(sin(iTime * 0.73), cos(iTime * 0.61));
                if (hasTouch) {
                    center = float2(iTouch.x / resolution.x * aspect, iTouch.y / resolution.y);
                }
                float liftRadius = 0.34 + 0.06 * sin(iTime * 0.41);
                float liftStrength = hasTouch ? mix(0.76, 0.98, iTouch.z) : 0.92;
                float pointsPerRow = 34.0;
                float baseDotOpacity = 0.48;
                float swapProgress = fract(iTime * 0.18);

                float liftFalloff = 1.0 - smoothstep(0.0, liftRadius, length(uv - center));
                float height = liftFalloff * liftStrength;

                float eps = 1.0 / resolution.y;
                float heightX =
                    (1.0 - smoothstep(0.0, liftRadius, length(float2(uv.x + eps, uv.y) - center))) *
                    liftStrength;
                float heightY =
                    (1.0 - smoothstep(0.0, liftRadius, length(float2(uv.x, uv.y + eps) - center))) *
                    liftStrength;
                float3 normal = normalize(float3(height - heightX, height - heightY, 0.35));

                float2 uvDisp = uv + (uv - center) * height * 0.25;
                float gridSize = pointsPerRow / aspect;
                float2 gridUv = uvDisp * gridSize;
                float2 gridId = floor(gridUv);
                float2 gridFrac = fract(gridUv);

                float dotRadius = 0.13;
                float dotMask = 0.0;

                for (int yy = -1; yy <= 1; ++yy) {
                    for (int xx = -1; xx <= 1; ++xx) {
                        float2 neighborId = gridId + float2(float(xx), float(yy));
                        float2 group = mod(neighborId, 2.0);
                        float2 neighborOffset = float2(0.0);

                        if (swapProgress < 0.5) {
                            float p = swapProgress * 2.0;
                            if (group.x > 0.5 && group.y < 0.5) {
                                neighborOffset = float2(-1.0, 1.0) * p;
                            } else if (group.x < 0.5 && group.y > 0.5) {
                                neighborOffset = float2(1.0, -1.0) * p;
                            }
                        } else {
                            float p = (swapProgress - 0.5) * 2.0;
                            if (group.x < 0.5 && group.y < 0.5) {
                                neighborOffset = float2(1.0, 1.0) * p;
                            } else if (group.x > 0.5 && group.y > 0.5) {
                                neighborOffset = float2(-1.0, -1.0) * p;
                            } else if (group.x > 0.5 && group.y < 0.5) {
                                neighborOffset = float2(-1.0, 1.0);
                            } else if (group.x < 0.5 && group.y > 0.5) {
                                neighborOffset = float2(1.0, -1.0);
                            }
                        }

                        float2 dotCenter = float2(float(xx), float(yy)) + 0.5 + neighborOffset;
                        float2 toDot = gridFrac - dotCenter;
                        float mask = smoothstep(dotRadius, dotRadius - 0.025, length(toDot));
                        float spacingMask = smoothstep(0.48, 0.50, max(abs(toDot.x), abs(toDot.y)));
                        dotMask = max(dotMask, mask * (1.0 - spacingMask));
                    }
                }

                float3 baseColor = float3(0.015, 0.022, 0.038);
                float3 dotColor = float3(0.80, 0.90, 1.0);
                float3 accentColor = float3(0.22, 0.64, 1.0);
                float3 lightDir = normalize(float3(-0.35, 0.5, 1.2));
                float diff = clamp(dot(normal, lightDir), 0.0, 1.0);
                float rim = pow(1.0 - clamp(normal.z, 0.0, 1.0), 2.0);
                float3 halfDir = normalize(lightDir + float3(0.0, 0.0, 1.0));
                float spec = pow(clamp(dot(normal, halfDir), 0.0, 1.0), 26.0);
                float glow = smoothstep(liftRadius, 0.0, length(uv - center));
                float intensity = 0.22 + diff * 0.9 + rim * 0.5 + height * 0.9;
                float dotOpacity = mix(baseDotOpacity, 1.0, liftFalloff);
                float wave = 0.5 + 0.5 * sin((uv.x + uv.y) * 16.0 - iTime * 3.2);

                float3 color = baseColor;
                color += dotColor * dotMask * intensity * dotOpacity;
                color += accentColor * glow * (0.16 + 0.10 * wave);
                color += spec * dotMask * 0.36;
                color *= mix(0.78, 1.18, height);

                return half4(half3(color), 1.0);
            }
        )";
        auto result = SkRuntimeEffect::MakeForShader(SkString(sksl));
        if (!result.effect) {
            std::fprintf(stderr, "[CSkia:shader] SkRuntimeEffect failed: %s\n", result.errorText.c_str());
        }
        effect = std::move(result.effect);
    }

    SkRect rect = SkRect::MakeWH(width, height);
    SkPaint paint;
    if (effect) {
        SkRuntimeShaderBuilder builder(effect);
        builder.uniform("iResolution") = SkV2{width, height};
        builder.uniform("iTime") = time;
        struct TouchUniform {
            float x;
            float y;
            float active;
        };
        builder.uniform("iTouch") = TouchUniform{touchX, touchY, touchActive};
        paint.setShader(builder.makeShader());
    } else {
        paint.setColor(0xFF20242A);
    }
    canvas->drawRect(rect, paint);
}

