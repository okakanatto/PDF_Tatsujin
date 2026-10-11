#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <functional>

namespace tatsu
{
void writeOfficePackage(const QMap<QString, QByteArray>& parts, const QString& path,
                        const QString& extension, const std::function<bool()>& cancelled = {});
}
