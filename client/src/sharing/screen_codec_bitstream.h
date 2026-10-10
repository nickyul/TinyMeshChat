#pragma once

#include <algorithm>
#include <api/video/encoded_image.h>
#include <api/video/video_frame.h>
#include <api/video_codecs/video_encoder.h>
#include <cstdint>
#include <modules/video_coding/include/video_codec_interface.h>
#include <span>
#include <vector>

namespace tmc {

// The RTP H.264 packetizer consumes Annex B access units, including SPS/PPS on IDR.
inline std::vector<std::span<const uint8_t>> h264Nalus(std::span<const uint8_t> bytes) {
    std::vector<std::span<const uint8_t>> result;
    if (bytes.size() > 8 * 1024 * 1024) {
        return result;
    }
    auto start = [&](size_t i) -> size_t {
        if (i + 3 <= bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            return 3;
        }
        if (i + 4 <= bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 &&
            bytes[i + 3] == 1) {
            return 4;
        }
        return 0;
    };
    size_t begin = bytes.size();
    for (size_t i = 0; i < bytes.size();) {
        if (result.size() >= 4096) {
            return {};
        }
        const auto prefix = start(i);
        if (!prefix) {
            ++i;
            continue;
        }
        if (begin < i) {
            result.emplace_back(bytes.data() + begin, i - begin);
        }
        begin = i + prefix;
        i = begin;
    }
    if (begin < bytes.size()) {
        result.emplace_back(bytes.data() + begin, bytes.size() - begin);
    }
    return result;
}

inline void appendNalu(std::vector<uint8_t>& out, std::span<const uint8_t> nalu) {
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), nalu.begin(), nalu.end());
}

inline void emitH264(webrtc::EncodedImageCallback* callback, std::span<const uint8_t> bytes,
                     uint32_t timestamp, int width, int height, bool key) {
    if (!callback || bytes.empty()) {
        return;
    }
    webrtc::EncodedImage image;
    image.SetEncodedData(webrtc::EncodedImageBuffer::Create(bytes.data(), bytes.size()));
    image.SetRtpTimestamp(timestamp);
    image._encodedWidth = width;
    image._encodedHeight = height;
    image._frameType =
        key ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta;
    webrtc::CodecSpecificInfo info{};
    info.codecType = webrtc::kVideoCodecH264;
    info.codecSpecific.H264.packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
    info.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
    info.codecSpecific.H264.idr_frame = key;
    callback->OnEncodedImage(image, &info);
}

} // namespace tmc
