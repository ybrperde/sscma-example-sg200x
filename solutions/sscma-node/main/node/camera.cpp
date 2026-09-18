
#include <alsa/asoundlib.h>

#include "camera.h"

namespace ma::node {

static constexpr char TAG[] = "ma::node::camera";

const char* VIDEO_FORMATS[] = {"raw", "jpeg", "h264"};


#define CAMERA_INIT()                                                                                                                        \
    {                                                                                                                                        \
        Thread::enterCritical();                                                                                                             \
        MA_LOGI(TAG, "start video");                                                                                                         \
        startVideo();                                                                                                                        \
        Thread::sleep(Tick::fromMilliseconds(100));                                                                                          \
        Thread::exitCritical();                                                                                                              \
        Thread::sleep(Tick::fromSeconds(1));                                                                                                 \
        server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", "enabled"}, {"code", MA_OK}, {"data", enabled_.load()}})); \
    }

#define CAMERA_DEINIT()                                                                                                                      \
    {                                                                                                                                        \
        Thread::enterCritical();                                                                                                             \
        MA_LOGI(TAG, "stop video");                                                                                                          \
        Thread::sleep(Tick::fromMilliseconds(100));                                                                                          \
        deinitVideo();                                                                                                                       \
        Thread::sleep(Tick::fromSeconds(1));                                                                                                 \
        Thread::exitCritical();                                                                                                              \
        server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", "enabled"}, {"code", MA_OK}, {"data", enabled_.load()}})); \
    }

CameraNode::CameraNode(std::string id)
    : Node("camera", std::move(id)),
      channels_(CHN_MAX),
      count_(0),
      light_(0),
      preview_(false),
      websocket_(true),
      audio_(80),
      mirror_(false),
      flip_(false),
      option_(0),
      fps_(30),
      frame_(60),
      thread_(nullptr),
      thread_audio_(nullptr),
      transport_(nullptr) {
    for (int i = 0; i < CHN_MAX; i++) {
        channels_[i].configured = false;
        channels_[i].enabled    = false;
        channels_[i].format     = MA_PIXEL_FORMAT_H264;
        channels_[i].fps        = 30;
    }
    channels_.shrink_to_fit();
}

CameraNode::~CameraNode() {
    onDestroy();
};

static inline bool isKeyFrame(int format) {
    bool isKey = false;
    switch (format) {
        case H264E_NALU_ISLICE:
        case H264E_NALU_SPS:
        case H264E_NALU_IDRSLICE:
        case H264E_NALU_SEI:
        case H264E_NALU_PPS:
            isKey = true;
            break;
    }
    return isKey;
}

int CameraNode::hwFromLogical(int logical) const {
    if (logical == CHN_AUDIO) {
        return logical;
    }
    if (option_ == 3) {
        switch (logical) {
            case CHN_H264:
                return VIDEO_CH0;
            case CHN_RAW:
                return VIDEO_CH1;
            case CHN_JPEG:
                /* CH0 JPEG/MJPEG bind never emits. 1080p stills worked on CH1 GetChnFrame. */
                if (jpegOnHwCh1()) {
                    return VIDEO_CH1;
                }
                return VIDEO_CH2;
            default:
                return logical;
        }
    }
    /* 1080p JPEG capture worked on CH1. CH0/CH2 H.264 get no frames in this HAL. */
    if (streamOnlyH264OnCh1()) {
        switch (logical) {
            case CHN_H264:
                return VIDEO_CH1;
            case CHN_JPEG:
                return VIDEO_CH2;
            case CHN_RAW:
                return VIDEO_CH0;
            default:
                return logical;
        }
    }
    return logical;
}

int CameraNode::logicalFromHw(int hw) const {
    if (option_ == 3) {
        switch (hw) {
            case VIDEO_CH0:
                return CHN_H264;
            case VIDEO_CH1:
                return jpegOnHwCh1() ? CHN_JPEG : CHN_RAW;
            case VIDEO_CH2:
                return CHN_JPEG;
            default:
                return hw;
        }
    }
    if (streamOnlyH264OnCh1()) {
        switch (hw) {
            case VIDEO_CH0:
                return CHN_RAW;
            case VIDEO_CH1:
                return CHN_H264;
            case VIDEO_CH2:
                return CHN_JPEG;
            default:
                return hw;
        }
    }
    return hw;
}

bool CameraNode::h264UsesHwCh0() const {
    return option_ == 3;
}

bool CameraNode::jpegOnHwCh0() const {
    /* CH0 JPEG/MJPEG + VPSS bind never produces GetStream on this SoC. */
    return false;
}

bool CameraNode::jpegOnHwCh1() const {
    /* Factory 1080p capture: JPEG on CH1 via GetChnFrame. Only when CH1 is free (no RAW/H.264). */
    return option_ == 3 && !channels_[CHN_H264].enabled && !channels_[CHN_RAW].enabled;
}

bool CameraNode::streamOnlyH264OnCh1() const {
    return option_ != 3 && channels_[CHN_H264].enabled && !channels_[CHN_RAW].enabled;
}

int CameraNode::channelWidth(int chn) const {
    if (chn < 0 || chn >= CHN_MAX) {
        return 0;
    }
    return channels_[chn].width;
}

int CameraNode::channelHeight(int chn) const {
    if (chn < 0 || chn >= CHN_MAX) {
        return 0;
    }
    return channels_[chn].height;
}

void CameraNode::sensorSize(int32_t& width, int32_t& height) const {
    if (option_ == 3) {
        width  = 2592;
        height = 1944;
    } else {
        width  = 1920;
        height = 1080;
    }
}

int CameraNode::vencCallback(void* pData, void* pArgs) {

    APP_DATA_CTX_S* pstDataCtx        = (APP_DATA_CTX_S*)pArgs;
    APP_DATA_PARAM_S* pstDataParam    = &pstDataCtx->stDataParam;
    APP_VENC_CHN_CFG_S* pstVencChnCfg = (APP_VENC_CHN_CFG_S*)pstDataParam->pParam;
    const int chn                     = logicalFromHw(pstVencChnCfg->VencChn);

    if (chn < 0 || chn >= CHN_MAX) {
        MA_LOGW(TAG, "invalid chn hw=%d logical=%d", pstVencChnCfg->VencChn, chn);
        return CVI_SUCCESS;
    }
    if (!started_ || !enabled_ || channels_[chn].msgboxes.empty()) {
        return CVI_SUCCESS;
    }

    VENC_STREAM_S* pstStream = (VENC_STREAM_S*)pData;
    VENC_PACK_S* ppack;

    for (int i = 0; i < pstStream->u32PackCount; i++) {
        videoFrame* frame = nullptr;
        ppack             = &pstStream->pstPack[i];
        if (chn == CHN_H264 && isKeyFrame(ppack->DataType.enH264EType)) {
            /* Group consecutive SPS/PPS/IDR/SEI. Never drop a lone SPS/PPS:
             * 5MP often emits one NAL per GetStream (video_demo forwards all). */
            int cnt    = 0;
            int offset = 0;
            int size   = 0;
            for (int j = i; j < pstStream->u32PackCount && isKeyFrame(pstStream->pstPack[j].DataType.enH264EType); j++) {
                size += pstStream->pstPack[j].u32Len - pstStream->pstPack[j].u32Offset;
                cnt++;
            }
            frame                  = new videoFrame();
            frame->chn             = chn;
            frame->timestamp       = Tick::current();
            frame->img.width       = channels_[chn].width;
            frame->img.height      = channels_[chn].height;
            frame->img.format      = channels_[chn].format;
            frame->img.size        = size;
            frame->img.key         = true;
            frame->img.physical    = false;
            frame->img.data        = new uint8_t[size];
            frame->fps             = channels_[chn].fps;
            channels_[chn].dropped = false;
            for (int j = i; j < i + cnt; j++) {
                memcpy(frame->img.data + offset, pstStream->pstPack[j].pu8Addr + pstStream->pstPack[j].u32Offset, pstStream->pstPack[j].u32Len - pstStream->pstPack[j].u32Offset);
                frame->blocks.push_back({frame->img.data + offset, pstStream->pstPack[j].u32Len - pstStream->pstPack[j].u32Offset});
                offset += pstStream->pstPack[j].u32Len - pstStream->pstPack[j].u32Offset;
            }
            i += (cnt - 1);
        } else if (chn == CHN_JPEG) {
            int size   = 0;
            int offset = 0;
            for (int j = i; j < pstStream->u32PackCount; j++) {
                size += static_cast<int>(pstStream->pstPack[j].u32Len - pstStream->pstPack[j].u32Offset);
            }
            frame               = new videoFrame();
            frame->chn          = chn;
            frame->timestamp    = Tick::current();
            frame->img.width    = channels_[chn].width;
            frame->img.height   = channels_[chn].height;
            frame->img.format   = channels_[chn].format;
            frame->img.size     = size;
            frame->img.key      = true;
            frame->img.physical = false;
            frame->img.data     = new uint8_t[size];
            frame->fps          = channels_[chn].fps;
            for (int j = i; j < pstStream->u32PackCount; j++) {
                const uint32_t n = pstStream->pstPack[j].u32Len - pstStream->pstPack[j].u32Offset;
                memcpy(frame->img.data + offset, pstStream->pstPack[j].pu8Addr + pstStream->pstPack[j].u32Offset, n);
                frame->blocks.push_back({frame->img.data + offset, n});
                offset += static_cast<int>(n);
            }
            i = pstStream->u32PackCount - 1;
        } else {
            if (chn == CHN_H264 && channels_[chn].dropped) {
                continue;
            }
            frame               = new videoFrame();
            frame->chn          = chn;
            frame->timestamp    = Tick::current();
            frame->img.width    = channels_[chn].width;
            frame->img.height   = channels_[chn].height;
            frame->img.format   = channels_[chn].format;
            frame->img.size     = ppack->u32Len - ppack->u32Offset;
            frame->img.key      = false;
            frame->img.physical = false;
            frame->img.data     = new uint8_t[ppack->u32Len - ppack->u32Offset];
            frame->fps          = channels_[chn].fps;
            frame->blocks.push_back({frame->img.data, ppack->u32Len - ppack->u32Offset});
            memcpy(frame->img.data, ppack->pu8Addr + ppack->u32Offset, ppack->u32Len - ppack->u32Offset);
        }
        if (frame != nullptr) {
            if (chn == CHN_H264) {
                static uint32_t h264_n;
                if ((h264_n++ % 15) == 0) {
                    MA_LOGI(TAG, "h264 n=%u packs=%u nalu=%d len=%u key=%d", h264_n, pstStream->u32PackCount, ppack->DataType.enH264EType, frame->img.size, frame->img.key);
                }
            } else if (chn == CHN_JPEG) {
                static uint32_t jpeg_n;
                if (jpeg_n < 3 || (jpeg_n % 15) == 0) {
                    MA_LOGI(TAG, "jpeg n=%u len=%u %dx%d", jpeg_n, frame->img.size, frame->img.width, frame->img.height);
                }
                jpeg_n++;
            }
            frame->ref(channels_[chn].msgboxes.size());
            int posted = 0;
            for (auto& msgbox : channels_[chn].msgboxes) {
                if (!msgbox->post(frame, Tick::fromMilliseconds(static_cast<int>(1000.0 / channels_[chn].fps)))) {
                    frame->release();
                } else {
                    posted++;
                }
            }
            /* One slow consumer (5MP websocket) must not starve RTSP. */
            if (posted == 0) {
                channels_[chn].dropped = true;
            }
        }
    }

    return CVI_SUCCESS;
}

int CameraNode::vpssCallback(void* pData, void* pArgs) {

    APP_VENC_CHN_CFG_S* pstVencChnCfg = (APP_VENC_CHN_CFG_S*)pArgs;
    VIDEO_FRAME_INFO_S* VpssFrame     = (VIDEO_FRAME_INFO_S*)pData;
    VIDEO_FRAME_S* f                  = &VpssFrame->stVFrame;
    const int chn                     = logicalFromHw(pstVencChnCfg->VencChn);

    if (chn < 0 || chn >= CHN_MAX) {
        MA_LOGW(TAG, "invalid chn hw=%d logical=%d", pstVencChnCfg->VencChn, chn);
        return CVI_SUCCESS;
    }
    if (!started_ || !enabled_ || channels_[chn].msgboxes.empty()) {
        return CVI_SUCCESS;
    }

    videoFrame* frame   = new videoFrame();
    frame->chn          = chn;
    frame->img.size     = f->u32Length[0] + f->u32Length[1] + f->u32Length[2];
    frame->img.width    = channels_[chn].width;
    frame->img.height   = channels_[chn].height;
    frame->img.format   = channels_[chn].format;
    frame->img.key      = true;
    frame->img.physical = true;
    frame->img.data     = reinterpret_cast<uint8_t*>(f->u64PhyAddr[0]);
    frame->timestamp    = Tick::current();
    frame->fps          = channels_[chn].fps;
    frame->ref(channels_[chn].msgboxes.size());
    for (auto& msgbox : channels_[chn].msgboxes) {
        if (msgbox->isFull() || !msgbox->post(frame, Tick::fromMilliseconds(5))) {
            frame->release();
        }
    }
    return CVI_SUCCESS;
}


void CameraNode::threadEntry() {
    videoFrame* frame = nullptr;
    ma_tick_t last    = Tick::current();

    while (started_) {
        if (frame_.fetch(reinterpret_cast<void**>(&frame), Tick::fromSeconds(1))) {
            Thread::enterCritical();
            if (transport_ && frame->img.format == MA_PIXEL_FORMAT_H264) {
                transport_->send(reinterpret_cast<const char*>(frame->img.data), frame->img.size);
            } else {
                if (Tick::current() - last > Tick::fromMilliseconds(100)) {
                    count_++;
                    json reply     = json::object({{"type", MA_MSG_TYPE_EVT}, {"name", "sample"}, {"code", MA_OK}, {"data", {{"count", count_}}}});
                    char* base64   = new char[4 * ((frame->img.size + 2) / 3 + 2)];
                    int base64_len = 4 * ((frame->img.size + 2) / 3 + 2);
                    ma::utils::base64_encode(frame->img.data, frame->img.size, base64, &base64_len);
                    reply["data"]["image"] = std::string(base64, base64_len);
                    delete[] base64;
                    server_->response(id_, reply);
                    last = Tick::current();
                }
            }
            frame->release();
            Thread::exitCritical();
        }
    }
}


void CameraNode::threadAudioEntry() {
    const char* device_name = AUDIO_DEVICE;
    snd_pcm_t* handle;
    int err = 0;
    snd_pcm_hw_params_t* params;
    snd_pcm_uframes_t frames;
    int pcm_return;
    uint16_t* buffer;
    snd_pcm_uframes_t buffer_size = 0;
    snd_pcm_uframes_t chunk_size  = 0;
    unsigned int buffer_time      = 0;
    unsigned period_time          = 0;
    unsigned int rate             = SAMPLE_RATE;
    int bits_per_sample           = snd_pcm_format_physical_width(FORMAT);

    char cmd[256] = {0};
    snprintf(cmd, sizeof(cmd), "amixer -D" AUDIO_DEVICE " cset name='ADC Capture Volume' %d", audio_ * 24 / 100);

    system(cmd);

    pcm_return = snd_pcm_open(&handle, device_name, SND_PCM_STREAM_CAPTURE, 0);
    if (pcm_return < 0) {
        MA_LOGE(TAG, "Unable to open PCM device: %s", snd_strerror(pcm_return));
        return;
    }
    MA_LOGI(TAG, "PCM device opened successfully. Volume: %d", audio_);

    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(handle, params);
    err = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        MA_LOGE(TAG, "Unable to set access type: %s", snd_strerror(err));
        snd_pcm_close(handle);
        return;
    }
    err = snd_pcm_hw_params_set_format(handle, params, FORMAT);
    if (err < 0) {
        MA_LOGE(TAG, "Unable to set sample format: %s", snd_strerror(err));
        snd_pcm_close(handle);
        return;
    }
    err = snd_pcm_hw_params_set_channels(handle, params, CHANNELS);
    if (err < 0) {
        MA_LOGE(TAG, "Unable to set channel count: %s", snd_strerror(err));
        snd_pcm_close(handle);
        return;
    }

    err = snd_pcm_hw_params_set_rate_near(handle, params, &rate, 0);
    if (err < 0) {
        MA_LOGE(TAG, "Unable to set sample rate: %s", snd_strerror(err));
        snd_pcm_close(handle);
        return;
    }

    err = snd_pcm_hw_params_get_buffer_time_max(params, &buffer_time, 0);
    if (err < 0) {
        MA_LOGE(TAG, "Unable to get buffer time: %s", snd_strerror(err));
        snd_pcm_close(handle);
        return;
    }
    if (buffer_time > 100000)
        buffer_time = 100000;

    period_time = buffer_time / 4;
    err         = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, 0);
    if (err < 0) {
        MA_LOGE(TAG, "Unable to set period time: %s", snd_strerror(err));
        snd_pcm_close(handle);
        return;
    }
    err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, 0);
    if (err < 0) {
        MA_LOGE(TAG, "Unable to set buffer time: %s", snd_strerror(err));
        snd_pcm_close(handle);
        return;
    }

