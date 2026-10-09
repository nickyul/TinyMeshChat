#include "screen_audio_factory.h"

#include <algorithm>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/make_ref_counted.h>

namespace tmc {
namespace {

std::vector<webrtc::AudioCodecSpec> stereoOpus(std::vector<webrtc::AudioCodecSpec> codecs) {
    std::erase_if(codecs, [](const auto& codec) { return codec.format.name != "opus"; });
    for (auto& codec : codecs) {
        codec.format.parameters["stereo"] = "1";
        codec.format.parameters["sprop-stereo"] = "1";
        codec.format.parameters["maxaveragebitrate"] = "128000";
        codec.info.num_channels = 2;
    }
    return codecs;
}

class EncoderFactory : public webrtc::AudioEncoderFactory {
    webrtc::scoped_refptr<webrtc::AudioEncoderFactory> builtin_{
        webrtc::CreateBuiltinAudioEncoderFactory()};

public:
    std::vector<webrtc::AudioCodecSpec> GetSupportedEncoders() override {
        return stereoOpus(builtin_->GetSupportedEncoders());
    }

    std::optional<webrtc::AudioCodecInfo>
    QueryAudioEncoder(const webrtc::SdpAudioFormat& format) override {
        return builtin_->QueryAudioEncoder(format);
    }

    std::unique_ptr<webrtc::AudioEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpAudioFormat& format,
                                                 Options options) override {
        return builtin_->Create(env, format, options);
    }
};

class DecoderFactory : public webrtc::AudioDecoderFactory {
    webrtc::scoped_refptr<webrtc::AudioDecoderFactory> builtin_{
        webrtc::CreateBuiltinAudioDecoderFactory()};

public:
    std::vector<webrtc::AudioCodecSpec> GetSupportedDecoders() override {
        return stereoOpus(builtin_->GetSupportedDecoders());
    }

    bool IsSupportedDecoder(const webrtc::SdpAudioFormat& format) override {
        return builtin_->IsSupportedDecoder(format);
    }

    std::unique_ptr<webrtc::AudioDecoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpAudioFormat& format) override {
        return builtin_->Create(env, format);
    }
};

} // namespace

webrtc::scoped_refptr<webrtc::AudioEncoderFactory> createScreenAudioEncoderFactory() {
    return webrtc::make_ref_counted<EncoderFactory>();
}

webrtc::scoped_refptr<webrtc::AudioDecoderFactory> createScreenAudioDecoderFactory() {
    return webrtc::make_ref_counted<DecoderFactory>();
}

} // namespace tmc
