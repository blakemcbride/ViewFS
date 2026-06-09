-- ViewFS schema, version 1 (property-driven membership model).
--
-- The current schema is set by store.c before this file is executed
-- (`SET search_path TO "<schema>"`). All CREATE TABLE statements therefore
-- create their objects inside the configured schema.
--
-- Membership is NOT stored. A directory carries a set of property/value
-- pairs (view_dir_props); a file (object) appears in a directory iff the
-- object's properties are a superset of that directory's *effective* set
-- (own pairs plus any ancestor pair marked flow=TRUE). There is no
-- per-file placement table.

CREATE TABLE schema_migrations (
    version    INTEGER PRIMARY KEY,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE objects (
    object_id      TEXT PRIMARY KEY,
    kind           TEXT NOT NULL CHECK (kind IN ('file','symlink')),
    name           TEXT NOT NULL DEFAULT '',   -- intrinsic display name
    size           BIGINT NOT NULL DEFAULT 0,
    mode           INTEGER NOT NULL,
    uid            INTEGER,
    gid            INTEGER,
    ctime_ns       BIGINT NOT NULL,
    mtime_ns       BIGINT NOT NULL,
    atime_ns       BIGINT NOT NULL,
    checksum       TEXT,
    checksum_state BYTEA,                       -- 112-byte SHA256_CTX; resumes appends
    source_path    TEXT,
    symlink_target TEXT
);

-- Object properties. Multi-valued: PK includes value. A "tag" is sugar
-- for a property whose key is 'tag'.
CREATE TABLE object_props (
    object_id TEXT NOT NULL REFERENCES objects(object_id) ON DELETE CASCADE,
    key       TEXT NOT NULL,
    value     TEXT NOT NULL,
    ctime_ns  BIGINT NOT NULL,
    mtime_ns  BIGINT NOT NULL,
    PRIMARY KEY (object_id, key, value)
);
CREATE INDEX object_props_kv  ON object_props (key, value);
CREATE INDEX object_props_obj ON object_props (object_id);

CREATE TABLE views (
    view_name   TEXT PRIMARY KEY,
    description TEXT,
    ctime_ns    BIGINT NOT NULL,
    mtime_ns    BIGINT NOT NULL
);

-- The per-view directory tree. Directories ONLY; files are never rows.
-- Root row: dir_path='/', parent_path='' (sentinel: root has no parent).
-- A direct child of root has parent_path='/'.
CREATE TABLE view_dirs (
    view_name   TEXT NOT NULL REFERENCES views(view_name) ON DELETE CASCADE,
    dir_path    TEXT NOT NULL,                  -- canonical, leading '/'
    parent_path TEXT NOT NULL,
    name        TEXT NOT NULL,                  -- final component; '' for root
    mode        INTEGER NOT NULL,
    ctime_ns    BIGINT NOT NULL,
    mtime_ns    BIGINT NOT NULL,
    PRIMARY KEY (view_name, dir_path)
);
CREATE INDEX view_dirs_parent ON view_dirs (view_name, parent_path);

-- Property pairs attached to each directory, with the per-pair flow flag.
CREATE TABLE view_dir_props (
    view_name TEXT NOT NULL,
    dir_path  TEXT NOT NULL,
    key       TEXT NOT NULL,
    value     TEXT NOT NULL,
    flow      BOOLEAN NOT NULL DEFAULT FALSE,
    ctime_ns  BIGINT NOT NULL,
    mtime_ns  BIGINT NOT NULL,
    PRIMARY KEY (view_name, dir_path, key, value),
    FOREIGN KEY (view_name, dir_path)
        REFERENCES view_dirs(view_name, dir_path)
        ON DELETE CASCADE ON UPDATE CASCADE
);
CREATE INDEX view_dir_props_flow ON view_dir_props (view_name, flow);
