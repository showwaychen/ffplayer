﻿// 包含头文件
#include <pthread.h>
#include "pktqueue.h"
#include "ffrender.h"
#include "ffplayer.h"
#include "log.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
}

// 内部常量定义
#define avcodec_decode_video avcodec_decode_video2
#define avcodec_decode_audio avcodec_decode_audio4
#define avcodec_open         avcodec_open2

// 内部类型定义
typedef struct
{
    // format
    AVFormatContext *pAVFormatContext;

    // audio
    AVCodecContext  *pAudioCodecContext;
    int              iAudioStreamIndex;
    double           dAudioTimeBase;

    // video
    AVCodecContext  *pVideoCodecContext;
    int              iVideoStreamIndex;
    double           dVideoTimeBase;

    // packet queue
    void            *hPacketQueue;

    // render
    void            *hRender;
    RECT             rtVideo;

    // auto slow down
    int              nAutoSlowDown;
    int              nMinPlaySpeed;
    int              nMaxPlaySpeed;

    // thread
    #define PS_D_PAUSE    (1 << 0)  // demux pause
    #define PS_A_PAUSE    (1 << 1)  // audio decoding pause
    #define PS_V_PAUSE    (1 << 2)  // video decoding pause
    #define PS_R_PAUSE    (1 << 3)  // rendering pause
    #define PS_A_SEEK     (1 << 4)  // seek audio
    #define PS_V_SEEK     (1 << 5)  // seek video
    #define PS_CLOSE      (1 << 6)  // close player
    int              nPlayerStatus;
    int64_t          nSeekToNewPTS;

    pthread_t        hAVDemuxThread;
    pthread_t        hADecodeThread;
    pthread_t        hVDecodeThread;
} PLAYER;

// 内部函数实现
static void* AVDemuxThreadProc(void *param)
{
    PLAYER   *player = (PLAYER*)param;
    AVPacket *packet = NULL;
    int       retv   = 0;

    while (!(player->nPlayerStatus & PS_CLOSE))
    {
        //++ when demux pause ++//
        if (player->nPlayerStatus & PS_D_PAUSE) {
            player->nPlayerStatus |= (PS_D_PAUSE << 16);
            usleep(20*1000); continue;
        }
        //-- when demux pause --//

        if (!pktqueue_write_request(player->hPacketQueue, &packet)) { usleep(20*1000); continue; }

        retv = av_read_frame(player->pAVFormatContext, packet);
        //++ play completed ++//
        if (retv < 0) {
            player->nPlayerStatus |= PS_D_PAUSE; continue;
        }
        //-- play completed --//

        // audio
        if (packet->stream_index == player->iAudioStreamIndex)
        {
            pktqueue_write_post_a(player->hPacketQueue);
        }

        // video
        if (packet->stream_index == player->iVideoStreamIndex)
        {
            pktqueue_write_post_v(player->hPacketQueue);
        }

        if (  packet->stream_index != player->iAudioStreamIndex
           && packet->stream_index != player->iVideoStreamIndex )
        {
            av_packet_unref(packet); // free packet
            pktqueue_write_cancel(player->hPacketQueue);
        }
    }

    return NULL;
}

