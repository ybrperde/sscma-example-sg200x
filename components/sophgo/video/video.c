#include "video.h"

static bool is_started   = false;
static bool video_mirror = false;
static bool video_flip   = false;

static int setVbPool(video_ch_index_t ch, const video_ch_param_t* param) {
    APP_PARAM_SYS_CFG_S* sys = app_ipcam_Sys_Param_Get();

    if (ch >= sys->vb_pool_num) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "ch(%d) > vb_pool_num(%d)\n", ch, sys->vb_pool_num);
        return -1;
    }
    if (param == NULL) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "param is null\n");
        return -1;
    }

    APP_PARAM_VB_CFG_S* vb = &sys->vb_pool[ch];
    uint32_t width         = param->width;
    uint32_t height        = param->height;

    /* Offline VI dumps full sensor frames; online VPSS keeps one pool per
     * output so a 640 RGB channel must not be inflated to 5MP. */
    VI_VPSS_MODE_E mode = sys->stVIVPSSMode.aenMode[0];
    if (mode == VI_OFFLINE_VPSS_OFFLINE || mode == VI_OFFLINE_VPSS_ONLINE) {
        APP_PARAM_VI_CTX_S* vi = app_ipcam_Vi_Param_Get();
        if (vi->astChnInfo[0].u32Width > width)
            width = vi->astChnInfo[0].u32Width;
        if (vi->astChnInfo[0].u32Height > height)
            height = vi->astChnInfo[0].u32Height;
    }

    vb->bEnable = 1;
    vb->width   = width;
    vb->height  = height;
    if (param->format == VIDEO_FORMAT_RGB888) {
        vb->fmt = PIXEL_FORMAT_RGB_888;
    } else if (param->format == VIDEO_FORMAT_JPEG) {
        /* JPEG JPU wants I420. NV21 looks like rainbow scanlines. */
        vb->fmt = PIXEL_FORMAT_YUV_PLANAR_420;
    } else {
        vb->fmt = PIXEL_FORMAT_NV21;
    }

    return 0;
}

static int setGrpChn(int grp, video_ch_index_t ch, const video_ch_param_t* param) {
    APP_PARAM_VPSS_CFG_T* vpss = app_ipcam_Vpss_Param_Get();

    if (grp >= vpss->u32GrpCnt) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "grp(%d) > u32GrpCnt(%d)\n", grp, vpss->u32GrpCnt);
        return -1;
    }
    if (ch >= VIDEO_CH_MAX) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "ch(%d) > VIDEO_CH_MAX(%d)\n", ch, VIDEO_CH_MAX);
        return -1;
    }
    if (param == NULL) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "param is null\n");
        return -1;
    }

    APP_VPSS_GRP_CFG_T* pgrp  = &vpss->astVpssGrpCfg[grp];
    pgrp->abChnEnable[ch]     = 1;
    pgrp->aAttachEn[ch]       = 1;
    VPSS_CHN_ATTR_S* vpss_chn = &pgrp->astVpssChnAttr[ch];
    vpss_chn->u32Width        = param->width;
    vpss_chn->u32Height       = param->height;
    if (param->format == VIDEO_FORMAT_RGB888) {
        vpss_chn->enPixelFormat = PIXEL_FORMAT_RGB_888;
    } else if (param->format == VIDEO_FORMAT_JPEG) {
        vpss_chn->enPixelFormat          = PIXEL_FORMAT_YUV_PLANAR_420;
        vpss_chn->stAspectRatio.enMode   = ASPECT_RATIO_NONE;
    } else {
        vpss_chn->enPixelFormat = PIXEL_FORMAT_NV21;
    }
    /* GetChnFrame (1080p H.264 on CH1 / 5MP JPEG stills) needs a queued frame. */
    vpss_chn->u32Depth = 1;

    return 0;
}

