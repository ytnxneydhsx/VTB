#pragma once

#include <QStringList>

namespace vtb::desktop {

inline auto board_ssh_arguments() -> QStringList {
    return { QStringLiteral("-T"), QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
        QStringLiteral("-o"), QStringLiteral("ConnectTimeout=4"), QStringLiteral("-o"),
        QStringLiteral("ServerAliveInterval=5"), QStringLiteral("-o"),
        QStringLiteral("ServerAliveCountMax=2"), QStringLiteral("rk3588-direct") };
}

} // namespace vtb::desktop
