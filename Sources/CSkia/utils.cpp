#include "Public/utils.h"
#include "utils_macos.h"

#include <__memory/voidify.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>
#include <vector>


#include "unicode/uclean.h"
#include "unicode/udata.h"
#include "unicode/utypes.h"

#undef udata_setCommonData
#undef u_init
extern "C" {
void udata_setCommonData(const void *data, UErrorCode *err);
void u_init(UErrorCode *status);
extern const unsigned char _binary_build_embedded_assets_icudtl_dat_start[];
extern const unsigned char _binary_build_embedded_assets_icudtl_dat_end[];
extern const unsigned char _binary_build_embedded_assets_Roboto_Regular_ttf_start[];
extern const unsigned char _binary_build_embedded_assets_Roboto_Regular_ttf_end[];
}

using namespace skia::textlayout;

template struct sk_sp<FontCollection>;
template struct sk_sp<SkSurface>;
template struct sk_sp<SkImage>;
template struct sk_sp<SkTypeface>;
template void SkSafeUnref<FontCollection>(FontCollection *obj);

#if defined(__ANDROID__)
namespace std {
template void *__voidify<skia::textlayout::FontArguments>(skia::textlayout::FontArguments &);
}
#endif

// MARK: - ParagraphBuilder

#if defined(SK_BUILD_FOR_MAC)
#include "utils_macos.cpp"
auto fontMgr = SkFontMgr_New_CoreText(nullptr);
#elif defined(SK_BUILD_FOR_WIN)
auto fontMgr = SkFontMgr_New_DirectWrite(nullptr);
#elif defined(__ANDROID__)
auto fontMgr = SkFontMgr_New_Custom_Directory("/system/fonts");
#elif defined(SK_FONTMGR_FONTCONFIG_AVAILABLE)
auto fontMgr = SkFontMgr_New_FontConfig(nullptr);
#else
sk_sp<SkFontMgr> fontMgr = nullptr;
#endif

auto typefaceProvider = sk_make_sp<TypefaceFontProvider>();

void skia_init_icu_from_file_once()
{
    static bool attempted = false;
    static std::vector<uint8_t> icuData;
    static std::vector<std::max_align_t> embeddedIcuData;
    if (attempted) {
        return;
    }
    attempted = true;

    const auto *embeddedStart = _binary_build_embedded_assets_icudtl_dat_start;
    const auto *embeddedEnd = _binary_build_embedded_assets_icudtl_dat_end;
    const auto embeddedSize = static_cast<size_t>(embeddedEnd - embeddedStart);
    UErrorCode embeddedStatus = U_ZERO_ERROR;
    if (embeddedSize > 0) {
        const auto unitSize = sizeof(std::max_align_t);
        embeddedIcuData.resize((embeddedSize + unitSize - 1) / unitSize);
        std::memcpy(embeddedIcuData.data(), embeddedStart, embeddedSize);
        udata_setCommonData(embeddedIcuData.data(), &embeddedStatus);
        if (U_SUCCESS(embeddedStatus)) {
            u_init(&embeddedStatus);
        }
        if (U_SUCCESS(embeddedStatus)) {
            return;
        }
        embeddedIcuData.clear();
    }

    const char *paths[] = {"/tmp/icudtl.dat", "icudtl.dat"};
    FILE *file = nullptr;
    for (const char *path : paths) {
        file = std::fopen(path, "rb");
        if (file != nullptr) {
            break;
        }
    }
    if (file == nullptr) {
        return;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return;
    }

    icuData.resize(static_cast<size_t>(size));
    const size_t read = std::fread(icuData.data(), 1, icuData.size(), file);
    std::fclose(file);
    if (read != icuData.size()) {
        icuData.clear();
        return;
    }

    UErrorCode status = U_ZERO_ERROR;
    udata_setCommonData(icuData.data(), &status);
    if (U_SUCCESS(status)) {
        u_init(&status);
    }
}

