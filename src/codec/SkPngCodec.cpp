/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkCodecPriv.h"
#include "SkColorPriv.h"
#include "SkColorTable.h"
#include "SkBitmap.h"
#include "SkMath.h"
#include "SkOpts.h"
#include "SkPngCodec.h"
#include "SkPngFilters.h"
#include "SkSize.h"
#include "SkStream.h"
#include "SkSwizzler.h"
#include "SkTemplates.h"

// png_struct::read_filter[] was added in libpng 1.5.7.
#if defined(__SSE2__) && PNG_LIBPNG_VER >= 10507
    #include "pngstruct.h"

    extern "C" void sk_png_init_filter_functions_sse2(png_structp png, unsigned int bpp) {
        if (bpp == 3) {
            png->read_filter[PNG_FILTER_VALUE_SUB  -1] =   sk_sub3_sse2;
            png->read_filter[PNG_FILTER_VALUE_AVG  -1] =   sk_avg3_sse2;
            png->read_filter[PNG_FILTER_VALUE_PAETH-1] = sk_paeth3_sse2;
        }
        if (bpp == 4) {
            png->read_filter[PNG_FILTER_VALUE_SUB  -1] =   sk_sub4_sse2;
            png->read_filter[PNG_FILTER_VALUE_AVG  -1] =   sk_avg4_sse2;
            png->read_filter[PNG_FILTER_VALUE_PAETH-1] = sk_paeth4_sse2;
        }
    }
#endif

///////////////////////////////////////////////////////////////////////////////
// Callback functions
///////////////////////////////////////////////////////////////////////////////

static void sk_error_fn(png_structp png_ptr, png_const_charp msg) {
    SkCodecPrintf("------ png error %s\n", msg);
    longjmp(png_jmpbuf(png_ptr), 1);
}

void sk_warning_fn(png_structp, png_const_charp msg) {
    SkCodecPrintf("----- png warning %s\n", msg);
}

static void sk_read_fn(png_structp png_ptr, png_bytep data,
                       png_size_t length) {
    SkStream* stream = static_cast<SkStream*>(png_get_io_ptr(png_ptr));
    const size_t bytes = stream->read(data, length);
    if (bytes != length) {
        // FIXME: We want to report the fact that the stream was truncated.
        // One way to do that might be to pass a enum to longjmp so setjmp can
        // specify the failure.
        png_error(png_ptr, "Read Error!");
    }
}

#ifdef PNG_READ_UNKNOWN_CHUNKS_SUPPORTED
static int sk_read_user_chunk(png_structp png_ptr, png_unknown_chunkp chunk) {
    SkPngChunkReader* chunkReader = (SkPngChunkReader*)png_get_user_chunk_ptr(png_ptr);
    // readChunk() returning true means continue decoding
    return chunkReader->readChunk((const char*)chunk->name, chunk->data, chunk->size) ? 1 : -1;
}
#endif

///////////////////////////////////////////////////////////////////////////////
// Helpers
///////////////////////////////////////////////////////////////////////////////

class AutoCleanPng : public SkNoncopyable {
public:
    AutoCleanPng(png_structp png_ptr)
        : fPng_ptr(png_ptr)
        , fInfo_ptr(nullptr) {}

    ~AutoCleanPng() {
        // fInfo_ptr will never be non-nullptr unless fPng_ptr is.
        if (fPng_ptr) {
            png_infopp info_pp = fInfo_ptr ? &fInfo_ptr : nullptr;
            png_destroy_read_struct(&fPng_ptr, info_pp, nullptr);
        }
    }

    void setInfoPtr(png_infop info_ptr) {
        SkASSERT(nullptr == fInfo_ptr);
        fInfo_ptr = info_ptr;
    }

    void detach() {
        fPng_ptr = nullptr;
        fInfo_ptr = nullptr;
    }

private:
    png_structp     fPng_ptr;
    png_infop       fInfo_ptr;
};
#define AutoCleanPng(...) SK_REQUIRE_LOCAL_VAR(AutoCleanPng)

// Method for coverting to either an SkPMColor or a similarly packed
// unpremultiplied color.
typedef uint32_t (*PackColorProc)(U8CPU a, U8CPU r, U8CPU g, U8CPU b);

