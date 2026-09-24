#pragma once

#include <QString>

#include <optional>
#include <utility>

namespace tmc {

template <class T> class [[nodiscard]] Result {
public:
    static Result success(T value) {
        return Result(std::move(value), {});
    }

    static Result failure(QString error) {
        return Result({}, std::move(error));
    }

    explicit operator bool() const {
        return value_.has_value();
    }

    const T& value() const {
        return *value_;
    }

    T& value() {
        return *value_;
    }

    const QString& error() const {
        return error_;
    }

private:
    Result(std::optional<T> value, QString error)
        : value_(std::move(value)), error_(std::move(error)) {
    }

    std::optional<T> value_;
    QString error_;
};

template <> class [[nodiscard]] Result<void> {
public:
    static Result success() {
        return Result(true, {});
    }

    static Result failure(QString error) {
        return Result(false, std::move(error));
    }

    explicit operator bool() const {
        return ok_;
    }

    const QString& error() const {
        return error_;
    }

private:
    Result(bool ok, QString error) : ok_(ok), error_(std::move(error)) {
    }

    bool ok_;
    QString error_;
};

} // namespace tmc
