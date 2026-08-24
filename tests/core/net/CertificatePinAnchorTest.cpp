#include "net/HttpClient.h"

#include <QCryptographicHash>
#include <QSslCertificate>
#include <QSslKey>
#include <QTest>

// What the TLS pin is anchored to, and why it is the issuer rather than the
// leaf.
//
// The relay sits behind Cloudflare (AGENTS.md section 8), which terminates
// TLS with a ~90-day Universal SSL certificate and generates a NEW KEY on
// every renewal. A leaf-SPKI pin therefore breaks roughly quarterly, on a
// schedule that has nothing to do with an attacker: measured against the
// live endpoint, the leaf was valid 2026-07-20 to 2026-10-18 while its
// issuer (Google Trust Services WE1) runs to 2029-02-20.
//
// That is not a cosmetic annoyance. The recovery path is a dialog with a
// "reconnect" button, so a quarterly false alarm trains the user to click
// through the one screen that exists to stop an impersonated server. The
// pin is worth less that way than not pinning at all.
//
// Pinning the issuer keeps the defence that actually applies here -- a
// proxy presenting a chain from some other CA, which is what hostile Wi-Fi
// and corporate MITM roots look like -- while routine rotation becomes
// invisible. It does NOT catch mis-issuance by that same CA; that is the
// deliberate trade, and it is the reason this is the issuer and not the
// root.
//
// The certificates below are generated, not fetched: a real root, two
// intermediates under it, and three leaves -- two sharing one intermediate
// with DIFFERENT keys (a rotation) and one under the other intermediate
// (the impersonation).
namespace {

const char* kLeaf1 =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC/TCCAeWgAwIBAgIUISUmS4GB0QL0j4KKTF1NbSB1sFAwDQYJKoZIhvcNAQEL\n"
    "BQAwFTETMBEGA1UEAwwKVGVzdCBJbnQgQTAeFw0yNjA4MjQxMjE1NTNaFw0yNjEx\n"
    "MjIxMjE1NTNaMBgxFjAUBgNVBAMMDXJlbGF5LmV4YW1wbGUwggEiMA0GCSqGSIb3\n"
    "DQEBAQUAA4IBDwAwggEKAoIBAQCviTmcNhWnoXx1AmGZrbnWSXbzDLnOBIr8Owni\n"
    "VrIXL4YyTEDtE6FDQQlFN9xpinUU/rUwaBBLNjt7Ngl4VqIxCLIWqrIw1hFU65Gj\n"
    "gSCgMIyYgGfe3b6RdVHKufWML0a8Xgd/aXnq080KTkYQfrNpJCuR9TSw8zlMWtHD\n"
    "L+E+8s7eADtW4bnv2qDPlOh38iY4Lm4937apqSf6wxJOi58cOQaKnDbaX0WBCagd\n"
    "0/YbdlYrHEY7V+75JdVrTVHAUa4uGhiuUWb6Y0XvOjr5Byq4h+X6IXM2fn3/bgnC\n"
    "ZORkGMjMH0Zk6eu+c4UJJgRt4441lk+jkO+zCw+aLbaJegUZAgMBAAGjQjBAMB0G\n"
    "A1UdDgQWBBSvRo7cCja8NnvnS7m4BGtx0kOB7zAfBgNVHSMEGDAWgBTZfQ43LRJc\n"
    "IGChSsBGU/LslQxCcTANBgkqhkiG9w0BAQsFAAOCAQEASefI241injGzF2O4QE0X\n"
    "lUOdbp6E2gYyt49w1GurkmCBNtVWEJyxSoBk+lk/ocNRAfW+feCvUh9XloTOgryz\n"
    "vyQDJ2SATT8TePCLY/BUpXCwVDZ7K0TUawZ4SpWUto4wF6ZFh5xOR4Z7NxxZAOto\n"
    "F4D9ZVM4VGOjO82z2HjZpiw1Kkf0inNUnFPsTewStcvzWEi9gAMl+u4HB8k3xseI\n"
    "+KB/3khIqBqWNmjl97r1vkNq0VWbL9XBMR4aGfDZ85Hz4la5/M7UVSCQYheN0qk3\n"
    "Ybxj0yCUCoObiUmEyjg/Mjkk0vMeey0EAnw0bWVmIHnFZHTGzkXN3kK0QqaesTPe\n"
    "iA==\n"
    "-----END CERTIFICATE-----\n";

const char* kLeaf2 =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC/TCCAeWgAwIBAgIUISUmS4GB0QL0j4KKTF1NbSB1sFEwDQYJKoZIhvcNAQEL\n"
    "BQAwFTETMBEGA1UEAwwKVGVzdCBJbnQgQTAeFw0yNjA4MjQxMjE1NTNaFw0yNjEx\n"
    "MjIxMjE1NTNaMBgxFjAUBgNVBAMMDXJlbGF5LmV4YW1wbGUwggEiMA0GCSqGSIb3\n"
    "DQEBAQUAA4IBDwAwggEKAoIBAQDYQrhXcVaepk8HF6PQFRYChjIKkFhHBWHZjS9E\n"
    "Ou+LmOFfDDrxHocQbiotv71JUXT4Tga8/Q+l9mk8gIVH4dCmwqttopIZY2YygsQa\n"
    "L2+ClP/nepR4uN/W9jbWUUsJEOElwZBSmDbtAVILuWyfjSRLLl0dTUFDnGVh0meV\n"
    "CskwMd7OO84dIB7YbKsy5m7Jd+UwbQ+RA8g8YNWR2W/uh7BW27kwGypPp8iHPnUX\n"
    "CyRBE1KmD0xMq8AFAoDU7GMfNZPhfz/pqEyUDkXnAQhY+Q8wiCK/BdZ/G6eIM+Bc\n"
    "HoU/doeaMMBeIWsB+CYQW6nBBsF1F7XiPhUWEtP+rwBlGAv3AgMBAAGjQjBAMB0G\n"
    "A1UdDgQWBBRiWgrawpI4gNje9OMPa6FCCj4KJzAfBgNVHSMEGDAWgBTZfQ43LRJc\n"
    "IGChSsBGU/LslQxCcTANBgkqhkiG9w0BAQsFAAOCAQEAFPk48I2Lv2eAs8EPRNDh\n"
    "k8aR4ftjUFaHOGnIDm0SrB2+faB/w5tGfPfUSx+VE+p7VklUdBkC3SCUMyheIffr\n"
    "ulvLgGHIdCVQHwW05fE6wcS+BCDInV4AlfSDD4tRW3txWhOOkm/NdnPbzZ4TI8vY\n"
    "P/nPHMvk++yeNpDUkcBtdHcVLWZRm5WaC9LkzbgnXNCL6V9d7+Jm6+4SWAALA6y8\n"
    "HXBBD3tMWR1ALDREH5a4as7caEGBAfqjdzjufDvyfL50vAyxweYXKepiLrr9HZGx\n"
    "gktPGYr4y+bWVJUHlmzjkD6vVQDpGfpLhi5tc/gBaCqGHJMUlyBNFTxU45idRqVE\n"
    "uQ==\n"
    "-----END CERTIFICATE-----\n";

const char* kLeaf3 =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC/TCCAeWgAwIBAgIUPydoVDIZjCRaCjHPG8Ic3NkvpFgwDQYJKoZIhvcNAQEL\n"
    "BQAwFTETMBEGA1UEAwwKVGVzdCBJbnQgQjAeFw0yNjA4MjQxMjE1NTNaFw0yNjEx\n"
    "MjIxMjE1NTNaMBgxFjAUBgNVBAMMDXJlbGF5LmV4YW1wbGUwggEiMA0GCSqGSIb3\n"
    "DQEBAQUAA4IBDwAwggEKAoIBAQCiqNAJUpjJ4/pS4GFwIJD6PSPahj36WhGvYKxP\n"
    "a44RsGVBAXuoBBT3yW8cKkucgQYzvYpGcGhlHNZRePG8ZCt2CEhLm+BSj+ihqEUS\n"
    "pi0EKkgkbssQQnp931pDj8+O9Ul2PQFSajUwLXTGb4nyoFJwYX5iNwhi156i129J\n"
    "C8KDXXQHj3Loa65CX1Fpvx3QH7g4KxSehlY0z8zqEKwGzFPxGqJQQAAQvE/IdvGq\n"
    "GLNnJ7oy2udmuY2WR+0OgNaaUbjkvi5JfwUhkGsaO3yDvs125R+Y0d2CMuYaK3un\n"
    "t/wzMlXyqdQWaya4/eM43dBb+xZa4yLCM9BlnsqqEQ5hYQQfAgMBAAGjQjBAMB0G\n"
    "A1UdDgQWBBTgg5uE45z2xE+yvPWMhGnocNEF+TAfBgNVHSMEGDAWgBQlVSx5PHdC\n"
    "pvQUr6rpKohW692SJzANBgkqhkiG9w0BAQsFAAOCAQEAtPdkSbgWP2jljTv3uIQ/\n"
    "wf/VFy1012EMDvcfgNgo8C9k9RnMBibd2VSueBAr2lp0PjqZKr+vn4yQi+XqJ1IO\n"
    "ciuycpoUJG6DZ9Kko1CPUahkaZHHPyh1YSGA6BMUg1vNUvZbMYTINVgpRD76A0+g\n"
    "XQv+6v+tnUiwQecBcLuvyWWiunbKOMiJWPbe+7uLrxHekt2ftgej9LRM/LoQlv+u\n"
    "pFPgz1cZklcJzoMO5ttqOvKjXOP4s3WzbOvPjI5NnqrIujtk0UHJl1EP5MykqukS\n"
    "Mbsxu8CPpQwLjyo8DE7kqKz4tr7j7O9zBSeiAAONzxkgpf1JlBkZzkm5yWo24T4F\n"
    "Tw==\n"
    "-----END CERTIFICATE-----\n";

const char* kIntermediateA =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDHTCCAgWgAwIBAgIUPp3ldBw2R8NgRu47ce8Ft9NBfpkwDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMVGVzdCBSb290IFI5MB4XDTI2MDgyNDEyMTU1MloXDTM0\n"
    "MTExMDEyMTU1MlowFTETMBEGA1UEAwwKVGVzdCBJbnQgQTCCASIwDQYJKoZIhvcN\n"
    "AQEBBQADggEPADCCAQoCggEBAKnSTt2pRNXIYx5ry+n2beglMkTvmkFwedZI4uU/\n"
    "zR47bSUFuY63iphxiEWYO/9wD8R0ddmo84CN8RKmaXb7uPtKxxAhdwRlGKwUaGDI\n"
    "yCWjuUkhfrL2VlvaEBtY03M237Kz8unD/EbHRVd9o6Qzag7rDVN18urqyHq0PPd1\n"
    "Y2vKzYiRqsQZPwWRvhVARJGSZ99tYUGj55TIH8SG9l58LB+l16NO+GMUYQQBPNvO\n"
    "VKdvnUmq4K/7WmR9lomqfUPEjsi/5tp4cLHlez6mDhMu1zpYPxmgbtg9aLmJh2AD\n"
    "1ed8F1q5UAdcyotge1X40vx0cTS7omufnmlSW5ofPBKq7N0CAwEAAaNjMGEwDwYD\n"
    "VR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAgQwHQYDVR0OBBYEFNl9DjctElwg\n"
    "YKFKwEZT8uyVDEJxMB8GA1UdIwQYMBaAFMk0ZGLpWNiKAONWlsslJIVO8RksMA0G\n"
    "CSqGSIb3DQEBCwUAA4IBAQCF7GrUzLs/zWM7EuvtXXpFD1rSEvJDu8VVCAhurj4g\n"
    "NUj5K8F1dHqMo9QswONrrKu7FNCSI9dHBcuGphExUqoqTRmnDtyRKpEsMgIDD72k\n"
    "lnQ61TRv8oLEcNRzqPUE7wZRzfdEihNtkXcd2WmB57VX3C5cHBiU8J5aTQe1Dyxr\n"
    "dr0eM/jbbiUH6l7OFwq36MCN0XHwFUxwQ6OKB7sCz6JbJwbnp+aJYtW8O0TjyYFi\n"
    "8PUtgVAvnCG+TM/XWR5xu+hzqxLggz1MfVgGdkI1B585V5msKyDNqpNfOOqH/BTy\n"
    "LAJqtKhdVDyGauK3jZ160aTUzeuhYWqhd4LMi5Ju7UDD\n"
    "-----END CERTIFICATE-----\n";

const char* kIntermediateB =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDHTCCAgWgAwIBAgIUPp3ldBw2R8NgRu47ce8Ft9NBfpowDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMVGVzdCBSb290IFI5MB4XDTI2MDgyNDEyMTU1M1oXDTM0\n"
    "MTExMDEyMTU1M1owFTETMBEGA1UEAwwKVGVzdCBJbnQgQjCCASIwDQYJKoZIhvcN\n"
    "AQEBBQADggEPADCCAQoCggEBAMnCniqY2kKOA1y4wZEVaVBJxBxGXx0qZRpU7juJ\n"
    "smbonFz++fLH0xf35AhOBP418Occ/TeQF0yRJg3fUIPDcXeTF0KFDgzldyUL5J9D\n"
    "hVUO5N+QM9l9yB1gLxx+Y7rStvf4OcAxwKgWhaRCJcaJ1U43/ZxSGPD1dymoPoRR\n"
    "a7+sFwgxFcLDr50MoZbSX5qtSaDhR5/A/akD4WuQzobnYi8F4Cr3cL9a1RTuasCS\n"
    "dZZMKm4yaItVI/8EDDkmQItsDREMlayjg0HWj1zQn6cCoedZKpBya5AysHg/oOz+\n"
    "FXfbm6NBjeZPKXwhyFXq3P1JSN1bHzf+O/6X1TPp/rktFFECAwEAAaNjMGEwDwYD\n"
    "VR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAgQwHQYDVR0OBBYEFCVVLHk8d0Km\n"
    "9BSvqukqiFbr3ZInMB8GA1UdIwQYMBaAFMk0ZGLpWNiKAONWlsslJIVO8RksMA0G\n"
    "CSqGSIb3DQEBCwUAA4IBAQCvKofR/wGSwwj7dooh8q+jo1zBhJ12zA4GuSqdRI52\n"
    "/5bktVNMdhaHkWTWD1zA+p33Dzg71xlqFj/pTPGXiXmQ07Z1gbvXexK27G6JVbLq\n"
    "Vw5/1Zv57jIo7l6drw6/Bk9kZdVj6R49vdZRFUfOBX22z7xoovBj2uD0IvkOsoNv\n"
    "DyzBCeozN6kUYCUNyuDCTPyBszccmjYG42dDwHagtZh7Lkz4FBxJhHD+fta8+k6w\n"
    "fnooZ4YYYz/rzBDd6Jw2AcqB3/GGkyTQQS4/2PvhV6kHBjEyVXCBXtUhTiie0Vlm\n"
    "4U5lCLAODaWNpnoJbbfy13Do+xTbHvJiLyC64QVCkbUK\n"
    "-----END CERTIFICATE-----\n";

const char* kRoot =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDDzCCAfegAwIBAgIUQyrNO1xLsz4zZ2t6PM59CvWeqdIwDQYJKoZIhvcNAQEL\n"
    "BQAwFzEVMBMGA1UEAwwMVGVzdCBSb290IFI5MB4XDTI2MDgyNDEyMTU1MloXDTM2\n"
    "MDgyMTEyMTU1MlowFzEVMBMGA1UEAwwMVGVzdCBSb290IFI5MIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsCUW0hSRkHcQLAMkx1zrdhpSYd01GQY6Eym+\n"
    "vuYJCmp85DfT7OiEqnX5a22iOGO77K9p8YtLcPsM6NzQ8HuVuDQFmJ8iMJxEci+E\n"
    "q3RlUD9+4I+ev7gSagt5ahMLJCe4TqXUxhR+Mgl5QMsWx+UahEFdV8V+qOPk1LKs\n"
    "/fcoLb4z9HRMK8coDKMYek/k9tR5inn5evzeIY+gp/ziQ/K6ab3f+3Z5VvxUF837\n"
    "J0XHsNnH0v7wklOfFwICHjyqGSHRruuoYkdP/RLX2GGTsoz/TUX2DaGPGnq1jTC6\n"
    "yFRGpL6dVKofLl25ObABrlye7TxfvUf4OVL+ErrdlmqV0HZozQIDAQABo1MwUTAd\n"
    "BgNVHQ4EFgQUyTRkYulY2IoA41aWyyUkhU7xGSwwHwYDVR0jBBgwFoAUyTRkYulY\n"
    "2IoA41aWyyUkhU7xGSwwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOC\n"
    "AQEAqyOgL7JRtWkWw7WDxhETelgYdKNOdbb9N44Zz7Z3uTjWW+ammluzGd80d071\n"
    "095y0cHHPOVb6IVYy2wqnf5um1x4ldG262myvxHCnkKwlcFZIAfGyPJM5+JJlJWz\n"
    "fFvOlAVNIIPnMXyjp37kS/DYwSnRn/1MPjq48+vjt5UoElCBBgels8ZwwfQ3JFgB\n"
    "dBsrdd16BanTltrxh/WxWnYaByageKeiUJ0R0Cm/1DW/biWI5eZTK2STIQY20uhk\n"
    "vL3mUGoe3e6Ia3tNWu5ygLdjhAmEL1+hzg/oUkmzCy23UYvur1nzvDb8Hbwd21I/\n"
    "sui3waUcbUWm75VTBEoKIGo/vw==\n"
    "-----END CERTIFICATE-----\n";

QList<QSslCertificate> chainOf(std::initializer_list<const char*> pems)
{
    QList<QSslCertificate> chain;
    for (const char* pem : pems)
        chain.append(QSslCertificate(QByteArray(pem), QSsl::Pem));
    return chain;
}

QByteArray spkiOf(const char* pem)
{
    const QSslCertificate cert{ QByteArray(pem), QSsl::Pem };
    return QCryptographicHash::hash(cert.publicKey().toDer(), QCryptographicHash::Sha256);
}

}

