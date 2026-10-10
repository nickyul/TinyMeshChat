#pragma once

#include <api/video_codecs/video_decoder_factory.h>
#include <api/video_codecs/video_encoder_factory.h>
#include <memory>

namespace tmc {

std::unique_ptr<webrtc::VideoEncoderFactory> createScreenEncoderFactory();
std::unique_ptr<webrtc::VideoDecoderFactory> createScreenDecoderFactory();
std::unique_ptr<webrtc::VideoEncoderFactory::EncoderSelectorInterface>
createScreenEncoderSelector();

struct NativeCodecSupport {
    bool encoder{}, decoder{}, hardware{};
};

NativeCodecSupport probeNativeScreenCodec();
std::unique_ptr<webrtc::VideoEncoder> createNativeScreenEncoder();
std::unique_ptr<webrtc::VideoDecoder> createNativeScreenDecoder();

} // namespace tmc
