-- `emails` was keyed on `message_id` alone, so a message could hold exactly
-- one row across the entire cache. The relay serves the same messageId from
-- every mailbox that holds it (INBOX and Archive after an archive, INBOX and
-- a keyword tab, Sent and a thread), and INSERT OR REPLACE collapsed those
-- onto one row whose `folder` was whichever mailbox synced last. The symptoms
-- were a message vanishing from one folder when another folder refreshed, and
-- `DELETE FROM emails WHERE message_id = ?` on a delta `removed` entry
-- evicting the copy in an unrelated folder.
--
-- This was dormant while the client never entered delta mode (see
-- MailRepository::planRefresh -- `since` was omitted, so the relay answered on
-- its classic path and returned no cursor). Turning delta sync on is what
-- makes it live, which is why the key change ships in the same commit.
--
-- SQLite cannot alter a PRIMARY KEY in place, so this is the documented
-- 12-step table rebuild, minus the foreign-key steps (this schema declares
-- none). Database::open() already runs each migration inside one transaction.
--
-- `folder` was nullable with no default and the pre-Task-35 rows predate it
-- being written consistently, so it is COALESCEd to '' on the way across:
-- a NULL half of a composite PRIMARY KEY does not compare equal to itself in
-- SQLite and would let duplicates back in through the new key.

CREATE TABLE emails_rekeyed (
    message_id TEXT NOT NULL,
    folder TEXT NOT NULL DEFAULT '',
    sender TEXT,
    sent_to TEXT,
    cc TEXT,
    bcc TEXT,
    subject TEXT,
    preview TEXT,
    body TEXT,
    label TEXT,
    keywords_json TEXT,
    status TEXT,
    at_utc TEXT,
    has_attachments INTEGER,
    source_mode TEXT,
    pgp_encrypted INTEGER DEFAULT 0,
    pgp_decrypt_error TEXT DEFAULT '',
    PRIMARY KEY (folder, message_id)
);

INSERT OR REPLACE INTO emails_rekeyed
    (message_id, folder, sender, sent_to, cc, bcc, subject, preview, body, label,
     keywords_json, status, at_utc, has_attachments, source_mode, pgp_encrypted, pgp_decrypt_error)
SELECT message_id, COALESCE(folder, ''), sender, sent_to, cc, bcc, subject, preview, body, label,
       keywords_json, status, at_utc, has_attachments, source_mode,
       COALESCE(pgp_encrypted, 0), COALESCE(pgp_decrypt_error, '')
FROM emails;

DROP TABLE emails;

ALTER TABLE emails_rekeyed RENAME TO emails;

CREATE INDEX idx_emails_folder_atutc ON emails(folder, at_utc);
