#include "tmc/sharing/screen_control.h"

#include "tmc/core/uuid.h"
#include "tmc/sharing/screen_quality.h"

#include <QJsonDocument>

#include <cmath>

namespace tmc {
namespace {

bool integer(const QJsonObject& m, const char* key, qint64 lo, qint64 hi) {
    const auto v = m.value(QLatin1StringView(key));
    const auto d = v.toDouble(-1);
    return v.isDouble() && std::isfinite(d) && std::floor(d) == d && d >= lo && d <= hi;
}

bool uuid(const QJsonObject& m, const char* key) {
    return m.value(QLatin1StringView(key)).isString() &&
           isCanonicalUuid(m.value(QLatin1StringView(key)).toString());
}

bool quality(const QJsonObject& m) {
    return integer(m, "height", 720, 1440) && integer(m, "fps", 30, 60) &&
           ScreenQuality{m.value("height").toInt(), m.value("fps").toInt()}.isValid();
}

} // namespace

Result<QJsonObject> decodeScreenControl(const QJsonObject& m) {
    bool valid = integer(m, "v", ScreenProtocolVersion, ScreenProtocolVersion) &&
                 m.value("kind").isString() &&
                 QJsonDocument(m).toJson(QJsonDocument::Compact).size() <= 60 * 1024;
    const auto kind = m.value("kind").toString();
    const bool revision = integer(m, "revision", 1, 9007199254740991LL);
    if (kind == "capability") {
        valid &= m.value("supported").isBool();
    } else if (kind == "publish") {
        valid &= uuid(m, "stream") && revision && quality(m) && m.value("audio").isBool() &&
                 m.value("title").isString() && m.value("title").toString().size() <= 512;
    } else if (kind == "unpublish") {
        valid &= uuid(m, "stream") && revision;
    } else if (kind == "subscribe") {
        valid &= uuid(m, "stream") && revision && quality(m) && m.value("video").isBool() &&
                 m.value("audio").isBool() &&
                 (!m.value("audio").toBool() || m.value("video").toBool());
    } else if (kind == "subscription-state") {
        valid &= uuid(m, "stream") && revision && quality(m) && m.value("error").isString() &&
                 m.value("error").toString().size() <= 512;
    } else if (kind == "media-request") {
        valid &= m.value("reset").isBool() &&
                 (m.value("software").isUndefined() || m.value("software").isBool());
    } else if (kind == "offer" || kind == "answer" || kind == "candidate" || kind == "binding") {
        valid &= uuid(m, "media") && integer(m, "generation", 1, 9007199254740991LL) &&
                 integer(m, "negotiation", 1, 9007199254740991LL);
        if (kind == "offer" || kind == "answer") {
            valid &= m.value("sdp").isString() && !m.value("sdp").toString().isEmpty() &&
                     m.value("sdp").toString().toUtf8().size() <= 48 * 1024;
        } else if (kind == "candidate") {
            valid &= m.value("candidate").isString() &&
                     m.value("candidate").toString().toUtf8().size() <= 4096 &&
                     m.value("mid").isString() && m.value("mid").toString().size() <= 64 &&
                     integer(m, "mline", 0, 1);
        } else {
            valid &= uuid(m, "stream") && revision && m.value("videoMid").isString() &&
                     m.value("videoMid").toString().size() <= 64;
        }
    } else {
        valid = false;
    }
    return valid ? Result<QJsonObject>::success(m)
                 : Result<QJsonObject>::failure("Invalid screen media control message");
}

} // namespace tmc
