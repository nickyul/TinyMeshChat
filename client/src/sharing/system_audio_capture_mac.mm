#include "tmc/sharing/system_audio_capture.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#include <atomic>
#include <vector>

@interface TMCSystemAudioOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::function<void(const float*, int)> sink;
    std::function<void(QString)> failure;
}
@end

@implementation TMCSystemAudioOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (@available(macOS 13.0, *)) {
        if (type != SCStreamOutputTypeAudio || !CMSampleBufferDataIsReady(sample)) return;
        const auto format = CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample));
        const auto frames = CMSampleBufferGetNumSamples(sample);
        if (!format || format->mSampleRate != 48000 || format->mChannelsPerFrame != 2 ||
            !(format->mFormatFlags & kAudioFormatFlagIsFloat) || format->mBitsPerChannel != 32 || frames <= 0 || frames > 8192) return;
        size_t required = 0;
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &required, nullptr, 0, nullptr, nullptr, 0, nullptr);
        if (required == 0 || required > 65536) return;
        std::vector<unsigned char> storage(required);
        auto* buffers = reinterpret_cast<AudioBufferList*>(storage.data());
        CMBlockBufferRef retained = nullptr;
        const auto result = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr, buffers, required,
            nullptr, nullptr, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &retained);
        if (result == noErr) {
            if (buffers->mNumberBuffers == 1 && buffers->mBuffers[0].mNumberChannels == 2 &&
                buffers->mBuffers[0].mDataByteSize >= frames * 2 * sizeof(float)) {
                sink(static_cast<const float*>(buffers->mBuffers[0].mData), static_cast<int>(frames));
            } else if (buffers->mNumberBuffers == 2 && buffers->mBuffers[0].mDataByteSize >= frames * sizeof(float) &&
                       buffers->mBuffers[1].mDataByteSize >= frames * sizeof(float)) {
                std::vector<float> interleaved(static_cast<size_t>(frames) * 2);
                const auto* left = static_cast<const float*>(buffers->mBuffers[0].mData);
                const auto* right = static_cast<const float*>(buffers->mBuffers[1].mData);
                for (CMItemCount i = 0; i < frames; ++i) { interleaved[i * 2] = left[i]; interleaved[i * 2 + 1] = right[i]; }
                sink(interleaved.data(), static_cast<int>(frames));
            }
        }
        if (retained) CFRelease(retained);
    }
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    failure(QStringLiteral("Захват системного звука остановлен: ") + QString::fromUtf8(error.localizedDescription.UTF8String));
}
@end

namespace tmc {
struct SystemAudioCapture::Native {
    struct Run {
        std::atomic<bool> active{true};
        SCStream* __strong stream{nil};
        TMCSystemAudioOutput* __strong output{nil};
        dispatch_queue_t queue{dispatch_queue_create("chat.tinymesh.system-audio", DISPATCH_QUEUE_SERIAL)};
    };
    std::shared_ptr<Run> run;
};
SystemAudioCapture::SystemAudioCapture(QObject* parent) : QObject(parent), native_(std::make_unique<Native>()) {}
SystemAudioCapture::~SystemAudioCapture() { stop(); }
bool SystemAudioCapture::supported() {
    if (@available(macOS 13.0, *)) return true;
    return false;
}
Result<void> SystemAudioCapture::start() {
    stop();
    if (@available(macOS 13.0, *)) {
        const auto sink = makeSink();
        const auto failure = makeErrorSink();
        const auto run = std::make_shared<Native::Run>();
        native_->run = run;
        [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
            completionHandler:^(SCShareableContent* content, NSError* error) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (!run->active) return;
                    if (error || content.displays.count == 0) {
                        failure(QStringLiteral("Разрешите запись экрана и системного звука в настройках macOS."));
                        return;
                    }
                    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:content.displays.firstObject excludingWindows:@[]];
                    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
                    config.width = 2;
                    config.height = 2;
                    config.minimumFrameInterval = CMTimeMake(1, 1);
                    config.capturesAudio = YES;
                    config.excludesCurrentProcessAudio = YES;
                    config.sampleRate = 48000;
                    config.channelCount = 2;
                    run->output = [[TMCSystemAudioOutput alloc] init];
                    run->output->sink = sink;
                    run->output->failure = failure;
                    run->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:run->output];
                    NSError* outputError = nil;
                    if (![run->stream addStreamOutput:run->output type:SCStreamOutputTypeAudio sampleHandlerQueue:run->queue error:&outputError]) {
                        failure(QStringLiteral("Не удалось включить захват системного звука."));
                        return;
                    }
                    [run->stream startCaptureWithCompletionHandler:^(NSError* startError) {
                        if (!run->active) { [run->stream stopCaptureWithCompletionHandler:nil]; return; }
                        if (startError) failure(QStringLiteral("Не удалось запустить системный звук: ") + QString::fromUtf8(startError.localizedDescription.UTF8String));
                    }];
                });
            }];
        return Result<void>::success();
    }
    return Result<void>::failure("Для системного звука требуется macOS 13 или новее.");
}
void SystemAudioCapture::stop() {
    invalidateSink();
    const auto run = native_->run;
    if (!run) return;
    run->active = false;
    [run->stream stopCaptureWithCompletionHandler:nil];
    native_->run.reset();
}
} // namespace tmc
