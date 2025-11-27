#!/usr/bin/env bash
set -euo pipefail

# Deploy helper:
# 1) Reads config.toml in the current directory (or a provided path)
# 2) Base64 uploads the artifact to /api/v1/upload
# 3) Uses returned URL to POST /api/v1/app/deploy
#
# Usage:
#   ./t4_deploy.sh [config-path] [api-base]
# Defaults: config.toml in cwd, api-base=http://localhost:3000

CONFIG_PATH="${1:-config.toml}"
API_BASE="${2:-http://localhost:3000}"

if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "Config not found: $CONFIG_PATH" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required" >&2
  exit 1
fi

echo "[t4] Using config: $CONFIG_PATH"
echo "[t4] API base:    $API_BASE"

# temp files for payloads
UPLOAD_JSON_BIN=$(mktemp)
UPLOAD_JSON_ASSETS=$(mktemp)
DEPLOY_JSON=$(mktemp)
trap 'rm -f "$UPLOAD_JSON_BIN" "$UPLOAD_JSON_ASSETS" "$DEPLOY_JSON"' EXIT

# Build upload payloads (binary + optional assets zip)
python3 - "$CONFIG_PATH" "$UPLOAD_JSON_BIN" "$UPLOAD_JSON_ASSETS" <<'PY'
import base64, json, sys, tomllib, pathlib, uuid, re, zipfile, tempfile

cfg_path = pathlib.Path(sys.argv[1]).resolve()
out_bin = pathlib.Path(sys.argv[2])
out_assets = pathlib.Path(sys.argv[3])
data = tomllib.loads(cfg_path.read_text())
base_dir = cfg_path.parent

deploy = data.get("deploy", {})
artifact = data.get("artifact", {})
path_spec = artifact.get("path", "")
entrypoint_spec = artifact.get("entrypoint", path_spec)
includes = artifact.get("include", [])

project_slug = deploy.get("project_slug", "app")
config_version = deploy.get("config_version", "v1")
safe_slug = re.sub(r"[^A-Za-z0-9_.-]", "-", project_slug) or "app"
safe_cfg = re.sub(r"[^A-Za-z0-9_.-]", "-", config_version) or "v1"
unique = uuid.uuid4().hex[:8]
bundle_name = f"{safe_slug}-{safe_cfg}-{unique}"

bin_path = (base_dir / path_spec).resolve()
if not bin_path.exists():
    raise SystemExit(f"Binary not found: {bin_path}")

def arcname_from_spec(spec: str, p: pathlib.Path) -> str:
    if spec:
        spec_path = pathlib.Path(spec)
        if not spec_path.is_absolute():
            return spec_path.as_posix().lstrip("./")
    p = p.resolve()
    try:
        return str(p.relative_to(base_dir))
    except ValueError:
        return p.name

# Binary payload (no zip)
bin_payload = {
    "filename": bin_path.name,
    "deployment": bundle_name,
    "entrypoint": arcname_from_spec(path_spec, bin_path),
    "data": base64.b64encode(bin_path.read_bytes()).decode("ascii"),
}
out_bin.write_text(json.dumps(bin_payload))

# Assets payload (zip) if includes present
if includes:
    zip_name = f"{bundle_name}-assets.zip"
    tmp_zip = pathlib.Path(tempfile.gettempdir()) / zip_name
    def arc_for_include(item_spec: str, file_path: pathlib.Path, item_root: pathlib.Path) -> str:
        prefix = pathlib.Path(item_spec).as_posix().lstrip("./").rstrip("/")
        rel = file_path.relative_to(item_root)
        rel_name = rel.as_posix()
        if prefix:
            return f"{prefix}/{rel_name}" if rel_name != "." else prefix
        return rel_name

    with zipfile.ZipFile(tmp_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        for item_spec in includes:
            item_path = (base_dir / item_spec).resolve()
            if item_path.is_dir():
                for p in item_path.rglob("*"):
                    if p.is_file():
                        zf.write(p, arc_for_include(item_spec, p, item_path))
            elif item_path.is_file():
                zf.write(item_path, arc_for_include(item_spec, item_path, item_path.parent))
    assets_payload = {
        "slug": project_slug,
        "filename": zip_name,
        "data": base64.b64encode(tmp_zip.read_bytes()).decode("ascii"),
    }
    out_assets.write_text(json.dumps(assets_payload))
    tmp_zip.unlink()
else:
    out_assets.write_text("")
PY

echo "[t4] Uploading assets (if any)..."
if [[ -s "$UPLOAD_JSON_ASSETS" ]]; then
  SLUG=$(python3 - <<'PY' "$UPLOAD_JSON_ASSETS"
import json,sys
print(json.load(open(sys.argv[1])).get("slug",""))
PY
)
  curl -sSf -X POST "$API_BASE/api/v1/upload_assets" \
    -H "Content-Type: application/json" \
    --data-binary "@$UPLOAD_JSON_ASSETS" >/dev/null
  echo "[t4] Assets uploaded to apps/$SLUG"
else
  echo "[t4] No assets to upload."
fi

echo "[t4] Uploading binary..."
upload_resp=$(curl -sSf -X POST "$API_BASE/api/v1/upload" \
  -H "Content-Type: application/json" \
  --data-binary "@$UPLOAD_JSON_BIN")

upload_url=$(python3 - "$upload_resp" <<'PY'
import json, sys
raw = sys.argv[1]
try:
    resp = json.loads(raw)
except Exception as e:
    raise SystemExit(f"Upload response not JSON: {raw}")
url = resp.get("url")
if not url:
    raise SystemExit(f"Upload failed/invalid response: {resp}")
print(url)
PY
)

echo "[t4] Binary upload complete: $upload_url"

# Build deploy payload with the uploaded URL (to temp file)
python3 - "$CONFIG_PATH" "$upload_url" "$DEPLOY_JSON" <<'PY'
import json, sys, tomllib, pathlib

cfg_path = pathlib.Path(sys.argv[1])
upload_url = sys.argv[2]
out_path = pathlib.Path(sys.argv[3])
cfg = tomllib.loads(cfg_path.read_text())

deploy = cfg.get("deploy", {})
project = cfg.get("project", {})
services = cfg.get("services", [])

payload = {
    "project_slug": deploy.get("project_slug", "sample-app"),
    "image": upload_url,
    "config_version": deploy.get("config_version", "v1"),
    "cloud_config": {
        "version": "0.1",
        "project": {
            "name": project.get("name", ""),
            "env": project.get("env", ""),
            "region": project.get("region", ""),
        },
        "services": services,
    },
}

out_path.write_text(json.dumps(payload))
PY

echo "[t4] Deploying..."
curl -sS -i -X POST "$API_BASE/api/v1/app/deploy" \
  -H "Content-Type: application/json" \
  --data-binary "@$DEPLOY_JSON"

echo
echo "[t4] Done."
echo "[t4] Interface URL: ${API_BASE}/interface/$(python3 - "$CONFIG_PATH" <<'PY'
import tomllib, sys, pathlib
cfg = tomllib.loads(pathlib.Path(sys.argv[1]).read_text())
print(cfg.get("deploy", {}).get("project_slug", "sample-app"))
PY
)/status"
