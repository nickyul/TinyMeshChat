#pragma once
#include <QObject>
#include <memory>
namespace tmc {
class ApplicationController;
#ifdef TMC_WITH_LIBDATACHANNEL
class PeerConnection;
#endif
class ConsoleController : public QObject {
    Q_OBJECT
  public:
    explicit ConsoleController(ApplicationController&, QObject* p = nullptr);
    int run();

  private:
    ApplicationController& app_;
#ifdef TMC_WITH_LIBDATACHANNEL
    std::shared_ptr<PeerConnection> peer_;
#endif
};
} // namespace tmc
