-- Migration for Release
CREATE TABLE IF NOT EXISTS releases (
    id TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    image TEXT NOT NULL,
    config_version TEXT NOT NULL,
    status TEXT,
    created_at TIMESTAMP
);

-- Migration for App
CREATE TABLE IF NOT EXISTS apps (
    id TEXT PRIMARY KEY,
    slug TEXT NOT NULL UNIQUE,
    default_domain TEXT,
    current_release_id TEXT,
    created_at TIMESTAMP,
    updated_at TIMESTAMP
);

-- Migration for Instance
CREATE TABLE IF NOT EXISTS instances (
    id TEXT PRIMARY KEY,
    service_id TEXT NOT NULL,
    node TEXT NOT NULL,
    ip TEXT NOT NULL,
    port INTEGER NOT NULL,
    state TEXT NOT NULL,
    started_at TIMESTAMP,
    last_heartbeat_at TIMESTAMP
);

-- Migration for Service
CREATE TABLE IF NOT EXISTS services (
    id TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    release_id TEXT NOT NULL,
    name TEXT NOT NULL,
    kind TEXT NOT NULL,
    mode TEXT NOT NULL,
    port INTEGER NOT NULL,
    min_replicas INTEGER,
    max_replicas INTEGER,
    desired_replicas INTEGER,
    state TEXT,
    created_at TIMESTAMP,
    updated_at TIMESTAMP
);