    err = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        MA_LOGE(TAG, "Unable to set parameters: %s", snd_strerror(err));
        snd_pcm_close(handle);
        return;
    }

    snd_pcm_hw_params_get_period_size(params, &chunk_size, 0);
    snd_pcm_hw_params_get_buffer_size(params, &buffer_size);

    if (buffer_size < chunk_size) {
        MA_LOGE(TAG, "Cannot allocate buffer of size %d", buffer_size);
        snd_pcm_close(handle);
        return;
    }

    buffer = new uint16_t[chunk_size * bits_per_sample / 8 * 2 + 1];

    while (started_) {
        pcm_return = snd_pcm_readi(handle, buffer, chunk_size * 2);
        if (pcm_return == -EPIPE) {
            MA_LOGW(TAG, "overrun occurred");
            if (snd_pcm_prepare(handle) < 0) {
                MA_LOGE(TAG, "prepare failed: %s", snd_strerror(pcm_return));
                break;
            }
            continue;
        } else if (pcm_return < 0) {
            MA_LOGE(TAG, "error from read: %s", snd_strerror(pcm_return));
            break;
        }
        if (!enabled_ || channels_[CHN_AUDIO].msgboxes.empty()) {
            continue;
        }
        audioFrame* frame = new audioFrame();
        frame->chn        = CHN_AUDIO;
        frame->data       = new uint8_t[chunk_size * bits_per_sample / 8 * 2];
        frame->size       = chunk_size * bits_per_sample / 8 * 2;
        frame->timestamp  = Tick::current();
        memcpy(frame->data, buffer, chunk_size * bits_per_sample / 8 * 2);
        frame->ref(channels_[CHN_AUDIO].msgboxes.size());
        for (auto& msgbox : channels_[CHN_AUDIO].msgboxes) {
            if (msgbox->isFull() || !msgbox->post(frame, Tick::fromMilliseconds(20))) {
                frame->release();
            }
        }
    }

    snd_pcm_close(handle);
    delete[] buffer;
}

