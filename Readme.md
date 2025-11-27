# Core Resources
App – a logical project (e.g. "my-aether-app")
Release – a specific image + config version for an app
Service – one process type (e.g. "api", "renderer", "email_worker") inside a release
Instance – one running container for a service
Domain – a host name mapped to a service, e.g. api.myapp.aether.run

IDs:
app_id – opaque string like "app_01H..."
release_id – "rel_01H..."
service_id – "svc_01H..."
instance_id – "ins_01H..."




# routes

POST /v1/apps/deploy
    Creates/updates an App, new Release, and configures Services based on aether-cloud.toml.

    Request Body:
    {
        "project_slug": "my-aether-app",
        "image": "registry.example.com/my-aether-app:abc123",
        "config_version": "b6c3d2e...",          // hash of aether-cloud.toml
        "cloud_config": {                         // raw parsed TOML -> JSON
            "version": "0.1",
            "project": {
            "name": "my-aether-app",
            "env": "production",
            "region": "us-east-1"
            },
            "services": [
            {
                "name": "api",
                "kind": "web_stateless",
                "mode": "always_on",
                "port": 8080,
                "autoscaling": {
                "min_replicas": 1,
                "max_replicas": 10,
                "target_cpu_utilization": 0.65
                },
                "env": {
                "AETHER_ENV": "production"
                }
            }
            ],
            "jobs": [],
            "crons": []
        }
        }

    {
  "app": {
    "id": "app_01HXYZ...",
    "slug": "my-aether-app",
    "default_domain": "my-aether-app.aether.run",
    "created_at": "2025-11-26T15:00:00Z"
  },
  "release": {
    "id": "rel_01HABC...",
    "image": "registry.example.com/my-aether-app:abc123",
    "config_version": "b6c3d2e...",
    "created_at": "2025-11-26T15:00:00Z",
    "status": "deploying"        // deploying | ready | failed
  },
  "services": [
    {
      "id": "svc_01HAPI...",
      "name": "api",
      "kind": "web_stateless",
      "mode": "always_on",
      "port": 8080,
      "desired_replicas": 1,
      "min_replicas": 1,
      "max_replicas": 10,
      "state": "scaling"         // scaling | ready | error
    }
  ]
}


