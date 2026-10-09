#pragma once
#include "pdfglobal.h"
#include <QByteArray>
#include <QString>

namespace tatsu
{
// Implemented in the core DLL: the pinned SDK's helper is not exported.
PDF4QTLIBCORESHARED_EXPORT QByteArray sdkPreparedPassword(const QString& password);
} // namespace tatsu