int CameraNode::vencCallbackStub(void* pData, void* pArgs, void* pUserData) {
    return reinterpret_cast<CameraNode*>(pUserData)->vencCallback(pData, pArgs);
}

int CameraNode::vpssCallbackStub(void* pData, void* pArgs, void* pUserData) {
    APP_VENC_CHN_CFG_S* pstVencChnCfg = (APP_VENC_CHN_CFG_S*)pArgs;
    return reinterpret_cast<CameraNode*>(pUserData)->vpssCallback(pData, pArgs);
}

void CameraNode::threadEntryStub(void* obj) {
    reinterpret_cast<CameraNode*>(obj)->threadEntry();
}


void CameraNode::threadAudioEntryStub(void* obj) {
    reinterpret_cast<CameraNode*>(obj)->threadAudioEntry();
}

ma_err_t CameraNode::onCreate(const json& config) {
    Guard guard(mutex_);


    if (initVideo() != 0) {
        MA_THROW(Exception(MA_EIO, "Not found camera device"));
    }

    option_ = 0;

    if (config.contains("option") && config["option"].is_string()) {
        std::string option = config["option"].get<std::string>();
        if (option.find("1080p") != std::string::npos) {
            option_ = 0;
        } else if (option.find("720p") != std::string::npos) {
            option_ = 1;
        } else if (option.find("360p") != std::string::npos) {
            option_ = 2;
        } else if (option.find("5mp") != std::string::npos || option.find("5MP") != std::string::npos || option.find("2592") != std::string::npos) {
            option_ = 3;
        }
    }

    if (config.contains("option") && config["option"].is_number()) {
        option_ = config["option"].get<int>();
    }

    if (config.contains("preview") && config["preview"].is_boolean()) {
        preview_ = config["preview"].get<bool>();
    }

    if (config.contains("websocket") && config["websocket"].is_boolean()) {
        websocket_ = config["websocket"].get<bool>();
    }

    if (config.contains("audio") && config["audio"].is_boolean()) {
        audio_ = config["audio"].get<bool>() ? 80 : 0;
    }

    if (config.contains("audio") && config["audio"].is_number()) {
        audio_ = config["audio"].get<int>();
        if (audio_ > 100) {
            audio_ = 100;
        }
        if (audio_ < 0) {
            audio_ = 0;
        }
    }

    if (config.contains("fps") && config["fps"].is_number()) {
        fps_ = config["fps"].get<int>();
        if (fps_ < 1) {
            fps_ = 1;
        }
        if (fps_ > 30) {
            fps_ = 30;
        }
    }

    if (config.contains("light") && config["light"].is_number()) {
        light_ = config["light"].get<int>();
    }

    if (config.contains("mirror") && config["mirror"].is_boolean()) {
        mirror_ = config["mirror"].get<bool>();
    }

    if (config.contains("flip") && config["flip"].is_boolean()) {
        flip_ = config["flip"].get<bool>();
    }

    setVideoMirror(mirror_);
    setVideoFlip(flip_);

    switch (option_) {
        case 1:
            channels_[CHN_H264].format = MA_PIXEL_FORMAT_H264;
            channels_[CHN_H264].width  = 1280;
            channels_[CHN_H264].height = 720;
            channels_[CHN_H264].fps    = 30;
            channels_[CHN_JPEG].format = MA_PIXEL_FORMAT_JPEG;
            channels_[CHN_JPEG].width  = 1280;
            channels_[CHN_JPEG].height = 720;
            channels_[CHN_JPEG].fps    = 30;
            break;
        case 2:
            channels_[CHN_H264].format = MA_PIXEL_FORMAT_H264;
            channels_[CHN_H264].width  = 640;
            channels_[CHN_H264].height = 480;
            channels_[CHN_H264].fps    = 30;
            channels_[CHN_JPEG].format = MA_PIXEL_FORMAT_JPEG;
            channels_[CHN_JPEG].width  = 640;
            channels_[CHN_JPEG].height = 480;
            channels_[CHN_JPEG].fps    = 30;
            break;
        case 3:
            channels_[CHN_H264].format = MA_PIXEL_FORMAT_H264;
            channels_[CHN_H264].width  = 2592;
            channels_[CHN_H264].height = 1944;
            channels_[CHN_H264].fps    = 15;
            channels_[CHN_JPEG].format = MA_PIXEL_FORMAT_JPEG;
            /* HW JPEG: 64-align width, max 1920 high. 2560×1920 ≈ 5MP. */
            channels_[CHN_JPEG].width  = 2560;
            channels_[CHN_JPEG].height = 1920;
            channels_[CHN_JPEG].fps    = 15;
            channels_[CHN_RAW].format  = MA_PIXEL_FORMAT_RGB888;
            channels_[CHN_RAW].width   = 640;
            channels_[CHN_RAW].height  = 640;
            channels_[CHN_RAW].fps     = 15;
            fps_                       = 15;
            break;
        default:
            channels_[CHN_H264].format = MA_PIXEL_FORMAT_H264;
            channels_[CHN_H264].width  = 1920;
            channels_[CHN_H264].height = 1080;
            channels_[CHN_H264].fps    = 30;
            channels_[CHN_JPEG].format = MA_PIXEL_FORMAT_JPEG;
            channels_[CHN_JPEG].width  = 1920;
            channels_[CHN_JPEG].height = 1080;
            channels_[CHN_JPEG].fps    = 30;
            break;
    }

    if (option_ == 3 && fps_ > 15) {
        fps_ = 15;
    }

    if (fps_ > 0) {
        channels_[CHN_RAW].fps  = fps_;
        channels_[CHN_H264].fps = fps_;
        channels_[CHN_JPEG].fps = fps_;
    }

    MA_LOGI(TAG, "camera option=%d %dx%d@%d", option_, channels_[CHN_H264].width, channels_[CHN_H264].height, channels_[CHN_H264].fps);

    /* 5MP H.264 on websocket 8080 saturates the socket and used to starve RTSP. */
    if (option_ == 3 && !config.contains("websocket")) {
        websocket_ = false;
    }

    if (preview_) {
        this->config(CHN_JPEG);
        this->attach(CHN_JPEG, &frame_);
    }

    if (websocket_) {
        TransportWebSocket::Config ws_config = {.port = 8080};
        MA_STORAGE_GET_POD(server_->getStorage(), MA_STORAGE_KEY_WS_PORT, ws_config.port, 8080);
        transport_ = new TransportWebSocket();
        if (transport_ != nullptr) {
            this->config(CHN_H264);
            this->attach(CHN_H264, &frame_);
            transport_->init(&ws_config);
        }
        MA_LOGI(TAG, "camera websocket server started on port %d", ws_config.port);
    } else {
        transport_ = nullptr;
    }

    if (preview_ || websocket_) {
        thread_ = new Thread((type_ + "#" + id_).c_str(), &CameraNode::threadEntryStub, this);
        if (thread_ == nullptr) {
            MA_THROW(Exception(MA_ENOMEM, "Not enough memory"));
        }
    }

    if (audio_) {
        channels_[CHN_AUDIO].enabled    = true;
        channels_[CHN_AUDIO].configured = true;
        thread_audio_                   = new Thread((type_ + "#" + id_ + "#audio").c_str(), &CameraNode::threadAudioEntryStub, this);
        if (thread_audio_ == nullptr) {
            delete thread_;
            MA_THROW(Exception(MA_ENOMEM, "Not enough memory"));
        }
    }


    server_->response(
        id_,
        json::object(
            {{"type", MA_MSG_TYPE_RESP}, {"name", "create"}, {"code", MA_OK}, {"data", {"width", channels_[CHN_H264].width, "height", channels_[CHN_H264].height, "fps", channels_[CHN_H264].fps}}}));

    created_ = true;

    return MA_OK;
}

