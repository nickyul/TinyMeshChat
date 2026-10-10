#include "screen_codec_factory.h"

#include "tmc/core/logger.h"

#include <algorithm>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>

namespace tmc {
namespace {

webrtc::SdpVideoFormat h264() {
    return {"H264",
            {{"profile-level-id", "42e034"},
             {"level-asymmetry-allowed", "1"},
             {"packetization-mode", "1"}}};
}

class EncoderFactory final : public webrtc::VideoEncoderFactory {
    NativeCodecSupport native_{probeNativeScreenCodec()};
    std::unique_ptr<webrtc::VideoEncoderFactory> software_{
        webrtc::CreateBuiltinVideoEncoderFactory()};

public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;
        if (native_.encoder && native_.decoder) {
            formats.push_back(h264());
        }
        for (const auto& format : software_->GetSupportedFormats()) {
            if (format.name == "VP8") {
                formats.push_back(format);
            }
        }
        return formats;
    }

    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                   std::optional<std::string> mode) const override {
        if (format.name == "H264") {
            return {native_.encoder && native_.decoder &&
                        format.IsCodecInList(GetSupportedFormats()) && !mode,
                    native_.hardware};
        }
        return software_->QueryCodecSupport(format, mode);
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        auto encoder =
            format.name == "H264" ? createNativeScreenEncoder() : software_->Create(env, format);
        Logger::instance().trace("screen_codec", QString("create_encoder codec=%1 success=%2")
                                                     .arg(QString::fromStdString(format.name))
                                                     .arg(bool(encoder)));
        return encoder;
    }
};

class DecoderFactory final : public webrtc::VideoDecoderFactory {
    NativeCodecSupport native_{probeNativeScreenCodec()};
    std::unique_ptr<webrtc::VideoDecoderFactory> software_{
        webrtc::CreateBuiltinVideoDecoderFactory()};

public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;
        if (native_.decoder && native_.encoder) {
            formats.push_back(h264());
        }
        for (const auto& format : software_->GetSupportedFormats()) {
            if (format.name == "VP8") {
                formats.push_back(format);
            }
        }
        return formats;
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        return format.name == "H264" ? createNativeScreenDecoder() : software_->Create(env, format);
    }
};

class EncoderSelector final : public webrtc::VideoEncoderFactory::EncoderSelectorInterface {
    bool software_{};

public:
    void OnCurrentEncoder(const webrtc::SdpVideoFormat& format) override {
        software_ = format.name == "VP8";
    }

    std::optional<webrtc::SdpVideoFormat> OnAvailableBitrate(const webrtc::DataRate&) override {
        return std::nullopt;
    }

    std::optional<webrtc::SdpVideoFormat> OnEncoderBroken() override {
        Logger::instance().trace("screen_codec", "encoder_broken");
        if (software_) {
            return std::nullopt;
        }
        software_ = true;
        return webrtc::SdpVideoFormat("VP8");
    }
};

} // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> createScreenEncoderFactory() {
    return std::make_unique<EncoderFactory>();
}

std::unique_ptr<webrtc::VideoDecoderFactory> createScreenDecoderFactory() {
    return std::make_unique<DecoderFactory>();
}

std::unique_ptr<webrtc::VideoEncoderFactory::EncoderSelectorInterface>
createScreenEncoderSelector() {
    return std::make_unique<EncoderSelector>();
}
#if !defined(_WIN32) && !defined(__APPLE__)
NativeCodecSupport probeNativeScreenCodec() {
    return {};
}

std::unique_ptr<webrtc::VideoEncoder> createNativeScreenEncoder() {
    return {};
}

std::unique_ptr<webrtc::VideoDecoder> createNativeScreenDecoder() {
    return {};
}
#endif

} // namespace tmc