class CertificatePinAnchorTest : public QObject
{
    Q_OBJECT

private slots:
    void theFixtureItselfIsSane();
    void thePinIsTheIssuerNotTheLeaf();
    void aRotatedLeafKeepsTheSamePin();
    void aDifferentIssuerYieldsADifferentPin();
    void aChainWithNoIssuerPinsNothing();
    void aChainWhoseSecondCertDidNotSignTheLeafPinsNothing();
    void anEmptyChainPinsNothing();
    void anUnreadableCertificatePinsNothing();
};

// If the generated leaves shared a key, or the intermediates did, every
// other test here would pass while proving nothing.
void CertificatePinAnchorTest::theFixtureItselfIsSane()
{
    QVERIFY(!spkiOf(kLeaf1).isEmpty());
    QVERIFY2(spkiOf(kLeaf1) != spkiOf(kLeaf2),
             "the two leaves must have different keys or the rotation test is vacuous");
    QVERIFY2(spkiOf(kIntermediateA) != spkiOf(kIntermediateB),
             "the two intermediates must differ or the impersonation test is vacuous");
}

void CertificatePinAnchorTest::thePinIsTheIssuerNotTheLeaf()
{
    const QByteArray pin =
        HttpClient::pinnedSpkiFromChain(chainOf({ kLeaf1, kIntermediateA, kRoot }));

    QCOMPARE(pin, spkiOf(kIntermediateA));
    QVERIFY2(pin != spkiOf(kLeaf1), "pinning the leaf is the bug this exists to fix");
    QVERIFY2(pin != spkiOf(kRoot), "the root is not the anchor -- it is too permissive");
}

