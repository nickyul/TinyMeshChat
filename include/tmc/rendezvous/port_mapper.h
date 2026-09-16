#pragma once

#include <QObject>
#include <QString>

namespace tmc {

enum class PortMappingMethod { Pcp, NatPmp, Upnp, Direct, Unavailable };

QString portMappingMethodName(PortMappingMethod method);

struct PortMappingResult {
    QString publicAddress;
    quint16 publicPort{0};
    PortMappingMethod method{PortMappingMethod::Unavailable};
};

class IPortMapper : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~IPortMapper() override = default;

    virtual void start(quint16 internalPort, quint16 preferredExternalPort) = 0;
    virtual void stop() = 0;

signals:
    void mappingReady(tmc::PortMappingResult result);
    void mappingFailed(QString reason);
};

class LibPlumPortMapper final : public IPortMapper {
    Q_OBJECT

public:
    explicit LibPlumPortMapper(QObject* parent = nullptr);
    ~LibPlumPortMapper() override;

    void start(quint16 internalPort, quint16 preferredExternalPort) override;
    void stop() override;

private:
    void processMappingState(int id, int state, QString address, quint16 port, int method);

    int mappingId_{-1};
    bool initialized_{false};
    quint16 internalPort_{0};
    quint16 preferredExternalPort_{0};
};

} // namespace tmc
