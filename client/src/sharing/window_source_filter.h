#pragma once

#include <QHash>
#include <QString>

namespace tmc {
// Qt 6.9 exposes window descriptions but no native window ID. An empty label
// rejects a title also used by a service window, rather than exposing that window.
using ApplicationWindowTitles = QHash<QString, QString>;
ApplicationWindowTitles applicationWindowTitles();

inline void rememberWindowTitle(ApplicationWindowTitles& titles, const QString& title,
                                const QString& label, bool allowed) {
    if (title.trimmed().isEmpty()) return;
    auto existing = titles.find(title);
    if (!allowed) titles.insert(title, {});
    else if (existing == titles.end()) titles.insert(title, label);
    else if (!existing->isEmpty() && *existing != label) *existing = title;
}
} // namespace tmc