static void* AudioDecodeThreadProc(void *param)
{
    PLAYER   *player = (PLAYER*)param;
    AVPacket *packet = NULL;
    AVFrame  *aframe = NULL;

    aframe = av_frame_alloc();
    if (!aframe) return NULL;

    while (!(player->nPlayerStatus & PS_CLOSE))
    {
        //++ when audio decode pause ++//
        if (player->nPlayerStatus & PS_A_PAUSE) {
            player->nPlayerStatus |= (PS_A_PAUSE << 16);
            usleep(20*1000); continue;
        }
        //-- when audio decode pause --//

        //++ for seek operation
        if (player->nPlayerStatus & (PS_A_SEEK << 16)) {
            usleep(20*1000); continue;
        }
        //++ for seek operation

        // read packet
        if (!pktqueue_read_request_a(player->hPacketQueue, &packet)) { usleep(20*1000); continue; }

        //++ decode audio packet ++//
        if (player->iAudioStreamIndex != -1) {
            while (packet->size > 0) {
                int consumed = 0;
                int gotaudio = 0;

                consumed = avcodec_decode_audio(player->pAudioCodecContext, aframe, &gotaudio, packet);
                if (consumed < 0) {
                    log_printf(TEXT("an error occurred during decoding audio.\n"));
                    break;
                }

                if (gotaudio) {
                    aframe->pts = (int64_t)(av_frame_get_best_effort_timestamp(aframe) * player->dAudioTimeBase);
                    //++ for seek operation
                    if ((player->nPlayerStatus & PS_A_SEEK)) {
                        if (player->nSeekToNewPTS - aframe->pts < 50) {
                            player->nPlayerStatus |= (PS_A_SEEK << 16);
                        }
                    }
                    //-- for seek operation
                    else render_audio(player->hRender, aframe);
                }

                packet->data += consumed;
                packet->size -= consumed;
            }
        }
        //-- decode audio packet --//

        // free packet
        av_packet_unref(packet);

        pktqueue_read_post_a(player->hPacketQueue);
    }

    av_frame_free(&aframe);
    return NULL;
}

static void* VideoDecodeThreadProc(void *param)
{
    PLAYER   *player = (PLAYER*)param;
    AVPacket *packet = NULL;
    AVFrame  *vframe = NULL;

    vframe = av_frame_alloc();
    if (!vframe) return NULL;
    else vframe->pts = -1;

    while (!(player->nPlayerStatus & PS_CLOSE))
    {
        //++ when video decode pause ++//
        if (player->nPlayerStatus & PS_V_PAUSE) {
            player->nPlayerStatus |= (PS_V_PAUSE << 16);
            usleep(20*1000); continue;
        }
        //-- when video decode pause --//

        //++ for seek operation
        if (player->nPlayerStatus & (PS_V_SEEK << 16)) {
            usleep(20*1000); continue;
        }
        //++ for seek operation

        // read packet
        if (!pktqueue_read_request_v(player->hPacketQueue, &packet)) {
            if ((player->nPlayerStatus & PS_V_SEEK)) {
                player->nPlayerStatus |= (PS_V_SEEK << 16);
            }
            else render_video(player->hRender, vframe);
            usleep(20*1000); continue;
        }

        //++ decode video packet ++//
        if (player->iVideoStreamIndex != -1) {
            //+ for auto slow down
            if (player->nAutoSlowDown) {
                int play_speed = 100;
                int slow_flag  = 0;
                render_getparam(player->hRender, PARAM_PLAY_SPEED, &play_speed);
                slow_flag = render_slowflag(player->hRender);
                if (slow_flag > 0 && play_speed <= player->nMinPlaySpeed) slow_flag = 0;
                if (slow_flag < 0 && play_speed >= player->nMaxPlaySpeed) slow_flag = 0;
                play_speed -= slow_flag;
                render_setparam(player->hRender, PARAM_PLAY_SPEED, &play_speed);
//              log_printf(TEXT("play speed: %d\n"), play_speed);
            }
            //- for auto slow down

            while (packet->size > 0) {
                int consumed = 0;
                int gotvideo = 0;

                consumed = avcodec_decode_video(player->pVideoCodecContext, vframe, &gotvideo, packet);
                if (consumed < 0) {
                    log_printf(TEXT("an error occurred during decoding video.\n"));
                    break;
                }

                if (gotvideo) {
                    vframe->pts = (int64_t)(av_frame_get_best_effort_timestamp(vframe) * player->dVideoTimeBase);
                    //++ for seek operation
                    if ((player->nPlayerStatus & PS_V_SEEK)) {
                        if (player->nSeekToNewPTS - vframe->pts < 50) {
                            player->nPlayerStatus |= (PS_V_SEEK << 16);
                        }
                    }
                    //-- for seek operation
                    else render_video(player->hRender, vframe);
                }

                packet->data += consumed;
                packet->size -= consumed;
            }
        }
        //-- decode video packet --//

        // free packet
        av_packet_unref(packet);

        pktqueue_read_post_v(player->hPacketQueue);
    }

    av_frame_free(&vframe);
    return NULL;
}

