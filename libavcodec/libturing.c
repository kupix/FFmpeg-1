/*
 * libturing encoder
 *
 * Copyright (c) 2016 Turing Codec contributors
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <turing.h>
#include <float.h>
#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avcodec.h"
#include "internal.h"

typedef struct libturingEncodeContext {
    const AVClass *class;
    turing_encoder *encoder;
    const char *options;
} libturingEncodeContext;

static av_cold int libturing_encode_close(AVCodecContext *avctx)
{
    libturingEncodeContext *ctx = avctx->priv_data;

    if (ctx->encoder)
        turing_destroy_encoder(ctx->encoder);

    return 0;
}

static av_cold int libturing_encode_init(AVCodecContext *avctx)
{
    libturingEncodeContext *ctx = avctx->priv_data;

    char options[1024];
    char* s = options;
    char *end = &options[sizeof(options)];

    char const* argv[32];
    char const** p = argv;
    turing_encoder_settings settings;

    *p++ = s;
    *p++ = s += 1 + snprintf(s, end - s, "turing");
    *p++ = s += 1 + snprintf(s, end - s, "--input-res=%dx%d", avctx->width, avctx->height);
    *p++ = s += 1 + snprintf(s, end - s, "--frame-rate=%f", (double)avctx->time_base.den / (avctx->time_base.num * avctx->ticks_per_frame));
    *p++ = s += 1 + snprintf(s, end - s, "--frames=0");

    {
        int const bit_depth = av_pix_fmt_desc_get(avctx->pix_fmt)->comp[0].depth;
        if (bit_depth != 8 && bit_depth != 10) {
            av_log(avctx, AV_LOG_ERROR, "Encoder input must be 8- or 10-bit.\n");
            turing_destroy_encoder(ctx->encoder);
            return AVERROR_INVALIDDATA;
        }
        *p++ = s += 1 + snprintf(s, end - s, "--bit-depth=%d", bit_depth);
        *p++ = s += 1 + snprintf(s, end - s, "--internal-bit-depth=%d", bit_depth);
    }

    if (avctx->sample_aspect_ratio.num > 0 && avctx->sample_aspect_ratio.den > 0) {
        int sar_num, sar_den;

        av_reduce(&sar_num, &sar_den,
            avctx->sample_aspect_ratio.num,
            avctx->sample_aspect_ratio.den, 65535);

        *p++ = s += 1 + snprintf(s, end - s, "--sar=%d:%d", sar_num, sar_den);
    }

    if (ctx->options) {
        AVDictionary *dict = NULL;
        AVDictionaryEntry *en = NULL;

        if (!av_dict_parse_string(&dict, ctx->options, "=", ":", 0)) {
            while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX))) {
                int const illegal_option = 
                    !strcmp("input-res", en->key) ||
                    !strcmp("frame-rate", en->key) ||
                    !strcmp("f", en->key) ||
                    !strcmp("frames", en->key) ||
                    !strcmp("sar", en->key) ||
                    !strcmp("bit-depth", en->key) ||
                    !strcmp("internal-bit-depth", en->key);
                if (illegal_option) 
                    av_log(avctx, AV_LOG_WARNING, "%s=%s ignored.\n", en->key, en->value);
                else
                    *p++ = s += 1 + snprintf(s, end - s, "--%s=%s", en->key, en->value);
            }
            av_dict_free(&dict);
        }
    }

    *p++ = s += 1 + snprintf(s, end - s, "dummy-input-filename");

    settings.argv = argv;
    settings.argc = p - argv - 1;

    for (int i=0; i<settings.argc; ++i)
        av_log(avctx, AV_LOG_INFO, "arg %d: %s\n", i, settings.argv[i]);

    ctx->encoder = turing_create_encoder(settings);

    if (!ctx->encoder) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create libturing encoder.\n");
        return AVERROR_INVALIDDATA;
    }

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        turing_bitstream const *bitstream;
        bitstream = turing_encode_headers(ctx->encoder);
        if (bitstream->size <= 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to encode headers.\n");
            turing_destroy_encoder(ctx->encoder);
            return AVERROR_INVALIDDATA;
        }

        avctx->extradata_size = bitstream->size;

        avctx->extradata = av_malloc(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata) {
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate HEVC extradata %d bytes\n", avctx->extradata_size);
            turing_destroy_encoder(ctx->encoder);
            return AVERROR(ENOMEM);
        }

        memcpy(avctx->extradata, bitstream->p, bitstream->size);
    }

    return 0;
}

static int libturing_encode_frame(AVCodecContext *avctx, AVPacket *pkt, const AVFrame *pic, int *got_packet)
{
    libturingEncodeContext *ctx = avctx->priv_data;
    turing_encoder_output const *output;
    int ret = 0;

    if (pic) {
        turing_picture picture;

        picture.image[0].p = pic->data[0];
        picture.image[1].p = pic->data[1];
        picture.image[2].p = pic->data[2];
        picture.image[0].stride = pic->linesize[0];
        picture.image[1].stride = pic->linesize[1];
        picture.image[2].stride = pic->linesize[2];
    picture.pts = pic->pts;

        output = turing_encode_picture(ctx->encoder, &picture);
    } else {
        output = turing_encode_picture(ctx->encoder, 0);
    }

    if (output->bitstream.size < 0)
        return AVERROR_EXTERNAL;

    if (output->bitstream.size ==0)
        return 0;

    ret = ff_alloc_packet(pkt, output->bitstream.size);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error getting output packet.\n");
        return ret;
    }

    memcpy(pkt->data, output->bitstream.p, output->bitstream.size);

    pkt->pts = output->pts;
    pkt->dts = output->dts;
    if (output->keyframe)
        pkt->flags |= AV_PKT_FLAG_KEY;

    *got_packet = 1;
    return 0;
}

static const enum AVPixelFormat turing_csp[] = {
    AV_PIX_FMT_YUV420P10,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NONE
};

static av_cold void libturing_encode_init_csp(AVCodec *codec)
{
    codec->pix_fmts = turing_csp;
}

static const AVOption options[] = {
    { "turing-params", "configure additional turing encoder paremeters", offsetof(libturingEncodeContext, options), AV_OPT_TYPE_STRING,{ 0 }, 0, 0, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
    { NULL }
};

static const AVClass class = {
    .class_name = "libturing",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libturing_encoder = {
    .name = "libturing",
    .long_name = NULL_IF_CONFIG_SMALL("libturing HEVC"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_HEVC,
    .init = libturing_encode_init,
    .init_static_data = libturing_encode_init_csp,
    .encode2 = libturing_encode_frame,
    .close = libturing_encode_close,
    .priv_data_size = sizeof(libturingEncodeContext),
    .priv_class = &class,
    .capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
};
