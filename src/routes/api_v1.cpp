#include "api/v1/upload.cpp"
#include "api/v1/upload_assets.cpp"
#include "api/v1/apps/deploy.cpp"
#include "api/v1/apps/get.cpp"

void registerAPIv1Routes(Server& app, ApiContext& ctx) {
    registerAPIv1UploadRoute(app);
    registerAPIv1UploadAssetsRoute(app);
    registerAPIv1AppGetRoute(app, ctx);
    registerAPIv1AppsDeployRoute(app, ctx);
}