// Note: SkColorTable claims to store SkPMColors, which is not necessarily
// the case here.
// TODO: If we add support for non-native swizzles, we'll need to handle that here.
bool SkPngCodec::decodePalette(bool premultiply, int* ctableCount) {

    int numColors;
    png_color* palette;
    if (!png_get_PLTE(fPng_ptr, fInfo_ptr, &palette, &numColors)) {
        return false;
    }

    // Note: These are not necessarily SkPMColors.
    SkPMColor colorPtr[256];

    png_bytep alphas;
    int numColorsWithAlpha = 0;
    if (png_get_tRNS(fPng_ptr, fInfo_ptr, &alphas, &numColorsWithAlpha, nullptr)) {
        // Choose which function to use to create the color table. If the final destination's
        // colortype is unpremultiplied, the color table will store unpremultiplied colors.
        PackColorProc proc;
        if (premultiply) {
            proc = &SkPremultiplyARGBInline;
        } else {
            proc = &SkPackARGB32NoCheck;
        }

        for (int i = 0; i < numColorsWithAlpha; i++) {
            // We don't have a function in SkOpts that combines a set of alphas with a set
            // of RGBs.  We could write one, but it's hardly worth it, given that this
            // is such a small fraction of the total decode time.
            colorPtr[i] = proc(alphas[i], palette->red, palette->green, palette->blue);
            palette++;
        }
    }

    if (numColorsWithAlpha < numColors) {
        // The optimized code depends on a 3-byte png_color struct with the colors
        // in RGB order.  These checks make sure it is safe to use.
        static_assert(3 == sizeof(png_color), "png_color struct has changed.  Opts are broken.");
#ifdef SK_DEBUG
        SkASSERT(&palette->red < &palette->green);
        SkASSERT(&palette->green < &palette->blue);
#endif

#ifdef SK_PMCOLOR_IS_RGBA
        SkOpts::RGB_to_RGB1(colorPtr + numColorsWithAlpha, palette, numColors - numColorsWithAlpha);
#else
        SkOpts::RGB_to_BGR1(colorPtr + numColorsWithAlpha, palette, numColors - numColorsWithAlpha);
#endif
    }

    // Pad the color table with the last color in the table (or black) in the case that
    // invalid pixel indices exceed the number of colors in the table.
    const int maxColors = 1 << fBitDepth;
    if (numColors < maxColors) {
        SkPMColor lastColor = numColors > 0 ? colorPtr[numColors - 1] : SK_ColorBLACK;
        sk_memset32(colorPtr + numColors, lastColor, maxColors - numColors);
    }

    // Set the new color count.
    if (ctableCount != nullptr) {
        *ctableCount = maxColors;
    }

    fColorTable.reset(new SkColorTable(colorPtr, maxColors));
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Creation
///////////////////////////////////////////////////////////////////////////////

bool SkPngCodec::IsPng(const char* buf, size_t bytesRead) {
    return !png_sig_cmp((png_bytep) buf, (png_size_t)0, bytesRead);
}

// Reads the header and initializes the output fields, if not NULL.
//
// @param stream Input data. Will be read to get enough information to properly
//      setup the codec.
// @param chunkReader SkPngChunkReader, for reading unknown chunks. May be NULL.
//      If not NULL, png_ptr will hold an *unowned* pointer to it. The caller is
//      expected to continue to own it for the lifetime of the png_ptr.
// @param png_ptrp Optional output variable. If non-NULL, will be set to a new
//      png_structp on success.
// @param info_ptrp Optional output variable. If non-NULL, will be set to a new
//      png_infop on success;
// @param imageInfo Optional output variable. If non-NULL, will be set to
//      reflect the properties of the encoded image on success.
// @param bitDepthPtr Optional output variable. If non-NULL, will be set to the
//      bit depth of the encoded image on success.
// @param numberPassesPtr Optional output variable. If non-NULL, will be set to
//      the number_passes of the encoded image on success.
// @return true on success, in which case the caller is responsible for calling
//      png_destroy_read_struct(png_ptrp, info_ptrp).
//      If it returns false, the passed in fields (except stream) are unchanged.
static bool read_header(SkStream* stream, SkPngChunkReader* chunkReader,
                        png_structp* png_ptrp, png_infop* info_ptrp,
                        SkImageInfo* imageInfo, int* bitDepthPtr, int* numberPassesPtr) {
    // The image is known to be a PNG. Decode enough to know the SkImageInfo.
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
                                                 sk_error_fn, sk_warning_fn);
    if (!png_ptr) {
        return false;
    }

    AutoCleanPng autoClean(png_ptr);

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == nullptr) {
        return false;
    }

    autoClean.setInfoPtr(info_ptr);

    // FIXME: Could we use the return value of setjmp to specify the type of
    // error?
    if (setjmp(png_jmpbuf(png_ptr))) {
        return false;
    }

    png_set_read_fn(png_ptr, static_cast<void*>(stream), sk_read_fn);

