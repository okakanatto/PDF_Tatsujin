#include "pdf_validation.h"
#include "owned_process.h"
#include "pdfsecurityhandler.h"
#include "private_temp.h"
#include "windows_path.h"
#include <QXmlStreamReader>
#include <windows.h>

namespace tatsu
{
namespace
{
int count(const QXmlStreamAttributes& attributes, const QString& name)
{
    bool valid = false;
    const auto result = attributes.value(name).toString().toInt(&valid);
    if (!valid || result < 0)
        fail("PDF規格検査の件数を確認できません。適合性は未判定です。");
    return result;
}
} // namespace
const QVector<PdfValidationProfile>& pdfValidationProfiles()
{
    static const QVector<PdfValidationProfile> profiles{
        {"2u", "PDF/A-2u", "PDF/A-2u validation profile", false},
        {"1b", "PDF/A-1b", "PDF/A-1b validation profile", false},
        {"ua1", "PDF/UA-1（機械検査）", "PDF/UA-1 validation profile", true}};
    return profiles;
}
const PdfValidationProfile& pdfValidationProfile(const QString& id)
{
    for (const auto& profile : pdfValidationProfiles())
        if (profile.id == id)
            return profile;
    fail("PDF/A-1b・PDF/A-2uまたはPDF/UA-1（機械検査）を指定してください。");
}
PdfValidationInput currentPdfValidationInput(const Document& document)
{
    if (!document.loaded() || document.busy || document.pendingInput)
        fail("PDFを開き、入力と現在の処理を完了してから検査してください。");
    PdfValidationInput result{document.pdf(), {}, {}};
    if (!document.dirty())
    {
        result.source = document.target.isEmpty() ? document.source : document.target;
        result.sourceHash = document.target.isEmpty() ? document.sourceHash : document.targetHash;
    }
    return result;
}
PdfValidationResult parsePdfValidationReport(const QByteArray& bytes, const QString& profile,
                                             int exitCode)
{
    const auto& specification = pdfValidationProfile(profile);
    if (bytes.isEmpty() || bytes.size() > 4 * 1024 * 1024 || (exitCode != 0 && exitCode != 1))
        fail("PDF規格検査は完了していません。適合性は未判定です。");
    QXmlStreamReader xml(bytes);
    PdfValidationResult result;
    result.profile = profile;
    int reports = 0, summaries = 0, jobs = 0, details = 0, totals = 0;
    QString root;
    while (!xml.atEnd())
    {
        xml.readNext();
        if (xml.isDTD() || xml.isEntityReference())
            fail("PDF規格検査の不正なXMLは利用できません。");
        if (!xml.isStartElement())
            continue;
        const auto name = xml.name();
        const auto attributes = xml.attributes();
        if (root.isEmpty())
            root = name.toString();
        if (name == "releaseDetails" && attributes.value("id") == "core")
        {
            if (!result.engineVersion.isEmpty())
                fail("PDF規格検査器の版が重複しています。");
            result.engineVersion = attributes.value("version").toString();
        }
        else if (name == "job")
            ++jobs;
        else if (name == "validationReport")
        {
            ++reports;
            const auto compliant = attributes.value("isCompliant");
            if ((compliant != "true" && compliant != "false") ||
                attributes.value("jobEndStatus") != "normal" ||
                attributes.value("profileName") != specification.reportName)
                fail("PDF規格検査の完了・対象プロファイルを確認できません。");
            result.checksPassed = compliant == "true";
        }
        else if (name == "details")
        {
            ++details;
            result.failedRules = count(attributes, "failedRules");
            result.failedChecks = count(attributes, "failedChecks");
        }
        else if (name == "rule")
        {
            if (attributes.value("status") != "failed" || result.rules.size() >= 1024)
                fail("PDF規格検査の規則結果を確認できません。");
            PdfValidationRule rule;
            rule.specification = attributes.value("specification").toString();
            rule.clause = attributes.value("clause").toString();
            rule.test = attributes.value("testNumber").toString();
            rule.failedChecks = count(attributes, "failedChecks");
            while (xml.readNextStartElement())
            {
                if (xml.name() == "description")
                    rule.description = xml.readElementText();
                else
                    xml.skipCurrentElement();
            }
            if (rule.description.size() > 8192 || rule.clause.size() > 128 ||
                rule.specification.size() > 256 || rule.test.size() > 128)
                fail("PDF規格検査の説明が上限を超えています。");
            result.rules << std::move(rule);
        }
        else if (name == "batchSummary")
        {
            ++summaries;
            if (count(attributes, "totalJobs") != 1 || count(attributes, "failedToParse") ||
                count(attributes, "encrypted") || count(attributes, "outOfMemory") ||
                count(attributes, "veraExceptions"))
                fail("PDF規格検査が一部失敗しました。適合性は未判定です。");
        }
        else if (name == "validationReports")
        {
            ++totals;
            if (count(attributes, "failedJobs") ||
                count(attributes, "compliant") != int(result.checksPassed) ||
                count(attributes, "nonCompliant") != int(!result.checksPassed))
                fail("PDF規格検査の集計が一致しません。");
        }
    }
    qint64 failedChecks = 0;
    for (const auto& rule : result.rules)
    {
        if (rule.failedChecks == 0)
            fail("PDF規格の不適合規則に失敗検査がありません。");
        failedChecks += rule.failedChecks;
    }
    if (xml.hasError() || root != "report" || jobs != 1 || reports != 1 || summaries != 1 ||
        details != 1 || totals != 1 || result.engineVersion.isEmpty() ||
        result.engineVersion.size() > 64 || result.failedRules != result.rules.size() ||
        failedChecks != result.failedChecks ||
        result.checksPassed != (result.failedRules == 0 && result.failedChecks == 0) ||
        exitCode != int(!result.checksPassed))
        fail("PDF規格検査結果が不完全です。適合性は未判定です。");
    return result;
}
PdfValidationResult validatePdfConformance(const PdfValidationInput& input, const QString& java,
                                           const QString& jar, const QString& profile,
                                           const std::function<bool()>& cancelled)
{
    pdfValidationProfile(profile);
    if (input.document.getStorage().getSecurityHandler()->getMode() != EncryptionMode::None)
        fail("暗号化PDFはPDF規格検査の対象にできません。無断で復号しません。");
    if (!QFileInfo(java).isFile() || !QFileInfo(jar).isFile() ||
        QFileInfo(jar).suffix().compare("jar", Qt::CaseInsensitive) != 0)
        fail("JavaとveraPDF CLIの実行先を指定してください。検査は未実行です。");
    if (cancelled && cancelled())
        fail("PDF規格検査を取り消しました。");
    QByteArray bytes;
    if (!input.source.isEmpty())
    {
        QFile source(input.source);
        if (!source.open(QIODevice::ReadOnly) || source.size() <= 0 ||
            source.size() > 512 * 1024 * 1024)
            fail("検査するPDFを読み取れません。512MiBまでです。");
        bytes = source.readAll();
        if (bytes.size() != source.size() ||
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != input.sourceHash)
            fail("ロード後にPDFが更新されています。開き直してから検査してください。");
    }
    else
        bytes = encodePdf(input.document);
    auto temporary = privateTemporaryDirectory(QDir::tempPath() + "/PDFTatsujin-pdfa-XXXXXX");
    if (!temporary->isValid())
        fail("PDF規格検査の作業フォルダを作成できません。");
    const auto source = temporary->filePath("snapshot.pdf");
    QFile file(source);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(bytes) != bytes.size() || !file.flush())
        fail("PDF規格検査の作業用コピーを作成できません。");
    file.close();
    const auto process = runOwnedProcess(
        QFileInfo(java).absoluteFilePath(),
        {"-Xmx512m", "-Djava.awt.headless=true", "-Djava.io.tmpdir=" + temporary->path(),
         "-Duser.home=" + temporary->path(), "-jar", QFileInfo(jar).absoluteFilePath(), "--format",
         "xml", "--flavour", profile, "--maxfailuresdisplayed", "1", source},
        temporary->path(), 120000, cancelled, 4 * 1024 * 1024,
        {{"APPDATA", temporary->path()},
         {"LOCALAPPDATA", temporary->path()},
         {"TMP", temporary->path()},
         {"TEMP", temporary->path()}});
    if (process.outputTruncated || (cancelled && cancelled()))
        fail("PDF規格検査は完了していません。結果を反映しません。");
    auto result = parsePdfValidationReport(process.output, profile, process.exitCode);
    if (!input.source.isEmpty() && fileHash(input.source) != input.sourceHash)
        fail("検査中に元PDFが更新されました。結果を反映しません。");
    result.snapshotHash =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return result;
}
QJsonObject pdfValidationResultJson(const PdfValidationResult& result)
{
    QJsonArray rules;
    for (const auto& rule : result.rules)
        rules.append(QJsonObject{{"specification", rule.specification},
                                 {"clause", rule.clause},
                                 {"test", rule.test},
                                 {"description", rule.description},
                                 {"failed_checks", rule.failedChecks}});
    QJsonObject json{{"profile", result.profile},
                     {"engine_version", result.engineVersion},
                     {"snapshot_sha256", result.snapshotHash},
                     {"failed_rules", result.failedRules},
                     {"failed_checks", result.failedChecks},
                     {"rules", rules}};
    const auto& profile = pdfValidationProfile(result.profile);
    if (profile.machineOnly)
    {
        json.insert("machine_compliant", result.checksPassed);
        json.insert("validation_scope", "machine-verifiable");
        json.insert("full_conformance", "not-determined");
    }
    else
        json.insert("compliant", result.checksPassed);
    return json;
}
void exportPdfValidationResult(const PdfValidationResult& result, const QString& path)
{
    const QFileInfo target(path);
    if (target.suffix().compare("json", Qt::CaseInsensitive) != 0 || target.exists() ||
        target.isSymLink())
        fail("新しい.jsonファイルを指定してください。既存ファイルは上書きしません。");
    auto temporary =
        privateTemporaryDirectory(target.absolutePath() + "/PDFTatsujin-pdfa-result-XXXXXX");
    if (!temporary->isValid())
        fail("結果の作業フォルダを作成できません。");
    const auto candidate = temporary->filePath("result.json");
    const auto bytes = QJsonDocument(pdfValidationResultJson(result)).toJson();
    QFile file(candidate);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(bytes) != bytes.size() || !file.flush())
        fail("結果を書き込めません。既存ファイルは保持しました。");
    file.close();
    QFile check(candidate);
    if (!check.open(QIODevice::ReadOnly) || check.readAll() != bytes)
        fail("結果の完成候補を検査できません。既存ファイルは保持しました。");
    check.close();
    const auto from = extendedWindowsPath(candidate),
               to = extendedWindowsPath(target.absoluteFilePath());
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_WRITE_THROUGH))
        fail("結果を保存できません。既存ファイルは保持しました。");
}
} // namespace tatsu
