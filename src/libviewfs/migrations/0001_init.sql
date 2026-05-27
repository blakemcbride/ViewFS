-- ViewFS schema, version 1.
--
-- The current schema is set by store.c before this file is executed
-- (`SET search_path TO "<schema>"`). All CREATE TABLE statements therefore
-- create their objects inside the configured schema.

CREATE TABLE schema_migrations (
    version    INTEGER PRIMARY KEY,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE objects (
    object_id      TEXT PRIMARY KEY,
    kind           TEXT NOT NULL CHECK (kind IN ('file','symlink')),
    size           BIGINT NOT NULL DEFAULT 0,
    mode           INTEGER NOT NULL,
    uid            INTEGER,
    gid            INTEGER,
    ctime_ns       BIGINT NOT NULL,
    mtime_ns       BIGINT NOT NULL,
    atime_ns       BIGINT NOT NULL,
    checksum       TEXT,
    source_path    TEXT,
    symlink_target TEXT
);

CREATE TABLE views (
    view_name   TEXT PRIMARY KEY,
    description TEXT,
    ctime_ns    BIGINT NOT NULL,
    mtime_ns    BIGINT NOT NULL
);

CREATE TABLE mappings (
    view_name     TEXT NOT NULL REFERENCES views(view_name) ON DELETE CASCADE,
    view_path     TEXT NOT NULL,
    parent_path   TEXT NOT NULL,
    name          TEXT NOT NULL,
    entry_kind    TEXT NOT NULL CHECK (entry_kind IN ('file','dir','symlink')),
    object_id     TEXT REFERENCES objects(object_id) ON DELETE SET NULL,
    mode_override INTEGER,
    ctime_ns      BIGINT NOT NULL,
    mtime_ns      BIGINT NOT NULL,
    PRIMARY KEY (view_name, view_path),
    CHECK ( (entry_kind = 'dir') = (object_id IS NULL) )
);
CREATE INDEX mappings_parent ON mappings (view_name, parent_path);
CREATE INDEX mappings_object ON mappings (object_id);

CREATE TABLE attributes (
    object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,
    key       TEXT NOT NULL,
    value     TEXT NOT NULL,
    ctime_ns  BIGINT NOT NULL,
    mtime_ns  BIGINT NOT NULL,
    PRIMARY KEY (object_id, key)
);
CREATE INDEX attributes_kv ON attributes (key, value);

CREATE TABLE tags (
    object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,
    tag       TEXT NOT NULL,
    ctime_ns  BIGINT NOT NULL,
    PRIMARY KEY (object_id, tag)
);
CREATE INDEX tags_tag ON tags (tag);
