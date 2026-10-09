#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <api/video/i420_buffer.h>
#include <atomic>
#include <cstring>
#include <libyuv.h>
#include <modules/video_coding/include/video_error_codes.h>

#include "screen_codec_bitstream.h"
#include "screen_codec_factory.h"

namespace tmc {
namespace {

class MacEncoder final : public webrtc::VideoEncoder {
    VTCompressionSessionRef session_{};
    std::atomic<webrtc::EncodedImageCallback*> callback_{};
    std::atomic<int> pending_{};
    std::atomic<bool> broken_{};
    int width_{}, height_{}, bitrate_{}, fps_{30};

    struct Frame {
        MacEncoder* encoder;
        uint32_t timestamp;
        int width, height;
    };

    static void encoded(void*, void* context, OSStatus status, VTEncodeInfoFlags flags,
                        CMSampleBufferRef sample) {
        std::unique_ptr<Frame> frame(static_cast<Frame*>(context));
        if (!frame) {
            return;
        }
        auto* self = frame->encoder;
        --self->pending_;
        if (status != noErr) {
            self->broken_ = true;
            return;
        }
        if (!sample || (flags & kVTEncodeInfo_FrameDropped)) {
            return;
        }
        const auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        const auto attachment =
            attachments && CFArrayGetCount(attachments)
                ? static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0))
                : nullptr;
        const bool key =
            !attachment || !CFDictionaryContainsKey(attachment, kCMSampleAttachmentKey_NotSync);
        std::vector<uint8_t> bytes;
        const auto format = CMSampleBufferGetFormatDescription(sample);
        size_t count{};
        int lengthBytes{};
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr, &count,
                                                           &lengthBytes);
        if (key) {
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* parameter{};
                size_t length{};
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                        format, i, &parameter, &length, nullptr, nullptr) == noErr) {
                    appendNalu(bytes, {parameter, length});
                }
            }
        }
        const auto block = CMSampleBufferGetDataBuffer(sample);
        const auto size = block ? CMBlockBufferGetDataLength(block) : 0;
        std::vector<uint8_t> avcc(size);
        if (!block || lengthBytes != 4 ||
            CMBlockBufferCopyDataBytes(block, 0, size, avcc.data()) != noErr) {
            self->broken_ = true;
            return;
        }
        for (size_t offset = 0; offset + 4 <= avcc.size();) {
            const size_t length = (uint32_t(avcc[offset]) << 24) |
                                  (uint32_t(avcc[offset + 1]) << 16) |
                                  (uint32_t(avcc[offset + 2]) << 8) | avcc[offset + 3];
            offset += 4;
            if (!length || length > avcc.size() - offset) {
                self->broken_ = true;
                return;
            }
            appendNalu(bytes, {avcc.data() + offset, length});
            offset += length;
        }
        emitH264(self->callback_.load(), bytes, frame->timestamp, frame->width, frame->height, key);
    }

    bool open() {
        NSDictionary* specification = @{
            (__bridge NSString*)
            kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES
        };
        NSDictionary* attributes = @{
            (__bridge NSString*)
            kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{}
        };
        if (VTCompressionSessionCreate(kCFAllocatorDefault, width_, height_, kCMVideoCodecType_H264,
                                       (__bridge CFDictionaryRef)specification,
                                       (__bridge CFDictionaryRef)attributes, nullptr, encoded,
                                       nullptr, &session_) != noErr) {
            return false;
        }
        bool ok = VTSessionSetProperty(session_, kVTCompressionPropertyKey_RealTime,
                                       kCFBooleanTrue) == noErr;
        ok &= VTSessionSetProperty(session_, kVTCompressionPropertyKey_AllowFrameReordering,
                                   kCFBooleanFalse) == noErr;
        ok &= VTSessionSetProperty(session_, kVTCompressionPropertyKey_ProfileLevel,
                                   kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel) == noErr;
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
                             (__bridge CFNumberRef) @2);
        rates();
        ok &= VTCompressionSessionPrepareToEncodeFrames(session_) == noErr;
        if (!ok) {
            Release();
        }
        return ok;
    }

    void rates() {
        if (!session_) {
            return;
        }
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_AverageBitRate,
                             (__bridge CFNumberRef) @(bitrate_));
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_ExpectedFrameRate,
                             (__bridge CFNumberRef) @(fps_));
        NSArray* limits = @[ @(std::max(bitrate_, 100000) / 8), @1 ];
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_DataRateLimits,
                             (__bridge CFArrayRef)limits);
    }