ma_err_t CameraNode::onControl(const std::string& control, const json& data) {
    Guard guard(mutex_);
    if (control == "preview" && data.is_boolean()) {
        if (data.get<bool>() != preview_) {
            preview_ = data.get<bool>();
            if (preview_) {
                config(CHN_JPEG);
                attach(CHN_JPEG, &frame_);
            } else {
                detach(CHN_JPEG, &frame_);
            }
        }
        server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", control}, {"code", MA_OK}, {"data", {"preview", preview_}}}));
    } else if (control == "light" && data.is_number()) {
        light_ = data.get<int>();
        if (light_ == 0) {
            system("echo 0 > /sys/devices/platform/leds/leds/white/brightness");
        } else {
            system("echo 1 > /sys/devices/platform/leds/leds/white/brightness");
        }
        server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", control}, {"code", MA_OK}, {"data", {"light", light_}}}));
    } else if (control == "enabled" && data.is_boolean()) {
        bool enabled = data.get<bool>();
        if (enabled_ != enabled) {
            enabled_.store(enabled);
            if (enabled_) {
                CAMERA_INIT();
            } else {
                CAMERA_DEINIT();
            }
        }
    } else {
        server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", control}, {"code", MA_ENOTSUP}, {"data", ""}}));
    }
    return MA_OK;
}

