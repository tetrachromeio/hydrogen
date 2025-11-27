


void bootLoader() {
    CoreStatus::set(CoreStatusCode::MAINTENANCE);


    // Load DB config and check health
    auto cfg = DBHealth::loadConfig();
    DBHealth::checkDatabaseHealth(cfg);




    CoreStatus::set(CoreStatusCode::OK);// once everything is passed

}