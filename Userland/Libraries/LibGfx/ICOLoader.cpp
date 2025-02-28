/*
 * Copyright (c) 2020, Paul Roukema <roukemap@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <AK/MemoryStream.h>
#include <AK/NonnullOwnPtrVector.h>
#include <AK/Types.h>
#include <LibGfx/BMPLoader.h>
#include <LibGfx/ICOLoader.h>
#include <LibGfx/PNGLoader.h>
#include <string.h>

namespace Gfx {

// FIXME: This is in little-endian order. Maybe need a NetworkOrdered<T> equivalent eventually.
struct ICONDIR {
    u16 must_be_0 = 0;
    u16 must_be_1 = 0;
    u16 image_count = 0;
};
static_assert(AssertSize<ICONDIR, 6>());

struct ICONDIRENTRY {
    u8 width;
    u8 height;
    u8 color_count;
    u8 reserved_0;
    u16 planes;
    u16 bits_per_pixel;
    u32 size;
    u32 offset;
};
static_assert(AssertSize<ICONDIRENTRY, 16>());

};

template<>
class AK::Traits<Gfx::ICONDIR> : public GenericTraits<Gfx::ICONDIR> {
public:
    static constexpr bool is_trivially_serializable() { return true; }
};

template<>
class AK::Traits<Gfx::ICONDIRENTRY> : public GenericTraits<Gfx::ICONDIRENTRY> {
public:
    static constexpr bool is_trivially_serializable() { return true; }
};

namespace Gfx {

struct ICOImageDescriptor {
    u16 width;
    u16 height;
    u16 bits_per_pixel;
    size_t offset;
    size_t size;
    RefPtr<Gfx::Bitmap> bitmap;
};

struct ICOLoadingContext {
    enum State {
        NotDecoded = 0,
        Error,
        DirectoryDecoded,
        BitmapDecoded
    };
    State state { NotDecoded };
    u8 const* data { nullptr };
    size_t data_size { 0 };
    Vector<ICOImageDescriptor> images;
    size_t largest_index;
};

static ErrorOr<size_t> decode_ico_header(AK::Stream& stream)
{
    auto header = TRY(stream.read_value<ICONDIR>());
    if (header.must_be_0 != 0 || header.must_be_1 != 1)
        return Error::from_string_literal("Invalid ICO header");
    return { header.image_count };
}

static ErrorOr<ICOImageDescriptor> decode_ico_direntry(AK::Stream& stream)
{
    auto entry = TRY(stream.read_value<ICONDIRENTRY>());
    ICOImageDescriptor desc = { entry.width, entry.height, entry.bits_per_pixel, entry.offset, entry.size, nullptr };
    if (desc.width == 0)
        desc.width = 256;
    if (desc.height == 0)
        desc.height = 256;

    return { desc };
}

static size_t find_largest_image(ICOLoadingContext const& context)
{
    size_t max_area = 0;
    size_t index = 0;
    size_t largest_index = 0;
    u16 max_bits_per_pixel = 0;
    for (auto const& desc : context.images) {
        if (static_cast<size_t>(desc.width) * static_cast<size_t>(desc.height) >= max_area) {
            if (desc.bits_per_pixel > max_bits_per_pixel) {
                max_area = desc.width * desc.height;
                largest_index = index;
                max_bits_per_pixel = desc.bits_per_pixel;
            }
        }
        ++index;
    }
    return largest_index;
}

static ErrorOr<void> load_ico_directory(ICOLoadingContext& context)
{
    auto stream = TRY(FixedMemoryStream::construct({ context.data, context.data_size }));

    auto image_count = TRY(decode_ico_header(*stream));
    if (image_count == 0)
        return Error::from_string_literal("ICO file has no images");

    for (size_t i = 0; i < image_count; ++i) {
        auto desc = TRY(decode_ico_direntry(*stream));
        if (desc.offset + desc.size < desc.offset // detect integer overflow
            || (desc.offset + desc.size) > context.data_size) {
            dbgln_if(ICO_DEBUG, "load_ico_directory: offset: {} size: {} doesn't fit in ICO size: {}", desc.offset, desc.size, context.data_size);
            return Error::from_string_literal("ICO size too large");
        }
        dbgln_if(ICO_DEBUG, "load_ico_directory: index {} width: {} height: {} offset: {} size: {}", i, desc.width, desc.height, desc.offset, desc.size);
        TRY(context.images.try_append(desc));
    }
    context.largest_index = find_largest_image(context);
    context.state = ICOLoadingContext::State::DirectoryDecoded;
    return {};
}

ErrorOr<void> ICOImageDecoderPlugin::load_ico_bitmap(ICOLoadingContext& context, Optional<size_t> index)
{
    if (context.state < ICOLoadingContext::State::DirectoryDecoded)
        TRY(load_ico_directory(context));

    size_t real_index = context.largest_index;
    if (index.has_value())
        real_index = index.value();
    if (real_index >= context.images.size())
        return Error::from_string_literal("Index out of bounds");

    ICOImageDescriptor& desc = context.images[real_index];
    if (TRY(PNGImageDecoderPlugin::sniff({ context.data + desc.offset, desc.size }))) {
        auto png_decoder = TRY(PNGImageDecoderPlugin::create({ context.data + desc.offset, desc.size }));
        if (png_decoder->initialize()) {
            auto decoded_png_frame = TRY(png_decoder->frame(0));
            if (!decoded_png_frame.image) {
                dbgln_if(ICO_DEBUG, "load_ico_bitmap: failed to load PNG encoded image index: {}", real_index);
                return Error::from_string_literal("Encoded image not null");
            }
            desc.bitmap = decoded_png_frame.image;
            return {};
        }
        return Error::from_string_literal("Couldn't initialize PNG Decoder");
    } else {
        auto bmp_decoder = TRY(BMPImageDecoderPlugin::create_as_included_in_ico({}, { context.data + desc.offset, desc.size }));
        // NOTE: We don't initialize a BMP decoder in the usual way, but rather
        // we just create an object and try to sniff for a frame when it's included
        // inside an ICO image.
        if (bmp_decoder->sniff_dib()) {
            auto decoded_bmp_frame = TRY(bmp_decoder->frame(0));
            if (!decoded_bmp_frame.image) {
                dbgln_if(ICO_DEBUG, "load_ico_bitmap: failed to load BMP encoded image index: {}", real_index);
                return Error::from_string_literal("Encoded image not null");
            }
            desc.bitmap = decoded_bmp_frame.image;
        } else {
            dbgln_if(ICO_DEBUG, "load_ico_bitmap: encoded image not supported at index: {}", real_index);
            return Error::from_string_literal("Encoded image not supported");
        }
        return {};
    }
}

ErrorOr<bool> ICOImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    auto stream = TRY(FixedMemoryStream::construct(data));
    return !decode_ico_header(*stream).is_error();
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> ICOImageDecoderPlugin::create(ReadonlyBytes data)
{
    return adopt_nonnull_own_or_enomem(new (nothrow) ICOImageDecoderPlugin(data.data(), data.size()));
}

ICOImageDecoderPlugin::ICOImageDecoderPlugin(u8 const* data, size_t size)
{
    m_context = make<ICOLoadingContext>();
    m_context->data = data;
    m_context->data_size = size;
}

ICOImageDecoderPlugin::~ICOImageDecoderPlugin() = default;

IntSize ICOImageDecoderPlugin::size()
{
    if (m_context->state == ICOLoadingContext::State::Error) {
        return {};
    }

    if (m_context->state < ICOLoadingContext::State::DirectoryDecoded) {
        if (!load_ico_directory(*m_context).is_error()) {
            m_context->state = ICOLoadingContext::State::Error;
            return {};
        }
        m_context->state = ICOLoadingContext::State::DirectoryDecoded;
    }

    return { m_context->images[m_context->largest_index].width, m_context->images[m_context->largest_index].height };
}

void ICOImageDecoderPlugin::set_volatile()
{
    if (m_context->images[0].bitmap)
        m_context->images[0].bitmap->set_volatile();
}

bool ICOImageDecoderPlugin::set_nonvolatile(bool& was_purged)
{
    if (!m_context->images[0].bitmap)
        return false;
    return m_context->images[0].bitmap->set_nonvolatile(was_purged);
}

bool ICOImageDecoderPlugin::initialize()
{
    auto stream_or_error = FixedMemoryStream::construct(ReadonlyBytes { m_context->data, m_context->data_size });
    if (stream_or_error.is_error())
        return false;
    return !decode_ico_header(*stream_or_error.value()).is_error();
}

bool ICOImageDecoderPlugin::is_animated()
{
    return false;
}

size_t ICOImageDecoderPlugin::loop_count()
{
    return 0;
}

size_t ICOImageDecoderPlugin::frame_count()
{
    return 1;
}

ErrorOr<ImageFrameDescriptor> ICOImageDecoderPlugin::frame(size_t index)
{
    if (index > 0)
        return Error::from_string_literal("ICOImageDecoderPlugin: Invalid frame index");

    if (m_context->state == ICOLoadingContext::State::Error)
        return Error::from_string_literal("ICOImageDecoderPlugin: Decoding failed");

    if (m_context->state < ICOLoadingContext::State::BitmapDecoded) {
        // NOTE: This forces the chunk decoding to happen.
        auto maybe_error = load_ico_bitmap(*m_context, {});
        if (maybe_error.is_error()) {
            m_context->state = ICOLoadingContext::State::Error;
            return Error::from_string_literal("ICOImageDecoderPlugin: Decoding failed");
        }
        m_context->state = ICOLoadingContext::State::BitmapDecoded;
    }

    VERIFY(m_context->images[m_context->largest_index].bitmap);
    return ImageFrameDescriptor { m_context->images[m_context->largest_index].bitmap, 0 };
}

ErrorOr<Optional<ReadonlyBytes>> ICOImageDecoderPlugin::icc_data()
{
    return OptionalNone {};
}

}
