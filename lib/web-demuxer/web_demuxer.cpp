#include <string>
#include <sstream>
#include <cstdint>
#include <vector>
#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

using namespace emscripten;

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/codec_id.h>
#include "video_codec_string.h"
#include "audio_codec_string.h"
};

typedef struct Tag
{
    std::string key;
    std::string value;
} Tag;

typedef struct WebAVStream
{
    int index;
    int id;
    /** Codec Info from codecpar */
    int codec_type;
    std::string codec_name;
    std::string codec_string;
    std::string profile;
    std::string pix_fmt;
    int level;
    int width;
    int height;
    int channels;
    int sample_rate;
    std::string sample_fmt;
    int64_t bit_rate;
    int extradata_size;
    std::vector<uint8_t> extradata;
    val get_extradata() const{
        return val(typed_memory_view(extradata.size(), extradata.data()));
    }
    /** Other Info */
    std::string r_frame_rate;
    std::string avg_frame_rate;
    std::string sample_aspect_ratio;
    std::string display_aspect_ratio;
    double start_time;
    double duration;
    int64_t nb_frames;
    std::vector<Tag> tags;

} WebAVStream;

typedef struct WebAVPacket
{
    int keyframe;
    double timestamp;
    double duration;
    int size;
    std::vector<uint8_t> data;
    val get_data() const{
        return val(typed_memory_view(data.size(), data.data()));
    }
} WebAVPacket;

typedef struct WebAVStreamList
{
    int size;
    std::vector<WebAVStream> streams;
} WebAVStreamList;

typedef struct WebAVPacketList
{
    int size;
    std::vector<WebAVPacket> packets;
} WebAVPacketList;

EM_ASYNC_JS(int, send_av_packet, (int msg_id, WebAVPacket *av_packet), {
    try
    {
        await sendAVPacket(msg_id, av_packet);
    }
    catch (e)
    {
        return 0;
    }

    return 1;
});

std::string gen_rational_str(AVRational rational, char sep)
{
    std::ostringstream oss;
    oss << rational.num << sep << rational.den;
    return oss.str();
}

void gen_web_packet(WebAVPacket &web_packet, AVPacket *packet, AVStream *stream)
{
    double packet_timestamp = packet->pts * av_q2d(stream->time_base);

    web_packet.keyframe = packet->flags & AV_PKT_FLAG_KEY;
    web_packet.timestamp = packet_timestamp > 0 ? packet_timestamp : 0;
    web_packet.duration = packet->duration * av_q2d(stream->time_base);
    web_packet.size = packet->size;
    if (packet->size > 0)
    {
        web_packet.data = std::vector<uint8_t>(packet->data, packet->data + packet->size);
    }
    else
    {
        web_packet.data = std::vector<uint8_t>();
    }
}

