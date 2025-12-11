/*
 * Copyright (c) 2024-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/Track.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace Media::FFmpeg {

static inline AVCodecID ffmpeg_codec_id_from_media_codec_id(CodecID codec)
{
    switch (codec) {
    case CodecID::VP8:
        return AV_CODEC_ID_VP8;
    case CodecID::VP9:
        return AV_CODEC_ID_VP9;
    case CodecID::H261:
        return AV_CODEC_ID_H261;
    case CodecID::MPEG1:
    case CodecID::H262:
        return AV_CODEC_ID_MPEG2VIDEO;
    case CodecID::H263:
        return AV_CODEC_ID_H263;
    case CodecID::H264:
        return AV_CODEC_ID_H264;
    case CodecID::H265:
        return AV_CODEC_ID_HEVC;
    case CodecID::MP3:
        return AV_CODEC_ID_MP3;
    case CodecID::AAC:
        return AV_CODEC_ID_AAC;
    case CodecID::AV1:
        return AV_CODEC_ID_AV1;
    case CodecID::Theora:
        return AV_CODEC_ID_THEORA;
    case CodecID::Vorbis:
        return AV_CODEC_ID_VORBIS;
    case CodecID::Opus:
        return AV_CODEC_ID_OPUS;
    case CodecID::FLAC:
        return AV_CODEC_ID_FLAC;
    case CodecID::Unknown:
        return AV_CODEC_ID_NONE;
    }
    VERIFY_NOT_REACHED();
}

static inline CodecID media_codec_id_from_ffmpeg_codec_id(AVCodecID codec)
{
    switch (codec) {
    case AV_CODEC_ID_VP8:
        return CodecID::VP8;
    case AV_CODEC_ID_VP9:
        return CodecID::VP9;
    case AV_CODEC_ID_H261:
        return CodecID::H261;
    case AV_CODEC_ID_MPEG2VIDEO:
        // FIXME: This could also map to CodecID::MPEG1
        return CodecID::H262;
    case AV_CODEC_ID_H263:
        return CodecID::H263;
    case AV_CODEC_ID_H264:
        return CodecID::H264;
    case AV_CODEC_ID_HEVC:
        return CodecID::H265;
    case AV_CODEC_ID_MP3:
        return CodecID::MP3;
    case AV_CODEC_ID_AAC:
        return CodecID::AAC;
    case AV_CODEC_ID_AV1:
        return CodecID::AV1;
    case AV_CODEC_ID_THEORA:
        return CodecID::Theora;
    case AV_CODEC_ID_VORBIS:
        return CodecID::Vorbis;
    case AV_CODEC_ID_OPUS:
        return CodecID::Opus;
    case AV_CODEC_ID_FLAC:
        return CodecID::FLAC;
    default:
        return CodecID::Unknown;
    }
}

static inline AVMediaType ffmpeg_media_type_from_track_type(TrackType track_type)
{
    switch (track_type) {
    case TrackType::Video:
        return AVMediaType::AVMEDIA_TYPE_VIDEO;
    case TrackType::Audio:
        return AVMediaType::AVMEDIA_TYPE_AUDIO;
    case TrackType::Subtitles:
        return AVMediaType::AVMEDIA_TYPE_SUBTITLE;
    case TrackType::Unknown:
        return AVMediaType::AVMEDIA_TYPE_UNKNOWN;
    }
    VERIFY_NOT_REACHED();
}

static inline TrackType track_type_from_ffmpeg_media_type(AVMediaType media_type)
{
    switch (media_type) {
    case AVMediaType::AVMEDIA_TYPE_VIDEO:
        return TrackType::Video;
    case AVMediaType::AVMEDIA_TYPE_AUDIO:
        return TrackType::Audio;
    case AVMediaType::AVMEDIA_TYPE_SUBTITLE:
        return TrackType::Subtitles;
    case AVMediaType::AVMEDIA_TYPE_DATA:
    case AVMediaType::AVMEDIA_TYPE_ATTACHMENT:
    case AVMediaType::AVMEDIA_TYPE_UNKNOWN:
        return TrackType::Unknown;
    case AVMediaType::AVMEDIA_TYPE_NB:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Audio::ChannelMap> av_channel_layout_to_channel_map(AVChannelLayout const&);
ErrorOr<AVChannelLayout> channel_map_to_av_channel_layout(Audio::ChannelMap const&);

constexpr StringView av_error_code_to_string(int error)
{
    switch (error) {
    case AVERROR_BSF_NOT_FOUND:
        return "Bitstream filter not found"sv;
    case AVERROR_BUG:
    case AVERROR_BUG2:
        return "Internal bug, should not have happened"sv;
    case AVERROR_BUFFER_TOO_SMALL:
        return "Buffer too small"sv;
    case AVERROR_DECODER_NOT_FOUND:
        return "Decoder not found"sv;
    case AVERROR_DEMUXER_NOT_FOUND:
        return "Demuxer not found"sv;
    case AVERROR_ENCODER_NOT_FOUND:
        return "Encoder not found"sv;
    case AVERROR_EOF:
        return "End of file"sv;
    case AVERROR_EXIT:
        return "Immediate exit requested"sv;
    case AVERROR_EXTERNAL:
        return "Generic error in an external library"sv;
    case AVERROR_FILTER_NOT_FOUND:
        return "Filter not found"sv;
    case AVERROR_INPUT_CHANGED:
        return "Input changed"sv;
    case AVERROR_INVALIDDATA:
        return "Invalid data found when processing input"sv;
    case AVERROR_MUXER_NOT_FOUND:
        return "Muxer not found"sv;
    case AVERROR_OPTION_NOT_FOUND:
        return "Option not found"sv;
    case AVERROR_OUTPUT_CHANGED:
        return "Output changed"sv;
    case AVERROR_PATCHWELCOME:
        return "Not yet implemented in FFmpeg, patches welcome"sv;
    case AVERROR_PROTOCOL_NOT_FOUND:
        return "Protocol not found"sv;
    case AVERROR_STREAM_NOT_FOUND:
        return "Stream not found"sv;
    case AVERROR_UNKNOWN:
        return "Unknown error occurred"sv;
    case AVERROR_EXPERIMENTAL:
        return "Experimental feature"sv;
    case AVERROR_HTTP_BAD_REQUEST:
        return "Server returned 400 Bad Request"sv;
    case AVERROR_HTTP_UNAUTHORIZED:
        return "Server returned 401 Unauthorized (authorization failed)"sv;
    case AVERROR_HTTP_FORBIDDEN:
        return "Server returned 403 Forbidden (access denied)"sv;
    case AVERROR_HTTP_NOT_FOUND:
        return "Server returned 404 Not Found"sv;
    case AVERROR_HTTP_TOO_MANY_REQUESTS:
        return "Server returned 429 Too Many Requests"sv;
    case AVERROR_HTTP_OTHER_4XX:
        return "Server returned 4XX Client Error, but not one of 40{0,1,3,4}"sv;
    case AVERROR_HTTP_SERVER_ERROR:
        return "Server returned 5XX Server Error reply"sv;
    case AVERROR(E2BIG):
        return "Argument list too long"sv;
    case AVERROR(EACCES):
        return "Permission denied"sv;
    case AVERROR(EAGAIN):
        return "Resource temporarily unavailable"sv;
    case AVERROR(EBADF):
        return "Bad file descriptor"sv;
    case AVERROR(EBUSY):
        return "Device or resource busy"sv;
    case AVERROR(ECHILD):
        return "No child processes"sv;
    case AVERROR(EDEADLK):
        return "Resource deadlock avoided"sv;
    case AVERROR(EDOM):
        return "Numerical argument out of domain"sv;
    case AVERROR(EEXIST):
        return "File exists"sv;
    case AVERROR(EFAULT):
        return "Bad address"sv;
    case AVERROR(EFBIG):
        return "File too large"sv;
    case AVERROR(EILSEQ):
        return "Illegal byte sequence"sv;
    case AVERROR(EINTR):
        return "Interrupted system call"sv;
    case AVERROR(EINVAL):
        return "Invalid argument"sv;
    case AVERROR(EIO):
        return "I/O error"sv;
    case AVERROR(EISDIR):
        return "Is a directory"sv;
    case AVERROR(EMFILE):
        return "Too many open files"sv;
    case AVERROR(EMLINK):
        return "Too many links"sv;
    case AVERROR(ENAMETOOLONG):
        return "File name too long"sv;
    case AVERROR(ENFILE):
        return "Too many open files in system"sv;
    case AVERROR(ENODEV):
        return "No such device"sv;
    case AVERROR(ENOENT):
        return "No such file or directory"sv;
    case AVERROR(ENOEXEC):
        return "Exec format error"sv;
    case AVERROR(ENOLCK):
        return "No locks available"sv;
    case AVERROR(ENOMEM):
        return "Cannot allocate memory"sv;
    case AVERROR(ENOSPC):
        return "No space left on device"sv;
    case AVERROR(ENOSYS):
        return "Function not implemented"sv;
    case AVERROR(ENOTDIR):
        return "Not a directory"sv;
    case AVERROR(ENOTEMPTY):
        return "Directory not empty"sv;
    case AVERROR(ENOTTY):
        return "Inappropriate I/O control operation"sv;
    case AVERROR(ENXIO):
        return "No such device or address"sv;
    case AVERROR(EPERM):
        return "Operation not permitted"sv;
    case AVERROR(EPIPE):
        return "Broken pipe"sv;
    case AVERROR(ERANGE):
        return "Result too large"sv;
    case AVERROR(EROFS):
        return "Read-only file system"sv;
    case AVERROR(ESPIPE):
        return "Illegal seek"sv;
    case AVERROR(ESRCH):
        return "No such process"sv;
    case AVERROR(EXDEV):
        return "Cross-device link"sv;
    default:
        return "Unknown error"sv;
    }
    VERIFY_NOT_REACHED();
}

}
