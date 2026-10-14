#include "document.h"
#include "pdfcms.h"
#include "pdfpagecontentprocessor.h"
#include "redaction_cmap.h"
#include "redaction_copy.h"
#include "redaction_font.h"
#include <QGuiApplication>
#ifdef Q_OS_WIN
#include <windows.h>

#include <tlhelp32.h>
#endif

namespace
{
using namespace tatsu;
constexpr auto criteriaPin = "2d9fc95ed0e62ac20dcf9181604055f0b3b580e3479379617af970bfeded2104";
void require(bool condition, const QString& message)
{
    if (!condition)
        fail(message);
}
QByteArray bytes(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read " + path);
    return file.readAll();
}
QString digest(const QByteArray& value)
{
    return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}
QString sourcePath(const QString& root, const QJsonObject& row)
{
    const auto relative = row["source"].toString();
    require(!relative.isEmpty() && !QDir::isAbsolutePath(relative) &&
                relative == QDir::cleanPath(relative) && !relative.startsWith("../"),
            "Frozen source path must stay relative to the project");
    const auto path = QDir(root).absoluteFilePath(relative);
    require(QFileInfo(path).canonicalFilePath().startsWith(QDir(root).canonicalPath() + "/",
                                                           Qt::CaseInsensitive),
            "Frozen source resolves outside the project");
    require(digest(bytes(path)) == row["sha256"].toString(), "Frozen source SHA mismatch");
    return path;
}
const PDFDictionary* pageFonts(const PDFDocument& document)
{
    const auto* resources =
        document.getDictionaryFromObject(document.getCatalog()->getPage(0)->getResources());
    require(resources != nullptr, "Page resources absent");
    const auto* fonts = document.getDictionaryFromObject(resources->get("Font"));
    require(fonts != nullptr, "Page fonts absent");
    return fonts;
}
QByteArray streamBytes(const PDFDocument& document, const PDFObject& object)
{
    const auto value = document.getObject(object);
    require(value.isStream(), "Expected a stream");
    return document.getDecodedStream(value.getStream());
}
const PDFDictionary* rawFont(const PDFDocument& document, const PDFDictionary* fonts)
{
    const PDFDictionary* found = nullptr;
    for (size_t i = 0; i < fonts->getCount(); ++i)
    {
        const auto* font = document.getDictionaryFromObject(fonts->getValue(i));
        if (!font || !font->hasKey("DescendantFonts") ||
            !document.getObject(font->get("Encoding")).isStream())
            continue;
        require(found == nullptr, "Boundary document has ambiguous raw font binding");
        found = font;
    }
    require(found != nullptr, "Raw mixed font absent");
    return found;
}
QJsonArray routes(const PDFDocument& document, const PDFDictionary& font)
{
    const auto encoding = QString::fromLatin1(streamBytes(document, font.get("Encoding")));
    const auto unicode = QString::fromLatin1(streamBytes(document, font.get("ToUnicode")));
    QMap<QString, int> cids;
    QMap<QString, QChar> characters;
    const QRegularExpression cidPattern(
        "<([0-9a-fA-F]{2}|[0-9a-fA-F]{4})>\\s+(\\d+)\\s*\\nendcidchar");
    const QRegularExpression unicodePattern(
        "<([0-9a-fA-F]{2}|[0-9a-fA-F]{4})>\\s+<([0-9a-fA-F]{4})>\\s*\\nendbfchar");
    auto cidMatches = cidPattern.globalMatch(encoding);
    while (cidMatches.hasNext())
    {
        const auto match = cidMatches.next();
        const auto key = match.captured(1).toLower();
        require(!cids.contains(key), "Duplicate emitted raw CID route");
        cids.insert(key, match.captured(2).toInt());
    }
    auto unicodeMatches = unicodePattern.globalMatch(unicode);
    while (unicodeMatches.hasNext())
    {
        const auto match = unicodeMatches.next();
        const auto key = match.captured(1).toLower();
        require(!characters.contains(key), "Duplicate emitted raw Unicode route");
        characters.insert(key, QChar(match.captured(2).toUShort(nullptr, 16)));
    }
    require(!cids.isEmpty() && cids.keys() == characters.keys(),
            "Raw Encoding and ToUnicode keysets differ");
    RedactionCMap decoder(document, font, {});
    QJsonArray result;
    for (auto it = cids.cbegin(); it != cids.cend(); ++it)
    {
        const auto code = QByteArray::fromHex(it.key().toLatin1());
        const auto decoded = decoder.decode(code);
        require(decoded.size() == 1 && int(decoded[0].cid) == it.value() &&
                    decoded[0].character == characters[it.key()] &&
                    decoded[0].sourceByteCount == code.size() &&
                    decoded[0].sourceValue == it.key().toUShort(nullptr, 16),
                "Generated route failed own CMap roundtrip");
        result.append(QJsonObject{{"code_hex", it.key()},
                                  {"cid", it.value()},
                                  {"unicode", QString(decoded[0].character)}});
    }
    return result;
}
class Glyphs final : public pdf::PDFPageContentProcessor
{
public:
    using PDFPageContentProcessor::PDFPageContentProcessor;
    QJsonArray measured;
    QString text;
    void performOutputCharacter(const pdf::PDFTextCharacterInfo& info) override
    {
        const auto origin = info.matrix.map(QPointF());
        text.append(info.character);
        measured.append(QJsonObject{{"unicode", QString(info.character)},
                                    {"origin_pt", QJsonArray{origin.x(), origin.y()}}});
    }
};
QJsonObject textMeasurement(PDFDocument& document)
{
    pdf::PDFFontCache fonts{128, 128};
    fonts.setDocument(pdf::PDFModifiedDocument(&document, nullptr));
    pdf::PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    Glyphs processor(document.getCatalog()->getPage(0), &document, &fonts, cms.data(), nullptr, {},
                     {});
    const auto errors = processor.processContents();
    require(errors.isEmpty(), "Boundary text measurement produced processor errors");
    return {{"glyphs", processor.measured}, {"raw_unicode", processor.text}};
}
QJsonObject documentCase(const QString& root, const QString& output, const QJsonObject& row)
{
    QJsonObject result{{"name", row["name"]},
                       {"expected_guard", row["expected_guard"]},
                       {"source_sha256", row["sha256"]},
                       {"status", "FAIL"}};
    try
    {
        const auto path = sourcePath(root, row);
        Document document;
        document.open(path);
        const auto before = encodePdf(document.pdf());
        const auto initialSource = document.source;
        const auto initialTarget = document.target;
        const auto sourceHash = document.sourceHash, targetHash = document.targetHash;
        const auto history = document.history.size();
        const auto cursor = document.cursor, saved = document.saved;
        const auto revision = document.revision;
        const auto pending = document.pendingInput, busy = document.busy, dirty = document.dirty();
        result["source_geometry"] = textMeasurement(document.pdf());
        if (row.contains("source_codespace_count"))
        {
            const auto* font = rawFont(document.pdf(), pageFonts(document.pdf()));
            RedactionCMap sourceDecoder(document.pdf(), *font, {});
            for (const auto& alias : row["alias_codes"].toArray())
            {
                const auto decoded =
                    sourceDecoder.decode(QByteArray::fromHex(alias.toString().toLatin1()));
                require(decoded.size() == 1 && int(decoded[0].cid) == row["alias_cid"].toInt() &&
                            QString(decoded[0].character) == row["alias_unicode"].toString(),
                        "Source alias route failed before export");
            }
            result["source_alias_decoder_passed"] = true;
        }
        const auto rect = row["rect"].toArray();
        require(rect.size() == 4, "Boundary rectangle malformed");
        const QRectF rectangle(rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble(),
                               rect[3].toDouble());
        const auto destination = QDir(output).absoluteFilePath(row["output"].toString());
        require(!QFileInfo::exists(destination), "Refusing to overwrite boundary output");
        bool refused = false;
        QString error;
        try
        {
            exportRedactedPdf(document.pdf(), {{0, {rectangle}}}, destination);
        }
        catch (const std::exception& exception)
        {
            refused = true;
            error = QString::fromUtf8(exception.what());
        }
        const bool unchanged =
            encodePdf(document.pdf()) == before && document.source == initialSource &&
            document.target == initialTarget && document.sourceHash == sourceHash &&
            document.targetHash == targetHash && document.history.size() == history &&
            document.cursor == cursor && document.saved == saved && document.revision == revision &&
            document.pendingInput == pending && document.busy == busy &&
            document.dirty() == dirty && digest(bytes(path)) == row["sha256"].toString();
        result["source_document_and_history_unchanged"] = unchanged;
        result["refused"] = refused;
        result["error"] = error;
        result["output_absent"] = !QFileInfo::exists(destination);
        require(unchanged, "Boundary operation changed caller state or source bytes");
        require(refused == row["refused"].toBool(),
                "Boundary acceptance differs from frozen condition");
        if (refused)
        {
            require(!QFileInfo::exists(destination) &&
                        error.contains(row["message_contains"].toString()),
                    "Refusal did not preserve output absence or expected guard error");
        }
        else
        {
            auto candidate = readPdf(destination);
            const auto* font = rawFont(candidate, pageFonts(candidate));
            const auto actual = routes(candidate, *font);
            QJsonArray keys;
            for (const auto& entry : actual)
                keys.append(entry.toObject()["code_hex"]);
            require(keys == row["exact_final_used_keys"].toArray() &&
                        actual.size() == row["used_codekey_count"].toInt(),
                    "Final raw keyset differs from fixed retained-only code union");
            const auto measured = textMeasurement(candidate);
            const auto text = measured["raw_unicode"].toString();
            require(!text.contains(row["remove"].toString()),
                    "Removed phrase remains in raw glyph Unicode");
            for (const auto& term : row["keep"].toArray())
                require(text.contains(term.toString()),
                        "Frozen retained phrase absent from raw glyph Unicode");
            int publicCid = -1, aliasCount = 0;
            for (const auto& entry : actual)
                if (entry.toObject()["code_hex"] == "41")
                    publicCid = entry.toObject()["cid"].toInt();
            require(publicCid > 0, "Retained public CID absent");
            for (const auto& entry : actual)
                if (entry.toObject()["code_hex"].toString().size() == 4)
                {
                    require(entry.toObject()["cid"].toInt() == publicCid &&
                                entry.toObject()["unicode"] == row["alias_unicode"],
                            "Alias routes do not share the retained public CID");
                    ++aliasCount;
                }
            require(aliasCount == row["alias_codes"].toArray().size(), "Final alias count differs");
            result["output"] = destination;
            result["output_sha256"] = digest(bytes(destination));
            result["raw_routes"] = actual;
            result["target_geometry"] = measured;
            result["external_render_and_sfnt_closure"] =
                "UNEXECUTED by native runner; independent examiner required";
        }
        result["status"] = "PASS";
    }
    catch (const std::exception& exception)
    {
        result["failure"] = QString::fromUtf8(exception.what());
    }
    return result;
}
QJsonObject directBuilderCase(const QString& root, const QJsonObject& fixed, bool oversized)
{
    QJsonObject result{{"name", oversized ? "builder-257" : "builder-256"}, {"status", "FAIL"}};
    try
    {
        const auto path = sourcePath(root, fixed);
        auto source = readPdf(path);
        const auto sourceBefore = encodePdf(source);
        const auto* fonts = pageFonts(source);
        const PDFObject* sourceFont = nullptr;
        QByteArray sourceId;
        for (size_t i = 0; i < fonts->getCount(); ++i)
        {
            const auto* font = source.getDictionaryFromObject(fonts->getValue(i));
            if (font && font->hasKey("DescendantFonts"))
            {
                require(sourceFont == nullptr, "Ambiguous source Type0 font");
                sourceFont = &fonts->getValue(i);
                sourceId = fonts->getKey(i).getString();
            }
        }
        require(sourceFont != nullptr, "Source Type0 font absent");
        pdf::PDFFontCache cache{128, 128};
        cache.setDocument(pdf::PDFModifiedDocument(&source, nullptr));
        const auto font = cache.getFont(*sourceFont, sourceId);
        const auto* descriptor = font->getFontDescriptor();
        require(digest(descriptor->fontFile2) == fixed["source_sfnt_sha256"].toString(),
                "Direct builder source SFNT mismatch");
        const auto glyph = fixed["glyph"].toObject();
        const auto character = glyph["unicode"].toString();
        require(character.size() == 1, "Direct glyph must be one BMP character");
        const QVector<RedactionGlyph> glyphs{
            {quint16(glyph["gid"].toInt()), character[0], glyph["width"].toDouble()}};
        QVector<RedactionCodeRoute> sourceCodes;
        const auto expected = fixed[oversized ? "invalid_keys" : "valid_keys"].toArray();
        for (const auto& code : expected)
            sourceCodes.append({QByteArray::fromHex(code.toString().toLatin1()), 1, character[0]});
        require(sourceCodes.size() == fixed[oversized ? "invalid_count" : "valid_count"].toInt(),
                "Frozen direct route count differs");
        pdf::PDFDocumentBuilder builder(&source);
        const auto builderBefore = encodePdf(builder.build());
        RedactionFontCvtUsage usage;
        bool refused = false;
        QString error;
        PDFObject generated;
        try
        {
            generated = rebuildRedactionFont(*descriptor, glyphs, builder, {}, &usage,
                                             RedactionFontBounds::EmbeddedCMapSource, sourceCodes);
        }
        catch (const std::exception& exception)
        {
            refused = true;
            error = QString::fromUtf8(exception.what());
        }
        require(refused == oversized, "Direct optional builder boundary differs");
        result["refused"] = refused;
        result["error"] = error;
        result["route_count"] = sourceCodes.size();
        if (oversized)
        {
            const bool atomic = encodePdf(builder.build()) == builderBefore && usage.empty();
            result["builder_storage_unchanged"] = atomic;
            require(atomic && error.contains("TrueType"),
                    "257 guard mutated builder before refusal");
        }
        else
        {
            auto built = builder.build();
            require(generated.isDictionary(), "Direct rebuilt font dictionary absent");
            const auto actual = routes(built, *generated.getDictionary());
            QSet<QString> actualKeys, expectedKeys;
            for (const auto& code : expected)
                expectedKeys.insert(code.toString());
            for (const auto& entry : actual)
            {
                const auto route = entry.toObject();
                actualKeys.insert(route["code_hex"].toString());
                require(route["cid"].toInt() == 1 && route["unicode"].toString() == character,
                        "Direct alias route differs from fixed CID/Unicode");
            }
            require(actual.size() == 256 && actualKeys == expectedKeys,
                    "Direct 256 route union differs");
            result["raw_routes"] = actual;
            result["own_decoder_roundtrip"] = true;
            // Preserve the exact source-frozen diagnostic literal.
            // clang-format off
            result["scope"] = "In-memory builder and decoder boundary, not document security or raster acceptance";
            // clang-format on
        }
        const bool unchanged =
            encodePdf(source) == sourceBefore && digest(bytes(path)) == fixed["sha256"].toString();
        result["source_unchanged"] = unchanged;
        require(unchanged, "Direct builder changed source");
        result["status"] = "PASS";
    }
    catch (const std::exception& exception)
    {
        result["failure"] = QString::fromUtf8(exception.what());
    }
    return result;
}
QJsonArray modules()
{
    QJsonArray result;
#ifdef Q_OS_WIN
    const HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    require(snapshot != INVALID_HANDLE_VALUE, "Cannot inspect own loaded modules");
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
        do
        {
            const auto name = QString::fromWCharArray(entry.szModule);
            if (name.contains("Pdf4Qt", Qt::CaseInsensitive) ||
                name.startsWith("Qt6", Qt::CaseInsensitive))
            {
                const auto path = QString::fromWCharArray(entry.szExePath);
                result.append(
                    QJsonObject{{"name", name}, {"path", path}, {"sha256", digest(bytes(path))}});
            }
        } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
#endif
    return result;
}
} // namespace

