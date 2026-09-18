#include <unistd.h>

#include <algorithm>
#include <cmath>

#include <opencv2/opencv.hpp>
namespace cv2 = cv;

#include "model.h"

namespace ma::node {

using namespace ma::engine;
using namespace ma::model;

static constexpr char TAG[] = "ma::node::model";

/* VPSS ASPECT_RATIO_AUTO: scale source into dest, pad the leftover. Boxes are
 * normalized in the square tensor; overlay dest is JPEG preview (or RAW). */
struct OverlayMap {
    float ax, bx, ay, by;
};

static OverlayMap makeLetterboxMap(int src_w, int src_h, int tensor_w, int tensor_h, int dest_w, int dest_h) {
    auto fit = [](int sw, int sh, int dw, int dh) {
        const float s  = std::min(dw / static_cast<float>(sw), dh / static_cast<float>(sh));
        const float px = (dw - sw * s) * 0.5f;
        const float py = (dh - sh * s) * 0.5f;
        return OverlayMap{s, px, s, py};
    };
    if (src_w <= 0 || src_h <= 0 || tensor_w <= 0 || tensor_h <= 0 || dest_w <= 0 || dest_h <= 0) {
        return {1.f, 0.f, 1.f, 0.f};
    }
    const OverlayMap t = fit(src_w, src_h, tensor_w, tensor_h);
    const OverlayMap d = fit(src_w, src_h, dest_w, dest_h);
    const float st     = (t.ax > 0.f) ? t.ax : 1.f;
    const float k      = d.ax / st;
    return {k, d.bx - t.bx * k, k, d.by - t.by * k};
}

static int16_t mapPxX(const OverlayMap& m, float nx, int tensor_w) {
    return static_cast<int16_t>(std::lround(nx * tensor_w * m.ax + m.bx));
}

static int16_t mapPxY(const OverlayMap& m, float ny, int tensor_h) {
    return static_cast<int16_t>(std::lround(ny * tensor_h * m.ay + m.by));
}

static int16_t mapPxW(const OverlayMap& m, float nw, int tensor_w) {
    return static_cast<int16_t>(std::lround(nw * tensor_w * m.ax));
}

static int16_t mapPxH(const OverlayMap& m, float nh, int tensor_h) {
    return static_cast<int16_t>(std::lround(nh * tensor_h * m.ay));
}

#define DEFAULT_MODEL "/userdata/Models/model.cvimodel"

ModelNode::ModelNode(std::string id)
    : Node("model", id),
      uri_(""),
      debug_(false),
      output_(false),
      trace_(false),
      counting_(false),
      count_(0),
      algorithm_(0),
      engine_(nullptr),
      model_(nullptr),
      thread_(nullptr),
      raw_frame_(1),
      jpeg_frame_(1),
      websocket_(true),
      transport_(nullptr),
      camera_(nullptr),
      preview_width_(640),
      preview_height_(640),
      preview_fps_(30) {}

ModelNode::~ModelNode() {
    onDestroy();
}
void ModelNode::threadEntry() {

    ma_err_t err    = MA_OK;
    videoFrame* raw  = nullptr;
    videoFrame* jpeg = nullptr;
    int32_t width    = 0;
    int32_t height   = 0;
    std::vector<std::string> labels;

    server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", "enabled"}, {"code", MA_OK}, {"data", enabled_.load()}}));

    while (started_) {

        if (!raw_frame_.fetch(reinterpret_cast<void**>(&raw), Tick::fromSeconds(2))) {
            continue;
        }
        if (debug_ && !jpeg_frame_.fetch(reinterpret_cast<void**>(&jpeg), Tick::fromSeconds(2))) {
            raw->release();
            continue;
        }

        if (!enabled_) {
            raw->release();
            if (debug_) {
                jpeg->release();
            }
            continue;
        }

        Thread::enterCritical();

        ma_tick_t start = Tick::current();

        json reply = json::object({{"type", MA_MSG_TYPE_EVT}, {"name", "invoke"}, {"code", MA_OK}, {"data", {{"count", ++count_}}}});
        if (debug_) {
            width  = jpeg->img.width;
            height = jpeg->img.height;
        } else if (camera_ != nullptr && camera_->option() == 3) {
            /* No preview JPEG: emit boxes in H.264 / RTSP space (2592×1944). */
            width  = camera_->channelWidth(CHN_H264);
            height = camera_->channelHeight(CHN_H264);
        } else {
            width  = raw->img.width;
            height = raw->img.height;
        }

        const int32_t tensor_w = raw->img.width;
        const int32_t tensor_h = raw->img.height;
        int32_t src_w          = tensor_w;
        int32_t src_h          = tensor_h;
        int32_t stream_w       = width;
        int32_t stream_h       = height;
        if (camera_ != nullptr) {
            camera_->sensorSize(src_w, src_h);
            stream_w = camera_->channelWidth(CHN_H264);
            stream_h = camera_->channelHeight(CHN_H264);
        }
        const OverlayMap overlay = makeLetterboxMap(src_w, src_h, tensor_w, tensor_h, width, height);

        reply["data"]["resolution"]        = json::array({width, height});
        reply["data"]["stream_resolution"] = json::array({stream_w, stream_h});

        ma_tensor_t tensor = {
            .size        = raw->img.size,
            .is_physical = raw->img.physical,
            .is_variable = false,
        };


        tensor.data.data = reinterpret_cast<void*>(raw->img.data);
        engine_->setInput(0, tensor);
        model_->setPreprocessDone([this, raw](void* ctx) { raw->release(); });

        reply["data"]["labels"] = json::array();

        if (model_->getOutputType() == MA_OUTPUT_TYPE_BBOX) {
            Detector* detector     = static_cast<Detector*>(model_);
            err                    = detector->run(nullptr);
            auto _results          = detector->getResults();
            reply["data"]["boxes"] = json::array();
            std::vector<ma_bbox_t> _bboxes;
            _bboxes.assign(_results.begin(), _results.end());
            if (trace_) {
                auto tracks             = tracker_.inplace_update(_bboxes);
                reply["data"]["tracks"] = tracks;
                for (int i = 0; i < _bboxes.size(); i++) {
                    reply["data"]["boxes"].push_back({mapPxX(overlay, _bboxes[i].x, tensor_w),
                                                      mapPxY(overlay, _bboxes[i].y, tensor_h),
                                                      mapPxW(overlay, _bboxes[i].w, tensor_w),
                                                      mapPxH(overlay, _bboxes[i].h, tensor_h),
                                                      static_cast<int8_t>(_bboxes[i].score * 100),
                                                      _bboxes[i].target});
                    if (labels_.size() > _bboxes[i].target) {
                        reply["data"]["labels"].push_back(labels_[_bboxes[i].target]);
                    } else {
                        reply["data"]["labels"].push_back(std::string("N/A-" + std::to_string(_bboxes[i].target)));
                    }
                    if (counting_) {
                        counter_.update(tracks[i], _bboxes[i].x * 100, _bboxes[i].y * 100);
                    }
                }
                if (counting_ && _bboxes.size() == 0) {
                    counter_.update(-1, 0, 0);
                }
            } else {
                for (int i = 0; i < _bboxes.size(); i++) {
                    reply["data"]["boxes"].push_back({mapPxX(overlay, _bboxes[i].x, tensor_w),
                                                      mapPxY(overlay, _bboxes[i].y, tensor_h),
                                                      mapPxW(overlay, _bboxes[i].w, tensor_w),
                                                      mapPxH(overlay, _bboxes[i].h, tensor_h),
                                                      static_cast<int8_t>(_bboxes[i].score * 100),
                                                      _bboxes[i].target});
                    if (labels_.size() > _bboxes[i].target) {
                        reply["data"]["labels"].push_back(labels_[_bboxes[i].target]);
                    } else {
                        reply["data"]["labels"].push_back(std::string("N/A-" + std::to_string(_bboxes[i].target)));
                    }
                }
            }
            if (counting_) {
                reply["data"]["counts"] = counter_.get();
                reply["data"]["lines"]  = json::array();
                reply["data"]["lines"].push_back(counter_.getSplitter());
            }
        } else if (model_->getOutputType() == MA_OUTPUT_TYPE_CLASS) {
            Classifier* classifier   = static_cast<Classifier*>(model_);
            err                      = classifier->run(nullptr);
            auto _results            = classifier->getResults();
            reply["data"]["classes"] = json::array();
            for (auto& result : _results) {
                reply["data"]["classes"].push_back({static_cast<int8_t>(result.score * 100), result.target});
                if (labels_.size() > result.target) {
                    reply["data"]["labels"].push_back(labels_[result.target]);
                } else {
                    reply["data"]["labels"].push_back(std::string("N/A-" + std::to_string(result.target)));
                }
            }
        } else if (model_->getOutputType() == MA_OUTPUT_TYPE_KEYPOINT) {
            PoseDetector* pose_detector = static_cast<PoseDetector*>(model_);
            err                         = pose_detector->run(nullptr);
            auto _results               = pose_detector->getResults();
            reply["data"]["keypoints"]  = json::array();
            for (auto& result : _results) {
                json pts = json::array();
                for (auto& pt : result.pts) {
                    pts.push_back({mapPxX(overlay, pt.x, tensor_w), mapPxY(overlay, pt.y, tensor_h), static_cast<int8_t>(pt.z * 100)});
                }
                json box = {mapPxX(overlay, result.box.x, tensor_w),
                            mapPxY(overlay, result.box.y, tensor_h),
                            mapPxW(overlay, result.box.w, tensor_w),
                            mapPxH(overlay, result.box.h, tensor_h),
                            static_cast<int8_t>(result.box.score * 100),
                            result.box.target};
                if (labels_.size() > result.box.target) {
                    reply["data"]["labels"].push_back(labels_[result.box.target]);
                } else {
                    reply["data"]["labels"].push_back(std::string("N/A-" + std::to_string(result.box.target)));
                }
                reply["data"]["keypoints"].push_back({box, pts});
            }
        } else if (model_->getOutputType() == MA_OUTPUT_TYPE_SEGMENT) {
            Segmentor* segmentor      = static_cast<Segmentor*>(model_);
            err                       = segmentor->run(nullptr);
            auto _results             = segmentor->getResults();
            reply["data"]["segments"] = json::array();
            for (auto& result : _results) {
                json box = {mapPxX(overlay, result.box.x, tensor_w),
                            mapPxY(overlay, result.box.y, tensor_h),
                            mapPxW(overlay, result.box.w, tensor_w),
                            mapPxH(overlay, result.box.h, tensor_h),
                            static_cast<int8_t>(result.box.score * 100),
                            result.box.target};
                if (labels_.size() > result.box.target) {
                    reply["data"]["labels"].push_back(labels_[result.box.target]);
                } else {
                    reply["data"]["labels"].push_back(std::string("N/A-" + std::to_string(result.box.target)));
                }

                cv2::Mat maskImage(result.mask.height, result.mask.width, CV_8UC1, cv2::Scalar(0));

                for (int i = 0; i < result.mask.height; ++i) {
                    for (int j = 0; j < result.mask.width; ++j) {
                        if (result.mask.data[i * result.mask.width / 8 + j / 8] & (1 << (j % 8))) {
                            maskImage.at<uchar>(i, j) = 255;
                        }
                    }
                }

                std::vector<std::vector<cv2::Point>> contours;
                std::vector<cv2::Vec4i> hierarchy;
                cv2::findContours(maskImage, contours, hierarchy, cv2::RETR_EXTERNAL, cv2::CHAIN_APPROX_SIMPLE);

                auto maxContour = std::max_element(contours.begin(), contours.end(), [](std::vector<cv2::Point>& a, std::vector<cv2::Point>& b) { return a.size() < b.size(); });

                std::vector<uint16_t> contour;
                if (maxContour != contours.end()) {
                    contour.reserve(maxContour->size() * 2);
                    const float inv_mw = (result.mask.width > 0) ? (1.f / result.mask.width) : 0.f;
                    const float inv_mh = (result.mask.height > 0) ? (1.f / result.mask.height) : 0.f;
                    for (auto& c : *maxContour) {
                        contour.push_back(static_cast<uint16_t>(mapPxX(overlay, c.x * inv_mw, tensor_w)));
                        contour.push_back(static_cast<uint16_t>(mapPxY(overlay, c.y * inv_mh, tensor_h)));
                    }
                }
                reply["data"]["segments"].push_back({box, contour});
            }
        }


        const auto _perf = model_->getPerf();

        reply["data"]["perf"].push_back({_perf.preprocess, _perf.inference, _perf.postprocess});

        if (debug_) {
            char* base64   = new char[4 * ((jpeg->img.size + 2) / 3 + 2)];
            int base64_len = 4 * ((jpeg->img.size + 2) / 3 + 2);
            ma::utils::base64_encode(jpeg->img.data, jpeg->img.size, base64, &base64_len);
            reply["data"]["image"] = std::string(base64, base64_len);
            delete[] base64;
            jpeg->release();
        } else {
            reply["data"]["image"] = "";
        }

        if (websocket_) {
            transport_->send(reinterpret_cast<const char*>(reply.dump().c_str()), reply.dump().size());
        }
        if (!output_) {
            reply["data"]["image"] = "";
        }
        server_->response(id_, reply);

        ma_tick_t end = Tick::current();
        if (debug_ && (end - start < Tick::fromMilliseconds(100))) {
            Thread::sleep(Tick::fromMilliseconds(100) - (end - start));
        }

        Thread::exitCritical();
    }
}

void ModelNode::threadEntryStub(void* obj) {
    reinterpret_cast<ModelNode*>(obj)->threadEntry();
}

ma_err_t ModelNode::onCreate(const json& config) {
    ma_err_t err = MA_OK;
    Guard guard(mutex_);

    labels_.clear();

    if (config.contains("algorithm") && config["algorithm"].is_number_integer()) {
        algorithm_ = config["algorithm"].get<int>();
    }

    if (config.contains("uri") && config["uri"].is_string()) {
        uri_ = config["uri"].get<std::string>();
    }

    if (uri_.empty()) {
        uri_ = DEFAULT_MODEL;
    }

    if (access(uri_.c_str(), R_OK) != 0) {
        MA_THROW(Exception(MA_ENOENT, "Model file not found " + uri_));
    }

    // find model.json
    size_t pos = uri_.find_last_of(".");
    if (pos != std::string::npos) {
        std::string path = uri_.substr(0, pos) + ".json";
        if (access(path.c_str(), R_OK) == 0) {
            std::ifstream ifs(path);
            if (!ifs.is_open()) {
                MA_THROW(Exception(MA_EINVAL, "Model config file not found " + path));
            }
            ifs >> info_;
            if (info_.is_object()) {
                if (info_.contains("classes") && info_["classes"].is_array()) {
                    labels_ = info_["classes"].get<std::vector<std::string>>();
                }
            }
        }
    }

    // override classes
    if (labels_.size() == 0 && config.contains("labels") && config["labels"].is_array() && config["labels"].size() > 0) {
        labels_ = config["labels"].get<std::vector<std::string>>();
    }

    MA_TRY {
        engine_ = new EngineDefault();

        if (engine_ == nullptr) {
            MA_THROW(Exception(MA_ENOMEM, "Engine init failed"));
        }
        if (engine_->init() != MA_OK) {
            MA_THROW(Exception(MA_EINVAL, "Engine init failed"));
        }
        if (engine_->load(uri_) != MA_OK) {
            MA_THROW(Exception(MA_EINVAL, "Engine load failed"));
        }
        model_ = ModelFactory::create(engine_, algorithm_);
        if (model_ == nullptr) {
            MA_THROW(Exception(MA_ENOTSUP, "Model not supported"));
        }

        MA_LOGI(TAG, "model: %s %s", uri_.c_str(), model_->getName());
        {  // extra config
            if (config.contains("tscore")) {
                model_->setConfig(MA_MODEL_CFG_OPT_THRESHOLD, config["tscore"].get<float>());
            }
            if (config.contains("tiou")) {
                model_->setConfig(MA_MODEL_CFG_OPT_NMS, config["tiou"].get<float>());
            }
            if (config.contains("topk")) {
                model_->setConfig(MA_MODEL_CFG_OPT_TOPK, config["tiou"].get<float>());
            }
            if (config.contains("debug")) {
                output_ = config["debug"].get<bool>();
            }
            if (config.contains("websocket") && config["websocket"].is_boolean()) {
                websocket_ = config["websocket"].get<bool>();
            }
            if (websocket_ || output_) {
                debug_ = true;
            }
            if (config.contains("trace")) {
                trace_ = config["trace"].get<bool>();
            }
            if (config.contains("counting")) {
                counting_ = config["counting"].get<bool>();
            }
            if (config.contains("splitter") && config["splitter"].is_array()) {
                counter_.setSplitter(config["splitter"].get<std::vector<int16_t>>());
            }
            if (config.contains("previewResolution") && config["previewResolution"].is_string()) {
                // if previewResolution is auto
                if (config["previewResolution"].get<std::string>() == "auto") {
                    preview_width_  = -1;
                    preview_height_ = -1;
                } else {
                    std::string resolution = config["previewResolution"].get<std::string>();
                    size_t pos             = resolution.find('x');
                    if (pos != std::string::npos) {
                        preview_width_  = std::stoi(resolution.substr(0, pos));
                        preview_height_ = std::stoi(resolution.substr(pos + 1));
                    }
                }
            }
            if (config.contains("previewFps") && config["previewFps"].is_number_integer()) {
                preview_fps_ = config["previewFps"].get<int32_t>();
            }
        }

        if (websocket_) {
            TransportWebSocket::Config ws_config = {.port = 8090};
            MA_STORAGE_GET_POD(server_->getStorage(), MA_STORAGE_KEY_WS_PORT, ws_config.port, 8090);
            transport_ = new TransportWebSocket();
            if (transport_ != nullptr) {
                transport_->init(&ws_config);
            }
            MA_LOGI(TAG, "camera websocket server started on port %d", ws_config.port);
        } else {
            transport_ = nullptr;
        }

        thread_ = new Thread((type_ + "#" + id_).c_str(), &ModelNode::threadEntryStub, this);
        if (thread_ == nullptr) {
            MA_THROW(Exception(MA_ENOMEM, "Not enough memory"));
        }
    }
    MA_CATCH(ma::Exception & e) {
        if (engine_ != nullptr) {
            delete engine_;
            engine_ = nullptr;
        }
        if (model_ != nullptr) {
            delete model_;
            model_ = nullptr;
        }
        if (thread_ != nullptr) {
            delete thread_;
            thread_ = nullptr;
        }
        MA_THROW(e);
    }
    MA_CATCH(std::exception & e) {
        if (engine_ != nullptr) {
            delete engine_;
            engine_ = nullptr;
        }
        if (model_ != nullptr) {
            delete model_;
            model_ = nullptr;
        }
        if (thread_ != nullptr) {
            delete thread_;
            thread_ = nullptr;
        }
        MA_THROW(Exception(MA_EINVAL, e.what()));
    }

    created_ = true;

    server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", "create"}, {"code", MA_OK}, {"data", info_}}));

    return MA_OK;
}

ma_err_t ModelNode::onControl(const std::string& control, const json& data) {
    Guard guard(mutex_);
    ma_err_t err = MA_OK;
    if (control == "config") {
        if (data.contains("tscore") && data["tscore"].is_number_float()) {
            model_->setConfig(MA_MODEL_CFG_OPT_THRESHOLD, data["tscore"].get<float>());
        }
        if (data.contains("tiou") && data["tiou"].is_number_float()) {
            model_->setConfig(MA_MODEL_CFG_OPT_NMS, data["tiou"].get<float>());
        }
        if (data.contains("topk") && data["topk"].is_number_integer()) {
            model_->setConfig(MA_MODEL_CFG_OPT_TOPK, data["topk"].get<int32_t>());
        }
        if (data.contains("debug") && data["debug"].is_boolean()) {
            debug_ = data["debug"].get<bool>();
        }
        if (data.contains("trace") && data["trace"].is_boolean()) {
            trace_ = data["trace"].get<bool>();
            tracker_.clear();
        }
        if (data.contains("counting") && data["counting"].is_boolean()) {
            counting_ = data["counting"].get<bool>();
            counter_.clear();
        }
        if (data.contains("splitter") && data["splitter"].is_array()) {
            counter_.setSplitter(data["splitter"].get<std::vector<int16_t>>());
        }
        server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", control}, {"code", MA_OK}, {"data", data}}));
    } else if (control == "enabled" && data.is_boolean()) {
        bool enabled = data.get<bool>();
        if (enabled_.load() != enabled) {
            enabled_.store(enabled);
        }
        server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", control}, {"code", MA_OK}, {"data", enabled_.load()}}));
    } else {
        server_->response(id_, json::object({{"type", MA_MSG_TYPE_RESP}, {"name", control}, {"code", MA_ENOTSUP}, {"data", "Not supported"}}));
    }
    return MA_OK;
}

ma_err_t ModelNode::onDestroy() {
    Guard guard(mutex_);

    if (!created_) {
        return MA_OK;
    }

    onStop();

    if (thread_ != nullptr) {
        delete thread_;
        thread_ = nullptr;
    }
    if (engine_ != nullptr) {
        delete engine_;
        engine_ = nullptr;
    }
    if (model_ != nullptr) {
        delete model_;
        model_ = nullptr;
    }
    if (camera_ != nullptr) {
        camera_->detach(CHN_RAW, &raw_frame_);
        if (debug_) {
            camera_->detach(CHN_JPEG, &jpeg_frame_);
        }
    }
    if (transport_ != nullptr) {
        transport_->deInit();
        delete transport_;
        transport_ = nullptr;
    }


    created_ = false;

    return MA_OK;
}

ma_err_t ModelNode::onStart() {
    Guard guard(mutex_);
    if (started_) {
        return MA_OK;
    }

    const ma_img_t* img = static_cast<const ma_img_t*>(model_->getInput());

    MA_LOGI(TAG, "onStart model: %s(%s) width %d height %d format %d", type_.c_str(), id_.c_str(), img->width, img->height, img->format);

    for (auto& dep : dependencies_) {
        if (dep.second->type() == "camera") {
            camera_ = static_cast<CameraNode*>(dep.second);
            break;
        }
    }

    if (camera_ == nullptr) {
        MA_THROW(Exception(MA_ENOTSUP, "No camera node found"));
        return MA_ENOTSUP;
    }

    camera_->config(CHN_RAW, img->width, img->height, preview_fps_, img->format);
    camera_->attach(CHN_RAW, &raw_frame_);
    if (debug_) {
        if (preview_width_ == -1 || preview_height_ == -1) {
            preview_width_  = img->width;
            preview_height_ = img->height;
        }
        if (camera_->option() == 3) {
            if (preview_fps_ > 15) {
                preview_fps_ = 15;
            }
            const int64_t pixels = static_cast<int64_t>(preview_width_) * preview_height_;
            if (preview_width_ >= 2592 || preview_height_ >= 1944 || pixels > 1280 * 960) {
                MA_LOGW(TAG, "5MP JPEG preview %dx%d too large, using 640x640", preview_width_, preview_height_);
                preview_width_  = 640;
                preview_height_ = 640;
            }
        }
        camera_->config(CHN_JPEG, preview_width_, preview_height_, preview_fps_, MA_PIXEL_FORMAT_JPEG);
        camera_->attach(CHN_JPEG, &jpeg_frame_);
        MA_LOGI(TAG, "overlay jpeg %dx%d@%d camera_option=%d", preview_width_, preview_height_, preview_fps_, camera_->option());
    }

    MA_LOGI(TAG, "start model: %s(%s)", type_.c_str(), id_.c_str());
    started_ = true;

    thread_->start(this);

    return MA_OK;
}

ma_err_t ModelNode::onStop() {
    Guard guard(mutex_);
    if (!started_) {
        return MA_OK;
    }
    started_ = false;

    if (thread_ != nullptr) {
        thread_->join();
    }

    if (camera_ != nullptr) {
        camera_->detach(CHN_RAW, &raw_frame_);
        if (debug_) {
            camera_->detach(CHN_JPEG, &jpeg_frame_);
        }
        camera_ = nullptr;
    }
    return MA_OK;
}

REGISTER_NODE_SINGLETON("model", ModelNode);

}  // namespace ma::node