public:
    ~MacEncoder() override {
        Release();
    }

    int InitEncode(const webrtc::VideoCodec* codec, const Settings&) override {
        Release();
        if (!codec || !codec->width || !codec->height) {
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        width_ = codec->width;
        height_ = codec->height;
        fps_ = codec->maxFramerate;
        bitrate_ = codec->startBitrate * 1000;
        broken_ = false;
        return open() ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        if (session_) {
            VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
            VTCompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(const webrtc::VideoFrame& frame,
                   const std::vector<webrtc::VideoFrameType>* types) override {
        if (!session_ || broken_) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (!bitrate_ || pending_ >= 3) {
            return WEBRTC_VIDEO_CODEC_OK;
        }
        const auto i420 = frame.video_frame_buffer()->ToI420();
        if (!i420) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (i420->width() != width_ || i420->height() != height_) {
            Release();
            width_ = i420->width();
            height_ = i420->height();
            if (!open()) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
        }
        CVPixelBufferRef pixel{};
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
                                               VTCompressionSessionGetPixelBufferPool(session_),
                                               &pixel) != kCVReturnSuccess) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        CVPixelBufferLockBaseAddress(pixel, 0);
        const auto converted = libyuv::I420ToNV12(
            i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(),
            i420->StrideV(), static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel, 0)),
            CVPixelBufferGetBytesPerRowOfPlane(pixel, 0),
            static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel, 1)),
            CVPixelBufferGetBytesPerRowOfPlane(pixel, 1), width_, height_);
        CVPixelBufferUnlockBaseAddress(pixel, 0);
        if (converted) {
            CFRelease(pixel);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        bool key = types && std::find(types->begin(), types->end(),
                                      webrtc::VideoFrameType::kVideoFrameKey) != types->end();
        NSDictionary* properties =
            key
                ? @{(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES}
                : nil;
        auto context = std::make_unique<Frame>(Frame{this, frame.rtp_timestamp(), width_, height_});
        ++pending_;
        const auto status = VTCompressionSessionEncodeFrame(
            session_, pixel, CMTimeMake(frame.timestamp_us(), 1000000), kCMTimeInvalid,
            (__bridge CFDictionaryRef)properties, context.get(), nullptr);
        CFRelease(pixel);
        if (status != noErr) {
            --pending_;
            broken_ = true;
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        context.release();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void SetRates(const RateControlParameters& rates) override {
        bitrate_ = rates.bitrate.get_sum_bps();
        if (rates.framerate_fps > 0) {
            fps_ = rates.framerate_fps;
        }
        this->rates();
    }

    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = "VideoToolbox H264";
        info.is_hardware_accelerated = true;
        info.supports_native_handle = false;
        info.requested_resolution_alignment = 2;
        return info;
    }
};

class MacDecoder final : public webrtc::VideoDecoder {
    VTDecompressionSessionRef session_{};
    CMVideoFormatDescriptionRef format_{};
    webrtc::DecodedImageCallback* callback_{};
    std::vector<uint8_t> sps_, pps_;

    bool configureSession() {
        if (sps_.empty() || pps_.empty()) {
            return false;
        }
        closeSession();
        const uint8_t* parameters[]{sps_.data(), pps_.data()};
        const size_t sizes[]{sps_.size(), pps_.size()};
        if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, parameters,
                                                                sizes, 4, &format_) != noErr) {
            return false;
        }
        const auto dimensions = CMVideoFormatDescriptionGetDimensions(format_);
        if (dimensions.width <= 0 || dimensions.height <= 0 || dimensions.width > 4096 ||
            dimensions.height > 4096) {
            return false;
        }
        NSDictionary* attributes = @{
            (__bridge NSString*)
            kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        };
        VTDecompressionOutputCallbackRecord record{decoded, this};
        const auto status =
            VTDecompressionSessionCreate(kCFAllocatorDefault, format_, nullptr,
                                         (__bridge CFDictionaryRef)attributes, &record, &session_);
        if (status == noErr) {
            VTSessionSetProperty(session_, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
        }
        return status == noErr;
    }

    void closeSession() {
        if (session_) {
            VTDecompressionSessionWaitForAsynchronousFrames(session_);
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (format_) {
            CFRelease(format_);
            format_ = nullptr;
        }
    }

    struct Frame {
        uint32_t timestamp;
        int64_t renderTime;
    };

    static void decoded(void* context, void* frameContext, OSStatus status, VTDecodeInfoFlags,
                        CVImageBufferRef image, CMTime, CMTime) {
        std::unique_ptr<Frame> metadata(static_cast<Frame*>(frameContext));
        auto* self = static_cast<MacDecoder*>(context);
        if (status != noErr || !image || !metadata || !self->callback_) {
            return;
        }
        const int width = CVPixelBufferGetWidth(image), height = CVPixelBufferGetHeight(image);
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            return;
        }
        auto buffer = webrtc::I420Buffer::Create(width, height);
        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        libyuv::NV12ToI420(
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 0)),
            CVPixelBufferGetBytesPerRowOfPlane(image, 0),
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 1)),
            CVPixelBufferGetBytesPerRowOfPlane(image, 1), buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(), buffer->MutableDataV(), buffer->StrideV(),
            width, height);
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        auto frame = webrtc::VideoFrame::Builder()
                         .set_video_frame_buffer(buffer)
                         .set_rtp_timestamp(metadata->timestamp)
                         .set_timestamp_ms(metadata->renderTime)
                         .build();
        self->callback_->Decoded(frame);
    }

