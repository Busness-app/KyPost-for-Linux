#include "version/VersionCompare.h"

#include <QStringList>

namespace {

// Fills `out` with exactly three non-negative components, or returns false.
bool parseVersion(const QString& version, int (&out)[3])
{
    QString trimmed = version.trimmed();
    if (trimmed.startsWith(QLatin1Char('v')))
        trimmed = trimmed.mid(1);

    const QStringList parts = trimmed.split(QLatin1Char('.'));
    if (parts.size() != 3)
        return false;

    for (int i = 0; i < 3; ++i) {
        // QString::toInt accepts a leading '+'/'-' and surrounding space, none
        // of which belongs in a version, so check the shape before converting.
        const QString& part = parts.at(i);
        if (part.isEmpty())
            return false;
        for (const QChar c : part) {
            if (!c.isDigit())
                return false;
        }
        bool ok = false;
        out[i] = part.toInt(&ok);
        if (!ok)
            return false;
    }
    return true;
}

} // namespace

namespace VersionCompare {

bool isNewer(const QString& latest, const QString& installed)
{
    int latestParts[3];
    int installedParts[3];
    if (!parseVersion(latest, latestParts) || !parseVersion(installed, installedParts))
        return false;

    for (int i = 0; i < 3; ++i) {
        if (latestParts[i] != installedParts[i])
            return latestParts[i] > installedParts[i];
    }
    return false;
}

bool isValid(const QString& version)
{
    int parts[3];
    return parseVersion(version, parts);
}

} // namespace VersionCompare
