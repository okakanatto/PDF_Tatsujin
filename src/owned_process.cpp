#include "owned_process.h"
#include "document.h"
#include <QElapsedTimer>
#include <QThread>
#include <vector>
#include <windows.h>

namespace tatsu
{
namespace
{
struct Handle
{
    HANDLE value = nullptr;
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
QString quoted(QString text)
{
    QString result = "\"";
    int slashes = 0;
    for (const auto character : text)
    {
        if (character == '\\')
            ++slashes;
        else
        {
            result += QString(character == '"' ? slashes * 2 + 1 : slashes, '\\');
            slashes = 0;
            result += character;
        }
    }
    return result + QString(slashes * 2, '\\') + '"';
}
void windowsCheck(bool ok, const char* operation)
{
    if (!ok)
        fail(QString("変換処理を開始できません: %1 (Windows %2)")
                 .arg(operation)
                 .arg(GetLastError()));
}
} // namespace
OwnedProcessResult runOwnedProcess(const QString& program, const QStringList& arguments,
                                   const QString& directory, int timeoutMs,
                                   const std::function<bool()>& cancelled)
{
    if (timeoutMs <= 0 || (cancelled && cancelled()))
        fail("変換を取り消しました。");
    Handle job;
    job.value = CreateJobObjectW(nullptr, nullptr);
    windowsCheck(job.value != nullptr, "CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    windowsCheck(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits,
                                         sizeof(limits)),
                 "SetInformationJobObject");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle outputRead, outputWrite, input;
    windowsCheck(CreatePipe(&outputRead.value, &outputWrite.value, &security, 0), "CreatePipe");
    windowsCheck(SetHandleInformation(outputRead.value, HANDLE_FLAG_INHERIT, 0),
                 "SetHandleInformation");
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    windowsCheck(input.value != INVALID_HANDLE_VALUE, "CreateFile NUL");
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    windowsCheck(InitializeProcThreadAttributeList(attributes, 2, 0, &bytes),
                 "InitializeProcThreadAttributeList");
    struct AttributeCleanup
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeCleanup()
        {
            DeleteProcThreadAttributeList(value);
        }
    } cleanup{attributes};
    HANDLE inherited[]{input.value, outputWrite.value};
    windowsCheck(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           inherited, sizeof(inherited), nullptr, nullptr),
                 "Handle list");
    windowsCheck(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                           &job.value, sizeof(job.value), nullptr, nullptr),
                 "Job list");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = outputWrite.value;
    startup.StartupInfo.hStdError = outputWrite.value;
    startup.lpAttributeList = attributes;
    QString command = quoted(program);
    for (const auto& argument : arguments)
        command += ' ' + quoted(argument);
    PROCESS_INFORMATION info{};
    const auto application = program.toStdWString();
    auto mutableCommand = command.toStdWString();
    const auto cwd = directory.toStdWString();
    windowsCheck(CreateProcessW(application.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                                cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info),
                 "CreateProcess");
    Handle process, thread;
    process.value = info.hProcess;
    thread.value = info.hThread;
    CloseHandle(outputWrite.value);
    outputWrite.value = nullptr;
    OwnedProcessResult result;
    QElapsedTimer timer;
    timer.start();
    QString failure;
    bool terminated = false;
    for (;;)
    {
        DWORD available = 0;
        while (PeekNamedPipe(outputRead.value, nullptr, 0, nullptr, &available, nullptr) &&
               available)
        {
            char buffer[4096];
            DWORD read = 0;
            if (!ReadFile(outputRead.value, buffer, qMin<DWORD>(available, sizeof(buffer)), &read,
                          nullptr) ||
                !read)
                break;
            if (result.output.size() < 65536)
                result.output.append(buffer, qMin<int>(read, 65536 - result.output.size()));
        }
        if (!terminated && ((cancelled && cancelled()) || timer.elapsed() > timeoutMs))
        {
            failure = timer.elapsed() > timeoutMs ? "変換が制限時間を超えました。"
                                                  : "変換を取り消しました。";
            windowsCheck(TerminateJobObject(job.value, 1), "TerminateJobObject");
            terminated = true;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        windowsCheck(QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation,
                                               &accounting, sizeof(accounting), nullptr),
                     "QueryInformationJobObject");
        if (!accounting.ActiveProcesses)
            break;
        QThread::msleep(20);
    }
    DWORD code = 0;
    windowsCheck(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0,
                 "Wait for owned process exit");
    windowsCheck(GetExitCodeProcess(process.value, &code), "GetExitCodeProcess");
    result.exitCode = static_cast<int>(code);
    if (!failure.isEmpty())
        fail(failure);
    return result;
}
} // namespace tatsu
