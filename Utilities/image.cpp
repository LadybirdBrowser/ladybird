/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

 #include <LibCore/ArgsParser.h>
 #include <LibCore/File.h>
 #include <LibCore/MappedFile.h>
 #include <LibGfx/ImageFormats/BMPWriter.h>
 #include <LibGfx/ImageFormats/ImageDecoder.h>
 #include <LibGfx/ImageFormats/JPEGWriter.h>
 #include <LibGfx/ImageFormats/PNGWriter.h>
 #include <LibGfx/ImageFormats/WebPSharedLossless.h>
 #include <LibGfx/ImageFormats/WebPWriter.h>
 
 using AnyBitmap = Variant<RefPtr<Gfx::Bitmap>, RefPtr<Gfx::CMYKBitmap>>;
 
 /**
  * A struct to hold the loaded image, along with its format and optional
  * ICC data (color profile).
  */
 struct LoadedImage {
     Gfx::NaturalFrameFormat internal_format;
     AnyBitmap bitmap;
     Optional<ReadonlyBytes> icc_data;
 };
 
 /**
  * Load a specific frame of an image using the provided decoder.
  * Returns a LoadedImage on success or an Error on failure.
  */
 static ErrorOr<LoadedImage> load_image(RefPtr<Gfx::ImageDecoder> const& decoder, int frame_index)
 {
     auto internal_format = decoder->natural_frame_format();
 
     auto bitmap = TRY([&]() -> ErrorOr<AnyBitmap> {
         switch (internal_format) {
         case Gfx::NaturalFrameFormat::RGB:
         case Gfx::NaturalFrameFormat::Grayscale:
         case Gfx::NaturalFrameFormat::Vector:
             return TRY(decoder->frame(frame_index)).image;
         case Gfx::NaturalFrameFormat::CMYK:
             return RefPtr(TRY(decoder->cmyk_frame()));
         }
         VERIFY_NOT_REACHED();
     }());
 
     return LoadedImage { internal_format, move(bitmap), TRY(decoder->icc_data()) };
 }
 
 /**
  * Invert CMYK channels in place. Fails for non-CMYK bitmaps.
  */
 static ErrorOr<void> invert_cmyk(LoadedImage& image)
 {
     if (!image.bitmap.has<RefPtr<Gfx::CMYKBitmap>>())
         return Error::from_string_literal("Can't --invert-cmyk with RGB bitmaps");
 
     auto& frame = image.bitmap.get<RefPtr<Gfx::CMYKBitmap>>();
     for (auto& pixel : *frame) {
         pixel.c = ~pixel.c;
         pixel.m = ~pixel.m;
         pixel.y = ~pixel.y;
         pixel.k = ~pixel.k;
     }
     return {};
 }
 
 /**
  * Crop the image to the specified rectangle. Fails for CMYK bitmaps.
  */
 static ErrorOr<void> crop_image(LoadedImage& image, Gfx::IntRect const& rect)
 {
     if (!image.bitmap.has<RefPtr<Gfx::Bitmap>>())
         return Error::from_string_literal("Can't --crop CMYK bitmaps yet");
 
     auto& frame = image.bitmap.get<RefPtr<Gfx::Bitmap>>();
     frame = TRY(frame->cropped(rect));
     return {};
 }
 
 /**
  * Move the alpha channel to the RGB channels, turning the bitmap into a grayscale
  * representation of what used to be alpha. Fails for CMYK bitmaps or unsupported
  * RGB formats.
  */
 static ErrorOr<void> move_alpha_to_rgb(LoadedImage& image)
 {
     if (!image.bitmap.has<RefPtr<Gfx::Bitmap>>())
         return Error::from_string_literal("Can't --move-alpha-to-rgb with CMYK bitmaps");
 
     auto& frame = image.bitmap.get<RefPtr<Gfx::Bitmap>>();
     switch (frame->format()) {
     case Gfx::BitmapFormat::Invalid:
         return Error::from_string_literal("Can't --move-alpha-to-rgb with invalid bitmaps");
     case Gfx::BitmapFormat::RGBA8888:
         return Error::from_string_literal("--move-alpha-to-rgb not implemented for RGBA8888");
     case Gfx::BitmapFormat::BGRA8888:
     case Gfx::BitmapFormat::BGRx8888:
         for (auto& pixel : *frame) {
             u8 alpha = pixel >> 24;
             pixel = 0xff000000 | (alpha << 16) | (alpha << 8) | alpha;
         }
         break;
     case Gfx::BitmapFormat::RGBx8888:
         return Error::from_string_literal("Can't --move-alpha-to-rgb with RGBx8888 bitmaps");
     }
     return {};
 }
 
 /**
  * Strip the alpha channel from the image if it exists. Fails for CMYK bitmaps
  * or unsupported formats.
  */
 static ErrorOr<void> strip_alpha(LoadedImage& image)
 {
     if (!image.bitmap.has<RefPtr<Gfx::Bitmap>>())
         return Error::from_string_literal("Can't --strip-alpha with CMYK bitmaps");
 
     auto& frame = image.bitmap.get<RefPtr<Gfx::Bitmap>>();
     switch (frame->format()) {
     case Gfx::BitmapFormat::Invalid:
         return Error::from_string_literal("Can't --strip-alpha with invalid bitmaps");
     case Gfx::BitmapFormat::RGBA8888:
         return Error::from_string_literal("--strip-alpha not implemented for RGBA8888");
     case Gfx::BitmapFormat::BGRA8888:
     case Gfx::BitmapFormat::BGRx8888:
         frame->strip_alpha_channel();
         break;
     case Gfx::BitmapFormat::RGBx8888:
         // No alpha channel to strip
         break;
     }
     return {};
 }
 
 /**
  * Save the given image to the specified path, using the provided JPEG quality
  * or WebP transform options if applicable.
  */
 static ErrorOr<void> save_image(LoadedImage& image, StringView out_path, u8 jpeg_quality, Optional<unsigned> webp_allowed_transforms)
 {
     auto stream = [out_path]() -> ErrorOr<NonnullOwnPtr<Core::OutputBufferedFile>> {
         auto output_stream = TRY(Core::File::open(out_path, Core::File::OpenMode::Write));
         return Core::OutputBufferedFile::create(move(output_stream));
     };
 
     // Because we only support writing out normal bitmaps here, we directly
     // retrieve the RefPtr<Gfx::Bitmap>. In a scenario where we handle
     // CMYK outputs, we'd adapt this logic.
     auto& frame = image.bitmap.get<RefPtr<Gfx::Bitmap>>();
 
     // Handle JPEG
     if (out_path.ends_with(".jpg"sv, CaseSensitivity::CaseInsensitive) || out_path.ends_with(".jpeg"sv, CaseSensitivity::CaseInsensitive)) {
         TRY(Gfx::JPEGWriter::encode(*TRY(stream()), *frame, { .icc_data = image.icc_data, .quality = jpeg_quality }));
         return {};
     }
 
     // Handle WebP
     if (out_path.ends_with(".webp"sv, CaseSensitivity::CaseInsensitive)) {
         Gfx::WebPWriter::Options options;
         options.icc_data = image.icc_data;
         if (webp_allowed_transforms.has_value())
             options.vp8l_options.allowed_transforms = webp_allowed_transforms.value();
         TRY(Gfx::WebPWriter::encode(*TRY(stream()), *frame, options));
         return {};
     }
 
     // Handle BMP or PNG
     ByteBuffer bytes;
     if (out_path.ends_with(".bmp"sv, CaseSensitivity::CaseInsensitive)) {
         bytes = TRY(Gfx::BMPWriter::encode(*frame, { .icc_data = image.icc_data }));
     } else if (out_path.ends_with(".png"sv, CaseSensitivity::CaseInsensitive)) {
         bytes = TRY(Gfx::PNGWriter::encode(*frame, { .icc_data = image.icc_data }));
     } else {
         return Error::from_string_literal("can only write .bmp, .gif, .jpg, .png, and .webp");
     }
     TRY(TRY(stream())->write_until_depleted(bytes));
 
     return {};
 }
 
 /**
  * Holds options parsed from the command line arguments.
  */
 struct Options {
     StringView in_path;
     StringView out_path;
     bool no_output = false;
     int frame_index = 0;
     bool invert_cmyk = false;
     Optional<Gfx::IntRect> crop_rect;
     bool move_alpha_to_rgb = false;
     bool strip_alpha = false;
     StringView assign_color_profile_path;
     StringView convert_color_profile_path;
     bool strip_color_profile = false;
     u8 quality = 75;
     Optional<unsigned> webp_allowed_transforms;
 };
 
 /**
  * Parse a comma-separated string of numeric values into a vector of type T.
  */
 template<class T>
 static ErrorOr<Vector<T>> parse_comma_separated_numbers(StringView rect_string)
 {
     auto parts = rect_string.split_view(',');
     Vector<T> part_numbers;
     part_numbers.ensure_capacity(parts.size());
 
     for (auto& item : parts) {
         auto part = item.to_number<T>();
         if (!part.has_value())
             return Error::from_string_literal("comma-separated parts must be numbers");
         TRY(part_numbers.try_append(part.value()));
     }
     return part_numbers;
 }
 
 /**
  * Parse a rectangle string "x,y,w,h" into a Gfx::IntRect.
  */
 static ErrorOr<Gfx::IntRect> parse_rect_string(StringView rect_string)
 {
     auto numbers = TRY(parse_comma_separated_numbers<i32>(rect_string));
     if (numbers.size() != 4)
         return Error::from_string_literal("rect must have 4 comma-separated parts");
 
     return Gfx::IntRect { numbers[0], numbers[1], numbers[2], numbers[3] };
 }
 
 /**
  * Parse a comma-separated list of WebP allowed transforms.
  */
 static ErrorOr<unsigned> parse_webp_allowed_transforms_string(StringView string)
 {
     unsigned allowed_transforms = 0;
     for (StringView part : string.split_view(',')) {
         if (part == "predictor" || part == "p")
             allowed_transforms |= 1 << Gfx::PREDICTOR_TRANSFORM;
         else if (part == "color" || part == "c")
             allowed_transforms |= 1 << Gfx::COLOR_TRANSFORM;
         else if (part == "subtract-green" || part == "sg")
             allowed_transforms |= 1 << Gfx::SUBTRACT_GREEN_TRANSFORM;
         else if (part == "color-indexing" || part == "ci")
             allowed_transforms |= 1 << Gfx::COLOR_INDEXING_TRANSFORM;
         else
             return Error::from_string_literal("unknown WebP transform; valid values: predictor, p, color, c, subtract-green, sg, color-indexing, ci");
     }
     return allowed_transforms;
 }
 
 /**
  * Parse command-line options, returning an Options struct that holds the
  * results of parsing. If required arguments are missing or invalid, returns an Error.
  */
 static ErrorOr<Options> parse_options(Main::Arguments arguments)
 {
     Options options;
     Core::ArgsParser args_parser;
 
     args_parser.add_positional_argument(options.in_path, "Path to input image file", "FILE");
     args_parser.add_option(options.out_path, "Path to output image file", "output", 'o', "FILE");
     args_parser.add_option(options.no_output, "Do not write output (only useful for benchmarking image decoding)", "no-output", {});
     args_parser.add_option(options.frame_index, "Which frame of a multi-frame input image (0-based)", "frame-index", {}, "INDEX");
     args_parser.add_option(options.invert_cmyk, "Invert CMYK channels", "invert-cmyk", {});
 
     StringView crop_rect_string;
     args_parser.add_option(crop_rect_string, "Crop to a rectangle", "crop", {}, "x,y,w,h");
 
     args_parser.add_option(options.move_alpha_to_rgb, "Copy alpha channel to rgb, clear alpha", "move-alpha-to-rgb", {});
     args_parser.add_option(options.strip_alpha, "Remove alpha channel", "strip-alpha", {});
     args_parser.add_option(options.assign_color_profile_path, "Load color profile from file and assign it to output image", "assign-color-profile", {}, "FILE");
     args_parser.add_option(options.quality, "Quality used for the JPEG encoder, the default value is 75 on a scale from 0 to 100", "quality", {}, {});
 
     StringView webp_allowed_transforms = "default"sv;
     args_parser.add_option(webp_allowed_transforms, "Comma-separated list of allowed transforms (predictor,p,color,c,subtract-green,sg,color-indexing,ci) for WebP output (default: all allowed)", "webp-allowed-transforms", {}, {});
 
     args_parser.parse(arguments);
 
     if (options.out_path.is_empty() ^ options.no_output)
         return Error::from_string_literal("exactly one of -o or --no-output is required");
 
     if (!crop_rect_string.is_empty())
         options.crop_rect = TRY(parse_rect_string(crop_rect_string));
 
     if (webp_allowed_transforms != "default"sv)
         options.webp_allowed_transforms = TRY(parse_webp_allowed_transforms_string(webp_allowed_transforms));
 
     return options;
 }
 
 /**
  * Entrypoint for the serenity_main utility, orchestrates loading the image, applying
  * transformations, and saving the result if requested.
  */
 ErrorOr<int> serenity_main(Main::Arguments arguments)
 {
     Options options = TRY(parse_options(arguments));
 
     auto file = TRY(Core::MappedFile::map(options.in_path));
     auto decoder = TRY(Gfx::ImageDecoder::try_create_for_raw_bytes(file->bytes()));
     if (!decoder)
         return Error::from_string_literal("Could not find decoder for input file");
 
     LoadedImage image = TRY(load_image(*decoder, options.frame_index));
 
     if (options.invert_cmyk)
         TRY(invert_cmyk(image));
 
     if (options.crop_rect.has_value())
         TRY(crop_image(image, options.crop_rect.value()));
 
     if (options.move_alpha_to_rgb)
         TRY(move_alpha_to_rgb(image));
 
     if (options.strip_alpha)
         TRY(strip_alpha(image));
 
     OwnPtr<Core::MappedFile> icc_file;
     if (!options.assign_color_profile_path.is_empty()) {
         icc_file = TRY(Core::MappedFile::map(options.assign_color_profile_path));
         image.icc_data = icc_file->bytes();
     }
 
     if (options.strip_color_profile)
         image.icc_data.clear();
 
     if (options.no_output)
         return 0;
 
     TRY(save_image(image, options.out_path, options.quality, options.webp_allowed_transforms));
     return 0;
 }
