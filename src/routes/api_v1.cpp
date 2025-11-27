#include "api/v1/apps/deploy.cpp"



void registerAPIv1Routes(Server& app, ApiContext& ctx) {

    registerAPIv1AppsDeployRoute(app, ctx);


}