#pragma once
#include "document.h"

namespace tatsu
{
struct SignatureTemplate
{
    QString id, name, text;
    OverlayKind kind = OverlayKind::SignatureText;
    double size = 20, width = 144;
    QColor color = Qt::black;
    QImage image;
};
class SignatureLibrary
{
public:
    explicit SignatureLibrary(QString path);
    QVector<SignatureTemplate> load() const;
    QString add(SignatureTemplate item);
    void remove(const QString& id);

private:
    QString path;
    void save(const QVector<SignatureTemplate>& items, const QByteArray& expected) const;
};
} // namespace tatsu
