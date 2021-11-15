/*
 * Copyright (c) 2017 Jun Zhao
 * Copyright (c) 2017 Kaixuan Liu
 *
 * HW Acceleration API (video decoding) decode sample
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * HW-Accelerated decoding example.
 *
 * @example hw_decode.c
 * This example shows how to do HW-accelerated decoding with output
 * frames from the HW video surfaces.
 */

#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>

#include "drmprime_out.h"

static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;
static FILE *output_file = NULL;
static long frames = 0;

static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
    int err = 0;

    ctx->hw_frames_ctx = NULL;
    if ((err = av_hwdevice_ctx_create(&ctx->hw_device_ctx, type,
                                      NULL, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }

    return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt)
            return *p;
    }

    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

static int decode_write(AVCodecContext * const avctx,
                        drmprime_out_env_t * const dpo,
                        AVPacket *packet)
{
    AVFrame *frame = NULL, *sw_frame = NULL;
    uint8_t *buffer = NULL;
    int size;
    int ret = 0;
    unsigned int i;

    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        return ret;
    }

    for (;;) {
        if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) {
            fprintf(stderr, "Can not alloc frame\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            av_frame_free(&sw_frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        drmprime_out_display(dpo, frame);

        if (output_file != NULL) {
            AVFrame *tmp_frame;

            if (frame->format == hw_pix_fmt) {
                /* retrieve data from GPU to CPU */
                if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
                    fprintf(stderr, "Error transferring the data to system memory\n");
                    goto fail;
                }
                tmp_frame = sw_frame;
            } else
                tmp_frame = frame;

            size = av_image_get_buffer_size(tmp_frame->format, tmp_frame->width,
                                            tmp_frame->height, 1);
            buffer = av_malloc(size);
            if (!buffer) {
                fprintf(stderr, "Can not alloc buffer\n");
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            ret = av_image_copy_to_buffer(buffer, size,
                                          (const uint8_t * const *)tmp_frame->data,
                                          (const int *)tmp_frame->linesize, tmp_frame->format,
                                          tmp_frame->width, tmp_frame->height, 1);
            if (ret < 0) {
                fprintf(stderr, "Can not copy image to buffer\n");
                goto fail;
            }

            if ((ret = fwrite(buffer, 1, size, output_file)) < 0) {
                fprintf(stderr, "Failed to dump raw data.\n");
                goto fail;
            }
        }

        if (frames == 0 || --frames == 0)
            ret = -1;

    fail:
        av_frame_free(&frame);
        av_frame_free(&sw_frame);
        av_freep(&buffer);
        if (ret < 0)
            return ret;
    }
    return 0;
}

void usage()
{
    fprintf(stderr, "Usage: hello_drmprime [-l loop_count] [-o yuv_output_file] <input file>\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    AVFormatContext *input_ctx = NULL;
    int video_stream, ret;
    AVStream *video = NULL;
    AVCodecContext *decoder_ctx = NULL;
    AVCodec *decoder = NULL;
    AVPacket packet;
    enum AVHWDeviceType type;
    const char * in_file;
    const char * hwdev = "drm";
    int i;
    drmprime_out_env_t * dpo;
    long loop_count = 0;
    long frame_count = -1;
    const char * out_name = NULL;

    {
        char * const * a = argv + 1;
        int n = argc - 1;

        while (n-- > 0 && a[0][0] == '-') {
            const char *arg = *a++;
            char *e;

            if (strcmp(arg, "-l") == 0 || strcmp(arg, "--loop") == 0) {
                if (n == 0)
                    usage();
                loop_count = strtol(*a, &e, 0);
                if (*e != 0)
                    usage();
                --n;
                ++a;
            }
            else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--frames") == 0) {
                if (n == 0)
                    usage();
                frame_count = strtol(*a, &e, 0);
                if (*e != 0)
                    usage();
                --n;
                ++a;
            }
            else if (strcmp(arg, "-o") == 0) {
                if (n == 0)
                    usage();
                out_name = *a;
                --n;
                ++a;
            }
            else
                break;
        }

        // Last arg is input file
        if (n != 0)
            usage();

        in_file = a[0];
    }

    type = av_hwdevice_find_type_by_name(hwdev);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        fprintf(stderr, "Device type %s is not supported.\n", hwdev);
        fprintf(stderr, "Available device types:");
        while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
            fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
        fprintf(stderr, "\n");
        return -1;
    }

    dpo = drmprime_out_new();
    if (dpo == NULL) {
        fprintf(stderr, "Failed to open drmprime output\n");
        return 1;
    }

    /* open the file to dump raw data */
    if (out_name != NULL) {
        if ((output_file = fopen(out_name, "w+")) == NULL) {
            fprintf(stderr, "Failed to open output file %s: %s\n", out_name, strerror(errno));
            return -1;
        }
    }

loopy:
    /* open the input file */
    if (avformat_open_input(&input_ctx, in_file, NULL, NULL) != 0) {
        fprintf(stderr, "Cannot open input file '%s'\n", in_file);
        return -1;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        fprintf(stderr, "Cannot find input stream information.\n");
        return -1;
    }

    /* find the video stream information */
    ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        fprintf(stderr, "Cannot find a video stream in the input file\n");
        return -1;
    }
    video_stream = ret;

    if (decoder->id == AV_CODEC_ID_H264) {
        if ((decoder = avcodec_find_decoder_by_name("h264_v4l2m2m")) == NULL) {
            fprintf(stderr, "Cannot find the h264 v4l2m2m decoder\n");
            return -1;
        }
        hw_pix_fmt = AV_PIX_FMT_DRM_PRIME;
    }
    else {
        for (i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                fprintf(stderr, "Decoder %s does not support device type %s.\n",
                        decoder->name, av_hwdevice_get_type_name(type));
                return -1;
            }
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == type) {
                hw_pix_fmt = config->pix_fmt;
                break;
            }
        }
    }

    if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
        return AVERROR(ENOMEM);

    video = input_ctx->streams[video_stream];
    if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
        return -1;

    decoder_ctx->get_format  = get_hw_format;

    if (hw_decoder_init(decoder_ctx, type) < 0)
        return -1;

    decoder_ctx->thread_count = 3;

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
        fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
        return -1;
    }

    /* actual decoding and dump the raw data */
    frames = frame_count;
    while (ret >= 0) {
        if ((ret = av_read_frame(input_ctx, &packet)) < 0)
            break;

        if (video_stream == packet.stream_index)
            ret = decode_write(decoder_ctx, dpo, &packet);

        av_packet_unref(&packet);
    }

    /* flush the decoder */
    packet.data = NULL;
    packet.size = 0;
    ret = decode_write(decoder_ctx, dpo, &packet);
    av_packet_unref(&packet);

    if (output_file)
        fclose(output_file);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);

    if (--loop_count > 0)
        goto loopy;

    drmprime_out_delete(dpo);

    return 0;
}