void register_embedded_roboto(FontCollection_sp &collection)
{
    const auto *start = _binary_build_embedded_assets_Roboto_Regular_ttf_start;
    const auto *end = _binary_build_embedded_assets_Roboto_Regular_ttf_end;
    const auto size = static_cast<size_t>(end - start);
    if (size == 0) {
        return;
    }

    auto data = SkData::MakeWithoutCopy(start, size);
    sk_sp<SkTypeface> typeface = nullptr;
    if (fontMgr) {
        typeface = fontMgr->makeFromData(data);
    }
    if (!typeface) {
        auto emptyFontMgr = SkFontMgr_New_Custom_Empty();
        typeface = emptyFontMgr->makeFromData(data);
    }
    if (!typeface) {
        return;
    }

    SkString familyName;
    typeface->getFamilyName(&familyName);
    if (familyName.isEmpty()) {
        familyName.set("Roboto");
    }

    typefaceProvider->registerTypeface(typeface, familyName);
    collection->setAssetFontManager(typefaceProvider);
    collection->setDynamicFontManager(typefaceProvider);
    collection->setDefaultFontManager(typefaceProvider, familyName.c_str());
    collection->enableFontFallback();
}

ParagraphBuilder *paragraph_builder_new(ParagraphStyle &style, const FontCollection_sp &fontCollection)
{
    auto unicode = SkUnicodes::ICU::Make();
    auto result = ParagraphBuilder::make(style, fontCollection, std::move(unicode));
    return result.release();
}

void paragraph_builder_add_text(ParagraphBuilder *builder, const char *text)
{
    builder->addText(text);
}

void paragraph_builder_push_style(ParagraphBuilder *builder, const TextStyle *style)
{
    builder->pushStyle(*style);
}

void paragraph_builder_pop(ParagraphBuilder *builder)
{
    builder->pop();
}

Paragraph *paragraph_builder_build(ParagraphBuilder *builder)
{
    return builder->Build().release();
}

void paragraph_builder_unref(ParagraphBuilder *builder)
{
    delete builder;
}

// MARK: - Paragraph

std::vector<Paragraph::FontInfo> paragraph_get_fonts(Paragraph *paragraph)
{
    return paragraph->getFonts();
}

void paragraph_layout(Paragraph *paragraph, float width)
{
    paragraph->layout(width);
}

void paragraph_paint(Paragraph *paragraph, SkCanvas *canvas, float x, float y)
{
    paragraph->paint(canvas, x, y);
}

float paragraph_get_width(Paragraph *paragraph)
{
    return paragraph->getMaxWidth();
}

float paragraph_get_height(Paragraph *paragraph)
{
    return paragraph->getHeight();
}

float paragraph_get_longest_line(Paragraph *paragraph)
{
    return paragraph->getLongestLine();
}

float paragraph_get_min_intrinsic_width(Paragraph *paragraph)
{
    return paragraph->getMinIntrinsicWidth();
}

float paragraph_get_max_intrinsic_width(Paragraph *paragraph)
{
    return paragraph->getMaxIntrinsicWidth();
}

float paragraph_get_alphabetic_baseline(Paragraph *paragraph)
{
    return paragraph->getAlphabeticBaseline();
}

float paragraph_get_ideographic_baseline(Paragraph *paragraph)
{
    return paragraph_get_alphabetic_baseline(paragraph);
}

PositionWithAffinity paragraph_get_glyph_position_at_coordinate(Paragraph *paragraph, SkScalar dx, SkScalar dy)
{
    PositionWithAffinity position = paragraph->getGlyphPositionAtCoordinate(dx, dy);
    return position;
}

SkRange<size_t> paragraph_get_word_boundary(Paragraph *paragraph, unsigned offset)
{
    return paragraph->getWordBoundary(offset);
}

std::vector<LineMetrics> paragraph_get_line_metrics(Paragraph *paragraph)
{
    std::vector<LineMetrics> metrics;
    paragraph->getLineMetrics(metrics);
    return metrics;
}

LineMetrics paragraph_get_line_metrics_at(Paragraph *paragraph, unsigned lineNumber)
{
    LineMetrics metrics;
    paragraph->getLineMetricsAt(lineNumber, &metrics);
    return metrics;
}

size_t paragraph_get_line_count(Paragraph *paragraph)
{
    return paragraph->lineNumber();
}

int paragraph_get_line_number_at(Paragraph *paragraph, size_t codeUnitIndex)
{
    return paragraph->getLineNumberAt(codeUnitIndex);
}

