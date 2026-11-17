// Every FFmpeg header and function the port uses (videodec.cpp, ajm_mp3.cpp, avdec.h): linked against the full build, the
// archive members the linker loads for it are the objects the game needs, the only ones vendor.ps1 copies.
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

int main(void) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* context = avcodec_alloc_context3(codec);
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_MP3);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwrContext* swr = NULL;
    AVChannelLayout layout = {0};
    char text[AV_ERROR_MAX_STRING_SIZE];
    avcodec_open2(context, codec, NULL);
    av_parser_parse2(parser, context, &packet->data, &packet->size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    avcodec_send_packet(context, packet);
    avcodec_receive_frame(context, frame);
    avcodec_flush_buffers(context);
    swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_S16, 48000, &layout, AV_SAMPLE_FMT_FLTP, 48000, 0, NULL);
    swr_init(swr);
    swr_convert_frame(swr, frame, frame);
    av_make_error_string(text, sizeof text, 0);
    av_frame_unref(frame);
    swr_free(&swr);
    av_parser_close(parser);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return 0;
}
