#pragma once
#include "core/result.h"
#include <QSqlDatabase>
namespace tmc {
class Database {
  public:
    Database();
    ~Database();
    Result<void> open(const QString& path);
    QSqlDatabase connection() const { return db_; }

  private:
    QString name_;
    QSqlDatabase db_;
};
} // namespace tmc
