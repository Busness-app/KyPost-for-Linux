#include "mail/RemoteContentInterceptor.h"

#include <QQuickWebEngineProfile>

#include <QtGlobal>
#include <QWebEngineUrlRequestInfo>

bool shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceType resourceType, bool imagesLoaded)
{
    // The document EmailDetail.qml's own loadHtml() produces. Always
    // allowed, in both states -- refusing it would render nothing at all.
    if (resourceType == QWebEngineUrlRequestInfo::ResourceTypeMainFrame)
        return false;

    if (!imagesLoaded)
        return true;

    // Opted in. An allowlist, not `return false`: the user asked to see the
    // pictures, not to grant the sender a stylesheet, a subframe, a font
    // fetch, an XHR or a beacon. Every one of those is an equally good
    // read-receipt channel and none of them is what the button says.
    return resourceType != QWebEngineUrlRequestInfo::ResourceTypeImage
        && resourceType != QWebEngineUrlRequestInfo::ResourceTypeFavicon;
}

RemoteContentInterceptor::RemoteContentInterceptor(QObject* parent)
    : QWebEngineUrlRequestInterceptor(parent)
{
}

bool RemoteContentInterceptor::imagesLoaded() const
{
    return m_imagesLoaded;
}

void RemoteContentInterceptor::setImagesLoaded(bool loaded)
{
    if (m_imagesLoaded == loaded)
        return;
    m_imagesLoaded = loaded;
    emit imagesLoadedChanged();
}

void RemoteContentInterceptor::installOn(QQuickWebEngineProfile* profile)
{
    if (profile == nullptr)
        return;

    // The profile this renders mail in must not persist anything.
    //
    // A decrypted OpenPGP message is rendered here, and MailController is
    // careful never to write it to the database -- which would be beside the
    // point if the web engine wrote it to a disk cache instead. Measured on
    // Qt 6.11: a WebEngineProfile declared with no storageName, which is what
    // EmailDetail.qml declares, comes up off-the-record with a memory-only
    // HTTP cache and no persistent cookies, and never creates the storage
    // directory it names.
    //
    // Checked here rather than trusted, because the regression is one line of
    // QML -- a storageName added to enable something -- and nothing else in
    // this repo would notice. A warning rather than a refusal: declining to
    // install the interceptor would leave remote content UNBLOCKED, which is
    // a worse outcome than the one being warned about.
    if (!profile->isOffTheRecord()) {
        qWarning("RemoteContentInterceptor: rendering mail in a profile that persists to disk (%s). "
                  "Decrypted message content can reach the web engine's cache.",
                  qUtf8Printable(profile->storageName()));
    }

    profile->setUrlRequestInterceptor(this);
}

void RemoteContentInterceptor::interceptRequest(QWebEngineUrlRequestInfo& info)
{
    if (shouldBlockRemoteContentRequest(info.resourceType(), m_imagesLoaded))
        info.block(true);
}
