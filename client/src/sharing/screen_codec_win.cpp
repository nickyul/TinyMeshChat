#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <api/video/i420_buffer.h>
#include <codecapi.h>
#include <libyuv.h>
#include <map>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <optional>
#include <windows.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include "screen_codec_bitstream.h"
#include "screen_codec_factory.h"

namespace tmc {
namespace {

using Microsoft::WRL::ComPtr;

struct ComScope {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};

    ~ComScope() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

bool mediaFoundation() {
    // MF's process lifetime outlives every peer, including asynchronous hardware MFTs.
    static const bool ready = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    return ready;
}

void codecValue(ICodecAPI* codec, const GUID& key, ULONG value) {
    if (!codec) {
        return;
    }
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_UI4;
    variant.ulVal = value;
    codec->SetValue(&key, &variant);
}

void codecBool(ICodecAPI* codec, const GUID& key, bool value) {
    if (!codec) {
        return;
    }
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_BOOL;
    variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    codec->SetValue(&key, &variant);
}

struct Transform {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<ICodecAPI> controls;
    bool hardware{}, asynchronous{};
    int inputTokens{}, outputTokens{};

    bool createEncoder(bool tryHardware) {
        if (!mediaFoundation()) {
            return false;
        }
        if (tryHardware) {
            IMFActivate** activations{};
            UINT32 count{};
            MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12},
                output{MFMediaType_Video, MFVideoFormat_H264};
            if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input,
                                    &output, &activations, &count))) {
                for (UINT32 i = 0; i < count; ++i) {
                    if (!mft && SUCCEEDED(activations[i]->ActivateObject(IID_PPV_ARGS(&mft)))) {
                        hardware = true;
                    }
                    activations[i]->Release();
                }
                CoTaskMemFree(activations);
            }
        }
        if (!mft && FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&mft)))) {
            return false;
        }
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(mft->GetAttributes(&attributes))) {
            UINT32 async{};
            attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
            asynchronous = async;
            if (asynchronous && FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) {
                return false;
            }
        }
        if (asynchronous && FAILED(mft.As(&events))) {
            return false;
        }
        mft.As(&controls);
        return true;
    }

    bool createDecoder() {
        return mediaFoundation() &&
               SUCCEEDED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&mft)));
    }

    void poll() {
        if (!events) {
            return;
        }
        for (int i = 0; i < 32; ++i) {
            ComPtr<IMFMediaEvent> event;
            if (FAILED(events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event))) {
                break;
            }
            MediaEventType type{};
            event->GetType(&type);
            if (type == METransformNeedInput) {
                ++inputTokens;
            } else if (type == METransformHaveOutput) {
                ++outputTokens;
            }
        }
    }

    void start() {
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        poll();
    }

    void close() {
        if (mft) {
            mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        events.Reset();
        controls.Reset();
        mft.Reset();
        inputTokens = outputTokens = 0;
        hardware = asynchronous = false;
    }

    HRESULT output(ComPtr<IMFSample>& sample) {
        poll();
        if (asynchronous && outputTokens <= 0) {
            return MF_E_TRANSFORM_NEED_MORE_INPUT;
        }
        MFT_OUTPUT_STREAM_INFO info{};
        auto hr = mft->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) {
            return hr;
        }
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(hr = MFCreateSample(&sample))) {
                return hr;
            }
            if (FAILED(hr = MFCreateAlignedMemoryBuffer(std::max<DWORD>(info.cbSize, 1024),
                                                        info.cbAlignment ? info.cbAlignment - 1 : 0,
                                                        &buffer))) {
                return hr;
            }
            sample->AddBuffer(buffer.Get());
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = sample.Get();
        DWORD status{};
        hr = mft->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) {
            output.pEvents->Release();
        }
        if (!sample && output.pSample) {
            sample.Attach(output.pSample);
        }
        if (asynchronous && SUCCEEDED(hr)) {
            --outputTokens;
        }
        return hr;
    }
};