void gen_web_stream(WebAVStream &web_stream, AVStream *stream, AVFormatContext *fmt_ctx)
{
    web_stream.index = stream->index;
    web_stream.id = stream->id;

    // codecpar info
    AVCodecParameters *par = stream->codecpar;
    web_stream.codec_type = (int)par->codec_type;
    web_stream.codec_name = avcodec_descriptor_get(par->codec_id)->name;

    char codec_string[40];

    if (par->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        set_video_codec_string(codec_string, sizeof(codec_string), par, &stream->avg_frame_rate);
    }
    else if (par->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        set_audio_codec_string(codec_string, sizeof(codec_string), par);
    }
    else
    {
        strcpy(codec_string, "undf");
    }

    web_stream.codec_string = codec_string;
    web_stream.profile = avcodec_profile_name(par->codec_id, par->profile);
    web_stream.pix_fmt = av_get_pix_fmt_name((AVPixelFormat)par->format);
    web_stream.level = par->level;
    web_stream.width = par->width;
    web_stream.height = par->height;
    web_stream.channels = par->ch_layout.nb_channels;
    web_stream.sample_rate = par->sample_rate;
    web_stream.sample_fmt = av_get_sample_fmt_name((AVSampleFormat)par->format);
    web_stream.bit_rate = par->bit_rate;
    web_stream.extradata_size = par->extradata_size;
    if (par->extradata_size > 0)
    {
        web_stream.extradata = std::vector<uint8_t>(par->extradata, par->extradata + par->extradata_size);
    }
    else
    {
        web_stream.extradata = std::vector<uint8_t>();
    }

    // other stream info
    web_stream.start_time = stream->start_time * av_q2d(stream->time_base);
    web_stream.duration = stream->duration > 0 ? stream->duration * av_q2d(stream->time_base) : fmt_ctx->duration * av_q2d(AV_TIME_BASE_Q); // TODO: some file type can not get stream duration

    int64_t nb_frames = stream->nb_frames;

    // vp8 codec does not have nb_frames
    if (nb_frames == 0)
    {
        nb_frames = (fmt_ctx->duration * (double)stream->avg_frame_rate.num) / ((double)stream->avg_frame_rate.den * AV_TIME_BASE);
    }
    web_stream.nb_frames = nb_frames;
    web_stream.r_frame_rate = gen_rational_str(stream->r_frame_rate, '/');
    web_stream.avg_frame_rate = gen_rational_str(stream->avg_frame_rate, '/');
    AVRational sar, dar;
    sar = av_guess_sample_aspect_ratio(fmt_ctx, stream, NULL);

    if (sar.num)
    {
        av_reduce(&dar.num, &dar.den, par->width * sar.num, par->height * sar.den, 1024 * 1024);
        web_stream.sample_aspect_ratio = gen_rational_str(sar, ':');
        web_stream.display_aspect_ratio = gen_rational_str(dar, ':');
    }
    else
    {
        web_stream.sample_aspect_ratio = std::string("N/A");
        web_stream.display_aspect_ratio = std::string("N/A");
    }

    AVDictionaryEntry *tag = NULL;

    while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
    {
        Tag t = {
            .key = tag->key,
            .value = tag->value,
        };
        web_stream.tags.push_back(t);
    }
}

void cleanup(AVFormatContext *fmt_ctx, AVPacket *packet = NULL)
{
    if (fmt_ctx)
    {
        avformat_close_input(&fmt_ctx);
    }

    if (packet)
    {
        av_packet_unref(packet);
        av_packet_free(&packet);
    }
}

