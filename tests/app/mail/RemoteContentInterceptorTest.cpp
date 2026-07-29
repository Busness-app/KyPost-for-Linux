#include "mail/RemoteContentInterceptor.h"

#include <QTest>

// Covers shouldBlockRemoteContentRequest() -- the pure decision logic
// backing RemoteContentInterceptor::interceptRequest() -- in isolation.
// QWebEngineUrlRequestInfo has no public constructor (QtWebEngineCore only
// ever builds one internally for a real request), so
// RemoteContentInterceptor's actual interceptRequest() override can't be
// exercised directly in a unit test; this pure function is the
// unit-testable surface, same pattern as NotificationDispatcher's
// pickTitle/pickText.
class RemoteContentInterceptorTest : public QObject
{
    Q_OBJECT

private slots:
    void mainFrameNavigationIsNeverBlocked();
    void imageRequestIsBlockedWhenImagesNotLoaded();
    void stylesheetRequestIsBlockedWhenImagesNotLoaded();
    void fontResourceRequestIsBlockedWhenImagesNotLoaded();
    void mediaRequestIsBlockedWhenImagesNotLoaded();
    void imagesAreAllowedOnceImagesLoaded();
    void nonImageResourcesStayBlockedOnceImagesLoaded();
    void subframesStayBlockedOnceImagesLoaded();
};

void RemoteContentInterceptorTest::mainFrameNavigationIsNeverBlocked()
{
    // The initial loadHtml() render must always go through, regardless of
    // the images-loaded state -- otherwise the email body itself would
    // never appear.
    QVERIFY(!shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeMainFrame, false));
    QVERIFY(!shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeMainFrame, true));
}

void RemoteContentInterceptorTest::imageRequestIsBlockedWhenImagesNotLoaded()
{
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeImage, false));
}

void RemoteContentInterceptorTest::stylesheetRequestIsBlockedWhenImagesNotLoaded()
{
    // VibeSec regression guard: this is the actual bypass -- CSS
    // stylesheets/@import were never gated by QWebEngineSettings::
    // AutoLoadImages at all, only by this interceptor.
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeStylesheet, false));
}

void RemoteContentInterceptorTest::fontResourceRequestIsBlockedWhenImagesNotLoaded()
{
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeFontResource, false));
}

void RemoteContentInterceptorTest::mediaRequestIsBlockedWhenImagesNotLoaded()
{
    // <video>/<audio> sources -- also never gated by AutoLoadImages.
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeMedia, false));
}

void RemoteContentInterceptorTest::imagesAreAllowedOnceImagesLoaded()
{
    // What the button actually promises.
    QVERIFY(!shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeImage, true));
    QVERIFY(!shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeFavicon, true));
}

void RemoteContentInterceptorTest::nonImageResourcesStayBlockedOnceImagesLoaded()
{
    // This case previously asserted the opposite, which is how the bypass
    // survived: "Show images" flipped the interceptor to allow-everything,
    // re-opening the very stylesheet and media vectors the three tests
    // above exist to guard. Opting in to images must not opt in to a
    // sender-controlled stylesheet, font fetch, XHR or beacon -- each is an
    // equally good read-receipt channel.
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeStylesheet, true));
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeMedia, true));
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeFontResource, true));
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeXhr, true));
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypePing, true));
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeScript, true));
}

void RemoteContentInterceptorTest::subframesStayBlockedOnceImagesLoaded()
{
    // A subframe is the one vector with no second line of defence:
    // EmailDetail.qml's navigationRequested handler only sees main-frame
    // navigations, so an <iframe src="https://tracker/..."> reached the
    // network on nothing but this decision.
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeSubFrame, false));
    QVERIFY(shouldBlockRemoteContentRequest(QWebEngineUrlRequestInfo::ResourceTypeSubFrame, true));
}

QTEST_GUILESS_MAIN(RemoteContentInterceptorTest)
#include "RemoteContentInterceptorTest.moc"