// The whole point: Cloudflare rolls the leaf and the app must not notice.
void CertificatePinAnchorTest::aRotatedLeafKeepsTheSamePin()
{
    const QByteArray before =
        HttpClient::pinnedSpkiFromChain(chainOf({ kLeaf1, kIntermediateA, kRoot }));
    const QByteArray after =
        HttpClient::pinnedSpkiFromChain(chainOf({ kLeaf2, kIntermediateA, kRoot }));

    QVERIFY(!before.isEmpty());
    QCOMPARE(after, before);
}

// ...while a chain from anywhere else still trips the alarm.
void CertificatePinAnchorTest::aDifferentIssuerYieldsADifferentPin()
{
    const QByteArray honest =
        HttpClient::pinnedSpkiFromChain(chainOf({ kLeaf1, kIntermediateA, kRoot }));
    const QByteArray impostor =
        HttpClient::pinnedSpkiFromChain(chainOf({ kLeaf3, kIntermediateB, kRoot }));

    QVERIFY(!impostor.isEmpty());
    QVERIFY2(impostor != honest, "a chain from another CA must not satisfy the pin");
}

// Fail closed, exactly as the leaf check already did for an unreadable key:
// deriving a pin from nothing degrades trust-on-first-use into
// trust-on-every-use.
void CertificatePinAnchorTest::aChainWithNoIssuerPinsNothing()
{
    QVERIFY(HttpClient::pinnedSpkiFromChain(chainOf({ kLeaf1 })).isEmpty());
}

