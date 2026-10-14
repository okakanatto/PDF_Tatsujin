#pragma once
#include <QIcon>
#include <QStyle>

namespace tatsu
{
// Share standard icon engines across document windows; retain native theme rendering.
QIcon uiIcon(QStyle::StandardPixmap icon);
} // namespace tatsu
