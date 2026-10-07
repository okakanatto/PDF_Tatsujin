#include "private_temp.h"
#include "document.h"
#include <QDirIterator>
#include <QScopeGuard>
#include <QUuid>
#include <aclapi.h>
#include <windows.h>

namespace tatsu
{
bool runningInAppContainer()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        fail("一時領域の実行権限を確認できません。");
    const auto closeToken = qScopeGuard([&] { CloseHandle(token); });
    DWORD container = 0, required = 0;
    if (!GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &required))
        fail("一時領域の実行権限を確認できません。");
    return container != 0;
}
PrivateTemporaryDirectory::PrivateTemporaryDirectory(const QString& pattern)
{
    if (!runningInAppContainer())
    {
        normal = std::make_unique<QTemporaryDir>(pattern);
        if (normal->isValid())
            location = normal->path();
        return;
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        fail("一時領域の実行権限を確認できません。");
    const auto closeToken = qScopeGuard([&] { CloseHandle(token); });
    // An AppContainer must match both the user and its package SID. Qt's
    // private DACL only names the user; create the correct DACL initially.
    auto information = [&](TOKEN_INFORMATION_CLASS kind)
    {
        DWORD size = 0;
        GetTokenInformation(token, kind, nullptr, 0, &size);
        std::vector<unsigned char> bytes(size);
        if (!size || !GetTokenInformation(token, kind, bytes.data(), size, &size))
            fail("一時領域の実行権限を確認できません。");
        return bytes;
    };
    const auto user = information(TokenUser), package = information(TokenAppContainerSid);
    PSID sids[]{
        reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid,
        reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(package.data())->TokenAppContainer};
    EXPLICIT_ACCESSW entries[2]{};
    for (int i = 0; i < 2; ++i)
    {
        entries[i].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[i].grfAccessMode = GRANT_ACCESS;
        entries[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[i].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        entries[i].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sids[i]);
    }
    PACL acl = nullptr;
    DWORD error = SetEntriesInAclW(2, entries, nullptr, &acl);
    if (error != ERROR_SUCCESS)
        fail(QString("専用一時領域の権限を準備できません（Windowsエラー %1）。").arg(error));
    const auto freeAcl = qScopeGuard([&] { LocalFree(acl); });
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        fail("専用一時領域の権限を準備できません。");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
    if (!pattern.endsWith("XXXXXX"))
        fail("専用一時領域のパターンが不正です。");
    const auto prefix = pattern.left(pattern.size() - 6);
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto candidate = prefix + QUuid::createUuid().toString(QUuid::Id128);
        if (CreateDirectoryW(reinterpret_cast<LPCWSTR>(candidate.utf16()), &attributes))
        {
            location = candidate;
            return;
        }
        error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
            break;
    }
    fail(QString("専用一時領域を作成できません（Windowsエラー %1）。").arg(error));
}
PrivateTemporaryDirectory::~PrivateTemporaryDirectory()
{
    if (normal || location.isEmpty())
        return;
    const QFileInfo root(location);
    if (root.isSymLink() || root.isJunction())
        return;
    QDirIterator entries(location,
                         QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext())
    {
        entries.next();
        if (entries.fileInfo().isSymLink() || entries.fileInfo().isJunction())
            return;
    }
    QDir(location).removeRecursively();
}
bool PrivateTemporaryDirectory::isValid() const
{
    return !location.isEmpty();
}
QString PrivateTemporaryDirectory::path() const
{
    return location;
}
QString PrivateTemporaryDirectory::filePath(const QString& name) const
{
    return QDir(location).filePath(name);
}
std::unique_ptr<PrivateTemporaryDirectory> privateTemporaryDirectory(const QString& pattern)
{
    return std::make_unique<PrivateTemporaryDirectory>(pattern);
}
} // namespace tatsu
