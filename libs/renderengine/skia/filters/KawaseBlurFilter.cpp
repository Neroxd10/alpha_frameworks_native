/*
 * Copyright 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "KawaseBlurFilter.h"
#include <SkAlphaType.h>
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkImageFilters.h>
#include <SkImageInfo.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkShader.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <log/log.h>
#include <utils/Trace.h>

namespace android {
namespace renderengine {
namespace skia {

KawaseBlurFilter::KawaseBlurFilter(): BlurFilter() {
    SkString blurString(
        "uniform shader child;"
        "uniform float in_blurOffset;"

        "half4 main(float2 xy) {"
            "half4 c = child.eval(xy);"
            "c += child.eval(xy + float2(+in_blurOffset, +in_blurOffset));"
            "c += child.eval(xy + float2(+in_blurOffset, -in_blurOffset));"
            "c += child.eval(xy + float2(-in_blurOffset, -in_blurOffset));"
            "c += child.eval(xy + float2(-in_blurOffset, +in_blurOffset));"
            "return half4(c.rgb * 0.2, 1.0);"
        "}");

    auto [blurEffect, error] = SkRuntimeEffect::MakeForShader(blurString);
    if (!blurEffect) {
        LOG_ALWAYS_FATAL("RuntimeShader error: %s", error.c_str());
    }
    mBlurEffect = std::move(blurEffect);
}

// Draws the given runtime shader on a GPU (Ganesh) surface and returns the result as an
// SkImage.
static sk_sp<SkImage> makeImage(SkSurface* surface, SkRuntimeShaderBuilder* builder) {
    sk_sp<SkShader> shader = builder->makeShader(nullptr);
    if (!shader) {
        return nullptr;
    }
    SkPaint paint;
    paint.setShader(std::move(shader));
    paint.setBlendMode(SkBlendMode::kSrc);
    surface->getCanvas()->drawPaint(paint);
    return surface->makeImageSnapshot();
}

sk_sp<SkImage> KawaseBlurFilter::generate(GrRecordingContext* context,
                                          const uint32_t blurRadius,
                                          const sk_sp<SkImage> input,
                                          const SkRect& blurRect) const {
    LOG_ALWAYS_FATAL_IF(context == nullptr, "%s: Needs GPU context", __func__);
    LOG_ALWAYS_FATAL_IF(input == nullptr, "%s: Invalid input image", __func__);

    if (blurRadius == 0) {
        return input;
    }

    // Kawase is an approximation of Gaussian, but it behaves differently from it.
    // A radius transformation is required for approximating them, and also to introduce
    // non-integer steps, necessary to smoothly interpolate large radii.
    float tmpRadius = (float)blurRadius / 8.0f;
    uint32_t numberOfPasses = std::min(kMaxPasses, (uint32_t)ceil(tmpRadius));
    float radiusByPasses = tmpRadius / (float)numberOfPasses;

    // For sampling Skia's API expects the inverse of what logically seems appropriate. In this
    // case you might expect Translate(blurRect.fLeft, blurRect.fTop) X Scale(kInverseInputScale)
    // but instead we must do the inverse.
    SkMatrix blurMatrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
    blurMatrix.postScale(kInputScale, kInputScale);

    // start by downscaling and doing the first blur pass
    SkSamplingOptions linear(SkFilterMode::kLinear, SkMipmapMode::kNone);
    SkRuntimeShaderBuilder blurBuilder(mBlurEffect);
    blurBuilder.child("child") =
            input->makeShader(SkTileMode::kMirror, SkTileMode::kMirror, linear, blurMatrix);
    blurBuilder.uniform("in_blurOffset") = radiusByPasses * kInputScale;

    // create blur surface with the bit depth and colorspace of the original surface
    SkImageInfo scaledInfo = input->imageInfo().makeWH(std::ceil(blurRect.width() * kInputScale),
                                                       std::ceil(blurRect.height() * kInputScale));

    constexpr int kSampleCount = 1;
    constexpr bool kMipmapped = false;
    constexpr SkSurfaceProps* kProps = nullptr;
    sk_sp<SkSurface> surface = SkSurfaces::RenderTarget(context, skgpu::Budgeted::kYes, scaledInfo,
                                                        kSampleCount, kTopLeft_GrSurfaceOrigin,
                                                        kProps, kMipmapped, input->isProtected());
    LOG_ALWAYS_FATAL_IF(!surface, "%s: Failed to create surface for blurring!", __func__);
    sk_sp<SkImage> tmpBlur = makeImage(surface.get(), &blurBuilder);

   for (auto i = 2; i <= numberOfPasses; i++) {
        blurBuilder.child("child") =
                tmpBlur->makeShader(SkTileMode::kMirror, SkTileMode::kMirror, linear);
        blurBuilder.uniform("in_blurOffset") = (float) i * radiusByPasses * kInputScale;
        sk_sp<SkImage> nextBlur = makeImage(surface.get(), &blurBuilder);
        if (!nextBlur) {
            return nullptr;
        }
        tmpBlur = std::move(nextBlur);
    }

    const float sigmaScale = blurRadius * kInputScale * 0.5f;

    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);

    sk_sp<SkImageFilter> finalFilter = SkImageFilters::Blur(
        sigmaScale, sigmaScale, SkTileMode::kMirror, nullptr);

    paint.setImageFilter(finalFilter);

    surface->getCanvas()->drawImage(tmpBlur.get(), 0, 0);

    return surface->makeImageSnapshot();
}

} // namespace skia
} // namespace renderengine
} // namespace android
