#pragma once

#include "pgp/PgpSendPlanner.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

// The request body, as a value.
//
// Separated from send() so the exact bytes this client would put on the wire
// can be handed to the RELAY'S OWN decoder and validators without standing up
// a socket -- see scripts/verify-pgp-send-request.sh. A wire format verified
// against a second opinion written here is worth much less than one verified
// against the code that will actually read it.
QJsonObject pgpSendRequestBody(const QString& from, const PgpSendPlan& plan, const QStringList& to,
                                const QStringList& cc, const QStringList& bcc, const QString& mode);
