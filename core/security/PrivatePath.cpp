#include "security/PrivatePath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

constexpr QFileDevice::Permissions kGroupAndOther = QFileDevice::ReadGroup | QFileDevice::WriteGroup
    | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;

constexpr QFileDevice::Permissions kOwnerOnlyDir =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;

constexpr QFileDevice::Permissions kOwnerOnlyFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

} // namespace

bool PrivatePath::isPrivate(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    return !(info.permissions() & kGroupAndOther);
}

PrivatePath::Status PrivatePath::ensureDirectory(const QString& path)
{
    if (!QDir().mkpath(path)) // succeeds if it already exists
        return Status::NotCreated;

    // Result deliberately not returned: it answers "did the call report
    // success", and isPrivate() below answers "is it actually shut". Only the
    // second one is worth acting on.
    QFile::setPermissions(path, kOwnerOnlyDir);

    return isPrivate(path) ? Status::Ready : Status::NotPrivate;
}

bool PrivatePath::ensureFile(const QString& path)
{
    if (!QFileInfo::exists(path))
        return false;
    QFile::setPermissions(path, kOwnerOnlyFile);
    return isPrivate(path);
}
