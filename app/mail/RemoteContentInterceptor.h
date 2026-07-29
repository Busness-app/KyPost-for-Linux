#pragma once

#include <QObject>
#include <QWebEngineUrlRequestInterceptor>

class QQuickWebEngineProfile;

// Installed onto EmailDetail.qml's WebEngineView profile via installOn()
// to fix a VibeSec finding: QWebEngineSettings::AutoLoadImages (the "Show
// images" toggle's underlying mechanism) only gates Blink's "Image"
// resource-loading policy -- <img> tags, CSS background-url, <video
// poster>, SVG <image>. It does NOT gate the "Stylesheet"/"Media" policies,
// so a sender's <link rel="stylesheet">, CSS @import, or <video>/<audio>
// element fired a tracking-pixel-equivalent remote request even with
// autoLoadImages false, defeating the toggle's entire purpose.
//
// The policy is an ALLOWLIST in both directions, which is the correction to
// how this class originally worked. It used to block everything but the
// document while images were off and then allow EVERYTHING once the user
// opted in -- so one click on "Show images" re-opened the exact stylesheet
// and media vectors this class exists to close, and additionally opened
// subframes, fonts, XHR, WebSocket and ping/beacon, none of which the user
// was asked about. Worse, a subframe is not covered by EmailDetail.qml's
// navigationRequested handler either (that only sees main-frame
// navigations), so an <iframe src="https://tracker/..."> in a message had
// no guard left at all.
//
// "Show images" now means images. The document load itself is always
// allowed (it is loadHtml()'s own local content); images and favicons are
// allowed once the user opts in; everything else is refused in both states.
class RemoteContentInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT
    Q_PROPERTY(bool imagesLoaded READ imagesLoaded WRITE setImagesLoaded NOTIFY imagesLoadedChanged)

public:
    explicit RemoteContentInterceptor(QObject* parent = nullptr);

    bool imagesLoaded() const;
    void setImagesLoaded(bool loaded);

    // QQuickWebEngineProfile::setUrlRequestInterceptor() is a plain C++
    // method in Qt6's WebEngineQuick QML API (unlike Qt5, where
    // WebEngineProfile.urlRequestInterceptor was a real Q_PROPERTY) -- QML
    // cannot assign it as a property. This Q_INVOKABLE wrapper is the
    // bridge, called from Component.onCompleted on the profile instance.
    Q_INVOKABLE void installOn(QQuickWebEngineProfile* profile);

    void interceptRequest(QWebEngineUrlRequestInfo& info) override;

signals:
    void imagesLoadedChanged();

private:
    bool m_imagesLoaded = false;
};

// Pure decision logic behind interceptRequest(), exposed for unit testing
// without needing a real QWebEngineUrlRequestInfo (QtWebEngineCore only
// ever constructs one internally -- there's no public constructor for
// tests to use). ResourceType is a plain public enum, passable by value
// with no instance needed.
bool shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceType resourceType, bool imagesLoaded);
