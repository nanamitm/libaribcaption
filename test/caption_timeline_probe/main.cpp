/*
 * Copyright (C) 2021 magicxqq <xqq@xqq.im>. All rights reserved.
 *
 * This file is part of libaribcaption.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
//
// Variant of caption2srt: instead of an SRT file, dumps a CSV timeline
// (index,streamIndex,startMs,endMs,durationMs,text) with the same schema as
// mmts-dsfilter's tools/mmts_compare_dump.cpp, so the two can be diffed
// directly for subtitle-timing verification ("Tool B" - what arib-splitter's
// own decode path, ffmpeg demux + libaribcaption, would actually show).
//
// Timing basis matches caption2srt exactly: caption PTS is rebased to the
// first video packet's PTS, i.e. "ms since first video frame" - the same
// basis tools/mmts_compare_dump.cpp uses for the live filter's schedule.

#ifdef _WIN32
    #include <windows.h>
#endif

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
}

#include <cinttypes>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <deque>
#include <fstream>
#include "aribcaption/decoder.hpp"

using namespace aribcaption;

#ifdef _WIN32
class UTF8CodePage {
public:
    UTF8CodePage() : old_codepage_(GetConsoleOutputCP()) {
        SetConsoleOutputCP(CP_UTF8);
    }
    ~UTF8CodePage() {
        SetConsoleOutputCP(old_codepage_);
    }
private:
    UINT old_codepage_;
};
#endif

class CaptionConverter {
public:
    explicit CaptionConverter() : decoder_(context_) {}

    ~CaptionConverter() {
        if (format_context_) {
            avformat_close_input(&format_context_);
        }
        if (ofs_.is_open()) {
            ofs_.close();
        }
    }
public:
    bool Open(const char* input_filename, const char* output_filename) {
        ofs_.open(output_filename, std::ios::binary);
        ofs_ << "index,streamIndex,startMs,endMs,durationMs,text\n";

        InitCaptionDecoder();

        format_context_ = avformat_alloc_context();

        int ret = 0;

        // Some broadcasts (observed on an 8K + 22.2ch + 5.1ch + ARIB caption
        // source) don't announce the caption PID in the PMT from the very
        // start of the file - it only appears once the broadcaster updates
        // the PMT later (e.g. once the actual program content with
        // narration begins, after a captionless lead-in), and that can be
        // well beyond any probesize/analyzeduration short of scanning
        // several GB up front. Use a moderate probe window for the
        // initially-present streams, and rescan for a newly-appeared
        // caption stream during RunLoop() as PMT updates are parsed
        // (see RunLoop()).
        AVDictionary* options = nullptr;
        av_dict_set(&options, "probesize", "200000000", 0);       // 200MB
        av_dict_set(&options, "analyzeduration", "100000000", 0); // 100s (us)

        if ((ret = avformat_open_input(&format_context_, input_filename, nullptr, &options)) < 0) {
            fprintf(stderr, "avformat_open_input failed\n");
            av_dict_free(&options);
            return false;
        }
        av_dict_free(&options);

        if ((ret = avformat_find_stream_info(format_context_, nullptr)) < 0) {
            fprintf(stderr, "avformat_find_stream_info failed\n");
            return false;
        }

        for (size_t i = 0; i < format_context_->nb_streams; i++) {
            AVStream* stream = format_context_->streams[i];
            AVCodecParameters* codec_params = stream->codecpar;

            fprintf(stderr, "stream %zu: codec_type=%d codec_id=%d codec_tag=0x%08x channels=%d\n",
                    i, (int)codec_params->codec_type, (int)codec_params->codec_id,
                    codec_params->codec_tag, codec_params->ch_layout.nb_channels);

            if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index_ == -1) {
                video_stream_index_ = stream->index;
            }

            if (codec_params->codec_id == AV_CODEC_ID_ARIB_CAPTION && arib_caption_index_ == -1) {
                arib_caption_index_ = stream->index;
            }
        }

        if (video_stream_index_ == -1) {
            fprintf(stderr, "Video stream not found\n");
            avformat_close_input(&format_context_);
            return false;
        }

        if (arib_caption_index_ == -1) {
            fprintf(stderr, "ARIB caption stream not in the initial probe window; "
                            "will keep looking for it while reading packets\n");
        }

        return true;
    }

    // Rescans newly-discovered streams (format_context_->nb_streams can grow
    // as av_read_frame() parses later PMT versions) for the caption stream,
    // in case it wasn't announced within the initial probe window.
    void RescanForCaptionStream() {
        if (arib_caption_index_ != -1 || format_context_->nb_streams == last_scanned_nb_streams_)
            return;
        last_scanned_nb_streams_ = format_context_->nb_streams;
        for (unsigned i = 0; i < format_context_->nb_streams; i++) {
            if (format_context_->streams[i]->codecpar->codec_id == AV_CODEC_ID_ARIB_CAPTION) {
                arib_caption_index_ = format_context_->streams[i]->index;
                fprintf(stderr, "ARIB caption stream found mid-stream: index=%d\n", arib_caption_index_);
                break;
            }
        }
    }

    void RunLoop() {
        int ret = 0;
        bool first_video_found = false;
        int64_t first_video_pts = 0;

        AVPacket packet{};

        while ((ret = av_read_frame(format_context_, &packet) == 0)) {
            if (packet.stream_index == video_stream_index_ && !first_video_found) {
                first_video_found = true;
                first_video_pts = packet.pts;
            } else if (packet.stream_index == arib_caption_index_) {
                AVStream* stream = format_context_->streams[arib_caption_index_];
                packet.pts -= first_video_pts;
                av_packet_rescale_ts(&packet, stream->time_base, AVRational{1, 1000});
                ConvertCaptionPacket(&packet);
            } else if (arib_caption_index_ == -1) {
                RescanForCaptionStream();
            }
            av_packet_unref(&packet);
        }

        if (arib_caption_index_ == -1) {
            fprintf(stderr, "ARIB caption stream never appeared in this file\n");
        }

        while (!caption_queue_.empty()) {
            Caption& caption = caption_queue_.front();
            if (caption.text.empty()) {
                caption_queue_.pop_front();
                continue;
            } else if (caption.wait_duration == DURATION_INDEFINITE) {
                caption.wait_duration = 1000;
            }
            DumpToCsv(caption);
            caption_queue_.pop_front();
        }
    }
private:
    void InitCaptionDecoder() {
        context_.SetLogcatCallback([](LogLevel level, const char* message) {
            if (level == LogLevel::kError || level == LogLevel::kWarning) {
                fprintf(stderr, "%s\n", message);
            } else {
                printf("%s\n", message);
            }
        });

        decoder_.Initialize(EncodingScheme::kAuto, CaptionType::kCaption);
    }

    static void WriteCsvField(std::ofstream& out, const std::string& field) {
        out << '"';
        for (char c : field) {
            if (c == '"') {
                out << "\"\"";
            } else if (c == '\n') {
                out << "\\n";
            } else if (c == '\r') {
                continue;
            } else {
                out << c;
            }
        }
        out << '"';
    }

    void DumpToCsv(const Caption& caption) {
        const int64_t startMs = caption.pts;
        const int64_t endMs = caption.pts + caption.wait_duration;
        ofs_ << csv_index_ << ',' << arib_caption_index_ << ',' << startMs << ',' << endMs << ','
             << (endMs - startMs) << ',';
        WriteCsvField(ofs_, caption.text);
        ofs_ << '\n';
        csv_index_++;
    }

    bool ConvertCaptionPacket(AVPacket* packet) {
        DecodeResult decode_result;

        auto status = decoder_.Decode(packet->data, packet->size, packet->pts, decode_result);

        if (status == DecodeStatus::kError) {
            fprintf(stderr, "Decoder::Decode() returned error\n");
            return false;
        } else if (status == DecodeStatus::kNoCaption) {
            return true;
        }

        std::unique_ptr<Caption> caption = std::move(decode_result.caption);

        if (caption->wait_duration == DURATION_INDEFINITE) {
            printf("[%.3lfs][INDEFINITE] %s\n",
                   (double)caption->pts / 1000.0f,
                   caption->text.c_str());
        } else {
            printf("[%.3lfs][%.7lfs] %s\n",
                   (double)caption->pts / 1000.0f,
                   (double)caption->wait_duration / 1000.0f,
                   caption->text.c_str());
        }
        fflush(stdout);

        if (!caption_queue_.empty()) {
            Caption& prev = caption_queue_.back();
            if (prev.wait_duration == DURATION_INDEFINITE) {
                prev.wait_duration = caption->pts - prev.pts - 1;
            }
        }
        caption_queue_.push_back(std::move(*caption));

        while (!caption_queue_.empty() && caption_queue_.front().wait_duration != DURATION_INDEFINITE) {
            Caption& cap = caption_queue_.front();
            if (cap.text.empty()) {
                caption_queue_.pop_front();
                continue;
            }

            DumpToCsv(cap);
            caption_queue_.pop_front();
        }

        return true;
    }
   private:
    AVFormatContext* format_context_ = nullptr;
    int video_stream_index_ = -1;
    int arib_caption_index_ = -1;
    unsigned last_scanned_nb_streams_ = 0;

    Context context_;
    Decoder decoder_;

    std::deque<Caption> caption_queue_;
    std::ofstream ofs_;

    int csv_index_ = 1;
};

int main(int argc, const char* argv[]) {
#ifdef _WIN32
    UTF8CodePage enable_utf8_console;
#endif

    if (argc < 3) {
        printf("Usage: %s [MPEG-TS INPUT] [CSV OUTPUT] \n\n", argv[0]);
        return -1;
    }

    CaptionConverter converter;

    if (!converter.Open(argv[1], argv[2])) {
        fprintf(stderr, "Open input MPEG-TS failed\n");
        return -1;
    }

    converter.RunLoop();
    return 0;
}
