/*
 * Id RoQ (.roq) File Demuxer
 * Copyright (c) 2003 The ffmpeg Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file idroq.c
 * Id RoQ format file demuxer
 * by Mike Melanson (melanson@pcisys.net)
 * for more information on the .roq file format, visit:
 *   http://www.csse.monash.edu.au/~timf/
 */

#include "avformat.h"

#define LE_16(x)  ((((uint8_t*)(x))[1] << 8) | ((uint8_t*)(x))[0])
#define LE_32(x)  ((((uint8_t*)(x))[3] << 24) | \
                   (((uint8_t*)(x))[2] << 16) | \
                   (((uint8_t*)(x))[1] << 8) | \
                    ((uint8_t*)(x))[0])

#define RoQ_MAGIC_NUMBER 0x1084
#define RoQ_CHUNK_PREAMBLE_SIZE 8
#define RoQ_AUDIO_SAMPLE_RATE 22050
#define RoQ_CHUNKS_TO_SCAN 30

#define RoQ_INFO           0x1001
#define RoQ_QUAD_CODEBOOK  0x1002
#define RoQ_QUAD_VQ        0x1011
#define RoQ_SOUND_MONO     0x1020
#define RoQ_SOUND_STEREO   0x1021

typedef struct RoqDemuxContext {

    int width;
    int height;
    int audio_channels;
    int framerate;
    int frame_pts_inc;

    int video_stream_index;
    int audio_stream_index;

    int64_t video_pts;
    unsigned int audio_frame_count;

} RoqDemuxContext;

