#pragma once

#include "models/Email.h"

#include <QAbstractListModel>
#include <QHash>
#include <QVariant>
#include <QVector>

// QML-facing read-only model wrapping a QVector<Email> (Task 32). Owned by
// MailController, which is the only writer -- setEmails() is called from
// MailController::applyFilter() whenever the current folder/keyword
// selection changes. One row per Email, one role per Email field (plus
// hasAttachments/keywords passed through their native Qt types so QML can
// test/iterate them directly rather than stringifying).
class EmailListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role
    {
        MessageIdRole = Qt::UserRole + 1,
        FolderRole,
        SenderRole,
        SentToRole,
        CcRole,
        BccRole,
        SubjectRole,
        PreviewRole,
        BodyRole,
        LabelRole,
        KeywordsRole,
        StatusRole,
        AtUtcRole,
        HasAttachmentsRole,
        SourceModeRole,
        // Presentation of the message's OpenPGP state: a glyph for rows the
        // user cannot read, plus its spoken equivalent. Exposed as roles
        // rather than computed in QML so the rule lives in one place
        // (core/domain/PgpMessageState.h + app/mail/PgpMessagePresentation.h)
        // and both roots render identically.
        PgpMarkerRole,
        PgpMarkerAccessibleNameRole,
    };

    explicit EmailListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setEmails(const QVector<Email>& emails);
    Email emailAt(int row) const; // out-of-range -> default-constructed Email

private:
    QVector<Email> m_emails;
};
