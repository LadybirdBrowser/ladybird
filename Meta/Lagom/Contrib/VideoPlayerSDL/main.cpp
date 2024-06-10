/*
 * Copyright (c) 2023, Stephan Vedder <stephan.vedder@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibMain/Main.h>
#include <LibMedia/PlaybackManager.h>
#include <SDL2/SDL.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView filename = ""sv;
    Core::ArgsParser args_parser;
    args_parser.add_positional_argument(filename, "The video file to display.", "filename", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    if (filename.is_empty()) {
        warnln("No filename given");
        return 1;
    }

    auto file = TRY(Core::File::open(filename, Core::File::OpenMode::Read));
    auto mapped_file = TRY(Core::MappedFile::map_from_file(move(file), filename));
    auto load_file_result = Media::PlaybackManager::from_mapped_file(move(mapped_file));
    if (load_file_result.is_error()) {
        warnln("Failed to decode file {}", filename);
        return 1;
    }

    SDL_Texture* texture = NULL;
    SDL_Renderer* renderer = NULL;
    SDL_Window* window = NULL;

    auto playback_manager = load_file_result.release_value();
    playback_manager->on_video_frame = [&texture, &renderer, &window](RefPtr<Gfx::Bitmap> frame) {
        // Delete texture if it doesn't match the frame size and resize window
        if (texture != NULL) {
            int width, height;
            SDL_QueryTexture(texture, NULL, NULL, &width, &height);
            if (width != frame->width() || height != frame->height()) {
                SDL_DestroyTexture(texture);
                texture = NULL;
                SDL_SetWindowSize(window, frame->width(), frame->height());
            }
        }

        // Create texture if it doesn't exist
        if (texture == NULL) {
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                frame->width(),
                frame->height());
        }

        u8* pixels = frame->scanline_u8(0);
        int result = SDL_UpdateTexture(texture, NULL, pixels, frame->pitch());
        if (result != 0) {
            warnln("Failed to update texture: {} from pixels {}", SDL_GetError(), pixels);
        }
    };

    playback_manager->on_decoder_error = [](auto error) {
        warnln("Decoder error: {}", error.description());
    };

    playback_manager->on_fatal_playback_error = [](auto) {
        warnln("Fatal decoder error");
        exit(1);
    };

    SDL_Init(SDL_INIT_VIDEO);

    auto& video_data = playback_manager->selected_video_track().video_data();
    window = SDL_CreateWindow(
        "VideoPlayer",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        video_data.pixel_width,
        video_data.pixel_height,
        0);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);

    playback_manager->resume_playback();

    Core::EventLoop event_loop;
    while (playback_manager->is_playing()) {
        SDL_Event event;
        if (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                break;
        }

        event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