std::vector<TextBox> paragraph_get_rects_for_range(Paragraph *paragraph, size_t start, size_t end, RectHeightStyle boxHeightStyle, RectWidthStyle boxWidthStyle)
{
    return paragraph->getRectsForRange(start, end, boxHeightStyle, boxWidthStyle);
}

std::vector<TextBox> paragraph_get_rects_for_placeholders(Paragraph *paragraph)
{
    return paragraph->getRectsForPlaceholders();
}

bool paragraph_get_glyph_info_at(Paragraph *paragraph, size_t codeUnitIndex, Paragraph::GlyphInfo *glyphInfo)
{
    return paragraph->getGlyphInfoAtUTF16Offset(codeUnitIndex, glyphInfo);
}

Paragraph::GlyphInfo paragraph_get_closest_glyph_info_at(Paragraph *paragraph, SkScalar dx, SkScalar dy)
{
    Paragraph::GlyphInfo info;
    paragraph->getClosestUTF16GlyphInfoAt(dx, dy, &info);
    return info;
}

void paragraph_unref(Paragraph *paragraph)
{
    std::default_delete<Paragraph>()(paragraph);
}

std::vector<SkString> skstring_vector_new()
{
    return std::vector<SkString>();
}

void skstring_c_str(const SkString &string, const char **out)
{
    *out = string.c_str();
}

sk_sp<SkColorSpace> color_space_new_srgb()
{
    return SkColorSpace::MakeSRGB();
}

sk_sp<SkColorSpace> color_space_new_null()
{
    return nullptr;
}

SkCanvas *sk_surface_get_canvas(const sk_sp<SkSurface> &surface)
{
    if (!surface) {
        return nullptr;
    }
    return surface->getCanvas();
}

SkSurface_sp sk_surface_make_raster_direct_rgba(int width, int height, void *pixels, size_t rowBytes)
{
    if (width <= 0 || height <= 0 || pixels == nullptr || rowBytes < static_cast<size_t>(width) * 4) {
        return nullptr;
    }
    auto imageInfo = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    return SkSurfaces::WrapPixels(imageInfo, pixels, rowBytes);
}

void sk_surface_flush(SkSurface_sp &surface)
{
    (void)surface;
}

// MARK: - Font

FontCollection_sp sk_fontcollection_new()
{
    skia_init_icu_from_file_once();
    auto collection = sk_make_sp<FontCollection>();
    collection->setDynamicFontManager(typefaceProvider);
    collection->setDefaultFontManager(fontMgr);
    collection->enableFontFallback();
    register_embedded_roboto(collection);
#if defined(SK_BUILD_FOR_MAC)
    RegisterSystemFonts(*typefaceProvider);
#endif

    return collection;
}

void sk_fontcollection_register_typeface(FontCollection_sp &collection, SkTypeface_sp &typeface)
{
    if (!typeface) {
        std::fprintf(stderr, "[CSkia] registerTypeface skipped: null typeface\n");
        return;
    }
    typefaceProvider->registerTypeface(typeface);
}

SkTypeface_sp sk_typeface_create_from_data(const FontCollection_sp &collection, const char *data, size_t length)
{
    auto bytes = SkData::MakeWithCopy(data, length);
    sk_sp<SkTypeface> typeface = nullptr;
    if (fontMgr) {
        typeface = fontMgr->makeFromData(bytes);
    }
    if (!typeface) {
        auto emptyFontMgr = SkFontMgr_New_Custom_Empty();
        typeface = emptyFontMgr->makeFromData(bytes);
    }
    if (!typeface) {
        std::fprintf(stderr, "[CSkia] makeTypefaceFromData length=%zu result=null\n", length);
        return nullptr;
    }
    return typeface;
}

std::vector<SkTypeface_sp> sk_fontcollection_find_typefaces(const FontCollection_sp &collection, const std::vector<SkString> &families, SkFontStyle style)
{
    return collection->findTypefaces(families, style);
}

SkTypeface_sp sk_fontcollection_default_fallback(const FontCollection_sp &collection, SkUnichar unicode, SkFontStyle style, const SkString &locale)
{
    std::optional<FontArguments> fontArgs;
    return collection->defaultFallback(unicode, style, locale, fontArgs);
}