// A reordered or cross-signed chain must not silently anchor us to whatever
// happens to sit at index 1 -- pinning the root would quietly widen trust to
// every site under it.
void CertificatePinAnchorTest::aChainWhoseSecondCertDidNotSignTheLeafPinsNothing()
{
    // leaf1 was issued by intermediate A, so a chain offering B is malformed.
    QVERIFY(HttpClient::pinnedSpkiFromChain(chainOf({ kLeaf1, kIntermediateB, kRoot })).isEmpty());
    // ...and so is one that puts the root where the issuer belongs.
    QVERIFY(HttpClient::pinnedSpkiFromChain(chainOf({ kLeaf1, kRoot })).isEmpty());
}

void CertificatePinAnchorTest::anEmptyChainPinsNothing()
{
    QVERIFY(HttpClient::pinnedSpkiFromChain({}).isEmpty());
}

void CertificatePinAnchorTest::anUnreadableCertificatePinsNothing()
{
    QList<QSslCertificate> chain;
    chain.append(QSslCertificate(QByteArray(kLeaf1), QSsl::Pem));
    chain.append(QSslCertificate()); // null: what Qt hands back for a key it cannot parse
    QVERIFY(HttpClient::pinnedSpkiFromChain(chain).isEmpty());
}

QTEST_MAIN(CertificatePinAnchorTest)
#include "CertificatePinAnchorTest.moc"
