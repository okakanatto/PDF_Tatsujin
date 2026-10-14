#include "redaction_shared_tests.h"
#include "pdfcms.h"
#include "pdfpagecontentprocessor.h"
#include "redaction_cmap.h"
#include "redaction_dialog.h"
#include "redaction_document.h"
#include "redaction_font.h"
#include "redaction_ui_test_support.h"
#include "window.h"
#include <QtTest/QTest>
#include <limits>

namespace tatsu
{
namespace
{
QJsonObject copyUiAtomic(const QString& fixtures, const QString& output, const QString& folder,
                         const QByteArray& pin, int success, int refusal, const QString& stem);
void check(bool value, const QString& message)
{
    if (!value)
        fail(message);
}
QJsonArray cases(const QString& fixtures)
{
    const auto path = fixtures + "/redaction-copy/shared-fonts-v1/criteria.json";
    check(fileHash(path).toHex() ==
              "a24478e7395e3bfcc4046eb0266aba324c5858d6c3c9db0e447aba5d9ec0f586",
          "Frozen shared font criteria");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read shared font criteria");
    return QJsonDocument::fromJson(file.readAll()).object()["cases"].toArray();
}
QRectF rectangle(const QJsonObject& row)
{
    const auto r = row["rect"].toArray();
    return {r[0].toDouble(), r[1].toDouble(), r[2].toDouble(), r[3].toDouble()};
}
PDFDocument input(const QString& fixtures, const QJsonObject& row,
                  const QString& folder = "shared-fonts-v1")
{
    const auto path = fixtures + "/redaction-copy/" + folder + "/" + row["file"].toString();
    check(fileHash(path).toHex() == row["sha256"].toString().toLatin1(), "Frozen shared input");
    return readPdf(path);
}
void retained(PDFDocument document, const QJsonObject& row, const PDFDocument* baseline = nullptr)
{
    const auto text = pageText(document, 0);
    check(!text.contains(row["remove"].toString()), "Target phrase removed");
    for (const auto& term : row["keep"].toArray())
    {
        auto source = baseline ? *baseline : PDFDocument();
        const auto original = baseline ? pageText(source, 0) : QString();
        if (baseline && (term == "KEEP_FOLLOWING_LINE" || term == "EXTRA_LINE") &&
            !original.contains(term.toString()))
        {
            bool matched = false;
            for (const auto& line : original.split('\n'))
            {
                auto joined = line;
                joined.remove(QRegularExpression("\\s"));
                if (joined == term.toString())
                {
                    check(text.contains(line), "Exact input Qt extraction of spaced outside line");
                    matched = true;
                }
            }
            check(matched, "Frozen spaced line is present in the input Qt extraction: " + original);
        }
        else
            check(text.contains(term.toString()),
                  "Outside target text retained: " + term.toString() + " in " + text);
    }
    for (const auto& term : row["second_page_terms"].toArray())
        check(pageText(document, 1).contains(term.toString()), "Other page text retained");
}
} // namespace
QJsonObject testRedactionShared(const QString& fixtures, const QString& output)
{
    QJsonArray results;
    int failed = 0;
    for (const auto& value : cases(fixtures))
    {
        const auto row = value.toObject();
        if (row["refused"].toBool())
            continue;
        const auto source = input(fixtures, row);
        const auto before = encodePdf(source);
        const auto path = output + "/shared-" + row["file"].toString();
        QJsonObject record{{"file", row["file"]}, {"status", "FAIL"}};
        try
        {
            exportRedactedPdf(source, {{0, {rectangle(row)}}}, path);
            retained(readPdf(path), row);
            check(encodePdf(source) == before, "Shared redaction source unchanged");
            record["status"] = "PASS";
        }
        catch (const std::exception& error)
        {
            record["error"] = QString::fromUtf8(error.what());
            ++failed;
        }
        results.append(record);
    }
    QFile file(output + "/shared-case-results.json");
    check(file.open(QIODevice::WriteOnly), "Save all individual shared outcomes");
    file.write(QJsonDocument(results).toJson());
    file.close();
    check(!failed, "Shared font cases failed: " + QString::number(failed));
    return {{"cases", results}};
}
QJsonObject testRedactionSharedFailures(const QString& fixtures, const QString& output)
{
    int rejected = 0;
    auto refuse =
        [&](PDFDocument source, QRectF bounds, QString name, std::function<bool()> cancelled = {})
    {
        const auto before = encodePdf(source);
        const auto path = output + "/shared-refused-" + name + ".pdf";
        bool refused = false;
        try
        {
            exportRedactedPdf(source, {{0, {bounds}}}, path, cancelled);
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        check(refused && !QFileInfo::exists(path) && encodePdf(source) == before,
              "Shared resource failure is atomic: " + name);
        ++rejected;
    };
    const auto rows = cases(fixtures);
    for (const auto& value : rows)
    {
        const auto row = value.toObject();
        if (row["refused"].toBool())
            refuse(input(fixtures, row), rectangle(row), row["file"].toString());
    }
    const auto row = rows[0].toObject();
    const auto source = input(fixtures, row);
    int observations = 0;
    prepareRedactionCandidate(source, {{0, {rectangle(row)}}},
                              [&]
                              {
                                  ++observations;
                                  return false;
                              });
    for (const int at : {1, observations / 2, observations - 1})
    {
        int calls = 0;
        refuse(source, rectangle(row), "cancel-" + QString::number(at),
               [&] { return ++calls >= at; });
        check(calls == at, "Shared cancellation reaches exact checkpoint");
    }
    return {{"rejected", rejected}, {"checkpoints", observations}};
}
namespace
{
QJsonObject sharedUi(const QString& fixtures, const QString& output, const QJsonObject& row,
                     const QString& folder, const QString& stem)
{
    const auto path = fixtures + "/redaction-copy/" + folder + "/" + row["file"].toString();
    Window original;
    original.openFile(path);
    original.doc.putSignature(1, "別ページの未保存署名", {60, 80}, 12, Qt::black);
    original.refresh();
    const auto before = encodePdf(original.doc.pdf());
    const auto cursor = original.doc.cursor, saved = original.doc.saved;
    if (stem == "hinting" || stem == "hint-boundary" || stem == "cvt" ||
        stem == "cvt-index-boundary" || stem == "mixed-cmap")
    {
        QFile snapshot(output + "/" + stem + "-ui-before.pdf");
        check(snapshot.open(QIODevice::WriteOnly) && snapshot.write(before) == before.size(),
              "Save independent hinting UI baseline with unsaved signature");
    }
    const auto destination = output + "/" + stem + "-ui.pdf";
    RedactionDialog dialog(original.doc.pdf(), 0, {{path, fileHash(path)}}, destination);
    dialog.show();
    testing::ready(dialog, 0);
    testing::add(dialog,
                 pageMatrix(original.doc.pdf().getCatalog()->getPage(0)).mapRect(rectangle(row)));
    dialog.findChild<QCheckBox*>("redactionConsent")->setChecked(true);
    dialog.findChild<QPushButton*>("saveRedactedCopy")->click();
    check(QTest::qWaitFor(
              [&] { return !dialog.findChild<QProgressBar*>("redactionProgress")->isVisible(); },
              30000),
          "Shared font UI worker completes");
    check(!dialog.savedPath().isEmpty(), dialog.findChild<QLabel*>("redactionMessage")->text());
    auto reopened = readPdf(destination);
    retained(reopened, row);
    check(signatures(reopened, 1).size() == 1 &&
              signatures(reopened, 1).first().text == "別ページの未保存署名",
          "Unrelated signature retained");
    check(encodePdf(original.doc.pdf()) == before && original.doc.cursor == cursor &&
              original.doc.saved == saved,
          "Original shared document and Undo untouched");
    original.doc.undo();
    original.doc.redo();
    check(encodePdf(original.doc.pdf()) == before, "Shared original Undo/Redo remains usable");
    Document editable;
    editable.open(destination);
    editable.putSignature(1, "共有字体の処理後も再編集", {60, 120}, 12, Qt::black);
    editable.save(output + "/" + stem + "-ui-reedited.pdf");
    auto reedited = readPdf(output + "/" + stem + "-ui-reedited.pdf");
    check(signatures(reedited, 1).size() == 2 &&
              signatures(reedited, 1).last().text == "共有字体の処理後も再編集",
          "Shared resource output remains editable");
    return {{"source_unchanged", true}, {"outside_signature_and_reedit", true}};
}
QJsonObject mixedDecoderSafety(const QString& fixtures)
{
    const auto safetyPath = fixtures + "/redaction-copy/mixed-cmap-v2/decoder-safety-r2.json";
    check(fileHash(safetyPath).toHex() ==
              "5f39b3584811a0411573999bc5e7e0aa7eb241127c1ae0266f6ab87299287efa",
          "Frozen direct mixed decoder safety criteria");
    QFile safetyFile(safetyPath);
    check(safetyFile.open(QIODevice::ReadOnly), "Read direct mixed decoder criteria");
    const auto safety = QJsonDocument::fromJson(safetyFile.readAll()).object();
    const auto sourcePath =
        fixtures + "/redaction-copy/mixed-cmap-v2/" + safety["source"].toString();
    check(fileHash(sourcePath).toHex() ==
              "2a599d6f56e6477c5d338d65bfeef459c8f91f741e5f95c5342bf5d6e749d124",
          "Frozen direct decoder PDF source");
    const auto source = readPdf(sourcePath);
    const auto before = encodePdf(source);
    const auto resources =
        source.getDictionaryFromObject(source.getCatalog()->getPage(0)->getResources());
    check(resources != nullptr, "Direct decoder source resources");
    const auto fonts = source.getDictionaryFromObject(resources->get("Font"));
    check(fonts != nullptr, "Direct decoder source fonts");
    const auto font = source.getDictionaryFromObject(fonts->get("F2+0"));
    check(font != nullptr, "Direct decoder frozen mixed font binding");
    bool armed = false;
    int observed = 0, cutoff = std::numeric_limits<int>::max();
    RedactionCMap map(source, *font, [&] { return armed && ++observed >= cutoff; });
    armed = true;
    const auto code = QByteArray::fromHex(safety["valid_code_hex"].toString().toLatin1());
    const auto decoded = map.decode(code.repeated(safety["successful_decode_codes"].toInt()));
    check(decoded.size() == safety["successful_decode_codes"].toInt() && observed == decoded.size(),
          "Direct decoder normal code count and scanning callbacks");
    for (const auto& value : decoded)
        check(value.cid == safety["expected_cid"].toInt() &&
                  value.character == safety["expected_unicode"].toString().front(),
              "Independent Encoding CID and ToUnicode character expectations");
    const auto valid = code.repeated(safety["successful_validation_codes"].toInt());
    observed = 0;
    map.validate(valid);
    check(observed == safety["successful_validation_codes"].toInt(),
          "Validation-only scanner visits each code once");
    const auto malformed = safety["malformed_input"].toObject();
    const auto invalid = QByteArray::fromHex(malformed["prefix_hex"].toString().toLatin1()) +
                         QByteArray::fromHex(malformed["tail_hex"].toString().toLatin1())
                             .repeated(malformed["tail_bytes"].toInt());
    observed = 0;
    bool refused = false;
    try
    {
        map.validate(invalid);
    }
    catch (const std::exception&)
    {
        refused = true;
    }
    check(refused && observed == malformed["expected_scanning_callbacks"].toInt(),
          "Malformed first prefix refuses before scanning its long tail");
    int cancelled = 0;
    for (const auto& value : safety["cancellation"].toObject()["cutoffs"].toArray())
        for (bool validationOnly : {false, true})
        {
            cutoff = value.toInt();
            observed = 0;
            refused = false;
            try
            {
                if (validationOnly)
                    map.validate(valid);
                else
                    map.decode(valid);
            }
            catch (const std::exception&)
            {
                refused = true;
            }
            check(refused && observed == cutoff && encodePdf(source) == before,
                  "Direct decoding and validation cancel at each fixed cutoff atomically");
            ++cancelled;
        }
    check(encodePdf(source) == before, "Direct scanner leaves source bytes unchanged");
    return QJsonObject{{"valid_codes", valid.size()},
                       {"invalid_tail_bytes", invalid.size() - 1},
                       {"cancelled", cancelled},
                       {"source_unchanged", true},
                       {"PDF_rendering", "not exercised by direct scanner test"}};
}
class MixedSpaceGeometry final : public pdf::PDFPageContentProcessor
{
public:
    using PDFPageContentProcessor::PDFPageContentProcessor;
    QString characters;
    QVector<QPointF> origins;

protected:
    void performOutputCharacter(const pdf::PDFTextCharacterInfo& info) override
    {
        if (info.fontSize == 22)
        {
            characters.append(info.character);
            origins.append(info.matrix.map(QPointF()));
        }
    }
};
QJsonObject mixedSpaceSelection(const QString& fixtures, const QString& output)
{
    const auto fixed = fixtures + "/redaction-copy/mixed-cmap-v2/space-selection.json";
    check(fileHash(fixed).toHex() ==
              "2168ad0c1ffd1bfb0d618d2068c80265197fb3f8f21e8f2511cf4754c26e7698",
          "Frozen full space selection condition");
    QFile file(fixed);
    check(file.open(QIODevice::ReadOnly), "Read full space selection condition");
    const auto row = QJsonDocument::fromJson(file.readAll()).object();
    const auto sourcePath = fixtures + "/redaction-copy/mixed-cmap-v2/" + row["source"].toString();
    check(fileHash(sourcePath).toHex() == row["source_sha256"].toString().toLatin1(),
          "Frozen full space selection input");
    const auto source = readPdf(sourcePath);
    const auto before = encodePdf(source);
    const auto destination = output + "/" + row["native_output"].toString();
    exportRedactedPdf(source, {{0, {rectangle(row)}}}, destination);
    check(encodePdf(source) == before, "Full space selection source unchanged");
    auto document = readPdf(destination);
    pdf::PDFFontCache fonts{128, 128};
    fonts.setDocument(pdf::PDFModifiedDocument(&document, nullptr));
    pdf::PDFCMSManager manager(nullptr);
    const auto cms = manager.getCurrentCMS();
    MixedSpaceGeometry geometry(document.getCatalog()->getPage(0), &document, &fonts, cms.data(),
                                nullptr, {}, {});
    check(geometry.processContents().isEmpty(), "Read full space selection output glyphs");
    QString expected;
    for (const auto& character : row["expected_retained_unicode"].toArray())
        expected += character.toString();
    check(geometry.characters == expected,
          "Exact retained Unicode sequence without inferred spaces");
    const auto origins = row["expected_retained_origins_pt"].toArray();
    check(geometry.origins.size() == origins.size(), "Retained space selection origin count");
    double maximum = 0;
    for (qsizetype index = 0; index < geometry.origins.size(); ++index)
    {
        const auto point = origins[index].toArray();
        maximum = qMax(maximum, QLineF(geometry.origins[index],
                                       QPointF(point[0].toDouble(), point[1].toDouble()))
                                    .length());
    }
    check(maximum <= row["max_position_error_pt"].toDouble(),
          "Frozen full space selection position threshold");
    return {{"source_unchanged", true},
            {"retained_glyphs", geometry.characters.size()},
            {"position_error_pt", maximum},
            {"full_space_removed", true},
            {"external_raster", "not exercised by this native condition"}};
}
QJsonObject pathCases(const QString& fixtures, const QString& output, const QString& folder,
                      const QByteArray& pin, const QString& stem = "path")
{
    const auto path = fixtures + "/redaction-copy/" + folder + "/criteria.json";
    check(fileHash(path).toHex() == pin, "Frozen vector path criteria");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read vector criteria");
    const auto rows = QJsonDocument::fromJson(file.readAll()).object()["cases"].toArray();
    QJsonArray results;
    int refused = 0;
    for (const auto& value : rows)
    {
        const auto row = value.toObject();
        const auto source = input(fixtures, row, folder);
        const auto before = encodePdf(source);
        const auto destination = output + "/" + stem + "-" + row["file"].toString();
        bool failed = false;
        QString errorMessage;
        QMap<int, QVector<QRectF>> plan{{0, {rectangle(row)}}};
        if (row["plan"].isArray())
        {
            plan.clear();
            for (const auto& entry : row["plan"].toArray())
            {
                const auto item = entry.toObject();
                plan[item["page"].toInt()].append(rectangle(item));
            }
        }
        try
        {
            exportRedactedPdf(source, plan, destination);
        }
        catch (const std::exception& error)
        {
            failed = true;
            errorMessage = QString::fromUtf8(error.what());
        }
        check(failed == row["refused"].toBool(),
              "Vector path acceptance: " + row["file"].toString() + " " + errorMessage);
        if (failed)
        {
            check(!QFileInfo::exists(destination), "Refused vector output absent");
            ++refused;
        }
        else
            retained(readPdf(destination), row,
                     folder == "custom-cmap-v1" || folder.startsWith("mixed-cmap-") ? &source
                                                                                    : nullptr);
        check(encodePdf(source) == before, "Vector source unchanged");
        results.append(QJsonObject{{"file", row["file"]}, {"refused", failed}});
    }
    return {{"cases", results}, {"refused", refused}};
}
} // namespace
QJsonObject testRedactionSharedUi(const QString& fixtures, const QString& output)
{
    return sharedUi(fixtures, output, cases(fixtures)[0].toObject(), "shared-fonts-v1", "shared");
}
QJsonObject testRedactionPaths(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "vector-paths-v1",
                     "027f0ab58bf461ce4f47762055244e6ddbe4e3141cfeffed5e60aa7483fbc869");
}
QJsonObject testRedactionSharedFollowup(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "shared-followup-v2",
                     "d4e635ed3ec17063fece57722f81dc84d9378919ad39835c730e8387dc848c5b");
}
QJsonObject testRedactionFontTables(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "font-tables-v1",
                     "7a8d6e747eba9816486ec202aac858acc1bdbf686c9c0931808fc9f725981cdd");
}
QJsonObject testRedactionHinting(const QString& fixtures, const QString& output)
{
    return {{"main", pathCases(fixtures, output, "hinting-v1",
                               "9704e6be7527959c40f97e3e1cb097798777f9edb17d855ea17430f30631ff27",
                               "hinting")},
            {"global_only",
             pathCases(fixtures, output, "hinting-global-v1",
                       "107fd6c0880449634310db823e4afe69e9937b0557ce28c321db8d94ef91170d",
                       "hinting-global")}};
}
QJsonObject testRedactionHintingUi(const QString& fixtures, const QString& output)
{
    const auto path = fixtures + "/redaction-copy/hinting-v1/criteria.json";
    check(fileHash(path).toHex() ==
              "9704e6be7527959c40f97e3e1cb097798777f9edb17d855ea17430f30631ff27",
          "Frozen hinting criteria");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read hinting criteria");
    const auto rows = QJsonDocument::fromJson(file.readAll()).object()["cases"].toArray();
    const auto row = rows[1].toObject(); // The shared two-page source.
    auto source = input(fixtures, row, "hinting-v1");
    const auto before = encodePdf(source);
    int checkpoints = 0;
    prepareRedactionCandidate(source, {{0, {rectangle(row)}}},
                              [&]
                              {
                                  ++checkpoints;
                                  return false;
                              });
    for (int at = 1; at <= checkpoints; ++at)
    {
        int observed = 0;
        bool refused = false;
        try
        {
            prepareRedactionCandidate(source, {{0, {rectangle(row)}}},
                                      [&] { return ++observed >= at; });
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        check(refused && observed == at && encodePdf(source) == before,
              "Hinting cancellation is atomic at each checkpoint");
    }
    auto result = copyUiAtomic(fixtures, output, "hinting-v1",
                               "9704e6be7527959c40f97e3e1cb097798777f9edb17d855ea17430f30631ff27",
                               1, 16, "hinting");
    result["cancellation_checkpoints"] = checkpoints;
    return result;
}
QJsonObject testRedactionHintBoundaries(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "hint-boundaries-v1",
                     "1fcd232da7b7ae1af28430c8fdb7a0e92318c72895637a258f66c874dd3e4941",
                     "hint-boundary");
}
QJsonObject testRedactionHintBoundariesUi(const QString& fixtures, const QString& output)
{
    return copyUiAtomic(fixtures, output, "hint-boundaries-v1",
                        "1fcd232da7b7ae1af28430c8fdb7a0e92318c72895637a258f66c874dd3e4941", 13, 6,
                        "hint-boundary");
}
QJsonObject testRedactionCustomCMap(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "custom-cmap-v1",
                     "044024c5bf7ec5a52802e6ba0d0265237aefee83471147207c61a882f7d4a36f");
}
QJsonObject testRedactionMixedCMap(const QString& fixtures, const QString& output)
{
    check(fileHash(fixtures + "/redaction-copy/mixed-cmap-v2/renderer-metrics.json").toHex() ==
              "7d0fdd3d8a1888796566467e385a0d8c11695c9b8b236065ba75241161b75056",
          "Frozen scoped source renderer bounds policy");
    auto result =
        pathCases(fixtures, output, "mixed-cmap-v2",
                  "796b7ae0ef979583d0180c91c1020dd30405530d044daf39e83339f0b2ae326a", "mixed-cmap");
    result["decoder_safety"] = mixedDecoderSafety(fixtures);
    result["space_selection"] = mixedSpaceSelection(fixtures, output);
    return result;
}
QJsonObject testRedactionMixedCMapUi(const QString& fixtures, const QString& output)
{
    return copyUiAtomic(fixtures, output, "mixed-cmap-v2",
                        "796b7ae0ef979583d0180c91c1020dd30405530d044daf39e83339f0b2ae326a", 14, 19,
                        "mixed-cmap");
}
QJsonObject testRedactionCvtReferences(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "cvt-references-v1",
                     "1234729946a784d36e2d32bca4fc1e10642b01c3e900539996f236a440858be7", "cvt");
}
QJsonObject testRedactionCvtReferencesUi(const QString& fixtures, const QString& output)
{
    return copyUiAtomic(fixtures, output, "cvt-references-v1",
                        "1234729946a784d36e2d32bca4fc1e10642b01c3e900539996f236a440858be7", 30, 12,
                        "cvt");
}
QJsonObject testRedactionCvtIndexBoundaries(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "cvt-index-boundaries-v1",
                     "d8243815eeec4a579639502d3d66533ba876bec483fdc6e3edc813d83453c596",
                     "cvt-index-boundary");
}
QJsonObject testRedactionCvtIndexBoundariesUi(const QString& fixtures, const QString& output)
{
    return copyUiAtomic(fixtures, output, "cvt-index-boundaries-v1",
                        "d8243815eeec4a579639502d3d66533ba876bec483fdc6e3edc813d83453c596", 6, 1,
                        "cvt-index-boundary");
}
QJsonObject testRedactionCvtReachability()
{
    const PDFObjectReference plain(101, 0), hinted(102, 0), unrelated(103, 0);
    RedactionFontCvtUsage usage;
    usage.record(plain, "source-A", {});
    usage.record(hinted, "source-A", {7});
    usage.record(unrelated, "source-B", {});
    auto rejected = [&](const std::set<PDFObjectReference>& reachable,
                        const std::function<bool()>& cancelled = {})
    {
        bool refused = false;
        try
        {
            usage.validate(reachable, cancelled);
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        check(refused, "CVT witness must belong to the same final reachable source");
    };
    rejected({plain}); // An overwritten MIAP subset cannot authorize the final plain subset.
    usage.validate({plain, hinted}, {});
    rejected({hinted, unrelated}); // A different source cannot borrow its witness.
    usage.validate({hinted}, {});
    usage.validate({}, {});
    rejected({hinted}, [] { return true; });
    return {{"reachable_shared_source", true},
            {"unreachable_witness_rejected", true},
            {"different_source_rejected", true},
            {"cancelled_rejected", true}};
}
QJsonObject testRedactionCMapFollowup(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "custom-cmap-followup-v1",
                     "463ab0757312ee1ceb00605452bf6369e71f25c1acdd1b15ea8eb1fcfbcb720c");
}
QJsonObject testRedactionRedundantClips(const QString& fixtures, const QString& output)
{
    return {
        {"main",
         pathCases(fixtures, output, "redundant-clips-v1",
                   "e3d31bbc74fdf0c28830fb746b1b6d29e4c058eca08f0d8940766cf401bd0c46", "clip")},
        {"followup",
         pathCases(fixtures, output, "redundant-clips-followup-v1",
                   "81489b5b301af21191ab4103e3425a417fe2da379232e47faf56d48b848d8bc0", "clip")}};
}
QJsonObject testRedactionFontAliases(const QString& fixtures, const QString& output)
{
    return {
        {"main",
         pathCases(fixtures, output, "font-aliases-v1",
                   "06fd7bbcef23bf82d58852c351bd3d0d5098b96cbfea1ea23d5639e61332bcd9", "alias")},
        {"followup",
         pathCases(fixtures, output, "font-aliases-followup-v1",
                   "4aa425628996b0a58f49d8f99b88421e19db3a3d61f355553607118519687550", "alias")},
        {"boundaries",
         pathCases(fixtures, output, "font-aliases-boundaries-v1",
                   "e0bef06a28cfd3ae32ccb1c09c67baa8373259fbfdb63eede56ef4b1b3aadf32", "alias")}};
}
QJsonObject testRedactionFontBinding(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "font-binding-v1",
                     "b167c53f03b5939aae9206d8bf0c38b205790413698e87c93462732b4c07f62b", "binding");
}
QJsonObject testRedactionDirectFontSchema(const QString& fixtures, const QString& output)
{
    return {
        {"main",
         pathCases(fixtures, output, "font-schema-direct-v1",
                   "8c40a3c704d4581ef751ddc6217295f970c92cd189bd39277ec8bb9e0c7d4f2c", "schema")},
        {"followup",
         pathCases(fixtures, output, "font-schema-direct-followup-v1",
                   "e7569c49f5917663c07232ee1cd542ccebbbc719fd545fb9841428ad1b87361d", "schema")}};
}
QJsonObject testRedactionStandardFontFamilies(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "standard-font-families-v2",
                     "708cfc9929203151427b7d09a9914cfe294b9112b13fab024abbd5fb5e58ed9d", "schema");
}
QJsonObject testRedactionFontBindingFollowup(const QString& fixtures, const QString& output)
{
    return pathCases(fixtures, output, "font-binding-followup-v1",
                     "7f585559474ea31e06f94c1b1f18c40a575e0b11e7e9dd5741d473d3bc3c9720", "binding");
}
QJsonObject testRedactionFontSchema(const QString& fixtures, const QString& output)
{
    return {
        {"main",
         pathCases(fixtures, output, "font-schema-v2",
                   "91765aeee26617872f16ba46f4b9cadba61468279ef157a141d8e9891630ad63", "schema")},
        {"followup",
         pathCases(fixtures, output, "font-schema-followup-v1",
                   "a2a73f6c9a8d2a087bc511509ae7b83f2eb81a267ad5dc42eb944d38440e02c6", "schema")}};
}
namespace
{
QJsonObject copyUiAtomic(const QString& fixtures, const QString& output, const QString& folder,
                         const QByteArray& pin, int success, int refusal, const QString& stem)
{
    const auto criteria = fixtures + "/redaction-copy/" + folder + "/criteria.json";
    check(fileHash(criteria).toHex() == pin, "Frozen copy UI criteria");
    QFile file(criteria);
    check(file.open(QIODevice::ReadOnly), "Read CMap UI criteria");
    const auto rows = QJsonDocument::fromJson(file.readAll()).object()["cases"].toArray();
    const auto row = rows[success].toObject();
    const auto source = input(fixtures, row, folder);
    const auto before = encodePdf(source);
    int observations = 0;
    prepareRedactionCandidate(source, {{0, {rectangle(row)}}},
                              [&]
                              {
                                  ++observations;
                                  return false;
                              });
    for (const int at : {1, observations / 2, observations - 1})
    {
        int calls = 0;
        bool refused = false;
        const auto destination = output + "/" + stem + "-cancel-" + QString::number(at) + ".pdf";
        try
        {
            exportRedactedPdf(source, {{0, {rectangle(row)}}}, destination,
                              [&] { return ++calls >= at; });
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        check(refused && calls == at && !QFileInfo::exists(destination) &&
                  encodePdf(source) == before,
              "CMap cancellation leaves source and destination untouched");
    }
    auto record = sharedUi(fixtures, output, row, folder, stem);
    const auto rejected = rows[refusal].toObject();
    const auto path = fixtures + "/redaction-copy/" + folder + "/" + rejected["file"].toString();
    check(fileHash(path).toHex() == rejected["sha256"].toString().toLatin1(),
          "Frozen CMap refusal");
    Window original;
    original.openFile(path);
    original.doc.putSignature(0, "拒否時にも保持する未保存署名", {60, 80}, 12, Qt::black);
    original.refresh();
    const auto snapshot = encodePdf(original.doc.pdf());
    const auto cursor = original.doc.cursor, saved = original.doc.saved;
    const auto hash = fileHash(path);
    const auto destination = output + "/" + stem + "-ui-refused.pdf";
    RedactionDialog dialog(original.doc.pdf(), 0, {{path, hash}}, destination);
    dialog.show();
    testing::ready(dialog, 0);
    testing::add(
        dialog,
        pageMatrix(original.doc.pdf().getCatalog()->getPage(0)).mapRect(rectangle(rejected)));
    dialog.findChild<QCheckBox*>("redactionConsent")->setChecked(true);
    dialog.findChild<QPushButton*>("saveRedactedCopy")->click();
    check(QTest::qWaitFor(
              [&] { return !dialog.findChild<QProgressBar*>("redactionProgress")->isVisible(); },
              30000),
          "CMap refusal UI completes");
    check(dialog.savedPath().isEmpty() && !QFileInfo::exists(destination) &&
              !dialog.findChild<QLabel*>("redactionMessage")->text().isEmpty() &&
              encodePdf(original.doc.pdf()) == snapshot && original.doc.cursor == cursor &&
              original.doc.saved == saved && fileHash(path) == hash,
          "CMap refusal UI preserves original, unsaved edits, Undo and output");
    original.doc.undo();
    original.doc.redo();
    check(encodePdf(original.doc.pdf()) == snapshot, "CMap refused original remains editable");
    record["cancellation_checkpoints"] = observations;
    record["cancelled"] = 3;
    record["refusal_UI_atomic"] = true;
    record["native_GUI_OS_clipboard"] = "未実行; Qt offscreen";
    return record;
}
} // namespace
QJsonObject testRedactionCustomCMapUi(const QString& fixtures, const QString& output)
{
    return copyUiAtomic(fixtures, output, "custom-cmap-v1",
                        "044024c5bf7ec5a52802e6ba0d0265237aefee83471147207c61a882f7d4a36f", 4, 16,
                        "cmap");
}
QJsonObject testRedactionRedundantClipsUi(const QString& fixtures, const QString& output)
{
    return copyUiAtomic(fixtures, output, "redundant-clips-v1",
                        "e3d31bbc74fdf0c28830fb746b1b6d29e4c058eca08f0d8940766cf401bd0c46", 12, 13,
                        "clip");
}
QJsonObject testRedactionFontAliasesUi(const QString& fixtures, const QString& output)
{
    return copyUiAtomic(fixtures, output, "font-aliases-v1",
                        "06fd7bbcef23bf82d58852c351bd3d0d5098b96cbfea1ea23d5639e61332bcd9", 2, 9,
                        "alias");
}
QJsonObject testRedactionFontSchemaUi(const QString& fixtures, const QString& output)
{
    return copyUiAtomic(fixtures, output, "font-schema-v2",
                        "91765aeee26617872f16ba46f4b9cadba61468279ef157a141d8e9891630ad63", 4, 7,
                        "schema");
}
QJsonObject testRedactionDirectFontSchemaUi(const QString& fixtures, const QString& output)
{
    return copyUiAtomic(fixtures, output, "font-schema-direct-v1",
                        "8c40a3c704d4581ef751ddc6217295f970c92cd189bd39277ec8bb9e0c7d4f2c", 0, 3,
                        "direct-schema");
}
} // namespace tatsu