std::vector<SkGlyphID> sk_typeface_get_glyphs(SkTypeface_sp &typeface, const SkUnichar *text, size_t length)
{
    std::vector<SkGlyphID> glyphs;
    glyphs.resize(length);
    typeface->unicharsToGlyphs({text, length}, {glyphs.data(), length});
    return glyphs;
}

SkGlyphID sk_typeface_get_glyph(SkTypeface_sp &typeface, SkUnichar unicode)
{
    return typeface->unicharToGlyph(unicode);
}

void sk_typeface_get_family_name(SkTypeface_sp &typeface, SkString *familyName)
{
    typeface->getFamilyName(familyName);
}

int sk_typeface_count_glyphs(SkTypeface_sp &typeface)
{
    return typeface->countGlyphs();
}

SkFont sk_font_new(SkTypeface_sp &typeface, float size)
{
    return SkFont(typeface, size);
}

float sk_font_get_size(SkFont &font)
{
    return font.getSize();
}

SkTextBlob_sp sk_text_blob_make_from_glyphs(const SkGlyphID *glyphs, const SkPoint *positions, size_t length, const SkFont &font)
{
    SkTextBlobBuilder builder;
    auto buffer = builder.allocRunPos(font, length);
    memcpy(buffer.glyphs, glyphs, length * sizeof(SkGlyphID));
    memcpy(buffer.points(), positions, length * sizeof(SkPoint));
    return builder.make();
}

// MARK: - TextStyle

void sk_textstyle_set_font_arguments(TextStyle *style, SkFontArguments fontArguments)
{
    std::optional<SkFontArguments> args;
    args = fontArguments;
    style->setFontArguments(args);
}

// MARK: - Canvas

void sk_canvas_concat(SkCanvas *canvas, const SkM44 &matrix)
{
    canvas->concat(matrix);
}

void sk_canvas_save(SkCanvas *canvas)
{
    canvas->save();
}

void sk_canvas_save_layer(SkCanvas *canvas, const SkRect *bounds, const SkPaint *paint)
{
    canvas->saveLayer(bounds, paint);
}

void sk_canvas_restore(SkCanvas *canvas)
{
    canvas->restore();
}

int sk_canvas_get_save_count(SkCanvas *canvas)
{
    return canvas->getSaveCount();
}

void sk_canvas_clear(SkCanvas *canvas, SkColor color)
{
    canvas->clear(color);
}

void sk_canvas_draw_line(SkCanvas *canvas, float x0, float y0, float x1, float y1, const SkPaint &paint)
{
    canvas->drawLine(x0, y0, x1, y1, paint);
}

void sk_canvas_draw_rect(SkCanvas *canvas, const SkRect &rect, const SkPaint &paint)
{
    canvas->drawRect(rect, paint);
}

void sk_canvas_draw_rrect(SkCanvas *canvas, const SkRRect &rrect, const SkPaint &paint)
{
    canvas->drawRRect(rrect, paint);
}

void sk_canvas_draw_drrect(SkCanvas *canvas, const SkRRect &outer, const SkRRect &inner, const SkPaint &paint)
{
    canvas->drawDRRect(outer, inner, paint);
}

void sk_canvas_draw_circle(SkCanvas *canvas, float x, float y, float radius, const SkPaint &paint)
{
    canvas->drawCircle(x, y, radius, paint);
}

void sk_canvas_draw_path(SkCanvas *canvas, const SkPath &path, const SkPaint &paint)
{
    canvas->drawPath(path, paint);
}

void sk_canvas_draw_image(SkCanvas *canvas, SkImage_sp &image, float x, float y, const SkPaint *paint)
{
    canvas->drawImage(image.get(), x, y, SkSamplingOptions(), paint);
}

void sk_canvas_draw_image_rect(SkCanvas *canvas, SkImage_sp &image, const SkRect &src, const SkRect &dst, const SkPaint *paint)
{
    canvas->drawImageRect(image, src, dst, SkSamplingOptions(), paint, SkCanvas::kFast_SrcRectConstraint);
}

void sk_canvas_draw_image_nine(SkCanvas *canvas, SkImage_sp &image, const SkIRect &center, const SkRect &dst, const SkPaint *paint)
{
    canvas->drawImageNine(image.get(), center, dst, SkFilterMode::kLinear, paint);
}

