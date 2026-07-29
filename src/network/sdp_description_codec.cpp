#include "tmc/network/sdp_description_codec.h"

#include <QAbstractSocket>
#include <QDataStream>
#include <QHostAddress>
#include <QIODevice>
#include <QStringList>

#include <rtc/rtc.hpp>

#include <algorithm>
#include <exception>
#include <optional>
#include <vector>

namespace tmc {

namespace {

constexpr quint16 SctpPort = 5000;
constexpr size_t MaxMessageSize = 256 * 1024;
constexpr qsizetype MaxCredentialBytes = 256;
constexpr qsizetype MaxFoundationBytes = 32;
constexpr qsizetype MaxScopeBytes = 64;
constexpr qsizetype MaxCandidates = 256;

constexpr quint8 CandidateIpv6 = 1 << 0;
constexpr quint8 CandidateServerReflexive = 1 << 1;
constexpr quint8 CandidateHasScope = 1 << 2;
constexpr quint8 CandidateFlagsMask =
    CandidateIpv6 | CandidateServerReflexive | CandidateHasScope;

bool writeBytes(QDataStream& stream, const QByteArray& bytes, qsizetype maxBytes) {
    if (bytes.isEmpty() || bytes.size() > maxBytes) {
        return false;
    }
    stream << static_cast<quint16>(bytes.size());
    return stream.writeRawData(bytes.constData(), bytes.size()) == bytes.size();
}

bool readBytes(QDataStream& stream, QByteArray& bytes, qsizetype maxBytes) {
    quint16 size{};
    stream >> size;
    if (stream.status() != QDataStream::Ok || size == 0 || size > maxBytes) {
        return false;
    }
    bytes.resize(size);
    return stream.readRawData(bytes.data(), bytes.size()) == bytes.size();
}

std::optional<quint8> encodeRole(rtc::Description::Role role) {
    switch (role) {
    case rtc::Description::Role::ActPass:
        return 0;
    case rtc::Description::Role::Passive:
        return 1;
    case rtc::Description::Role::Active:
        return 2;
    }
    return std::nullopt;
}

std::optional<rtc::Description::Role> decodeRole(quint8 role) {
    switch (role) {
    case 0:
        return rtc::Description::Role::ActPass;
    case 1:
        return rtc::Description::Role::Passive;
    case 2:
        return rtc::Description::Role::Active;
    default:
        return std::nullopt;
    }
}

std::optional<quint8> encodeFingerprintAlgorithm(
    rtc::CertificateFingerprint::Algorithm algorithm) {
    switch (algorithm) {
    case rtc::CertificateFingerprint::Algorithm::Sha1:
        return 0;
    case rtc::CertificateFingerprint::Algorithm::Sha224:
        return 1;
    case rtc::CertificateFingerprint::Algorithm::Sha256:
        return 2;
    case rtc::CertificateFingerprint::Algorithm::Sha384:
        return 3;
    case rtc::CertificateFingerprint::Algorithm::Sha512:
        return 4;
    }
    return std::nullopt;
}

std::optional<rtc::CertificateFingerprint::Algorithm> decodeFingerprintAlgorithm(
    quint8 algorithm) {
    switch (algorithm) {
    case 0:
        return rtc::CertificateFingerprint::Algorithm::Sha1;
    case 1:
        return rtc::CertificateFingerprint::Algorithm::Sha224;
    case 2:
        return rtc::CertificateFingerprint::Algorithm::Sha256;
    case 3:
        return rtc::CertificateFingerprint::Algorithm::Sha384;
    case 4:
        return rtc::CertificateFingerprint::Algorithm::Sha512;
    default:
        return std::nullopt;
    }
}

qsizetype fingerprintSize(rtc::CertificateFingerprint::Algorithm algorithm) {
    switch (algorithm) {
    case rtc::CertificateFingerprint::Algorithm::Sha1:
        return 20;
    case rtc::CertificateFingerprint::Algorithm::Sha224:
        return 28;
    case rtc::CertificateFingerprint::Algorithm::Sha256:
        return 32;
    case rtc::CertificateFingerprint::Algorithm::Sha384:
        return 48;
    case rtc::CertificateFingerprint::Algorithm::Sha512:
        return 64;
    }
    return 0;
}

bool isHexDigit(char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

Result<QByteArray> packFingerprint(const rtc::CertificateFingerprint& fingerprint) {
    auto hex = QByteArray::fromStdString(fingerprint.value);
    hex.replace(":", "");
    const auto expectedSize = fingerprintSize(fingerprint.algorithm);
    if (hex.size() != expectedSize * 2 ||
        !std::all_of(hex.cbegin(), hex.cend(), isHexDigit)) {
        return Result<QByteArray>::failure("SDP contains an invalid certificate fingerprint");
    }
    return Result<QByteArray>::success(QByteArray::fromHex(hex));
}

QString fingerprintText(const QByteArray& digest) {
    return QString::fromLatin1(digest.toHex(':').toUpper());
}

bool writeCandidate(QDataStream& stream, const rtc::Candidate& candidate) {
    const auto fields =
        QString::fromStdString(candidate.candidate()).split(' ', Qt::SkipEmptyParts);
    if (fields.size() < 8 || !fields[0].startsWith("candidate:") || fields[1] != "1" ||
        fields[2].compare("UDP", Qt::CaseInsensitive) != 0 || fields[6] != "typ") {
        return false;
    }

    const auto foundation = fields[0].sliced(QString("candidate:").size()).toLatin1();
    bool priorityOk{};
    bool portOk{};
    const auto priority = fields[3].toUInt(&priorityOk);
    const auto port = fields[5].toUShort(&portOk);
    QHostAddress address(fields[4]);
    const bool ipv6 = address.protocol() == QAbstractSocket::IPv6Protocol;
    if (foundation.isEmpty() || foundation.size() > MaxFoundationBytes || !priorityOk ||
        !portOk || port == 0 ||
        (!ipv6 && address.protocol() != QAbstractSocket::IPv4Protocol)) {
        return false;
    }

    bool serverReflexive{};
    if (fields[7] == "host") {
        if (fields.size() != 8) {
            return false;
        }
    } else if (fields[7] == "srflx") {
        serverReflexive = true;
        if (fields.size() != 12 || fields[8] != "raddr" || fields[9] != "0.0.0.0" ||
            fields[10] != "rport" || fields[11] != "0") {
            return false;
        }
    } else {
        return false;
    }

    const auto scope = address.scopeId().toUtf8();
    if (scope.size() > MaxScopeBytes) {
        return false;
    }
    quint8 flags = ipv6 ? CandidateIpv6 : 0;
    flags |= serverReflexive ? CandidateServerReflexive : 0;
    flags |= scope.isEmpty() ? 0 : CandidateHasScope;

    stream << flags << static_cast<quint8>(foundation.size());
    stream.writeRawData(foundation.constData(), foundation.size());
    stream << static_cast<quint32>(priority) << static_cast<quint16>(port);
    if (ipv6) {
        const auto bytes = address.toIPv6Address();
        stream.writeRawData(reinterpret_cast<const char*>(bytes.c), 16);
    } else {
        stream << address.toIPv4Address();
    }
    if (!scope.isEmpty()) {
        stream << static_cast<quint8>(scope.size());
        stream.writeRawData(scope.constData(), scope.size());
    }
    return stream.status() == QDataStream::Ok;
}

Result<rtc::Candidate> readCandidate(QDataStream& stream) {
    quint8 flags{};
    quint8 foundationSize{};
    stream >> flags >> foundationSize;
    if (stream.status() != QDataStream::Ok || (flags & ~CandidateFlagsMask) != 0 ||
        foundationSize == 0 || foundationSize > MaxFoundationBytes ||
        ((flags & CandidateHasScope) != 0 && (flags & CandidateIpv6) == 0)) {
        return Result<rtc::Candidate>::failure("Invalid compact ICE candidate header");
    }

    QByteArray foundation(foundationSize, Qt::Uninitialized);
    if (stream.readRawData(foundation.data(), foundation.size()) != foundation.size() ||
        std::any_of(foundation.cbegin(), foundation.cend(), [](char value) {
            return value <= ' ' || value > '~';
        })) {
        return Result<rtc::Candidate>::failure("Invalid compact ICE foundation");
    }

    quint32 priority{};
    quint16 port{};
    stream >> priority >> port;
    if (stream.status() != QDataStream::Ok || port == 0) {
        return Result<rtc::Candidate>::failure("Invalid compact ICE endpoint");
    }

    QHostAddress address;
    if ((flags & CandidateIpv6) != 0) {
        Q_IPV6ADDR bytes{};
        if (stream.readRawData(reinterpret_cast<char*>(bytes.c), 16) != 16) {
            return Result<rtc::Candidate>::failure("Truncated compact IPv6 candidate");
        }
        address.setAddress(bytes);
    } else {
        quint32 bytes{};
        stream >> bytes;
        address.setAddress(bytes);
    }

    if ((flags & CandidateHasScope) != 0) {
        quint8 scopeSize{};
        stream >> scopeSize;
        if (stream.status() != QDataStream::Ok || scopeSize == 0 ||
            scopeSize > MaxScopeBytes) {
            return Result<rtc::Candidate>::failure("Invalid compact IPv6 scope");
        }
        QByteArray scope(scopeSize, Qt::Uninitialized);
        if (stream.readRawData(scope.data(), scope.size()) != scope.size()) {
            return Result<rtc::Candidate>::failure("Truncated compact IPv6 scope");
        }
        address.setScopeId(QString::fromUtf8(scope));
    }

    const auto type =
        (flags & CandidateServerReflexive) != 0 ? QStringLiteral("srflx")
                                                : QStringLiteral("host");
    auto text = QString("candidate:%1 1 UDP %2 %3 %4 typ %5")
                    .arg(QString::fromLatin1(foundation))
                    .arg(priority)
                    .arg(address.toString())
                    .arg(port)
                    .arg(type);
    if ((flags & CandidateServerReflexive) != 0) {
        text += " raddr 0.0.0.0 rport 0";
    }

    try {
        return Result<rtc::Candidate>::success(rtc::Candidate(text.toStdString(), "0"));
    } catch (const std::exception& error) {
        return Result<rtc::Candidate>::failure(
            "Invalid compact ICE candidate: " + QString::fromUtf8(error.what()));
    }
}

bool hasExpectedShape(const rtc::Description& description) {
    const auto* application = description.application();
    if (description.mediaCount() != 1 || application == nullptr ||
        application->isRemoved() || application->type() != "application" ||
        application->protocol() != "UDP/DTLS/SCTP" ||
        application->description() != "webrtc-datachannel" || application->mid() != "0" ||
        application->direction() != rtc::Description::Direction::SendRecv ||
        application->sctpPort() != SctpPort ||
        application->maxMessageSize() != MaxMessageSize ||
        !application->attributes().empty() || !description.ended()) {
        return false;
    }

    const auto options = description.iceOptions();
    if (options != std::vector<std::string>{"ice2", "trickle"}) {
        return false;
    }

    const auto attributes = description.attributes();
    return attributes.empty() ||
           (attributes.size() == 1 && attributes.front() == "msid-semantic:WMS *");
}

} // namespace

Result<QByteArray> SdpDescriptionCodec::pack(const QString& sdp) {
    try {
        const rtc::Description description(sdp.toStdString());
        if (!hasExpectedShape(description)) {
            return Result<QByteArray>::failure(
                "SDP is not the expected completed data-channel description");
        }

        const auto role = encodeRole(description.role());
        const auto ufrag = description.iceUfrag();
        const auto password = description.icePwd();
        const auto fingerprint = description.fingerprint();
        const auto candidates = description.candidates();
        if (!role || !ufrag || !password || !fingerprint || candidates.empty() ||
            candidates.size() > MaxCandidates) {
            return Result<QByteArray>::failure("SDP has missing or unsupported connection data");
        }

        const auto algorithm = encodeFingerprintAlgorithm(fingerprint->algorithm);
        auto digest = packFingerprint(*fingerprint);
        if (!algorithm || !digest) {
            return Result<QByteArray>::failure(
                algorithm ? digest.error() : "SDP uses an unsupported fingerprint algorithm");
        }

        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::BigEndian);
        stream << *role;
        if (!writeBytes(stream, QByteArray::fromStdString(*ufrag), MaxCredentialBytes) ||
            !writeBytes(stream, QByteArray::fromStdString(*password), MaxCredentialBytes)) {
            return Result<QByteArray>::failure("SDP contains invalid ICE credentials");
        }
        stream << *algorithm;
        stream.writeRawData(digest.value().constData(), digest.value().size());
        stream << static_cast<quint16>(candidates.size());
        for (const auto& candidate : candidates) {
            if (!writeCandidate(stream, candidate)) {
                return Result<QByteArray>::failure(
                    "SDP contains an unsupported ICE candidate");
            }
        }
        if (stream.status() != QDataStream::Ok) {
            return Result<QByteArray>::failure("Failed to pack SDP connection data");
        }
        return Result<QByteArray>::success(std::move(payload));
    } catch (const std::exception& error) {
        return Result<QByteArray>::failure(
            "Invalid SDP description: " + QString::fromUtf8(error.what()));
    }
}

Result<QString> SdpDescriptionCodec::unpack(const QByteArray& payload,
                                            SdpDescriptionType type) {
    try {
        QDataStream stream(payload);
        stream.setByteOrder(QDataStream::BigEndian);

        quint8 encodedRole{};
        stream >> encodedRole;
        const auto role = decodeRole(encodedRole);
        QByteArray ufrag;
        QByteArray password;
        if (!role || !readBytes(stream, ufrag, MaxCredentialBytes) ||
            !readBytes(stream, password, MaxCredentialBytes)) {
            return Result<QString>::failure("Invalid compact SDP connection data");
        }

        quint8 encodedAlgorithm{};
        stream >> encodedAlgorithm;
        const auto algorithm = decodeFingerprintAlgorithm(encodedAlgorithm);
        if (!algorithm) {
            return Result<QString>::failure("Unsupported compact fingerprint algorithm");
        }
        const auto digestSize = fingerprintSize(*algorithm);
        QByteArray digest(digestSize, Qt::Uninitialized);
        if (stream.readRawData(digest.data(), digest.size()) != digest.size()) {
            return Result<QString>::failure("Truncated compact certificate fingerprint");
        }

        quint16 candidateCount{};
        stream >> candidateCount;
        if (stream.status() != QDataStream::Ok || candidateCount == 0 ||
            candidateCount > MaxCandidates) {
            return Result<QString>::failure("Invalid compact ICE candidate count");
        }

        const auto descriptionType = type == SdpDescriptionType::Offer
                                         ? rtc::Description::Type::Offer
                                         : rtc::Description::Type::Answer;
        rtc::Description description("", descriptionType, *role);
        description.addIceOption("ice2");
        description.addIceOption("trickle");
        description.setIceAttribute(ufrag.toStdString(), password.toStdString());
        description.setFingerprint(
            rtc::CertificateFingerprint{*algorithm, fingerprintText(digest).toStdString()});
        description.addApplication("0");
        auto* application = description.application();
        application->setSctpPort(SctpPort);
        application->setMaxMessageSize(MaxMessageSize);

        for (quint16 index = 0; index < candidateCount; ++index) {
            auto candidate = readCandidate(stream);
            if (!candidate) {
                return Result<QString>::failure(candidate.error());
            }
            description.addCandidate(std::move(candidate.value()));
        }
        if (!stream.atEnd()) {
            return Result<QString>::failure("Compact SDP contains trailing data");
        }
        description.endCandidates();
        return Result<QString>::success(
            QString::fromStdString(description.generateApplicationSdp()));
    } catch (const std::exception& error) {
        return Result<QString>::failure(
            "Invalid compact SDP: " + QString::fromUtf8(error.what()));
    }
}

} // namespace tmc