// 函数实现
void* player_open(char *file, void *extra)
{
    PLAYER        *player   = NULL;
    AVCodec       *decoder  = NULL;
    AVRational     vrate    = { 30, 1 };
    int            vformat  = 0;
    int            width    = 0;
    int            height   = 0;
    uint64_t       alayout  = 0;
    int            aformat  = 0;
    int            arate    = 0;
    uint32_t       i        = 0;

    // init log
    log_init(TEXT("DEBUGER"));

    // av register all
    av_register_all();

    // alloc player context
    player = (PLAYER*)malloc(sizeof(PLAYER));
    memset(player, 0, sizeof(PLAYER));

    // create packet queue
    player->hPacketQueue = pktqueue_create(0);

    // open input file
    if (avformat_open_input(&player->pAVFormatContext, file, NULL, 0) != 0) {
        goto error_handler;
    }

    // find stream info
    if (avformat_find_stream_info(player->pAVFormatContext, NULL) < 0) {
        goto error_handler;
    }

    // get video & audio codec context
    player->iAudioStreamIndex = -1;
    player->iVideoStreamIndex = -1;
    for (i=0; i<player->pAVFormatContext->nb_streams; i++)
    {
        switch (player->pAVFormatContext->streams[i]->codec->codec_type)
        {
        case AVMEDIA_TYPE_AUDIO:
            player->iAudioStreamIndex  = i;
            player->pAudioCodecContext = player->pAVFormatContext->streams[i]->codec;
            player->dAudioTimeBase     = av_q2d(player->pAVFormatContext->streams[i]->time_base) * 1000;
            break;

        case AVMEDIA_TYPE_VIDEO:
            player->iVideoStreamIndex  = i;
            player->pVideoCodecContext = player->pAVFormatContext->streams[i]->codec;
            player->dVideoTimeBase     = av_q2d(player->pAVFormatContext->streams[i]->time_base) * 1000;
            vrate = player->pAVFormatContext->streams[i]->r_frame_rate;
            if (vrate.num / vrate.den > 100) {
                vrate.num = 30;
                vrate.den = 1;
            }
            break;
        }
    }

    // open audio codec
    if (player->iAudioStreamIndex != -1)
    {
        decoder = avcodec_find_decoder(player->pAudioCodecContext->codec_id);
        if (!decoder || avcodec_open(player->pAudioCodecContext, decoder, NULL) < 0)
        {
            log_printf(TEXT("failed to find or open decoder for audio !\n"));
            player->iAudioStreamIndex = -1;
        }
    }

    // open video codec
    if (player->iVideoStreamIndex != -1)
    {
        decoder = avcodec_find_decoder(player->pVideoCodecContext->codec_id);
        if (!decoder || avcodec_open(player->pVideoCodecContext, decoder, NULL) < 0)
        {
            log_printf(TEXT("failed to find or open decoder for video !\n"));
            player->iVideoStreamIndex = -1;
        }
    }

    // for audio
    if (player->iAudioStreamIndex != -1)
    {
        alayout = player->pAudioCodecContext->channel_layout;
        //++ fix audio channel layout issue
        if (alayout == 0) {
            alayout = av_get_default_channel_layout(player->pAudioCodecContext->channels);
        }
        //-- fix audio channel layout issue
        aformat = player->pAudioCodecContext->sample_fmt;
        arate   = player->pAudioCodecContext->sample_rate;
    }

    // for video
    if (player->iVideoStreamIndex != -1)
    {
        vformat = player->pVideoCodecContext->pix_fmt;
        width   = player->pVideoCodecContext->width;
        height  = player->pVideoCodecContext->height;
    }

    // open render
    player->hRender = render_open(extra, vrate, vformat, width, height,
        arate, (AVSampleFormat)aformat, alayout);
    if (player->iVideoStreamIndex == -1) {
        int effect = VISUAL_EFFECT_WAVEFORM;
        render_setparam(player->hRender, PARAM_VISUAL_EFFECT, &effect);
    }

    // make sure player status paused
    player->nPlayerStatus = (PS_D_PAUSE|PS_A_PAUSE|PS_V_PAUSE);
    pthread_create(&player->hAVDemuxThread, NULL, AVDemuxThreadProc    , player);
    pthread_create(&player->hADecodeThread, NULL, AudioDecodeThreadProc, player);
    pthread_create(&player->hVDecodeThread, NULL, VideoDecodeThreadProc, player);

    // for auto slow down
    player->nAutoSlowDown = 0;
    player->nMinPlaySpeed = 50;
    player->nMaxPlaySpeed = 100;
    return player;

error_handler:
    player_close(player);
    return NULL;
}

