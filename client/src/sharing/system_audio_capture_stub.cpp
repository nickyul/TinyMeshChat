#include "tmc/sharing/system_audio_capture.h"
namespace tmc {
struct SystemAudioCapture::Native {};
SystemAudioCapture::SystemAudioCapture(QObject* parent) : QObject(parent), native_(std::make_unique<Native>()) {}
SystemAudioCapture::~SystemAudioCapture() = default;
bool SystemAudioCapture::supported() { return false; }
Result<void> SystemAudioCapture::start() { return Result<void>::failure("Захват системного звука недоступен на этой платформе."); }
void SystemAudioCapture::stop() { invalidateSink(); }
} // namespace tmc