static int roq_probe(AVProbeData *p)
{
    if ((LE_16(&p->buf[0]) != RoQ_MAGIC_NUMBER) ||
        (LE_32(&p->buf[2]) != 0xFFFFFFFF))
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int roq_read_header(AVFormatContext *s,
                           AVFormatParameters *ap)
{
    RoqDemuxContext *roq = s->priv_data;
    ByteIOContext *pb = &s->pb;
    AVStream *st;
    unsigned char preamble[RoQ_CHUNK_PREAMBLE_SIZE];
    int i;
    unsigned int chunk_size;
    unsigned int chunk_type;

    /* get the main header */
    if (get_buffer(pb, preamble, RoQ_CHUNK_PREAMBLE_SIZE) != 
        RoQ_CHUNK_PREAMBLE_SIZE)
        return AVERROR_IO;
    roq->framerate = LE_16(&preamble[6]);
    roq->frame_pts_inc = 90000 / roq->framerate;

    /* set the pts reference (1 pts = 1/90000) */
    s->pts_num = 1;
    s->pts_den = 90000;

    /* init private context parameters */
    roq->width = roq->height = roq->audio_channels = roq->video_pts = 
    roq->audio_frame_count = 0;

    /* scan the first n chunks searching for A/V parameters */
    for (i = 0; i < RoQ_CHUNKS_TO_SCAN; i++) {
        if (get_buffer(pb, preamble, RoQ_CHUNK_PREAMBLE_SIZE) != 
            RoQ_CHUNK_PREAMBLE_SIZE)
            return AVERROR_IO;

        chunk_type = LE_16(&preamble[0]);
        chunk_size = LE_32(&preamble[2]);

        switch (chunk_type) {

        case RoQ_INFO:
            /* fetch the width and height; reuse the preamble bytes */
            if (get_buffer(pb, preamble, RoQ_CHUNK_PREAMBLE_SIZE) != 
                RoQ_CHUNK_PREAMBLE_SIZE)
                return AVERROR_IO;
            roq->width = LE_16(&preamble[0]);
            roq->height = LE_16(&preamble[2]);
            break;

        case RoQ_QUAD_CODEBOOK:
        case RoQ_QUAD_VQ:
            /* ignore during this scan */
            url_fseek(pb, chunk_size, SEEK_CUR);
            break;

        case RoQ_SOUND_MONO:
            roq->audio_channels = 1;
            url_fseek(pb, chunk_size, SEEK_CUR);
            break;

        case RoQ_SOUND_STEREO:
            roq->audio_channels = 2;
            url_fseek(pb, chunk_size, SEEK_CUR);
            break;

        default:
            printf (" unknown RoQ chunk type (%04X)\n", LE_16(&preamble[0]));
            return AVERROR_INVALIDDATA;
            break;
        }

        /* if all necessary parameters have been gathered, exit early */
        if ((roq->width && roq->height) && roq->audio_channels)
            break;
    }

    /* seek back to the first chunk */
    url_fseek(pb, RoQ_CHUNK_PREAMBLE_SIZE, SEEK_SET);

    /* initialize the decoders */
    st = av_new_stream(s, 0);
    if (!st)
        return AVERROR_NOMEM;
    roq->video_stream_index = st->index;
    st->codec.codec_type = CODEC_TYPE_VIDEO;
    st->codec.codec_id = CODEC_ID_ROQ;
    st->codec.codec_tag = 0;  /* no fourcc */
    st->codec.width = roq->width;
    st->codec.height = roq->height;

    if (roq->audio_channels) {
        st = av_new_stream(s, 0);
        if (!st)
            return AVERROR_NOMEM;
        roq->audio_stream_index = st->index;
        st->codec.codec_type = CODEC_TYPE_AUDIO;
        st->codec.codec_id = CODEC_ID_ROQ_DPCM;
        st->codec.codec_tag = 0;  /* no tag */
        st->codec.channels = roq->audio_channels;
        st->codec.sample_rate = RoQ_AUDIO_SAMPLE_RATE;
        st->codec.bits_per_sample = 16;
        st->codec.bit_rate = st->codec.channels * st->codec.sample_rate *
            st->codec.bits_per_sample;
        st->codec.block_align = st->codec.channels * st->codec.bits_per_sample;
    }
printf (" video is %d x %d, audio is %d channels\n",
  roq->width, roq->height, roq->audio_channels);

    return 0;
}

static int roq_read_packet(AVFormatContext *s,
                           AVPacket *pkt)
{
    RoqDemuxContext *roq = s->priv_data;
    ByteIOContext *pb = &s->pb;
    int ret = 0;
    unsigned int chunk_size;
    unsigned int chunk_type;
    unsigned int codebook_size;
    unsigned char preamble[RoQ_CHUNK_PREAMBLE_SIZE];
    int packet_read = 0;
    offset_t codebook_offset;

    while (!packet_read) {

        if (url_feof(&s->pb))
            return -EIO;

        /* get the next chunk preamble */
        if ((ret = get_buffer(pb, preamble, RoQ_CHUNK_PREAMBLE_SIZE)) != 
            RoQ_CHUNK_PREAMBLE_SIZE)
            return -EIO;

        chunk_type = LE_16(&preamble[0]);
        chunk_size = LE_32(&preamble[2]);

        switch (chunk_type) {

        case RoQ_INFO:
            /* don't care about this chunk anymore */
            url_fseek(pb, RoQ_CHUNK_PREAMBLE_SIZE, SEEK_CUR);
            break;

        case RoQ_QUAD_CODEBOOK:
            /* packet needs to contain both this codebook and next VQ chunk */
            codebook_offset = url_ftell(pb) - RoQ_CHUNK_PREAMBLE_SIZE;
            codebook_size = chunk_size;
            url_fseek(pb, codebook_size, SEEK_CUR);
            if (get_buffer(pb, preamble, RoQ_CHUNK_PREAMBLE_SIZE) != 
                RoQ_CHUNK_PREAMBLE_SIZE)
                return -EIO;
            chunk_size = LE_32(&preamble[2]) + RoQ_CHUNK_PREAMBLE_SIZE * 2 + 
                codebook_size;

            /* rewind */
            url_fseek(pb, codebook_offset, SEEK_SET);

            /* load up the packet */
            if (av_new_packet(pkt, chunk_size))
                return -EIO;
            pkt->stream_index = roq->video_stream_index;
            pkt->pts = roq->video_pts;
            ret = get_buffer(pb, pkt->data, chunk_size);
            if (ret != chunk_size)
                ret = -EIO;

            roq->video_pts += roq->frame_pts_inc;
            packet_read = 1;
            break;

        case RoQ_SOUND_MONO:
        case RoQ_SOUND_STEREO:
        case RoQ_QUAD_VQ:
            /* load up the packet */
            if (av_new_packet(pkt, chunk_size + RoQ_CHUNK_PREAMBLE_SIZE))
                return -EIO;
            /* copy over preamble */
            memcpy(pkt->data, preamble, RoQ_CHUNK_PREAMBLE_SIZE);

            if (chunk_type == RoQ_QUAD_VQ) {
                pkt->stream_index = roq->video_stream_index;
                pkt->pts = roq->video_pts;
                roq->video_pts += roq->frame_pts_inc;
            } else {
                pkt->stream_index = roq->audio_stream_index;
                pkt->pts = roq->audio_frame_count;
                pkt->pts *= 90000;
                pkt->pts /= RoQ_AUDIO_SAMPLE_RATE;
                roq->audio_frame_count += (chunk_size / roq->audio_channels);
            }

            ret = get_buffer(pb, pkt->data, chunk_size);
            if (ret != chunk_size)
                ret = -EIO;

            packet_read = 1;
            break;

        default:
            printf ("  unknown RoQ chunk (%04X)\n", chunk_type);
            return AVERROR_INVALIDDATA;
            break;
        }
    }

    return ret;
}

static int roq_read_close(AVFormatContext *s)
{
//    RoqDemuxContext *roq = (RoqDemuxContext *)s->priv_data;

    return 0;
}

static AVInputFormat roq_iformat = {
    "RoQ",
    "Id RoQ format",
    sizeof(RoqDemuxContext),
    roq_probe,
    roq_read_header,
    roq_read_packet,
    roq_read_close,
};

int roq_init(void)
{
    av_register_input_format(&roq_iformat);
    return 0;
}