void player_close(void *hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    player->nPlayerStatus = PS_CLOSE;
    if (player->hRender) {
        render_reset(player->hRender);
    }

    // wait audio/video demuxing thread exit
    pthread_join(player->hAVDemuxThread, NULL);

    // wait audio decoding thread exit
    pthread_join(player->hADecodeThread, NULL);

    // wait video decoding thread exit
    pthread_join(player->hVDecodeThread, NULL);

    // destroy packet queue
    pktqueue_destroy(player->hPacketQueue);

    if (player->hRender           ) render_close (player->hRender);
    if (player->pVideoCodecContext) avcodec_close(player->pVideoCodecContext);
    if (player->pAudioCodecContext) avcodec_close(player->pAudioCodecContext);
    if (player->pAVFormatContext  ) avformat_close_input(&player->pAVFormatContext);

    free(player);

    // close log
    log_done();
}

void player_play(void *hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;
    player->nPlayerStatus = 0;
    render_start(player->hRender);
}

void player_pause(void *hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;
    player->nPlayerStatus |= PS_R_PAUSE;
    render_pause(player->hRender);
}

void player_setrect(void *hplayer, int type, int x, int y, int w, int h)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    //++ if set visual effect rect
    if (type == 1) {
        render_setrect(player->hRender, type, x, y, w, h);
        return;
    }
    //-- if set visual effect rect

    int vw, vh;
    int rw, rh;
    int mode;
    player_getparam(hplayer, PARAM_VIDEO_WIDTH , &vw  );
    player_getparam(hplayer, PARAM_VIDEO_HEIGHT, &vh  );
    player_getparam(hplayer, PARAM_VIDEO_MODE  , &mode);
    if (!vw || !vh) return;

    player->rtVideo.left   = x;
    player->rtVideo.top    = y;
    player->rtVideo.right  = x + w;
    player->rtVideo.bottom = y + h;

    switch (mode)
    {
    case VIDEO_MODE_LETTERBOX:
        if (w * vh < h * vw) { rw = w; rh = rw * vh / vw; }
        else                 { rh = h; rw = rh * vw / vh; }
        break;

    case VIDEO_MODE_STRETCHED: rw = w; rh = h; break;
    }

    if (rw <= 0) rw = 1;
    if (rh <= 0) rh = 1;
    render_setrect(player->hRender, type, x + (w - rw) / 2, y + (h - rh) / 2, rw, rh);
}

