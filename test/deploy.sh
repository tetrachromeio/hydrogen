curl -i -X POST http://localhost:3000/api/v1/app/deploy \
  -H "Content-Type: application/json" \
  -d '{
    "project_slug": "my-aether-app",
    "image": "registry.example.com/my-aether-app:abc123",
    "config_version": "abcd1234",
    "cloud_config": {
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
          "port": 8080
        }
      ]
    }
  }'