void sk_canvas_draw_text_blob(SkCanvas *canvas, SkTextBlob_sp &blob, float x, float y, const SkPaint &paint)
{
    canvas->drawTextBlob(blob.get(), x, y, paint);
}

void sk_canvas_clip_rect(SkCanvas *canvas, const SkRect &rect, SkClipOp op, bool doAntiAlias)
{
    canvas->clipRect(rect, op, doAntiAlias);
}

void sk_canvas_clip_rrect(SkCanvas *canvas, const SkRRect &rrect, SkClipOp op, bool doAntiAlias)
{
    canvas->clipRRect(rrect, op, doAntiAlias);
}

void sk_canvas_translate(SkCanvas *canvas, float dx, float dy)
{
    canvas->translate(dx, dy);
}

void sk_canvas_scale(SkCanvas *canvas, float sx, float sy)
{
    canvas->scale(sx, sy);
}

void sk_canvas_rotate(SkCanvas *canvas, float radians)
{
    canvas->rotate(radians);
}

// MARK: - Paint

void sk_paint_set_maskfilter_blur(SkPaint *paint, SkBlurStyle style, SkScalar sigma)
{
    // Setting the mask filter involves sk_sp. To avoid memory leaks, we need to
    // do this in c rather than swift.
    paint->setMaskFilter(SkMaskFilter::MakeBlur(style, sigma));
}

void sk_paint_clear_maskfilter(SkPaint *paint)
{
    paint->setMaskFilter(nullptr);
}

// MARK: - Path

void sk_path_move_to(SkPath *path, SkScalar x, SkScalar y)
{
    path->moveTo(x, y);
}

void sk_path_line_to(SkPath *path, SkScalar x, SkScalar y)
{
    path->lineTo(x, y);
}

void sk_path_reset(SkPath *path)
{
    path->reset();
}

// MARK: - Image

SkAnimatedImage_sp sk_animated_image_create(const void *data, size_t length)
{
    auto bytes = SkData::MakeWithCopy(data, length);
    auto aCodec = SkAndroidCodec::MakeFromData(std::move(bytes));
    if (aCodec == nullptr)
    {
        return nullptr;
    }
    return SkAnimatedImage::Make(std::move(aCodec));
}

int sk_animated_image_get_frame_count(SkAnimatedImage_sp &image)
{
    return image->getFrameCount();
}

int sk_animated_image_get_repetition_count(SkAnimatedImage_sp &image)
{
    return image->getRepetitionCount();
}

int sk_animated_image_decode_next_frame(SkAnimatedImage_sp &image)
{
    return image->decodeNextFrame();
}

SkImage_sp sk_animated_image_get_current_frame(SkAnimatedImage_sp &image)
{
    return image->getCurrentFrame();
}

int sk_image_get_width(SkImage_sp &image)
{
    return image->width();
}

int sk_image_get_height(SkImage_sp &image)
{
    return image->height();
}

// MARK: - GL

#if !defined(__ANDROID__)
GrGLInterface_sp gr_glinterface_create_native_interface()
{
    return GrGLMakeNativeInterface();
}

GrDirectContext_sp gr_direct_context_make_gl(GrGLInterface_sp &glInterface)
{
    return GrDirectContexts::MakeGL(glInterface);
}

const GrDirectContext *gr_direct_context_unwrap(GrDirectContext_sp &context)
{
    return context.get();
}

void gr_direct_context_flush_and_submit(GrDirectContext_sp &context, GrSyncCpu syncCPU)
{
    context->flushAndSubmit(syncCPU);
}
#endif

// MARK: - Metal

#if defined(SK_BUILD_FOR_MAC)
GrDirectContext_sp gr_mtl_direct_context_make(GrMtlBackendContext &context)
{
    return GrDirectContexts::MakeMetal(context);
}
#endif

// An hack to avoid linking error on Linux
#if defined(__linux__)
namespace swift
{
    namespace threading
    {

        void fatal(const char *msg, ...)
        {
            std::va_list val;

            va_start(val, msg);
            std::vfprintf(stderr, msg, val);
            va_end(val);

            std::abort();
        }

    } // namespace threading
} // namespace swift
#endif
