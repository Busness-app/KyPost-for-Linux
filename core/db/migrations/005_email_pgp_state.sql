-- OpenPGP state for cached messages, mirroring the `pgpEncrypted` /
-- `pgpDecryptError` fields on /api/inbox rows (backend:
-- internal/api/server.go's inbox entry struct, both `omitempty`).
--
-- Cached rather than recomputed because the inbox list renders from the
-- local snapshot before any network call returns: without these columns a
-- client-protected message would render as an ordinary empty message on
-- every cold start, which is the exact confusion this feature removes.
--
-- Defaults match the wire's absent-means-nothing semantics, so rows written
-- by an older build read back as "no PGP content" instead of NULL.

ALTER TABLE emails ADD COLUMN pgp_encrypted INTEGER DEFAULT 0;
ALTER TABLE emails ADD COLUMN pgp_decrypt_error TEXT DEFAULT '';