static int setVencChn(video_ch_index_t ch, const video_ch_param_t* param) {
    APP_PARAM_VENC_CTX_S* venc = app_ipcam_Venc_Param_Get();

    if (ch >= venc->s32VencChnCnt) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "ch(%d) > u32ChnCnt(%d)\n", ch, venc->s32VencChnCnt);
        return -1;
    }

    if (param == NULL) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "param is null\n");
        return -1;
    }

    PAYLOAD_TYPE_E enType = PT_JPEG;
    if (VIDEO_FORMAT_H264 == param->format) {
        enType = PT_H264;
    } else if (VIDEO_FORMAT_H265 == param->format) {
        enType = PT_H265;
    }
    app_ipcam_Param_setVencChnType(ch, enType);
    APP_VENC_CHN_CFG_S* pvchn = &venc->astVencChnCfg[ch];
    pvchn->bEnable            = 1;
    pvchn->u32Width           = param->width;
    pvchn->u32Height          = param->height;
    pvchn->u32DstFrameRate    = param->fps;

    if ((VIDEO_FORMAT_RGB888 == param->format) || (VIDEO_FORMAT_NV21 == param->format)) {
        pvchn->no_need_venc = 1;
        pvchn->enBindMode   = VENC_BIND_DISABLE;
    } else if ((enType == PT_H264 || enType == PT_H265) && param->width <= 1920 && param->height <= 1080) {
        /* 1080p H.264: VPSS→VENC bind on CH0/CH2 yields venc timeout. */
        pvchn->enBindMode = VENC_BIND_DISABLE;
    } else if (enType == PT_JPEG && param->width > 1920) {
        /* CH0 JPEG/MJPEG bind does not emit. Same GetChnFrame path as 1080p stills. */
        pvchn->enType           = PT_JPEG;
        pvchn->enBindMode       = VENC_BIND_DISABLE;
        pvchn->u32StreamBufSize = (8 << 20);
        pvchn->stJpegCodecParam.quality   = 92;
        pvchn->stJpegCodecParam.MCUPerECS = 0;
        if (param->fps > 0) {
            pvchn->u32SrcFrameRate = param->fps;
            pvchn->u32DstFrameRate = param->fps;
        } else {
            pvchn->u32SrcFrameRate = 15;
            pvchn->u32DstFrameRate = 15;
        }
        APP_PARAM_VPSS_CFG_T* vpss = app_ipcam_Vpss_Param_Get();
        vpss->astVpssGrpCfg[0].astVpssChnAttr[ch].u32Depth = 1;
        printf("5MP JPEG: GetChnFrame ch%d %ux%u@%u depth=1\n", ch, param->width, param->height, param->fps);
        fflush(stdout);
    }

    return 0;
}

int initVideo(void) {
    APP_CHK_RET(app_ipcam_Param_Load(), "load global parameter");
    video_mirror = false;
    video_flip = false;

    return 0;
}

int setVideoSensorOutput(uint32_t width, uint32_t height, float fps) {
    return app_ipcam_Param_SetSensorOutput(width, height, fps);
}

int deinitVideo(void) {
    if (is_started) {
        APP_CHK_RET(app_ipcam_Venc_Stop(APP_VENC_ALL), "Venc Stop");
        APP_CHK_RET(app_ipcam_Vpss_DeInit(), "Vpss DeInit");
        APP_CHK_RET(app_ipcam_Vi_DeInit(), "Vi DeInit");
        APP_CHK_RET(app_ipcam_Sys_DeInit(), "System DeInit");
        is_started = false;
    }
    return 0;
}

int startVideo() {
    /* init modules include <Peripheral; Sys; VI; VB; OSD; Venc; AI; Audio; etc.> */
    APP_CHK_RET(app_ipcam_Sys_Init(), "init systerm");
    APP_CHK_RET(app_ipcam_Vi_Init(), "init vi module");
    APP_CHK_RET(app_ipcam_Vpss_Init(), "init vpss module");
    APP_CHK_RET(app_ipcam_Venc_Init(APP_VENC_ALL), "init video encode");

    /* start video encode */
    APP_CHK_RET(app_ipcam_Venc_Start(APP_VENC_ALL), "start video processing");

    is_started = true;
    return 0;
}

int setupVideo(video_ch_index_t ch, const video_ch_param_t* param) {
    if (ch >= VIDEO_CH_MAX) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "video ch(%d) index is out of range\n", ch);
        return -1;
    }
    if (param == NULL) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "video ch(%d) param is null\n", ch);
        return -1;
    }
    if (param->format >= VIDEO_FORMAT_COUNT) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "video ch(%d) format(%d) is not support\n", ch, param->format);
        return -1;
    }

    video_ch_param_t p = *param;
    /* CVI JPEG/MJPEG: width 64-aligned, height 16-aligned, max 1920 high. */
    if (p.format == VIDEO_FORMAT_JPEG && p.width > 1920) {
        p.width  = p.width & ~63u;
        if (p.height > 1920) {
            p.height = 1920;
        }
        p.height = p.height & ~15u;
        if (p.fps < 15) {
            p.fps = 15;
        }
    }

    setVbPool(ch, &p);
    setGrpChn(0, ch, &p);
    setVencChn(ch, &p);

    return 0;
}

int registerVideoFrameHandler(video_ch_index_t ch, int index, pfpDataConsumes handler, void* pUserData) {
    app_ipcam_Venc_Consumes_Set(ch, index, handler, pUserData);
    return 0;
}

int setVideoMirror(bool mirror) {
    video_mirror = mirror;
    return 0;
}
int setVideoFlip(bool flip) {
    video_flip = flip;
    return 0;
}
int getVideoMirror() {
    return video_mirror;
}
int getVideoFlip() {
    return video_flip;
}