ComPtr<IMFMediaType> videoType(const GUID& subtype, int width, int height, int fps) {
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    if (!type) {
        return {};
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return type;
}

class WinEncoder final : public webrtc::VideoEncoder {
    Transform transform_;
    webrtc::EncodedImageCallback* callback_{};
    int width_{}, height_{}, fps_{30}, bitrate_{};
    bool comInitialized_{};
    std::map<LONGLONG, uint32_t> timestamps_;

    bool open(bool hardware) {
        if (!transform_.createEncoder(hardware)) {
            return false;
        }
        codecBool(transform_.controls.Get(), CODECAPI_AVLowLatencyMode, true);
        codecValue(transform_.controls.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        codecValue(transform_.controls.Get(), CODECAPI_AVEncCommonRateControlMode,
                   eAVEncCommonRateControlMode_CBR);
        codecValue(transform_.controls.Get(), CODECAPI_AVEncCommonMeanBitRate,
                   std::max(bitrate_, 100000));
        auto output = videoType(MFVideoFormat_H264, width_, height_, fps_),
             input = videoType(MFVideoFormat_NV12, width_, height_, fps_);
        if (!output || !input) {
            return false;
        }
        output->SetUINT32(MF_MT_AVG_BITRATE, std::max(bitrate_, 100000));
        output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        output->SetUINT32(MF_MT_MPEG2_LEVEL, 52);
        if (FAILED(transform_.mft->SetOutputType(0, output.Get(), 0)) ||
            FAILED(transform_.mft->SetInputType(0, input.Get(), 0))) {
            return false;
        }
        transform_.start();
        return true;
    }

    int drain() {
        for (int i = 0; i < 8; ++i) {
            ComPtr<IMFSample> sample;
            auto hr = transform_.output(sample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return WEBRTC_VIDEO_CODEC_OK;
            }
            if (FAILED(hr) || !sample) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            BYTE* data{};
            DWORD size{};
            if (FAILED(buffer->Lock(&data, nullptr, &size))) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            LONGLONG time{};
            sample->GetSampleTime(&time);
            UINT32 key{};
            sample->GetUINT32(MFSampleExtension_CleanPoint, &key);
            const auto found = timestamps_.find(time);
            if (found != timestamps_.end()) {
                // Media Foundation H.264 outputs Annex B. Reject foreign formats rather than
                // sending corrupt RTP.
                if (h264Nalus({data, size}).empty()) {
                    buffer->Unlock();
                    return WEBRTC_VIDEO_CODEC_ERROR;
                }
                emitH264(callback_, {data, size}, found->second, width_, height_, key != 0);
                timestamps_.erase(found);
            }
            buffer->Unlock();
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

public:
    ~WinEncoder() override {
        Release();
    }

    int InitEncode(const webrtc::VideoCodec* codec, const Settings&) override {
        ComScope com;
        Release();
        if (!codec || !codec->width || !codec->height) {
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        width_ = codec->width;
        height_ = codec->height;
        fps_ = codec->maxFramerate;
        bitrate_ = codec->startBitrate * 1000;
        if (!open(true)) {
            transform_.close();
            if (!open(false)) {
                transform_.close();
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        ComScope com;
        transform_.close();
        timestamps_.clear();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* types) override {
        ComScope com;
        if (!transform_.mft) {
            return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        }
        if (drain() != WEBRTC_VIDEO_CODEC_OK) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (!bitrate_) {
            return WEBRTC_VIDEO_CODEC_OK;
        }
        const auto i420 = frame.video_frame_buffer()->ToI420();
        if (!i420) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (width_ != i420->width() || height_ != i420->height()) {
            transform_.close();
            timestamps_.clear();
            width_ = i420->width();
            height_ = i420->height();
            if (!open(true)) {
                transform_.close();
                if (!open(false)) {
                    return WEBRTC_VIDEO_CODEC_ERROR;
                }
            }
        }
        transform_.poll();
        if ((transform_.asynchronous && transform_.inputTokens <= 0) || timestamps_.size() >= 4) {
            if (callback_) {
                callback_->OnFrameDropped(frame.rtp_timestamp(), 0, true);
            }
            return WEBRTC_VIDEO_CODEC_OK;
        }
        if (types && std::find(types->begin(), types->end(),
                               webrtc::VideoFrameType::kVideoFrameKey) != types->end()) {
            codecValue(transform_.controls.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        }
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        const DWORD size = width_ * height_ * 3 / 2;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(size, &buffer))) {
            return WEBRTC_VIDEO_CODEC_MEMORY;
        }
        BYTE* data{};
        if (FAILED(buffer->Lock(&data, nullptr, nullptr))) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        const auto converted = libyuv::I420ToNV12(
            i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(),
            i420->StrideV(), data, width_, data + width_ * height_, width_, width_, height_);
        buffer->Unlock();
        if (converted) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        buffer->SetCurrentLength(size);
        sample->AddBuffer(buffer.Get());
        const LONGLONG time = frame.timestamp_us() * 10;
        sample->SetSampleTime(time);
        sample->SetSampleDuration(10000000 / std::max(fps_, 1));
        const auto hr = transform_.mft->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (callback_) {
                callback_->OnFrameDropped(frame.rtp_timestamp(), 0, true);
            }
            return drain();
        }
        if (FAILED(hr)) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (transform_.asynchronous) {
            --transform_.inputTokens;
        }
        timestamps_[time] = frame.rtp_timestamp();
        return drain();
    }

    void SetRates(const RateControlParameters& rates) override {
        ComScope com;
        bitrate_ = rates.bitrate.get_sum_bps();
        if (rates.framerate_fps > 0) {
            fps_ = rates.framerate_fps;
        }
        codecValue(transform_.controls.Get(), CODECAPI_AVEncCommonMeanBitRate,
                   std::max(bitrate_, 100000));
    }

    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = transform_.hardware ? "Media Foundation H264 hardware"
                                                       : "Media Foundation H264 software";
        info.is_hardware_accelerated = transform_.hardware;
        info.supports_native_handle = false;
        info.requested_resolution_alignment = 2;
        return info;
    }
};

class WinDecoder final : public webrtc::VideoDecoder {
    Transform transform_;
    webrtc::DecodedImageCallback* callback_{};
    ComPtr<IMFMediaType> outputType_;

    struct Metadata {
        uint32_t timestamp;
        int64_t renderTime;
    };

    std::map<LONGLONG, Metadata> timestamps_;
    LONGLONG sequence_{};
    bool comInitialized_{};

    bool outputType() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(transform_.mft->GetOutputAvailableType(0, i, &type))) {
                return false;
            }
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12 &&
                SUCCEEDED(transform_.mft->SetOutputType(0, type.Get(), 0))) {
                outputType_ = type;
                return true;
            }
        }
    }

    int drain() {
        for (int i = 0; i < 8; ++i) {
            ComPtr<IMFSample> sample;
            auto hr = transform_.output(sample);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!outputType()) {
                    return WEBRTC_VIDEO_CODEC_ERROR;
                }
                continue;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return WEBRTC_VIDEO_CODEC_OK;
            }
            if (FAILED(hr) || !sample || !outputType_) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            UINT32 width{}, height{}, stride{};
            MFGetAttributeSize(outputType_.Get(), MF_MT_FRAME_SIZE, &width, &height);
            outputType_->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
            if (!stride) {
                stride = width;
            }
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            BYTE* data{};
            DWORD size{};
            if (FAILED(buffer->Lock(&data, nullptr, &size))) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            if (!width || !height || width > 4096 || height > 4096 || stride < width ||
                size < size_t(stride) * height * 3 / 2) {
                buffer->Unlock();
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            auto i420 = webrtc::I420Buffer::Create(width, height);
            const auto converted = libyuv::NV12ToI420(
                data, stride, data + stride * height, stride, i420->MutableDataY(), i420->StrideY(),
                i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(), i420->StrideV(), width,
                height);
            buffer->Unlock();
            if (converted) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            LONGLONG time{};
            sample->GetSampleTime(&time);
            auto found = timestamps_.find(time);
            if (found != timestamps_.end()) {
                auto frame = webrtc::VideoFrame::Builder()
                                 .set_video_frame_buffer(i420)
                                 .set_rtp_timestamp(found->second.timestamp)
                                 .set_timestamp_ms(found->second.renderTime)
                                 .build();
                if (callback_) {
                    callback_->Decoded(frame);
                }
                timestamps_.erase(found);
            }
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

public:
    ~WinDecoder() override {
        Release();
    }

    bool Configure(const Settings& settings) override {
        ComScope com;
        Release();
        comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (!transform_.createDecoder()) {
            return false;
        }
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform_.mft->GetAttributes(&attributes))) {
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        auto input =
            videoType(MFVideoFormat_H264, std::max(settings.max_render_resolution().Width(), 1280),
                      std::max(settings.max_render_resolution().Height(), 720), 30);
        if (!input || FAILED(transform_.mft->SetInputType(0, input.Get(), 0))) {
            return false;
        }
        // Actual dimensions are announced through MF_E_TRANSFORM_STREAM_CHANGE after the first SPS.
        outputType();
        transform_.start();
        return true;
    }

    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        ComScope com;
        transform_.close();
        outputType_.Reset();
        timestamps_.clear();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Decode(const webrtc::EncodedImage& image, int64_t renderTime) override {
        ComScope com;
        if (!transform_.mft || image.size() > 8 * 1024 * 1024) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (drain() != WEBRTC_VIDEO_CODEC_OK) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (timestamps_.size() >= 8) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateSample(&sample)) ||
            FAILED(MFCreateMemoryBuffer(image.size(), &buffer))) {
            return WEBRTC_VIDEO_CODEC_MEMORY;
        }
        BYTE* data{};
        if (FAILED(buffer->Lock(&data, nullptr, nullptr))) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        memcpy(data, image.data(), image.size());
        buffer->Unlock();
        buffer->SetCurrentLength(image.size());
        sample->AddBuffer(buffer.Get());
        const auto time = ++sequence_ * 333333;
        sample->SetSampleTime(time);
        sample->SetSampleDuration(333333);
        sample->SetUINT32(MFSampleExtension_CleanPoint,
                          image._frameType == webrtc::VideoFrameType::kVideoFrameKey);
        if (FAILED(transform_.mft->ProcessInput(0, sample.Get(), 0))) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        timestamps_[time] = {image.RtpTimestamp(), renderTime};
        return drain();
    }

    DecoderInfo GetDecoderInfo() const override {
        DecoderInfo info;
        info.implementation_name = "Media Foundation H264";
        info.is_hardware_accelerated = false;
        return info;
    }
};

} // namespace

NativeCodecSupport probeNativeScreenCodec() {
    static const auto support = [] {
        ComScope com;
        Transform encoder, decoder;
        const bool enc = encoder.createEncoder(true), dec = decoder.createDecoder();
        const auto result = NativeCodecSupport{enc, dec, encoder.hardware};
        encoder.close();
        decoder.close();
        return result;
    }();
    return support;
}

std::unique_ptr<webrtc::VideoEncoder> createNativeScreenEncoder() {
    return std::make_unique<WinEncoder>();
}

std::unique_ptr<webrtc::VideoDecoder> createNativeScreenDecoder() {
    return std::make_unique<WinDecoder>();
}

} // namespace tmc
