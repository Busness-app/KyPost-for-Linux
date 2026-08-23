#include "security/WipeTripwire.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

WipeTripwire::WipeTripwire(const QString& markerPath) : m_markerPath(markerPath)
{
}

bool WipeTripwire::arm()
{
    if (m_markerPath.isEmpty())
        return false;

    if (isArmed())
        return true; // already recorded; re-arming is not a second event

    const QFileInfo info(m_markerPath);
    if (!QDir().mkpath(info.absolutePath()))
        return false;

    // QSaveFile, so the marker is either fully present or absent -- never a
    // zero-length file left by a crash between create and write, which
    // isArmed() would count as armed on the strength of the filename alone.
    // That direction happens to be safe here, but only by accident, and the
    // next reader of this file should not have to work that out.
    QSaveFile marker(m_markerPath);
    if (!marker.open(QIODevice::WriteOnly))
        return false;
    // Content is for a human reading the profile directory, not for this
    // class: presence is the whole signal.
    if (marker.write("wipe started and not yet reported complete\n") < 0)
        return false;
    return marker.commit();
}

bool WipeTripwire::disarm()
{
    if (m_markerPath.isEmpty())
        return false;
    if (!QFile::exists(m_markerPath))
        return true; // nothing armed: the post-condition already holds
    return QFile::remove(m_markerPath);
}

bool WipeTripwire::isArmed() const
{
    return !m_markerPath.isEmpty() && QFile::exists(m_markerPath);
}
