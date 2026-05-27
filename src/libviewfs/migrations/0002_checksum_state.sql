-- ViewFS schema, migration 2: persisted SHA-256 intermediate state
-- so the FUSE daemon can resume an interrupted "append-only" hash on
-- the next open without re-reading the entire file.
--
-- The column holds an OpenSSL SHA256_CTX struct as raw bytes
-- (VFS_SHA256_STATE_LEN = 112 bytes today). NULL means "no resumable
-- state available" — the daemon will refuse to update the hash on
-- write and instead set checksum = NULL when the file is closed.

ALTER TABLE objects ADD COLUMN checksum_state BYTEA;
