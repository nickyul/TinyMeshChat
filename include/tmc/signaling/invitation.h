#pragma once

#include <QString>

namespace tmc {

struct Invitation {
    enum class Kind { Offer, Answer };
    Kind kind{Kind::Offer};
    QString meshId;
    QString connectionId;
    QString sdp;
};

} // namespace tmc
