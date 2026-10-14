#include "save_interruption_worker.h"
#include "document.h"
#include "pdf_objects.h"
#include "pdfdocumentbuilder.h"
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QThread>

namespace tatsu
{
void saveInterruptionWorker(const QString& fixtures, const QString& output, bool existing)
{
    // The parent supplies a fresh, owned synthetic-test directory. No product
    // hook or delay changes the actual Document::save path being interrupted.
    Document document;
    document.open(fixtures + "/D03.pdf");
    document.putSignature(0, "中断試験の署名", {60, 100}, 14, Qt::black);
    pdf::PDFDocumentBuilder builder(&document.pdf());
    auto page = document.pdf().getCatalog()->getPage(0)->getPageReference();
    auto dictionary = *builder.getObjectByReference(page).getDictionary();
    QByteArray payload(24 * 1024 * 1024, Qt::Uninitialized);
    QRandomGenerator random(0x54415453);
    for (qsizetype i = 0; i < payload.size(); ++i)
        payload[i] = char(random.generate());
    // An unused, deterministic stream enlarges this owned probe document
    // without changing the frozen input or its visible page content.
    const auto stream = builder.addObject(detail::streamObject({}, payload));
    detail::set(dictionary, "TatsuInterruptionProbe", pdf::PDFObject::createReference(stream));
    builder.setObject(page, detail::dictObject(dictionary));
    document.commit(builder.build());
    const auto destination = output + "/destination.pdf";
    if (existing && !QFile::copy(fixtures + "/D01.pdf", destination))
        fail("Cannot prepare the owned interruption destination");
    const auto baseline = fileHash(destination);
    writeCandidate(document.pdf(), output + "/expected.pdf");
    QSaveFile ready(output + "/prepared.json");
    if (!ready.open(QIODevice::WriteOnly))
        fail("Cannot announce interruption preparation");
    const auto bytes =
        QJsonDocument(QJsonObject{{"destination", existing ? "existing" : "new"},
                                  {"baseline_sha256", QString::fromLatin1(baseline.toHex())},
                                  {"expected_sha256",
                                   QString::fromLatin1(fileHash(output + "/expected.pdf").toHex())},
                                  {"expected_bytes", QFileInfo(output + "/expected.pdf").size()}})
            .toJson();
    if (ready.write(bytes) != bytes.size() || !ready.commit())
        fail("Cannot flush interruption preparation");
    QElapsedTimer waiting;
    waiting.start();
    while (!QFileInfo::exists(output + "/begin"))
    {
        if (waiting.elapsed() > 30000)
            fail("Parent did not start its owned interruption test");
        QThread::msleep(1);
    }
    document.save(destination, baseline);
    QFile finished(output + "/completed.json");
    if (!finished.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        fail("Cannot record a completed interruption worker");
    finished.write(QJsonDocument(QJsonObject{{"status", "COMPLETED"}}).toJson());
}
} // namespace tatsu
