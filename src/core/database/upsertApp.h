

/*
 std::string project_slug  = body.at("project_slug").get<std::string>();
            std::string image         = body.at("image").get<std::string>();
            std::string config_hash   = body.at("config_version").get<std::string>();
            json        cloud_config  = body.at("cloud_config");

            // TODO: 2. Upsert App in DB (apps table)
            // auto appModel = ctx.appRepo.upsertBySlug(project_slug, ...);
            // 
*/

/*
[model]
name = "App"
table = "apps"

[fields.id]
type = "string"
primary_key = true

[fields.slug]
type = "string"
unique = true
nullable = false

[fields.default_domain]
type = "string"
nullable = true

[fields.current_release_id]
type = "string"
nullable = true
foreign_key = "Releases.id"

[fields.created_at]
type = "datetime"
default = "now"

[fields.updated_at]
type = "datetime"
default = "now"
*/