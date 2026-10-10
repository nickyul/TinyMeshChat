#pragma once

#include <api/audio_codecs/audio_decoder_factory.h>
#include <api/audio_codecs/audio_encoder_factory.h>
#include <api/scoped_refptr.h>

namespace tmc {

webrtc::scoped_refptr<webrtc::AudioEncoderFactory> createScreenAudioEncoderFactory();
webrtc::scoped_refptr<webrtc::AudioDecoderFactory> createScreenAudioDecoderFactory();

} // namespace tmc