ma_err_t CameraNode::onDestroy() {
    onStop();

    Guard guard(mutex_);
    if (!created_) {
        return MA_OK;
    }

    if (thread_ != nullptr) {
        delete thread_;
        thread_ = nullptr;
    }

    if (transport_ != nullptr) {
        transport_->deInit();
        delete transport_;
        transport_ = nullptr;
    }

    created_ = false;

    return MA_OK;
}

ma_err_t CameraNode::onStart() {
    Guard guard(mutex_);

    if (started_) {
        return MA_OK;
    }
    // checek enabled
    for (int i = 0; i < CHN_MAX; i++) {
        if (channels_[i].enabled) {
            break;
        }
        if (i == CHN_MAX - 1) {
            MA_LOGW(TAG, "no channel enabled");
            return MA_EINVAL;
        }
    }

    // check configured
    for (int i = 0; i < CHN_MAX; i++) {
        if (channels_[i].enabled && !channels_[i].configured) {
            return MA_AGAIN;
        }
    }

    if (light_ == 0) {
        system("echo 0 > /sys/devices/platform/leds/leds/white/brightness");
    } else {
        system("echo 1 > /sys/devices/platform/leds/leds/white/brightness");
    }

    if (option_ == 3) {
        MA_LOGI(TAG, "5MP sensor output 2592x1944@15 (H264->CH0 RAW->CH1 JPEG->%s)",
                jpegOnHwCh1() ? "CH1" : "CH2");
        if (setVideoSensorOutput(2592, 1944, 15.0f) != 0) {
            MA_LOGE(TAG, "setVideoSensorOutput 5MP failed");
            return MA_EIO;
        }
        for (int i = 0; i < CHN_MAX; i++) {
            if (i != CHN_AUDIO && channels_[i].fps > 15) {
                channels_[i].fps = 15;
            }
        }
    } else {
        const float sns_fps = (fps_ > 0) ? static_cast<float>(fps_) : 30.0f;
        if (setVideoSensorOutput(1920, 1080, sns_fps) != 0) {
            MA_LOGE(TAG, "setVideoSensorOutput 1080p failed");
            return MA_EIO;
        }
    }

    for (int i = 0; i < CHN_MAX; i++) {
        if (i == CHN_AUDIO) {
            continue;
        }
        video_ch_param_t param;
        switch (channels_[i].format) {
            case MA_PIXEL_FORMAT_JPEG:
                param.format = VIDEO_FORMAT_JPEG;
                break;
            case MA_PIXEL_FORMAT_H264:
                param.format = VIDEO_FORMAT_H264;
                break;
            case MA_PIXEL_FORMAT_H265:
                param.format = VIDEO_FORMAT_H265;
                break;
            case MA_PIXEL_FORMAT_RGB888:
                param.format = VIDEO_FORMAT_RGB888;
                break;
            case MA_PIXEL_FORMAT_YUV422:
                param.format = VIDEO_FORMAT_NV21;
                break;
            default:
                break;
        }
        param.width  = channels_[i].width;
        param.height = channels_[i].height;
        param.fps    = channels_[i].fps;
        const int hw = hwFromLogical(i);
        MA_LOGI(TAG, "start channel logical=%d hw=%d format %d width %d height %d fps %d enabled=%d", i, hw, param.format, param.width, param.height, param.fps, channels_[i].enabled);
        if (channels_[i].enabled) {
            if (i == CHN_H264 && streamOnlyH264OnCh1()) {
                MA_LOGI(TAG, "1080p stream: H.264 on hw CH1 (GetChnFrame)");
            }
            if (i == CHN_JPEG && jpegOnHwCh1()) {
                MA_LOGI(TAG, "5MP stills: JPEG on hw CH1 (GetChnFrame)");
            }
            setupVideo(static_cast<video_ch_index_t>(hw), &param);
            if (i == CHN_RAW) {
                registerVideoFrameHandler(static_cast<video_ch_index_t>(hw), 0, vpssCallbackStub, this);
            } else {
                registerVideoFrameHandler(static_cast<video_ch_index_t>(hw), 0, vencCallbackStub, this);
            }
        }
    }

    started_ = true;

    if (thread_ != nullptr) {
        thread_->start(this);
    }

    if (audio_ && thread_audio_ != nullptr) {
        thread_audio_->start(this);
    }

    CAMERA_INIT();
    return MA_OK;
}