#ifdef PNG_READ_UNKNOWN_CHUNKS_SUPPORTED
    // Hookup our chunkReader so we can see any user-chunks the caller may be interested in.
    // This needs to be installed before we read the png header.  Android may store ninepatch
    // chunks in the header.
    if (chunkReader) {
        png_set_keep_unknown_chunks(png_ptr, PNG_HANDLE_CHUNK_ALWAYS, (png_byte*)"", 0);
        png_set_read_user_chunk_fn(png_ptr, (png_voidp) chunkReader, sk_read_user_chunk);
    }
#endif

    // The call to png_read_info() gives us all of the information from the
    // PNG file before the first IDAT (image data chunk).
    png_read_info(png_ptr, info_ptr);
    png_uint_32 origWidth, origHeight;
    int bitDepth, encodedColorType;
    png_get_IHDR(png_ptr, info_ptr, &origWidth, &origHeight, &bitDepth,
                 &encodedColorType, nullptr, nullptr, nullptr);

    if (bitDepthPtr) {
        *bitDepthPtr = bitDepth;
    }

    // Tell libpng to strip 16 bit/color files down to 8 bits/color.
    // TODO: Should we handle this in SkSwizzler?  Could this also benefit
    //       RAW decodes?
    if (bitDepth == 16) {
        SkASSERT(PNG_COLOR_TYPE_PALETTE != encodedColorType);
        png_set_strip_16(png_ptr);
    }

    // Now determine the default colorType and alphaType and set the required transforms.
    // Often, we depend on SkSwizzler to perform any transforms that we need.  However, we
    // still depend on libpng for many of the rare and PNG-specific cases.
    SkColorType colorType = kUnknown_SkColorType;
    SkAlphaType alphaType = kUnknown_SkAlphaType;
    switch (encodedColorType) {
        case PNG_COLOR_TYPE_PALETTE:
            // Extract multiple pixels with bit depths of 1, 2, and 4 from a single
            // byte into separate bytes (useful for paletted and grayscale images).
            if (bitDepth < 8) {
                // TODO: Should we use SkSwizzler here?
                png_set_packing(png_ptr);
            }

            colorType = kIndex_8_SkColorType;
            // Set the alpha type depending on if a transparency chunk exists.
            alphaType = png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS) ?
                    kUnpremul_SkAlphaType : kOpaque_SkAlphaType;
            break;
        case PNG_COLOR_TYPE_RGB:
            if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
                // Convert to RGBA if transparency chunk exists.
                png_set_tRNS_to_alpha(png_ptr);
                alphaType = kUnpremul_SkAlphaType;
            } else {
                alphaType = kOpaque_SkAlphaType;
            }
            colorType = kN32_SkColorType;
            break;
        case PNG_COLOR_TYPE_GRAY:
            // Expand grayscale images to the full 8 bits from 1, 2, or 4 bits/pixel.
            if (bitDepth < 8) {
                // TODO: Should we use SkSwizzler here?
                png_set_expand_gray_1_2_4_to_8(png_ptr);
            }

            if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
                // Convert to RGBA if there is a transparency chunk.
                png_set_tRNS_to_alpha(png_ptr);
                png_set_gray_to_rgb(png_ptr);
                colorType = kN32_SkColorType;
                alphaType = kUnpremul_SkAlphaType;
            } else {
                colorType = kGray_8_SkColorType;
                alphaType = kOpaque_SkAlphaType;
            }
            break;
        case PNG_COLOR_TYPE_GRAY_ALPHA:
            // Convert to RGBA if the image has alpha.
            png_set_gray_to_rgb(png_ptr);
            colorType = kN32_SkColorType;
            alphaType = kUnpremul_SkAlphaType;
            break;
        case PNG_COLOR_TYPE_RGBA:
            colorType = kN32_SkColorType;
            alphaType = kUnpremul_SkAlphaType;
            break;
        default:
            // All the color types have been covered above.
            SkASSERT(false);
    }

    int numberPasses = png_set_interlace_handling(png_ptr);
    if (numberPassesPtr) {
        *numberPassesPtr = numberPasses;
    }

    // FIXME: Also need to check for sRGB ( https://bug.skia.org/3471 ).

    if (imageInfo) {
        *imageInfo = SkImageInfo::Make(origWidth, origHeight, colorType, alphaType);
    }
    autoClean.detach();
    if (png_ptrp) {
        *png_ptrp = png_ptr;
    }
    if (info_ptrp) {
        *info_ptrp = info_ptr;
    }

    return true;
}

