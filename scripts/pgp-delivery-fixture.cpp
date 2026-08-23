// Emits one PGP/MIME delivery and one protected-headers content part, so
// scripts/verify-pgp-delivery-shape.sh can feed them to the RELAY'S OWN
// validators rather than to a second opinion written here.
//
// Compiled by that script against core/pgp/PgpMimeWriter.cpp directly. It is
// committed rather than heredoc'd into the script because a throwaway harness
// that does not compile proves nothing, and this one is built and run every
// time the check is.

#include "pgp/PgpMimeWriter.h"

#include <QFile>

#include <cstdio>

namespace {

bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "could not write %s\n", qUtf8Printable(path));
        return false;
    }
    return file.write(bytes) == bytes.size();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: pgp-delivery-fixture <delivery-out> <protected-out>\n");
        return 2;
    }

    OutgoingMessage message;
    message.from = QStringLiteral("me@example.com");
    message.to = { QStringLiteral("you@example.com"), QStringLiteral("other@example.com") };
    message.cc = { QStringLiteral("cc@example.com") };
    // Non-ASCII on purpose: the relay's ExtractProtectedSubject has to decode
    // whatever this client encodes, and an em dash plus an accent is where an
    // encoded-word implementation goes wrong.
    message.subject = QStringLiteral("Café — Redundancies confirmed");
    message.body = QStringLiteral("The real body.");
    message.mode = QStringLiteral("plain");
    message.date = QStringLiteral("Sat, 23 Aug 2026 12:00:00 +0000");

    // A realistically SHAPED armor, not a three-line token: the relay runs
    // mailmsg.PrepareSMTPMessage over these bytes, which normalises line
    // endings and dot-stuffs, and a stub too short to contain a line
    // beginning with "." or a blank line exercises none of that.
    QString armor = QStringLiteral("-----BEGIN PGP MESSAGE-----\r\n\r\n");
    for (int line = 0; line < 60; ++line) {
        // 64 base64 characters, the width gpg emits. One line deliberately
        // starts with a period, which is the byte SMTP has to escape.
        armor += (line == 7 ? QStringLiteral(".") : QStringLiteral("h"))
            + QStringLiteral("QEMA0abc123def456ghi789jkl012mno345pqr678stu901vwx234yz567AB")
            + QStringLiteral("%1").arg(line, 4, 10, QLatin1Char('0')) + QStringLiteral("\r\n");
    }
    armor += QStringLiteral("=xY1z\r\n-----END PGP MESSAGE-----");

    if (!write(QString::fromLocal8Bit(argv[1]),
                pgpMimeDelivery(message, armor, randomMimeBoundary()))) {
        return 1;
    }
    if (!write(QString::fromLocal8Bit(argv[2]),
                protectedContent(message, randomMimeBoundary()))) {
        return 1;
    }
    return 0;
}
