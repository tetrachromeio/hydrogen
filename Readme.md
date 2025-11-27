# Hydrogen

Lightweight control-plane experiment for running uploaded binaries and exposing them via interface proxy routes.

## What’s here
- Control server on port 3000 (health/status + API).
- `/api/v1/upload` stores binaries under `/uploads`.
- `/api/v1/app/deploy` upserts app/release/service records, spawns instances locally with `PORT` assigned by the backend, and tracks readiness.
- Interface proxy at `/interface/<slug>/...` that fetches from a ready instance and streams the response without changing the browser URL.
- Helper script `test/test_deploy/t4_deploy.sh` to upload + deploy using `test/test_deploy/config.toml`.

## Quick start
```bash
aeon build
./build/main   # runs control plane on :3000
```

## Deploying a binary
1) Ensure your binary listens on the `PORT` environment variable.
2) Put your test binary at `test/test_deploy/main` (or adjust `config.toml`).
3) Run:
   ```bash
   ./test/test_deploy/t4_deploy.sh
   ```
   - Uploads to `/api/v1/upload`
   - Deploys via `/api/v1/app/deploy`
   - Prints the interface URL at the end
4) Control-plane logs print instance ports: `[deploy] instance ... listening at 127.0.0.1:<port>`.

## Accessing the app
- Proxy (URL stays on control plane):
  - Root: `http://localhost:3000/interface/<project_slug>/`
  - Subpaths: `http://localhost:3000/interface/<project_slug>/<path>`
  - Proxy selects a ready instance for the app’s current release; if none reachable, returns `no running instance`.
- Direct (debug): `http://127.0.0.1:<port>` from the deploy logs.

## Notes / Troubleshooting
- If proxy returns `no running instance`, check `/tmp/<instanceId>.log` and ensure the binary binds to `PORT`.
- Control plane uses port 3000; instances get random free ports. Avoid hardcoded ports in your app.
- Proxy only uses `state='ready'` instances tied to the app’s `current_release_id`.



next.. set up cold start... so on a service with cold start when url is called we start a service on a port fetch, load the response then close it