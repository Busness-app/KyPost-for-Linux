#include "mail/EmailListModel.h"

#include "mail/PgpMessagePresentation.h"

EmailListModel::EmailListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int EmailListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return m_emails.size();
}

QVariant EmailListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_emails.size())
        return QVariant();

    const Email& email = m_emails.at(index.row());
    switch (role) {
    case MessageIdRole:
        return email.messageId;
    case FolderRole:
        return email.folder;
    case SenderRole:
        return email.sender;
    case SentToRole:
        return email.sentTo;
    case CcRole:
        return email.cc;
    case BccRole:
        return email.bcc;
    case SubjectRole:
        return email.subject;
    case PreviewRole:
        return email.preview;
    case BodyRole:
        return email.body.value_or(QString());
    case LabelRole:
        return email.label;
    case KeywordsRole:
        return email.keywords;
    case StatusRole:
        return email.status;
    case AtUtcRole:
        return email.atUtc;
    case HasAttachmentsRole:
        return email.hasAttachments;
    case SourceModeRole:
        return email.sourceMode;
    case PgpMarkerRole:
        return pgpRowMarker(pgpMessageStateOf(email));
    case PgpMarkerAccessibleNameRole:
        return pgpRowMarkerAccessibleName(pgpMessageStateOf(email));
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> EmailListModel::roleNames() const
{
    return {
        { MessageIdRole, "messageId" },
        { FolderRole, "folder" },
        { SenderRole, "sender" },
        { SentToRole, "sentTo" },
        { CcRole, "cc" },
        { BccRole, "bcc" },
        { SubjectRole, "subject" },
        { PreviewRole, "preview" },
        { BodyRole, "body" },
        { LabelRole, "label" },
        { KeywordsRole, "keywords" },
        { StatusRole, "status" },
        { AtUtcRole, "atUtc" },
        { HasAttachmentsRole, "hasAttachments" },
        { SourceModeRole, "sourceMode" },
        { PgpMarkerRole, "pgpMarker" },
        { PgpMarkerAccessibleNameRole, "pgpMarkerAccessibleName" },
    };
}

void EmailListModel::setEmails(const QVector<Email>& emails)
{
    beginResetModel();
    m_emails = emails;
    endResetModel();
}

Email EmailListModel::emailAt(int row) const
{
    if (row < 0 || row >= m_emails.size())
        return Email();
    return m_emails.at(row);
}
