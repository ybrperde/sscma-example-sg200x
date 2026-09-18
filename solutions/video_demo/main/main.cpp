#include <iostream>
#include <signal.h>
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

#include "rtsp_demo.h"
#include "video.h"

/* Dual-channel proof (plan paso 3): 5MP H.264 on CH0 + scaled RGB for AI.
 * Optional JPEG 640 on CH2 (preview). If ION fails at startVideo, rebuild with
 * VIDEO_DEMO_JPEG 0 — RGB/AI is independent of preview. */
#ifndef VIDEO_DEMO_RAW_CH
#define VIDEO_DEMO_RAW_CH VIDEO_CH1
#endif
#ifndef VIDEO_DEMO_JPEG_CH
#define VIDEO_DEMO_JPEG_CH VIDEO_CH2
#endif
#ifndef VIDEO_DEMO_JPEG
#define VIDEO_DEMO_JPEG 1
#endif

static CVI_VOID app_ipcam_ExitSig_handle(CVI_S32 signo) {
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    if ((SIGINT == signo) || (SIGTERM == signo)) {
        deinitVideo();
        deinitRtsp();
        APP_PROF_LOG_PRINT(LEVEL_INFO, "ipcam receive a signal(%d) from terminate\n", signo);
    }

    exit(-1);
}

#if 0
static int fpSaveVencFrame(void* pData, void* pArgs, void* pUserData) {
    VENC_STREAM_S* pstStream = (VENC_STREAM_S*)pData;

    APP_DATA_CTX_S* pstDataCtx        = (APP_DATA_CTX_S*)pArgs;
    APP_DATA_PARAM_S* pstDataParam    = &pstDataCtx->stDataParam;
    APP_VENC_CHN_CFG_S* pstVencChnCfg = (APP_VENC_CHN_CFG_S*)pstDataParam->pParam;

    static uint32_t count = 0;
    /* dynamic touch a file: /tmp/rec then start save sreaming to flash or SD Card */
    /*if (access("/tmp/rec", F_OK) == 0)*/ {
        if (pstVencChnCfg->pFile == NULL) {
            char szFilePath[100] = {0};

            sprintf(szFilePath,
                    "/userdata/local/VENC%d_%d%s",
                    pstVencChnCfg->VencChn,
                    count++,
                    app_ipcam_Postfix_Get(pstVencChnCfg->enType));
            count %= 10;
            APP_PROF_LOG_PRINT(LEVEL_INFO, "start save file to %s\n", szFilePath);
            pstVencChnCfg->pFile = fopen(szFilePath, "wb");
            if (pstVencChnCfg->pFile == NULL) {
                APP_PROF_LOG_PRINT(LEVEL_ERROR, "open file err, %s\n", szFilePath);
                return CVI_FAILURE;
            }
        }

        VENC_PACK_S* ppack;
        /* just from I-frame saving */
        if ((pstVencChnCfg->enType == PT_H264) || (pstVencChnCfg->enType == PT_H265)) {
            if (pstVencChnCfg->fileNum == 0 && pstStream->u32PackCount < 2) {
                APP_PROF_LOG_PRINT(LEVEL_INFO, "skip first I-frame\n");
                return CVI_SUCCESS;
            }
        }
        for (CVI_U32 i = 0; i < pstStream->u32PackCount; i++) {
            ppack = &pstStream->pstPack[i];
            fwrite(ppack->pu8Addr + ppack->u32Offset,
                   ppack->u32Len - ppack->u32Offset,
                   1,
                   pstVencChnCfg->pFile);

            APP_PROF_LOG_PRINT(
                LEVEL_DEBUG,
                "pack[%d], PTS = %lu, Addr = %p, Len = 0x%X, Offset = 0x%X DataType=%d\n",
                i,
                ppack->u64PTS,
                ppack->pu8Addr,
                ppack->u32Len,
                ppack->u32Offset,
                ppack->DataType.enH265EType);
        }

        if (pstVencChnCfg->enType == PT_JPEG) {
            fclose(pstVencChnCfg->pFile);
            pstVencChnCfg->pFile = NULL;
            APP_PROF_LOG_PRINT(LEVEL_INFO, "End save! \n");
            // remove("/tmp/rec");
        } else {
            if (++pstVencChnCfg->fileNum > pstVencChnCfg->u32Duration) {
                pstVencChnCfg->fileNum = 0;
                fclose(pstVencChnCfg->pFile);
                pstVencChnCfg->pFile = NULL;
                APP_PROF_LOG_PRINT(LEVEL_INFO, "End save! \n");
                // remove("/tmp/rec");
            }
        }
    }

    return CVI_SUCCESS;
}

