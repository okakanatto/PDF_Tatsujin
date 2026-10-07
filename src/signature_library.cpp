#include "signature_library.h"

namespace tatsu
{
SignatureLibrary::SignatureLibrary(QString file) : path(std::move(file)) {}
QVector<SignatureTemplate> SignatureLibrary::load() const
{
    if (!QFile::exists(path))
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("保存済み署名を読み込めません。");
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !json.isObject() ||
        json.object()["version"].toInt() != 1 || !json.object()["items"].isArray())
        fail("保存済み署名の形式が不正です。ファイルは変更していません。");
    QVector<SignatureTemplate> items;
    for (const auto& value : json.object()["items"].toArray())
    {
        const auto object = value.toObject();
        SignatureTemplate item;
        item.id = object["id"].toString();
        item.name = object["name"].toString();
        item.kind = OverlayKind(object["kind"].toInt());
        item.text = object["text"].toString();
        item.fontFamily = object["fontFamily"].toString(signatureFont());
        item.size = object["size"].toDouble(20);
        item.width = object["width"].toDouble(144);
        item.color = QColor(object["color"].toString());
        if (isImage(item.kind))
            item.image = QImage::fromData(
                QByteArray::fromBase64(object["png"].toString().toLatin1()), "PNG");
        if (item.id.isEmpty() || item.name.isEmpty() ||
            (item.kind != OverlayKind::SignatureText && item.kind != OverlayKind::SignatureImage) ||
            (isImage(item.kind) ? item.image.isNull() : item.text.trimmed().isEmpty()))
            fail("保存済み署名に不正な項目があります。ファイルは変更していません。");
        items.append(std::move(item));
    }
    return items;
}
void SignatureLibrary::save(const QVector<SignatureTemplate>& items,
                            const QByteArray& expected) const
{
    QJsonArray array;
    for (const auto& item : items)
    {
        QJsonObject object{
            {"id", item.id},
            {"name", item.name},
            {"kind", int(item.kind)},
            {"text", item.text},
            {"size", item.size},
            {"width", item.width},
            {"color", item.color.name()},
            {"fontFamily", item.fontFamily.isEmpty() ? signatureFont() : item.fontFamily}};
        if (isImage(item.kind))
        {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            if (!buffer.open(QIODevice::WriteOnly) || !item.image.save(&buffer, "PNG"))
                fail("署名画像を保存できません。");
            object["png"] = QString::fromLatin1(bytes.toBase64());
        }
        array.append(object);
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        fail("署名の保存先を作成できません。");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"items", array}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        fileHash(path) != expected || !file.commit())
        fail("署名を保存できませんでした。既存の保存済み署名は保持しました。");
}
QString SignatureLibrary::add(SignatureTemplate item)
{
    const auto expected = fileHash(path);
    auto items = load();
    if (item.name.trimmed().isEmpty() ||
        (item.kind != OverlayKind::SignatureText && item.kind != OverlayKind::SignatureImage) ||
        (isImage(item.kind) ? item.image.isNull() : item.text.trimmed().isEmpty()))
        fail("保存する署名と名前を指定してください。");
    item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    items.append(item);
    save(items, expected);
    return item.id;
}
void SignatureLibrary::remove(const QString& id)
{
    const auto expected = fileHash(path);
    auto items = load();
    auto it =
        std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.id == id; });
    if (it == items.end())
        fail("保存済み署名が見つかりません。");
    items.erase(it);
    save(items, expected);
}
} // namespace tatsu
