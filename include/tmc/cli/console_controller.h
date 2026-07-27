#pragma once

#include <QObject>

#include <memory>

namespace tmc {

class ApplicationController;
class PeerConnection;

class ConsoleController : public QObject {
    Q_OBJECT

public:
    explicit ConsoleController(ApplicationController&, QObject* p = nullptr);

    int run();

private:
    ApplicationController& app_;
    std::shared_ptr<PeerConnection> peer_;
};

} // namespace tmc