void player_seek(void *hplayer, LONGLONG ms)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    // render seek start
    #define PAUSE_REQ ((PS_D_PAUSE|PS_A_PAUSE|PS_V_PAUSE) << 0 )
    #define PAUSE_ACK ((PS_D_PAUSE|PS_A_PAUSE|PS_V_PAUSE) << 16)
    player->nPlayerStatus |= PAUSE_REQ;
    player->nPlayerStatus &=~PAUSE_ACK;

    // wait for demuxing, audio decoding & video decoding threads paused
    render_start(player->hRender);
    while ((player->nPlayerStatus & PAUSE_ACK) != PAUSE_ACK) usleep(20*1000);

    // seek frame
    av_seek_frame(player->pAVFormatContext, -1, ms / 1000 * AV_TIME_BASE, AVSEEK_FLAG_BACKWARD);
    if (player->iAudioStreamIndex != -1) avcodec_flush_buffers(player->pAudioCodecContext);
    if (player->iVideoStreamIndex != -1) avcodec_flush_buffers(player->pVideoCodecContext);

    // reset packet queue
    pktqueue_reset(player->hPacketQueue);

    // reset render
    render_reset   (player->hRender);
    render_setparam(player->hRender, PARAM_MEDIA_POSITION, &ms);

    // restart all thread and render
    int SEEK_REQ = 0;
    int SEEK_ACK = 0;
    if (player->iAudioStreamIndex != -1) { SEEK_REQ |= PS_A_SEEK; SEEK_ACK |= PS_A_SEEK << 16; }
    if (player->iVideoStreamIndex != -1) { SEEK_REQ |= PS_V_SEEK; SEEK_ACK |= PS_V_SEEK << 16; }
    player->nSeekToNewPTS  = ms;
    player->nPlayerStatus &= ~SEEK_ACK;
    player->nPlayerStatus |=  SEEK_REQ;
    player->nPlayerStatus &= ~(PAUSE_REQ | PAUSE_ACK);
    while ( !(player->nPlayerStatus & (PS_D_PAUSE << 16))
          && (player->nPlayerStatus & SEEK_ACK) != SEEK_ACK ) {
        usleep(20*1000);
    }
    player->nPlayerStatus &= ~(SEEK_REQ | SEEK_ACK);

    // pause render if needed
    if (player->nPlayerStatus & PS_R_PAUSE) {
        usleep(20*1000); render_pause(player->hRender);
    }
}

void player_setparam(void *hplayer, DWORD id, void *param)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    switch (id)
    {
    case PARAM_VIDEO_MODE:
        render_setparam(player->hRender, PARAM_VIDEO_MODE, param);
        player_setrect(hplayer, 0,
            player->rtVideo.left, player->rtVideo.top,
            player->rtVideo.right - player->rtVideo.left,
            player->rtVideo.bottom - player->rtVideo.top);
        break;
    case PARAM_PLAY_SPEED:
        render_setparam(player->hRender, PARAM_PLAY_SPEED, param);
        player->nMaxPlaySpeed = *(int*)param;
        break;
    case PARAM_AUTO_SLOW_DOWN:
        player->nAutoSlowDown = *(int*)param;
        break;
    case PARAM_MIN_PLAY_SPEED:
        player->nMinPlaySpeed = *(int*)param;
        break;
    case PARAM_MAX_PLAY_SPEED:
        player->nMaxPlaySpeed = *(int*)param;
        break;
    default:
        render_setparam(player->hRender, id, param);
        break;
    }
}

void player_getparam(void *hplayer, DWORD id, void *param)
{
    if (!hplayer || !param) return;
    PLAYER *player = (PLAYER*)hplayer;

    switch (id)
    {
    case PARAM_MEDIA_DURATION:
        if (!player->pAVFormatContext) *(int64_t*)param = 0;
        else *(int64_t*)param = (player->pAVFormatContext->duration * 1000 / AV_TIME_BASE);
        break;
    case PARAM_VIDEO_WIDTH:
        if (!player->pVideoCodecContext) *(int*)param = 0;
        else *(int*)param = player->pVideoCodecContext->width;
        break;
    case PARAM_VIDEO_HEIGHT:
        if (!player->pVideoCodecContext) *(int*)param = 0;
        else *(int*)param = player->pVideoCodecContext->height;
        break;
    case PARAM_AUTO_SLOW_DOWN:
        *(int*)param = player->nAutoSlowDown;
        break;
    case PARAM_MIN_PLAY_SPEED:
        *(int*)param = player->nMinPlaySpeed;
        break;
    case PARAM_MAX_PLAY_SPEED:
        *(int*)param = player->nMaxPlaySpeed;
        break;
    default:
        render_getparam(player->hRender, id, param);
        break;
    }
}