SkPngCodec::SkPngCodec(const SkImageInfo& info, SkStream* stream, SkPngChunkReader* chunkReader,
                       png_structp png_ptr, png_infop info_ptr, int bitDepth, int numberPasses)
    : INHERITED(info, stream)
    , fPngChunkReader(SkSafeRef(chunkReader))
    , fPng_ptr(png_ptr)
    , fInfo_ptr(info_ptr)
    , fSrcConfig(SkSwizzler::kUnknown)
    , fNumberPasses(numberPasses)
    , fBitDepth(bitDepth)
{}

SkPngCodec::~SkPngCodec() {
    this->destroyReadStruct();
}

void SkPngCodec::destroyReadStruct() {
    if (fPng_ptr) {
        // We will never have a nullptr fInfo_ptr with a non-nullptr fPng_ptr
        SkASSERT(fInfo_ptr);
        png_destroy_read_struct(&fPng_ptr, &fInfo_ptr, nullptr);
        fPng_ptr = nullptr;
        fInfo_ptr = nullptr;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Getting the pixels
///////////////////////////////////////////////////////////////////////////////

SkCodec::Result SkPngCodec::initializeSwizzler(const SkImageInfo& requestedInfo,
                                               const Options& options,
                                               SkPMColor ctable[],
                                               int* ctableCount) {
    // FIXME: Could we use the return value of setjmp to specify the type of
    // error?
    if (setjmp(png_jmpbuf(fPng_ptr))) {
        SkCodecPrintf("setjmp long jump!\n");
        return kInvalidInput;
    }
    png_read_update_info(fPng_ptr, fInfo_ptr);

    // srcColorType was determined in read_header() which determined png color type
    const SkColorType srcColorType = this->getInfo().colorType();

    switch (srcColorType) {
        case kIndex_8_SkColorType:
            //decode palette to Skia format
            fSrcConfig = SkSwizzler::kIndex;
            if (!this->decodePalette(kPremul_SkAlphaType == requestedInfo.alphaType(),
                    ctableCount)) {
                return kInvalidInput;
            }
            break;
        case kGray_8_SkColorType:
            fSrcConfig = SkSwizzler::kGray;
            break;
        case kN32_SkColorType:
            if (this->getInfo().alphaType() == kOpaque_SkAlphaType) {
                    fSrcConfig = SkSwizzler::kRGB;
                } else {
                    fSrcConfig = SkSwizzler::kRGBA;
            }
            break;
        default:
            // We will always recommend one of the above colorTypes.
            SkASSERT(false);
    }

    // Copy the color table to the client if they request kIndex8 mode
    copy_color_table(requestedInfo, fColorTable, ctable, ctableCount);

    // Create the swizzler.  SkPngCodec retains ownership of the color table.
    const SkPMColor* colors = get_color_ptr(fColorTable.get());
    fSwizzler.reset(SkSwizzler::CreateSwizzler(fSrcConfig, colors, requestedInfo, options));
    SkASSERT(fSwizzler);

    return kSuccess;
}


bool SkPngCodec::onRewind() {
    // This sets fPng_ptr and fInfo_ptr to nullptr. If read_header
    // succeeds, they will be repopulated, and if it fails, they will
    // remain nullptr. Any future accesses to fPng_ptr and fInfo_ptr will
    // come through this function which will rewind and again attempt
    // to reinitialize them.
    this->destroyReadStruct();

    png_structp png_ptr;
    png_infop info_ptr;
    if (!read_header(this->stream(), fPngChunkReader.get(), &png_ptr, &info_ptr,
                     nullptr, nullptr, nullptr)) {
        return false;
    }

    fPng_ptr = png_ptr;
    fInfo_ptr = info_ptr;
    return true;
}

SkCodec::Result SkPngCodec::onGetPixels(const SkImageInfo& requestedInfo, void* dst,
                                        size_t dstRowBytes, const Options& options,
                                        SkPMColor ctable[], int* ctableCount,
                                        int* rowsDecoded) {
    if (!conversion_possible(requestedInfo, this->getInfo())) {
        return kInvalidConversion;
    }
    if (options.fSubset) {
        // Subsets are not supported.
        return kUnimplemented;
    }

    // Note that ctable and ctableCount may be modified if there is a color table
    const Result result = this->initializeSwizzler(requestedInfo, options, ctable, ctableCount);
    if (result != kSuccess) {
        return result;
    }
    // FIXME: Could we use the return value of setjmp to specify the type of
    // error?
    int row = 0;
    // This must be declared above the call to setjmp to avoid memory leaks on incomplete images.
    SkAutoTMalloc<uint8_t> storage;
    if (setjmp(png_jmpbuf(fPng_ptr))) {
        // Assume that any error that occurs while reading rows is caused by an incomplete input.
        if (fNumberPasses > 1) {
            // FIXME (msarett): Handle incomplete interlaced pngs.
            return kInvalidInput;
        }
        // FIXME: We do a poor job on incomplete pngs compared to other decoders (ex: Chromium,
        // Ubuntu Image Viewer).  This is because we use the default buffer size in libpng (8192
        // bytes), and if we can't fill the buffer, we immediately fail.
        // For example, if we try to read 8192 bytes, and the image (incorrectly) only contains
        // half that, which may have been enough to contain a non-zero number of lines, we fail
        // when we could have decoded a few more lines and then failed.
        // The read function that we provide for libpng has no way of indicating that we have
        // made a partial read.
        // Making our buffer size smaller improves our incomplete decodes, but what impact does
        // it have on regular decode performance?  Should we investigate using a different API
        // instead of png_read_row(s)?  Chromium uses png_process_data.
        *rowsDecoded = row;
        return kIncompleteInput;
    }

    // FIXME: We could split these out based on subclass.
    void* dstRow = dst;
    if (fNumberPasses > 1) {
        const int width = requestedInfo.width();
        const int height = requestedInfo.height();
        const int bpp = SkSwizzler::BytesPerPixel(fSrcConfig);
        const size_t srcRowBytes = width * bpp;

        storage.reset(width * height * bpp);
        uint8_t* const base = storage.get();

        for (int i = 0; i < fNumberPasses; i++) {
            uint8_t* srcRow = base;
            for (int y = 0; y < height; y++) {
                uint8_t* bmRow = srcRow;
                png_read_rows(fPng_ptr, &bmRow, nullptr, 1);
                srcRow += srcRowBytes;
            }
        }

        // Now swizzle it.
        uint8_t* srcRow = base;
        for (int y = 0; y < height; y++) {
            fSwizzler->swizzle(dstRow, srcRow);
            dstRow = SkTAddOffset<void>(dstRow, dstRowBytes);
            srcRow += srcRowBytes;
        }
    } else {
        storage.reset(requestedInfo.width() * SkSwizzler::BytesPerPixel(fSrcConfig));
        uint8_t* srcRow = storage.get();
        for (; row < requestedInfo.height(); row++) {
            png_read_rows(fPng_ptr, &srcRow, nullptr, 1);
            // FIXME: Only call IsOpaque once, outside the loop. Same for onGetScanlines.
            fSwizzler->swizzle(dstRow, srcRow);
            dstRow = SkTAddOffset<void>(dstRow, dstRowBytes);
        }
    }

    if (setjmp(png_jmpbuf(fPng_ptr))) {
        // We've already read all the scanlines. This is a success.
        return kSuccess;
    }

    // read rest of file, and get additional comment and time chunks in info_ptr
    png_read_end(fPng_ptr, fInfo_ptr);

    return kSuccess;
}

uint32_t SkPngCodec::onGetFillValue(SkColorType colorType, SkAlphaType alphaType) const {
    const SkPMColor* colorPtr = get_color_ptr(fColorTable.get());
    if (colorPtr) {
        return get_color_table_fill_value(colorType, colorPtr, 0);
    }
    return INHERITED::onGetFillValue(colorType, alphaType);
}

// Subclass of SkPngCodec which supports scanline decoding
class SkPngScanlineDecoder : public SkPngCodec {
public:
    SkPngScanlineDecoder(const SkImageInfo& srcInfo, SkStream* stream,
            SkPngChunkReader* chunkReader, png_structp png_ptr, png_infop info_ptr, int bitDepth)
        : INHERITED(srcInfo, stream, chunkReader, png_ptr, info_ptr, bitDepth, 1)
        , fSrcRow(nullptr)
    {}

    Result onStartScanlineDecode(const SkImageInfo& dstInfo, const Options& options,
            SkPMColor ctable[], int* ctableCount) override {
        if (!conversion_possible(dstInfo, this->getInfo())) {
            return kInvalidConversion;
        }

        const Result result = this->initializeSwizzler(dstInfo, options, ctable,
                                                       ctableCount);
        if (result != kSuccess) {
            return result;
        }

        fStorage.reset(this->getInfo().width() * SkSwizzler::BytesPerPixel(this->srcConfig()));
        fSrcRow = fStorage.get();

        return kSuccess;
    }

    int onGetScanlines(void* dst, int count, size_t rowBytes) override {
        // Assume that an error in libpng indicates an incomplete input.
        int row = 0;
        if (setjmp(png_jmpbuf(this->png_ptr()))) {
            SkCodecPrintf("setjmp long jump!\n");
            return row;
        }

        void* dstRow = dst;
        for (; row < count; row++) {
            png_read_rows(this->png_ptr(), &fSrcRow, nullptr, 1);
            this->swizzler()->swizzle(dstRow, fSrcRow);
            dstRow = SkTAddOffset<void>(dstRow, rowBytes);
        }

        return row;
    }

    bool onSkipScanlines(int count) override {
        // Assume that an error in libpng indicates an incomplete input.
        if (setjmp(png_jmpbuf(this->png_ptr()))) {
            SkCodecPrintf("setjmp long jump!\n");
            return false;
        }
        //there is a potential tradeoff of memory vs speed created by putting this in a loop.
        //calling png_read_rows in a loop is insignificantly slower than calling it once with count
        //as png_read_rows has it's own loop which calls png_read_row count times.
        for (int row = 0; row < count; row++) {
            png_read_rows(this->png_ptr(), &fSrcRow, nullptr, 1);
        }
        return true;
    }

private:
    SkAutoTMalloc<uint8_t>      fStorage;
    uint8_t*                    fSrcRow;

    typedef SkPngCodec INHERITED;
};


class SkPngInterlacedScanlineDecoder : public SkPngCodec {
public:
    SkPngInterlacedScanlineDecoder(const SkImageInfo& srcInfo, SkStream* stream,
            SkPngChunkReader* chunkReader, png_structp png_ptr, png_infop info_ptr,
            int bitDepth, int numberPasses)
        : INHERITED(srcInfo, stream, chunkReader, png_ptr, info_ptr, bitDepth, numberPasses)
        , fHeight(-1)
        , fCanSkipRewind(false)
    {
        SkASSERT(numberPasses != 1);
    }

    Result onStartScanlineDecode(const SkImageInfo& dstInfo, const Options& options,
            SkPMColor ctable[], int* ctableCount) override {
        if (!conversion_possible(dstInfo, this->getInfo())) {
            return kInvalidConversion;
        }

        const Result result = this->initializeSwizzler(dstInfo, options, ctable,
                                                       ctableCount);
        if (result != kSuccess) {
            return result;
        }

        fHeight = dstInfo.height();
        // FIXME: This need not be called on a second call to onStartScanlineDecode.
        fSrcRowBytes = this->getInfo().width() * SkSwizzler::BytesPerPixel(this->srcConfig());
        fGarbageRow.reset(fSrcRowBytes);
        fGarbageRowPtr = static_cast<uint8_t*>(fGarbageRow.get());
        fCanSkipRewind = true;

        return SkCodec::kSuccess;
    }

    int onGetScanlines(void* dst, int count, size_t dstRowBytes) override {
        // rewind stream if have previously called onGetScanlines,
        // since we need entire progressive image to get scanlines
        if (fCanSkipRewind) {
            // We already rewound in onStartScanlineDecode, so there is no reason to rewind.
            // Next time onGetScanlines is called, we will need to rewind.
            fCanSkipRewind = false;
        } else {
            // rewindIfNeeded resets fCurrScanline, since it assumes that start
            // needs to be called again before scanline decoding. PNG scanline
            // decoding is the exception, since it needs to rewind between
            // calls to getScanlines. Keep track of fCurrScanline, to undo the
            // reset.
            const int currScanline = this->nextScanline();
            // This method would never be called if currScanline is -1
            SkASSERT(currScanline != -1);

            if (!this->rewindIfNeeded()) {
                return kCouldNotRewind;
            }
            this->updateCurrScanline(currScanline);
        }

        if (setjmp(png_jmpbuf(this->png_ptr()))) {
            SkCodecPrintf("setjmp long jump!\n");
            // FIXME (msarett): Returning 0 is pessimistic.  If we can complete a single pass,
            // we may be able to report that all of the memory has been initialized.  Even if we
            // fail on the first pass, we can still report than some scanlines are initialized.
            return 0;
        }
        SkAutoTMalloc<uint8_t> storage(count * fSrcRowBytes);
        uint8_t* storagePtr = storage.get();
        uint8_t* srcRow;
        const int startRow = this->nextScanline();
        for (int i = 0; i < this->numberPasses(); i++) {
            // read rows we planned to skip into garbage row
            for (int y = 0; y < startRow; y++){
                png_read_rows(this->png_ptr(), &fGarbageRowPtr, nullptr, 1);
            }
            // read rows we care about into buffer
            srcRow = storagePtr;
            for (int y = 0; y < count; y++) {
                png_read_rows(this->png_ptr(), &srcRow, nullptr, 1);
                srcRow += fSrcRowBytes;
            }
            // read rows we don't want into garbage buffer
            for (int y = 0; y < fHeight - startRow - count; y++) {
                png_read_rows(this->png_ptr(), &fGarbageRowPtr, nullptr, 1);
            }
        }
        //swizzle the rows we care about
        srcRow = storagePtr;
        void* dstRow = dst;
        for (int y = 0; y < count; y++) {
            this->swizzler()->swizzle(dstRow, srcRow);
            dstRow = SkTAddOffset<void>(dstRow, dstRowBytes);
            srcRow += fSrcRowBytes;
        }

        return count;
    }

    bool onSkipScanlines(int count) override {
        // The non-virtual version will update fCurrScanline.
        return true;
    }

    SkScanlineOrder onGetScanlineOrder() const override {
        return kNone_SkScanlineOrder;
    }

private:
    int                         fHeight;
    size_t                      fSrcRowBytes;
    SkAutoMalloc                fGarbageRow;
    uint8_t*                    fGarbageRowPtr;
    // FIXME: This imitates behavior in SkCodec::rewindIfNeeded. That function
    // is called whenever some action is taken that reads the stream and
    // therefore the next call will require a rewind. So it modifies a boolean
    // to note that the *next* time it is called a rewind is needed.
    // SkPngInterlacedScanlineDecoder has an extra wrinkle - calling
    // onStartScanlineDecode followed by onGetScanlines does *not* require a
    // rewind. Since rewindIfNeeded does not have this flexibility, we need to
    // add another layer.
    bool                        fCanSkipRewind;

    typedef SkPngCodec INHERITED;
};

SkCodec* SkPngCodec::NewFromStream(SkStream* stream, SkPngChunkReader* chunkReader) {
    SkAutoTDelete<SkStream> streamDeleter(stream);
    png_structp png_ptr;
    png_infop info_ptr;
    SkImageInfo imageInfo;
    int bitDepth;
    int numberPasses;

    if (!read_header(stream, chunkReader, &png_ptr, &info_ptr, &imageInfo, &bitDepth,
                     &numberPasses)) {
        return nullptr;
    }

    if (1 == numberPasses) {
        return new SkPngScanlineDecoder(imageInfo, streamDeleter.detach(), chunkReader,
                                        png_ptr, info_ptr, bitDepth);
    }

    return new SkPngInterlacedScanlineDecoder(imageInfo, streamDeleter.detach(), chunkReader,
                                              png_ptr, info_ptr, bitDepth, numberPasses);
}