static int fpSaveVpssFrame(void* pData, void* pArgs, void* pUserData) {
    APP_VENC_CHN_CFG_S* pstVencChnCfg = (APP_VENC_CHN_CFG_S*)pArgs;
    VIDEO_FRAME_INFO_S* VpssFrame     = (VIDEO_FRAME_INFO_S*)pData;
    VIDEO_FRAME_S* f                  = &VpssFrame->stVFrame;

    char name[32]         = "";
    static uint32_t count = 0;
    snprintf(
        name, sizeof(name), "/mnt/sd/%d_%dx%d%s", count++, f->u32Width, f->u32Height, pUserData);
    if (count >= 10) {
        count = 0;
    }
    APP_PROF_LOG_PRINT(LEVEL_INFO, "save frame to %s\n", name);

    FILE* fp = fopen(name, "w");
    if (fp == CVI_NULL) {
        CVI_TRACE_LOG(CVI_DBG_ERR, "open data file error\n");
        return CVI_FAILURE;
    }

    for (uint32_t i = 0; i < 3; i++) {
        if (f->u32Length[i]) {
            f->pu8VirAddr[i] = (CVI_U8*)CVI_SYS_Mmap(f->u64PhyAddr[i], f->u32Length[i]);
            if (fwrite(f->pu8VirAddr[i], f->u32Length[i], 1, fp) <= 0) {
                CVI_TRACE_LOG(CVI_DBG_ERR, "fwrite data(%d) error\n", f->u32TimeRef);
            }
            CVI_SYS_Munmap(f->pu8VirAddr[i], f->u32Length[i]);
        }
    }

    fclose(fp);

    return CVI_SUCCESS;
}
#endif

static int fpCountAndDumpRgb(void* pData, void* pArgs, void* pUserData) {
    (void)pArgs;
    (void)pUserData;
    VIDEO_FRAME_INFO_S* VpssFrame = (VIDEO_FRAME_INFO_S*)pData;
    VIDEO_FRAME_S* f              = &VpssFrame->stVFrame;

    static uint32_t frames = 0;
    frames++;

    if ((frames % 30) == 0) {
        printf("rgb ch%d frames=%u size=%ux%u stride=%u fmt=%d\n",
               VIDEO_DEMO_RAW_CH,
               frames,
               f->u32Width,
               f->u32Height,
               f->u32Stride[0],
               f->enPixelFormat);
    }

    /* Overwrite one PPM so /tmp does not fill. */
    if ((frames % 30) == 1 && f->u32Length[0] && f->u32Width && f->u32Height) {
        char name[64];
        snprintf(name, sizeof(name), "/tmp/ch%d_%ux%u.ppm", VIDEO_DEMO_RAW_CH, f->u32Width, f->u32Height);
        FILE* fp = fopen(name, "wb");
        if (fp == NULL) {
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "open %s failed\n", name);
            return CVI_SUCCESS;
        }

        CVI_U8* vir = (CVI_U8*)CVI_SYS_Mmap(f->u64PhyAddr[0], f->u32Length[0]);
        if (vir == NULL) {
            fclose(fp);
            APP_PROF_LOG_PRINT(LEVEL_ERROR, "mmap rgb frame failed\n");
            return CVI_SUCCESS;
        }

        fprintf(fp, "P6\n%u %u\n255\n", f->u32Width, f->u32Height);
        if (f->enPixelFormat == PIXEL_FORMAT_RGB_888) {
            uint32_t stride = f->u32Stride[0] ? f->u32Stride[0] : (f->u32Width * 3);
            for (uint32_t y = 0; y < f->u32Height; y++) {
                fwrite(vir + y * stride, 1, f->u32Width * 3, fp);
            }
        } else {
            APP_PROF_LOG_PRINT(LEVEL_WARN, "rgb dump skip: fmt %d not RGB888\n", f->enPixelFormat);
        }
        CVI_SYS_Munmap(vir, f->u32Length[0]);
        fclose(fp);
        APP_PROF_LOG_PRINT(LEVEL_INFO, "dumped %s (%u frames)\n", name, frames);
    }

    return CVI_SUCCESS;
}