WebAVStream get_av_stream(std::string filename, int type, int wanted_stream_nb)
{
    // av_log_set_level(AV_LOG_QUIET);

    AVFormatContext *fmt_ctx = NULL;
    int ret;

    if ((ret = avformat_open_input(&fmt_ctx, filename.c_str(), NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot open input file");
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot find stream information");
    }

    int stream_index = av_find_best_stream(fmt_ctx, (AVMediaType)type, wanted_stream_nb, -1, NULL, 0);

    if (stream_index < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find wanted stream in the input file\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot find wanted stream in the input file");
    }

    AVStream *stream = fmt_ctx->streams[stream_index];
    WebAVStream web_stream;

    gen_web_stream(web_stream, stream, fmt_ctx);

    avformat_close_input(&fmt_ctx);

    return web_stream;
}

WebAVStreamList get_av_streams(std::string filename)
{
    AVFormatContext *fmt_ctx = NULL;
    int ret;

    if ((ret = avformat_open_input(&fmt_ctx, filename.c_str(), NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot open input file");
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot find stream information");
    }

    int num_streams = fmt_ctx->nb_streams;

    WebAVStreamList stream_list = {
        .size = num_streams,
        .streams = std::vector<WebAVStream>(num_streams),
    };

    for (int stream_index = 0; stream_index < num_streams; stream_index++)
    {
        AVStream *stream = fmt_ctx->streams[stream_index];

        gen_web_stream(stream_list.streams[stream_index], stream, fmt_ctx);
    }

    avformat_close_input(&fmt_ctx);

    return stream_list;
}

// TODO: seek type & default value
WebAVPacket get_av_packet(std::string filename, double timestamp, int type, int wanted_stream_nb)
{
    AVFormatContext *fmt_ctx = NULL;
    int ret;

    if ((ret = avformat_open_input(&fmt_ctx, filename.c_str(), NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot open input file");
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot find stream information");
    }

    int stream_index = av_find_best_stream(fmt_ctx, (AVMediaType)type, wanted_stream_nb, -1, NULL, 0);

    if (stream_index < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find wanted stream in the input file\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot find wanted stream in the input file");
    }

    AVPacket *packet = NULL;
    packet = av_packet_alloc();

    if (!packet)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot allocate packet\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot allocate packet");
    }

    int64_t int64_timestamp = (int64_t)(timestamp * AV_TIME_BASE);
    int64_t seek_time_stamp = av_rescale_q(int64_timestamp, AV_TIME_BASE_Q, fmt_ctx->streams[stream_index]->time_base);

    if ((ret = av_seek_frame(fmt_ctx, stream_index, seek_time_stamp, AVSEEK_FLAG_BACKWARD)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot seek to the specified timestamp\n");
        avformat_close_input(&fmt_ctx);
        av_packet_unref(packet);
        av_packet_free(&packet);
        throw std::runtime_error("Cannot seek to the specified timestamp");
    }

    while (av_read_frame(fmt_ctx, packet) >= 0)
    {
        if (packet->stream_index == stream_index)
        {
            break;
        }
        av_packet_unref(packet);
    }

    if (!packet)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to get av packet at timestamp\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Failed to get av packet at timestamp");
    }

    WebAVPacket web_packet;

    gen_web_packet(web_packet, packet, fmt_ctx->streams[stream_index]);

    avformat_close_input(&fmt_ctx);
    av_packet_unref(packet);
    av_packet_free(&packet);

    return web_packet;
}

WebAVPacketList get_av_packets(std::string filename, double timestamp)
{
    AVFormatContext *fmt_ctx = NULL;
    int ret;

    if ((ret = avformat_open_input(&fmt_ctx, filename.c_str(), NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot open input file");
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot find stream information");
    }

    int num_streams = fmt_ctx->nb_streams;
    int num_packets = num_streams;
    WebAVPacketList web_packet_list = {
        .size = num_packets,
        .packets = std::vector<WebAVPacket>(num_packets),
    };

    AVPacket *packet = NULL;
    packet = av_packet_alloc();

    if (!packet)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot allocate packet\n");
        avformat_close_input(&fmt_ctx);
        throw std::runtime_error("Cannot allocate packet");
    }

    for (int stream_index = 0; stream_index < num_streams; stream_index++)
    {
        int64_t int64_timestamp = (int64_t)(timestamp * AV_TIME_BASE);
        int64_t seek_time_stamp = av_rescale_q(int64_timestamp, AV_TIME_BASE_Q, fmt_ctx->streams[stream_index]->time_base);

        if ((ret = av_seek_frame(fmt_ctx, stream_index, seek_time_stamp, AVSEEK_FLAG_BACKWARD)) < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot seek to the specified timestamp\n");
            throw std::runtime_error("Cannot seek to the specified timestamp");
        }

        while (av_read_frame(fmt_ctx, packet) >= 0)
        {
            if (packet->stream_index == stream_index)
            {
                break;
            }
            av_packet_unref(packet);
        }

        if (!packet)
        {
            av_log(NULL, AV_LOG_ERROR, "Failed to get av packet at timestamp\n");
            throw std::runtime_error("Failed to get av packet at timestamp");
        }

        gen_web_packet(web_packet_list.packets[stream_index], packet, fmt_ctx->streams[stream_index]);
    }

    av_packet_unref(packet);
    av_packet_free(&packet);
    avformat_close_input(&fmt_ctx);

    return web_packet_list;
}

int read_av_packet(int msg_id, std::string filename, double start, double end, int type, int wanted_stream_nb)
{
    AVFormatContext *fmt_ctx = NULL;
    int ret;

    if ((ret = avformat_open_input(&fmt_ctx, filename.c_str(), NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        avformat_close_input(&fmt_ctx);
        return 0;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        avformat_close_input(&fmt_ctx);
        return 0;
    }

    int stream_index = av_find_best_stream(fmt_ctx, (AVMediaType)type, wanted_stream_nb, -1, NULL, 0);

    if (stream_index < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find wanted stream in the input file\n");
        avformat_close_input(&fmt_ctx);
        return 0;
    }

    AVPacket *packet = NULL;
    packet = av_packet_alloc();

    if (!packet)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot allocate packet\n");
        avformat_close_input(&fmt_ctx);
        return 0;
    }

    if (start > 0)
    {
        int64_t start_timestamp = (int64_t)(start * AV_TIME_BASE);
        int64_t rescaled_start_time_stamp = av_rescale_q(start_timestamp, AV_TIME_BASE_Q, fmt_ctx->streams[stream_index]->time_base);

        if ((ret = av_seek_frame(fmt_ctx, stream_index, rescaled_start_time_stamp, AVSEEK_FLAG_BACKWARD)) < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Cannot seek to the specified timestamp\n");
            avformat_close_input(&fmt_ctx);
            av_packet_unref(packet);
            av_packet_free(&packet);
            return 0;
        }
    }

    while (av_read_frame(fmt_ctx, packet) >= 0)
    {
        if (packet->stream_index == stream_index)
        {
            if (end > 0)
            {
                int64_t end_timestamp = (int64_t)(end * AV_TIME_BASE);
                int64_t rescaled_end_timestamp = av_rescale_q(end_timestamp, AV_TIME_BASE_Q, fmt_ctx->streams[stream_index]->time_base);

                if (packet->pts > rescaled_end_timestamp)
                {
                    break;
                }
            }

            if (packet)
            {
                WebAVPacket web_packet;

                gen_web_packet(web_packet, packet, fmt_ctx->streams[stream_index]);

                // call js method to send packet
                int send_ret = send_av_packet(msg_id, &web_packet);

                if (send_ret == 0)
                {
                    break;
                }
            }
            else
            {
                av_log(NULL, AV_LOG_ERROR, "Failed to get av packet at timestamp\n");
                break;
            }
        }
        av_packet_unref(packet);
    }

    // call js method to end send packet
    send_av_packet(msg_id, NULL);

    avformat_close_input(&fmt_ctx);
    av_packet_unref(packet);
    av_packet_free(&packet);

    return 1;
}

EMSCRIPTEN_BINDINGS(web_demuxer)
{
    value_object<Tag>("Tag")
        .field("key", &Tag::key)
        .field("value", &Tag::value);
    
     class_<WebAVStream>("WebAVStream")
        .constructor<>()
        .property("index", &WebAVStream::index)
        .property("id", &WebAVStream::id)
        .property("codec_type", &WebAVStream::codec_type)
        .property("codec_name", &WebAVStream::codec_name)
        .property("codec_string", &WebAVStream::codec_string)
        .property("profile", &WebAVStream::profile)
        .property("pix_fmt", &WebAVStream::pix_fmt)
        .property("level", &WebAVStream::level)
        .property("width", &WebAVStream::width)
        .property("height", &WebAVStream::height)
        .property("channels", &WebAVStream::channels)
        .property("sample_rate", &WebAVStream::sample_rate)
        .property("sample_fmt", &WebAVStream::sample_fmt)
        // .property("bit_rate", &WebAVStream::bit_rate)
        .property("extradata_size", &WebAVStream::extradata_size)
        .property("extradata", &WebAVStream::get_extradata) // export extradata as typed_memory_view
        .property("r_frame_rate", &WebAVStream::r_frame_rate)
        .property("avg_frame_rate", &WebAVStream::avg_frame_rate)
        .property("sample_aspect_ratio", &WebAVStream::sample_aspect_ratio)
        .property("display_aspect_ratio", &WebAVStream::display_aspect_ratio)
        .property("start_time", &WebAVStream::start_time)
        .property("duration", &WebAVStream::duration)
        // .property("nb_frames", &WebAVStream::nb_frames)
        .property("tags", &WebAVStream::tags);

    value_object<WebAVStreamList>("WebAVStreamList")
        .field("size", &WebAVStreamList::size)
        .field("streams", &WebAVStreamList::streams);


    class_<WebAVPacket>("WebAVPacket")
        .constructor<>()
        .property("keyframe", &WebAVPacket::keyframe)
        .property("timestamp", &WebAVPacket::timestamp)
        .property("duration", &WebAVPacket::duration)
        .property("size", &WebAVPacket::size)
        .property("data", &WebAVPacket::get_data); // export data as typed_memory_view

    value_object<WebAVPacketList>("WebAVPacketList")
        .field("size", &WebAVPacketList::size)
        .field("packets", &WebAVPacketList::packets);

    function("get_av_stream", &get_av_stream); // TODO: return value policy
    function("get_av_streams", &get_av_streams);
    function("get_av_packet", &get_av_packet);
    function("get_av_packets", &get_av_packets);
    function("read_av_packet", &read_av_packet);

    register_vector<uint8_t>("vector<uint8_t>");
    register_vector<Tag>("vector<Tag>");
    register_vector<WebAVStream>("vector<WebAVStream>");
    register_vector<WebAVPacket>("vector<WebAVPacket>");
}