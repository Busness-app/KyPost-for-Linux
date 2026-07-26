#include "EmailDao.h"

#include "SqlUtil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSqlQuery>

namespace {

QString encodeKeywords(const QStringList& keywords)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(keywords)).toJson(QJsonDocument::Compact));
}

QStringList decodeKeywords(const QString& json)
{
    QStringList result;
    if (json.isEmpty())
        return result;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    for (const QJsonValue& value : doc.array())
        result.append(value.toString());
    return result;
}

Email emailFromQuery(const QSqlQuery& query)
{
    Email email;
    email.messageId = query.value(QStringLiteral("message_id")).toString();
    email.folder = query.value(QStringLiteral("folder")).toString();
    email.sender = query.value(QStringLiteral("sender")).toString();
    email.sentTo = query.value(QStringLiteral("sent_to")).toString();
    email.cc = query.value(QStringLiteral("cc")).toString();
    email.bcc = query.value(QStringLiteral("bcc")).toString();
    email.subject = query.value(QStringLiteral("subject")).toString();
    email.preview = query.value(QStringLiteral("preview")).toString();
    email.body = variantToOptionalString(query.value(QStringLiteral("body")));
    email.label = query.value(QStringLiteral("label")).toString();
    email.keywords = decodeKeywords(query.value(QStringLiteral("keywords_json")).toString());
    email.status = query.value(QStringLiteral("status")).toString();
    email.atUtc = query.value(QStringLiteral("at_utc")).toString();
    email.hasAttachments = query.value(QStringLiteral("has_attachments")).toInt() != 0;
    email.sourceMode = query.value(QStringLiteral("source_mode")).toString();
    email.pgpEncrypted = query.value(QStringLiteral("pgp_encrypted")).toInt() != 0;
    email.pgpDecryptError = query.value(QStringLiteral("pgp_decrypt_error")).toString();
    return email;
}

QVector<Email> collect(QSqlQuery& query)
{
    QVector<Email> results;
    while (query.next())
        results.append(emailFromQuery(query));
    return results;
}

} // namespace

EmailDao::EmailDao(QSqlDatabase& db) : m_db(db)
{
}

bool EmailDao::insertOrReplace(const Email& email)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO emails "
        "(message_id, folder, sender, sent_to, cc, bcc, subject, preview, body, label, "
        "keywords_json, status, at_utc, has_attachments, source_mode, pgp_encrypted, pgp_decrypt_error) "
        "VALUES (:message_id, :folder, :sender, :sent_to, :cc, :bcc, :subject, :preview, :body, "
        ":label, :keywords_json, :status, :at_utc, :has_attachments, :source_mode, "
        ":pgp_encrypted, :pgp_decrypt_error)"));
    query.bindValue(QStringLiteral(":message_id"), email.messageId);
    query.bindValue(QStringLiteral(":folder"), email.folder);
    query.bindValue(QStringLiteral(":sender"), email.sender);
    query.bindValue(QStringLiteral(":sent_to"), email.sentTo);
    query.bindValue(QStringLiteral(":cc"), email.cc);
    query.bindValue(QStringLiteral(":bcc"), email.bcc);
    query.bindValue(QStringLiteral(":subject"), email.subject);
    query.bindValue(QStringLiteral(":preview"), email.preview);
    query.bindValue(QStringLiteral(":body"), optionalStringToVariant(email.body));
    query.bindValue(QStringLiteral(":label"), email.label);
    query.bindValue(QStringLiteral(":keywords_json"), encodeKeywords(email.keywords));
    query.bindValue(QStringLiteral(":status"), email.status);
    query.bindValue(QStringLiteral(":at_utc"), email.atUtc);
    query.bindValue(QStringLiteral(":has_attachments"), email.hasAttachments ? 1 : 0);
    query.bindValue(QStringLiteral(":source_mode"), email.sourceMode);
    query.bindValue(QStringLiteral(":pgp_encrypted"), email.pgpEncrypted ? 1 : 0);
    query.bindValue(QStringLiteral(":pgp_decrypt_error"), email.pgpDecryptError);
    return query.exec();
}

std::optional<Email> EmailDao::findById(const QString& messageId) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM emails WHERE message_id = :message_id"));
    query.bindValue(QStringLiteral(":message_id"), messageId);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return emailFromQuery(query);
}

QVector<Email> EmailDao::findByFolder(const QString& folder) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT * FROM emails WHERE folder = :folder"));
    query.bindValue(QStringLiteral(":folder"), folder);
    if (!query.exec())
        return {};
    return collect(query);
}

QVector<Email> EmailDao::findAll() const
{
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT * FROM emails")))
        return {};
    return collect(query);
}

bool EmailDao::deleteById(const QString& messageId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM emails WHERE message_id = :message_id"));
    query.bindValue(QStringLiteral(":message_id"), messageId);
    return query.exec();
}

bool EmailDao::deleteAll()
{
    QSqlQuery query(m_db);
    return query.exec(QStringLiteral("DELETE FROM emails"));
}

bool EmailDao::deleteByFolder(const QString& folder)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM emails WHERE folder = :folder"));
    query.bindValue(QStringLiteral(":folder"), folder);
    return query.exec();
}

bool EmailDao::replaceFolderSnapshot(const QString& folder, const QVector<Email>& emails)
{
    if (!m_db.transaction())
        return false;

    if (!deleteByFolder(folder)) {
        m_db.rollback();
        return false;
    }

    for (const Email& email : emails) {
        if (!insertOrReplace(email)) {
            m_db.rollback();
            return false;
        }
    }

    return m_db.commit();
}