// Separate boundary test executable; no additional main/full selftest names.
int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() != 4)
        return 2; // project root, fixed boundary criteria, fresh output directory
    const auto root = QDir(arguments[1]).absolutePath();
    const auto specPath = QFileInfo(arguments[2]).absoluteFilePath();
    const auto output = QDir(arguments[3]).absolutePath();
    if (QFileInfo::exists(output) || !QDir().mkpath(output))
        return 2;
    QJsonObject result{
        {"fixture_criteria_sha256", criteriaPin},
        {"pid", double(QCoreApplication::applicationPid())},
        {"scope", "RAW boundary document atomicity and optional font-builder limits"},
        {"status", "FAIL"}};
    int exitCode = 1;
    try
    {
        const auto raw = bytes(specPath);
        require(digest(raw) == criteriaPin, "Boundary criteria pin mismatch");
        const auto fixed = QJsonDocument::fromJson(raw).object();
        const auto primary = fixed["main_43_criteria"].toObject();
        require(digest(bytes(QDir(root).absoluteFilePath(primary["path"].toString()))) ==
                    primary["sha256"].toString(),
                "Main 43 criteria changed");
        result["criteria_path"] = specPath;
        result["chronology"] = fixed["chronology"];
        const auto executable = QCoreApplication::applicationFilePath();
        result["executable"] =
            QJsonObject{{"path", executable}, {"sha256", digest(bytes(executable))}};
        const auto loaded = modules();
        result["loaded_modules"] = loaded;
        bool coreLoaded = false, widgetsLoaded = false;
        for (const auto& module : loaded)
        {
            const auto name = module.toObject()["name"].toString();
            coreLoaded = coreLoaded || name.contains("Pdf4QtLibCore", Qt::CaseInsensitive);
            widgetsLoaded = widgetsLoaded || name.contains("Pdf4QtLibWidgets", Qt::CaseInsensitive);
        }
#ifdef Q_OS_WIN
        require(coreLoaded, "Isolated runner Core module was not captured");
#endif
        result["widgets_module_state"] =
            widgetsLoaded ? "LOADED (see actual module path/SHA)" : "UNLOADED";
        result["module_scope"] =
            "Own boundary PID/executable/Core/Qt only; primary app runtime proof is separate";
        QJsonArray cases;
        int failures = 0;
        require(fixed["cases"].toArray().size() == 4, "Expected four frozen document cases");
        for (const auto& row : fixed["cases"].toArray())
        {
            const auto entry = documentCase(root, output, row.toObject());
            cases.append(entry);
            failures += entry["status"] != "PASS";
        }
        for (bool oversized : {false, true})
        {
            const auto entry =
                directBuilderCase(root, fixed["direct_builder"].toObject(), oversized);
            cases.append(entry);
            failures += entry["status"] != "PASS";
        }
        result["cases"] = cases;
        result["native_pass"] = 6 - failures;
        result["native_fail"] = failures;
        result["external_raster_and_security"] = "UNEXECUTED here";
        result["status"] = failures ? "FAIL" : "PASS";
        exitCode = failures ? 1 : 0;
    }
    catch (const std::exception& exception)
    {
        result["failure"] = QString::fromUtf8(exception.what());
    }
    result["completed_at_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QFile report(QDir(output).absoluteFilePath("boundary-results.json"));
    const auto serialized = QJsonDocument(result).toJson();
    if (!report.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        report.write(serialized) != serialized.size())
        return 3;
    return exitCode;
}