#if VIDEO_DEMO_JPEG
static int fpCountAndDumpJpeg(void* pData, void* pArgs, void* pUserData) {
    (void)pArgs;
    (void)pUserData;
    VENC_STREAM_S* pstStream = (VENC_STREAM_S*)pData;
    if (pstStream == NULL || pstStream->u32PackCount == 0) {
        return CVI_SUCCESS;
    }

    static uint32_t frames = 0;
    frames++;

    if ((frames % 30) == 0) {
        printf("jpeg ch%d frames=%u packs=%u len0=%u\n",
               VIDEO_DEMO_JPEG_CH,
               frames,
               pstStream->u32PackCount,
               pstStream->pstPack[0].u32Len - pstStream->pstPack[0].u32Offset);
    }

    if ((frames % 30) == 1) {
        FILE* fp = fopen("/tmp/ch2_preview.jpg", "wb");
        if (fp == NULL) {
            return CVI_SUCCESS;
        }
        for (CVI_U32 i = 0; i < pstStream->u32PackCount; i++) {
            VENC_PACK_S* ppack = &pstStream->pstPack[i];
            fwrite(ppack->pu8Addr + ppack->u32Offset, ppack->u32Len - ppack->u32Offset, 1, fp);
        }
        fclose(fp);
        APP_PROF_LOG_PRINT(LEVEL_INFO, "dumped /tmp/ch2_preview.jpg (%u frames)\n", frames);
    }

    return CVI_SUCCESS;
}
#endif

int main(int argc, char* argv[]) {
    signal(SIGINT, app_ipcam_ExitSig_handle);
    signal(SIGTERM, app_ipcam_ExitSig_handle);

    if (initVideo())
        return -1;

    if (setVideoSensorOutput(2592, 1944, 15.0f) != 0) {
        APP_PROF_LOG_PRINT(LEVEL_ERROR, "setVideoSensorOutput 5MP failed\n");
        return -1;
    }

    video_ch_param_t param;

    /* CH0: 5MP H.264 RTSP (PHY ch0; ch2 is ~1080p-capped on CV181x). */
    param.format = VIDEO_FORMAT_H264;
    param.width  = 2592;
    param.height = 1944;
    param.fps    = 15;
    setupVideo(VIDEO_CH0, &param);
    registerVideoFrameHandler(VIDEO_CH0, 0, fpStreamingSendToRtsp, NULL);
    initRtsp((0x01 << VIDEO_CH0));

    /* CH1 (or CH2): scaled RGB for a future model node. Pool is 640, not 5MP. */
    param.format = VIDEO_FORMAT_RGB888;
    param.width  = 640;
    param.height = 640;
    param.fps    = 15;
    setupVideo(VIDEO_DEMO_RAW_CH, &param);
    registerVideoFrameHandler(VIDEO_DEMO_RAW_CH, 0, fpCountAndDumpRgb, NULL);

#if VIDEO_DEMO_JPEG
    /* CH2: JPEG 640 preview (same size as model input, not 5MP). */
    param.format = VIDEO_FORMAT_JPEG;
    param.width  = 640;
    param.height = 640;
    param.fps    = 15;
    setupVideo(VIDEO_DEMO_JPEG_CH, &param);
    registerVideoFrameHandler(VIDEO_DEMO_JPEG_CH, 0, fpCountAndDumpJpeg, NULL);
#endif

    printf("video_demo: H264 %dx%d ch0 + RGB %dx%d ch%d"
#if VIDEO_DEMO_JPEG
           " + JPEG %dx%d ch%d"
#endif
           " @ 15fps\n",
           2592,
           1944,
           640,
           640,
           VIDEO_DEMO_RAW_CH
#if VIDEO_DEMO_JPEG
           ,
           640,
           640,
           VIDEO_DEMO_JPEG_CH
#endif
    );

    startVideo();

    while (1) {
        sleep(1);
    }

    return 0;
}