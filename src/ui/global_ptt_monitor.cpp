#include "tmc/ui/global_ptt_monitor.h"

#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>

#include <optional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#endif

namespace tmc {

namespace {

QString keyDisplayName(const QKeyEvent& event) {
    switch (event.key()) {
    case Qt::Key_Control:
        return "Ctrl";
    case Qt::Key_Shift:
        return "Shift";
    case Qt::Key_Alt:
        return "Alt";
    case Qt::Key_Meta:
        return "Meta";
    default:
        return QKeySequence(event.key()).toString(QKeySequence::NativeText);
    }
}

std::optional<quint32> mouseButtonCode(Qt::MouseButton button) {
#ifdef Q_OS_WIN
    switch (button) {
    case Qt::MiddleButton:
        return VK_MBUTTON;
    case Qt::BackButton:
        return VK_XBUTTON1;
    case Qt::ForwardButton:
        return VK_XBUTTON2;
    default:
        return std::nullopt;
    }
#elif defined(Q_OS_MACOS)
    switch (button) {
    case Qt::MiddleButton:
        return 2;
    case Qt::BackButton:
        return 3;
    case Qt::ForwardButton:
        return 4;
    default:
        return std::nullopt;
    }
#else
    Q_UNUSED(button)
    return std::nullopt;
#endif
}

QString mouseButtonName(Qt::MouseButton button) {
    switch (button) {
    case Qt::MiddleButton:
        return GlobalPttMonitor::tr("Средняя кнопка мыши");
    case Qt::BackButton:
        return GlobalPttMonitor::tr("Боковая кнопка мыши 1");
    case Qt::ForwardButton:
        return GlobalPttMonitor::tr("Боковая кнопка мыши 2");
    default:
        return {};
    }
}

} // namespace

GlobalPttMonitor::GlobalPttMonitor(QObject* parent) : QObject(parent), binding_(defaultBinding()) {
    pollTimer_.setInterval(10);
    pollTimer_.setTimerType(Qt::PreciseTimer);
    connect(&pollTimer_, &QTimer::timeout, this, &GlobalPttMonitor::poll);
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->installEventFilter(this);
    }
}

GlobalPttMonitor::~GlobalPttMonitor() {
    if (QCoreApplication::instance()) {
        QCoreApplication::instance()->removeEventFilter(this);
    }
}

PttBinding GlobalPttMonitor::binding() const {
    return binding_;
}

bool GlobalPttMonitor::capturing() const {
    return capturing_;
}

void GlobalPttMonitor::setBinding(const PttBinding& binding) {
    const auto currentPlatform = platformName();
    binding_ = binding.platform == currentPlatform && binding.isValid() ? binding : defaultBinding();
    forceReleased();
}

void GlobalPttMonitor::beginCapture() {
    if (capturing_) {
        return;
    }
    capturing_ = true;
    forceReleased();
    emit capturingChanged(true);
}

void GlobalPttMonitor::cancelCapture() {
    if (!capturing_) {
        return;
    }
    capturing_ = false;
    emit capturingChanged(false);
}

void GlobalPttMonitor::setMonitoringEnabled(bool enabled) {
    if (monitoring_ == enabled) {
        return;
    }
    monitoring_ = enabled;
    forceReleased();
    if (monitoring_) {
        pollTimer_.start();
    } else {
        pollTimer_.stop();
    }
}

void GlobalPttMonitor::forceReleased() {
    acceptingPresses_ = false;
    setPressed(false);
}

void GlobalPttMonitor::finishCapture(PttBinding binding) {
    capturing_ = false;
    forceReleased();
    emit bindingCaptured(binding);
    emit capturingChanged(false);
}

bool GlobalPttMonitor::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched)
    if (!capturing_) {
        return false;
    }

    if (event->type() == QEvent::KeyPress) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->isAutoRepeat()) {
            return true;
        }
        if (keyEvent->key() == Qt::Key_Escape) {
            cancelCapture();
            return true;
        }
        const auto name = keyDisplayName(*keyEvent);
        if (keyEvent->key() == Qt::Key_unknown || name.isEmpty()) {
            return true;
        }
#ifdef Q_OS_WIN
        if (keyEvent->nativeVirtualKey() == 0) {
            return true;
        }
#endif
        finishCapture(
            {PttBindingType::Keyboard, platformName(), keyEvent->nativeVirtualKey(), name});
        return true;
    }

    if (event->type() == QEvent::MouseButtonPress) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        const auto code = mouseButtonCode(mouseEvent->button());
        if (!code) {
            return true;
        }
        finishCapture(
            {PttBindingType::Mouse, platformName(), *code, mouseButtonName(mouseEvent->button())});
        return true;
    }

    return false;
}

QString GlobalPttMonitor::platformName() {
#ifdef Q_OS_WIN
    return "windows";
#elif defined(Q_OS_MACOS)
    return "macos";
#else
    return "unsupported";
#endif
}

PttBinding GlobalPttMonitor::defaultBinding() {
#ifdef Q_OS_WIN
    return {PttBindingType::Keyboard, platformName(), 'V', "V"};
#elif defined(Q_OS_MACOS)
    return {PttBindingType::Keyboard, platformName(), kVK_ANSI_V, "V"};
#else
    return {PttBindingType::Keyboard, {}, 'V', "V"};
#endif
}

bool GlobalPttMonitor::bindingDown() const {
#ifdef Q_OS_WIN
    return (GetAsyncKeyState(static_cast<int>(binding_.code)) & 0x8000) != 0;
#elif defined(Q_OS_MACOS)
    if (binding_.type == PttBindingType::Mouse) {
        return CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,
                                        static_cast<CGMouseButton>(binding_.code));
    }
    return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState,
                                 static_cast<CGKeyCode>(binding_.code));
#else
    return false;
#endif
}

void GlobalPttMonitor::poll() {
    if (!monitoring_ || capturing_) {
        setPressed(false);
        return;
    }
    const bool down = bindingDown();
    if (!acceptingPresses_) {
        if (!down) {
            acceptingPresses_ = true;
        }
        setPressed(false);
        return;
    }
    setPressed(down);
}

void GlobalPttMonitor::setPressed(bool pressed) {
    if (pressed_ == pressed) {
        return;
    }
    pressed_ = pressed;
    emit pressedChanged(pressed_);
}

} // namespace tmc