ma_err_t CameraNode::onStop() {
    {
        Guard guard(mutex_);
        if (!started_) {
            return MA_OK;
        }
        if (preview_) {
            detach(CHN_JPEG, &frame_);
        }
        started_ = false;

        if (thread_ != nullptr) {
            thread_->join();
        }
        if (audio_ && thread_audio_ != nullptr) {
            thread_audio_->join();
        }
    }
    /* Release the camera mutex before tearing VI down so the venc thread
     * can finish its last callback instead of deadlocking the join. */
    CAMERA_DEINIT();
    return MA_OK;
}

ma_err_t CameraNode::config(int chn, int32_t width, int32_t height, int32_t fps, ma_pixel_format_t format, bool enabled) {
    Guard guard(mutex_);
    if (chn < 0 || chn >= CHN_MAX) {
        return MA_EINVAL;
    }
    // if (channels_[chn].configured) {
    //     return MA_EBUSY;
    // }
    if (option_ == 3 && fps > 15) {
        fps = 15;
    }
    int32_t out_w = width > 0 ? width : channels_[chn].width;
    int32_t out_h = height > 0 ? height : channels_[chn].height;
    /* 5MP stills go on CH0 (2592×1920 JPEG; HW max height 1920). CH2 cannot do 5MP. */
    if (option_ == 3 && chn == CHN_JPEG && out_w > 0 && out_h > 0) {
        if (channels_[CHN_H264].enabled) {
            const int64_t pixels = static_cast<int64_t>(out_w) * out_h;
            if (out_w > 1920 || out_h > 1080 || pixels > 1920 * 1080) {
                MA_LOGW(TAG, "5MP JPEG %dx%d with H.264: CH2 cap, clamping to 1920x1080", out_w, out_h);
                out_w = 1920;
                out_h = 1080;
            }
        } else if (out_h > 1920 || (out_w % 64) != 0) {
            if (out_w % 64) {
                out_w = out_w & ~63;
            }
            if (out_h > 1920) {
                out_h = 1920;
            }
            MA_LOGW(TAG, "5MP JPEG aligned to %dx%d", out_w, out_h);
        }
    }
    channels_[chn].width      = out_w;
    channels_[chn].height     = out_h;
    channels_[chn].fps        = fps > 0 ? fps : channels_[chn].fps;
    channels_[chn].format     = format != MA_PIXEL_FORMAT_UNKNOWN ? format : channels_[chn].format;
    channels_[chn].enabled    = enabled;
    channels_[chn].configured = true;

    MA_LOGI(TAG, "config channel %d width %d height %d fps %d format %d", chn, channels_[chn].width, channels_[chn].height, channels_[chn].fps, channels_[chn].format);


    return MA_OK;
}

ma_err_t CameraNode::attach(int chn, MessageBox* msgbox) {
    Guard guard(mutex_);
    if (channels_[chn].enabled) {
        MA_LOGI(TAG, "attach %p to %d", msgbox, chn);
        channels_[chn].msgboxes.push_back(msgbox);
    }
    return MA_OK;
}


ma_err_t CameraNode::detach(int chn, MessageBox* msgbox) {
    Guard guard(mutex_);
    auto it = std::find(channels_[chn].msgboxes.begin(), channels_[chn].msgboxes.end(), msgbox);
    if (it != channels_[chn].msgboxes.end()) {
        enabled_ = false;
        MA_LOGI(TAG, "detach %p from %d", msgbox, chn);
        Thread::sleep(Tick::fromMilliseconds(50));  // skip last frame
        channels_[chn].msgboxes.erase(it);
        enabled_ = true;
    }
    return MA_OK;
}

REGISTER_NODE_SINGLETON("camera", CameraNode);


}  // namespace ma::node