public:
    ~MacDecoder() override {
        Release();
    }

    bool Configure(const Settings&) override {
        Release();
        return true;
    }

    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        closeSession();
        sps_.clear();
        pps_.clear();
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Decode(const webrtc::EncodedImage& image, int64_t renderTime) override {
        const auto nalus = h264Nalus({image.data(), image.size()});
        bool changed = false;
        std::vector<uint8_t> avcc;
        for (const auto nalu : nalus) {
            if (nalu.empty()) {
                continue;
            }
            if ((nalu[0] & 31) == 7 || (nalu[0] & 31) == 8) {
                auto& parameter = (nalu[0] & 31) == 7 ? sps_ : pps_;
                const std::vector<uint8_t> value(nalu.begin(), nalu.end());
                changed |= parameter != value;
                parameter = value;
            }
            const uint32_t size = nalu.size();
            avcc.insert(avcc.end(), {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8),
                                     uint8_t(size)});
            avcc.insert(avcc.end(), nalu.begin(), nalu.end());
        }
        if ((changed || !session_) && !configureSession()) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (avcc.empty()) {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        CMBlockBufferRef block{};
        CMSampleBufferRef sample{};
        auto status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, avcc.size(),
                                                         kCFAllocatorDefault, nullptr, 0,
                                                         avcc.size(), 0, &block);
        if (status == noErr) {
            status = CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
        }
        const size_t size = avcc.size();
        if (status == noErr) {
            status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format_, 1, 0, nullptr,
                                               1, &size, &sample);
        }
        auto metadata = std::make_unique<Frame>(Frame{image.RtpTimestamp(), renderTime});
        if (status == noErr) {
            status =
                VTDecompressionSessionDecodeFrame(session_, sample, 0, metadata.get(), nullptr);
        }
        if (status == noErr) {
            metadata.release();
            VTDecompressionSessionWaitForAsynchronousFrames(session_);
        }
        if (sample) {
            CFRelease(sample);
        }
        if (block) {
            CFRelease(block);
        }
        return status == noErr ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
    }

    DecoderInfo GetDecoderInfo() const override {
        DecoderInfo info;
        info.implementation_name = "VideoToolbox H264";
        info.is_hardware_accelerated = VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
        return info;
    }
};

} // namespace

NativeCodecSupport probeNativeScreenCodec() {
    static const auto support = [] {
        VTCompressionSessionRef session{};
        NSDictionary* required = @{
            (__bridge NSString*)
            kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES
        };
        const bool encoder =
            VTCompressionSessionCreate(kCFAllocatorDefault, 1280, 720, kCMVideoCodecType_H264,
                                       (__bridge CFDictionaryRef)required, nullptr, nullptr,
                                       nullptr, nullptr, &session) == noErr;
        if (session) {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
        return NativeCodecSupport{encoder, VTIsHardwareDecodeSupported(kCMVideoCodecType_H264),
                                  encoder};
    }();
    return support;
}

std::unique_ptr<webrtc::VideoEncoder> createNativeScreenEncoder() {
    return std::make_unique<MacEncoder>();
}

std::unique_ptr<webrtc::VideoDecoder> createNativeScreenDecoder() {
    return std::make_unique<MacDecoder>();
}

} // namespace tmc
