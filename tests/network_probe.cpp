#include <filesystem>
#include <fstream>
#include <vector>
#include <windows.h>
#include <winsock2.h>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4)
        return 2;
    std::ofstream trace{std::filesystem::path(std::wstring(argv[1]) + L".trace")};
    if (!trace.good())
        return 7;
    trace << "token\n" << std::flush;
    wchar_t temporary[32768]{};
    using TempPath = DWORD(WINAPI*)(DWORD, LPWSTR);
    auto tempPath = reinterpret_cast<TempPath>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTempPath2W"));
    if (!tempPath)
        tempPath = GetTempPathW;
    const auto length = tempPath(32768, temporary);
    if (length && length < 32768)
    {
        const auto bytes =
            WideCharToMultiByte(CP_UTF8, 0, temporary, length, nullptr, 0, nullptr, nullptr);
        std::string utf8(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, temporary, length, utf8.data(), bytes, nullptr, nullptr);
        std::ofstream path{std::filesystem::path(std::wstring(argv[1]) + L".temp-path.txt")};
        path << utf8;
    }
    HANDLE token = nullptr;
    DWORD container = 0, required = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) ||
        !GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &required))
        return 3;
    GetTokenInformation(token, TokenCapabilities, nullptr, 0, &required);
    std::vector<unsigned char> buffer(required);
    if (!GetTokenInformation(token, TokenCapabilities, buffer.data(), required, &required))
    {
        CloseHandle(token);
        return 4;
    }
    const auto capabilities = reinterpret_cast<TOKEN_GROUPS*>(buffer.data())->GroupCount;
    CloseHandle(token);
    trace << "winsock\n" << std::flush;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data))
        return 5;
    SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(std::stoi(argv[2])));
    trace << "connect\n" << std::flush;
    unsigned long nonblocking = 1;
    ioctlsocket(connection, FIONBIO, &nonblocking);
    int connected = connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int error = connected == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (error == WSAEWOULDBLOCK)
    {
        fd_set writable, failure;
        FD_ZERO(&writable);
        FD_ZERO(&failure);
        FD_SET(connection, &writable);
        FD_SET(connection, &failure);
        timeval timeout{2, 0};
        if (select(0, nullptr, &writable, &failure, &timeout) > 0)
        {
            int length = sizeof(error);
            getsockopt(connection, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
            connected = error ? SOCKET_ERROR : 0;
        }
        else
            error = WSAETIMEDOUT;
    }
    trace << "result " << error << "\n" << std::flush;
    closesocket(connection);
    WSACleanup();
    const bool control = std::wstring(argv[3]) == L"control";
    const bool pass =
        control ? !container && connected == 0
                : container && capabilities == 0 && (error == WSAEACCES || error == WSAETIMEDOUT);
    std::ofstream report{std::filesystem::path(argv[1])};
    report << "{\"is_AppContainer\":" << container << ",\"capability_count\":" << capabilities
           << ",\"loopback_connect_error\":" << error << ",\"passed\":" << (pass ? "true" : "false")
           << "}\n";
    return report.good() && pass ? 0 : 6;
}
