#include "mail/RemoteContentInterceptor.h"

#include <QQuickWebEngineProfile>
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
    if (profile)
        profile->setUrlRequestInterceptor(this);
}

void RemoteContentInterceptor::interceptRequest(QWebEngineUrlRequestInfo& info)
{
    if (shouldBlockRemoteContentRequest(info.resourceType(), m_imagesLoaded))
        info.block(true);
}
