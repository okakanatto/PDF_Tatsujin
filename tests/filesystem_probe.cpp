#include "disk_full_tests.h"
#include "document.h"
#include "save_candidate.h"
#include "save_interruption_worker.h"
#include "windows_path.h"
#include <QScopeGuard>
#include <cstdio>
#include <windows.h>

namespace
{
void require(bool condition, const QString& message)
{
    if (!condition)
        tatsu::fail(message);
}
QJsonObject lockedSave(const QString& fixture, const QString& output, bool overwriteSource)
{
    QTemporaryDir root(output + "/save-lock-XXXXXX");
    require(root.isValid(), "Cannot create owned probe directory");
    const auto source = root.filePath("原本.pdf"), destination = root.filePath("別名保存.pdf");
    require(QFile::copy(fixture, source), "Cannot copy synthetic input");
    const auto original = tatsu::fileHash(source);
    tatsu::Document document;
    document.open(source);
    document.putSignature(0, "保存失敗後も署名を保持", {60, 100}, 18, Qt::black);
    if (!overwriteSource)
    {
        document.save(destination);
        document.rotate(0);
    }
    const auto target = overwriteSource ? source : destination;
    const auto beforeFile = tatsu::fileHash(target);
    const auto beforePdf = tatsu::encodePdf(document.pdf());
    const auto beforeImage = tatsu::renderPage(document.pdf(), 0, 1);
    const auto beforeTarget = document.target;
    const auto beforeTargetHash = document.targetHash;
    const auto beforeSourceHash = document.sourceHash;
    const auto cursor = document.cursor, saved = document.saved;
    const auto history = document.history.size();
    const auto revision = document.revision;
    QString failure;
    {
        const auto native = tatsu::extendedWindowsPath(target);
        HANDLE handle =
            CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "Cannot lock own synthetic destination");
        const auto close = qScopeGuard([&] { CloseHandle(handle); });
        try
        {
            document.save(target);
        }
        catch (const std::exception& error)
        {
            failure = QString::fromUtf8(error.what());
        }
        require(failure.startsWith("保存の置換に失敗しました"),
                "Expected real Windows replacement failure after candidate validation");
        require(tatsu::fileHash(target) == beforeFile &&
                    tatsu::encodePdf(document.pdf()) == beforePdf && document.dirty() &&
                    document.cursor == cursor && document.saved == saved &&
                    document.history.size() == history && document.revision == revision &&
                    document.target == beforeTarget && document.targetHash == beforeTargetHash &&
                    document.sourceHash == beforeSourceHash,
                "Failed replacement changed file, document, history or save target");
        require(
            QDir(root.path())
                .entryList({".pdf-tatsujin-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                .isEmpty(),
            "Failed save left its private candidate directory");
        document.undo();
        document.redo();
        require(document.dirty() && tatsu::encodePdf(document.pdf()) == beforePdf,
                "Failed save damaged Undo/Redo");
    }
    document.save(target);
    require(!document.dirty() && document.target == target, "Retry failed after releasing lock");
    tatsu::Document reopened;
    reopened.open(target);
    require(tatsu::renderPage(reopened.pdf(), 0, 1) == beforeImage &&
                tatsu::signatures(reopened.pdf(), 0).size() == 1,
            "Retry changed saved appearance or editability");
    document.undo();
    require(document.dirty(), "Save discarded Undo history");
    if (!overwriteSource)
        require(tatsu::fileHash(source) == original, "Alternate save modified source");
    return {{"case",
             overwriteSource ? "overwrite owned source copy" : "overwrite alternate destination"},
            {"status", "PASS"},
            {"failure", failure},
            {"file_document_history_and_target_preserved", true},
            {"private_candidate_removed", true},
            {"unlock_retry_reopen_and_Undo", true}};
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try
    {
        const auto arguments = app.arguments();
        if (arguments.size() == 4 && arguments[1] == "--cleanup-save-candidates")
        {
            const auto path = QFileInfo(arguments[2]).absoluteFilePath();
            require(QFileInfo::exists(path + "/prepared.json") &&
                        QFileInfo::exists(path + "/expected.pdf") &&
                        QFileInfo::exists(path + "/begin"),
                    "Cleanup probe requires the prepared owned interruption test");
            const auto before = QDir(path).entryList(
                {".pdf-tatsujin-save-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
            const auto expected = path + "/expected.pdf";
            const auto original = tatsu::fileHash(expected);
            tatsu::Document retry;
            retry.open(expected);
            const auto image = tatsu::renderPage(retry.pdf(), 0, 1);
            const auto text = tatsu::signatures(retry.pdf(), 0).front().text;
            retry.save(path + "/retry.pdf");
            tatsu::Document reopened;
            reopened.open(path + "/retry.pdf");
            require(!retry.dirty() && reopened.pages() == retry.pages() &&
                        tatsu::renderPage(reopened.pdf(), 0, 1) == image &&
                        tatsu::signatures(reopened.pdf(), 0).front().text == text &&
                        tatsu::fileHash(expected) == original,
                    "Actual retry changed the expected PDF, appearance or editability");
            QStringList removed;
            for (const auto& name : before)
                if (!QFileInfo::exists(path + "/" + name))
                    removed.append(name);
            QFile record(arguments[3]);
            require(record.open(QIODevice::WriteOnly | QIODevice::NewOnly),
                    "Cleanup report must be new");
            record.write(QJsonDocument(QJsonObject{{"removed", QJsonArray::fromStringList(removed)},
                                                   {"actual_save_retry_and_reopen", true}})
                             .toJson());
            return 0;
        }
        const bool diskFull = arguments.size() == 5 && arguments[1] == "--disk-full";
        const bool interrupt = arguments.size() == 5 && arguments[1] == "--interrupt-save-worker";
        require(arguments.size() == 3 || diskFull || interrupt,
                "Expected fixtures/output, --disk-full fixtures/output/volume, or "
                "--interrupt-save-worker fixtures/output/new|existing");
        const bool worker = diskFull || interrupt;
        const auto fixtureRoot = arguments[worker ? 2 : 1];
        const auto output = QFileInfo(arguments[worker ? 3 : 2]).absoluteFilePath();
        require(!QFileInfo::exists(output) && QDir().mkpath(output), "Output must be new");
        if (interrupt)
        {
            require(arguments[4] == "new" || arguments[4] == "existing", "Invalid worker case");
            tatsu::saveInterruptionWorker(fixtureRoot, output, arguments[4] == "existing");
            return 0;
        }
        const auto fixture = fixtureRoot + "/D01.pdf";
        const auto original = tatsu::fileHash(fixture);
        QJsonArray tests;
        if (diskFull)
            tests.append(tatsu::testDiskFullSave(fixtureRoot, arguments[4]));
        else
            tests = {lockedSave(fixture, output, false), lockedSave(fixture, output, true)};
        require(tatsu::fileHash(fixture) == original, "Frozen fixture changed");
        QJsonArray screens;
        for (auto screen : app.screens())
        {
            const auto area = screen->availableGeometry();
            screens.append(QJsonObject{{"device_pixel_ratio", screen->devicePixelRatio()},
                                       {"available_width", area.width()},
                                       {"available_height", area.height()}});
        }
        QFile report(output + "/filesystem-probe.json");
        require(report.open(QIODevice::WriteOnly), "Cannot write probe record");
        report.write(
            QJsonDocument(
                QJsonObject{{"status", "PASS"},
                            {"tests", tests},
                            {"screens", screens},
                            {"platform", QGuiApplication::platformName()},
                            {"scope", diskFull ? "real space-limited NTFS VHD and PDF API; "
                                                 "no window or GUI input"
                                               : "real Windows sharing locks and PDF API; "
                                                 "no window or GUI input; screen metadata "
                                                 "does not establish OS scaling acceptance"}})
                .toJson());
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
