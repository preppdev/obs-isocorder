/*
 * Isolated Record - OBS Studio plugin module entry point.
 */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "isolated-record.hpp"
#include "recorder-api.hpp"
#include "audio-recorder.hpp"
#include "dock.hpp"
#include "version.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("isolated-record", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Record individual OBS sources to their own separate files.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Isolated Record";
}

bool obs_module_load(void)
{
	obs_register_source(&isolated_record_filter_info);

	ir::load_settings();
	air::load();

	/* Add the mission-control dock. Created on the UI thread during startup;
	 * the frontend takes ownership of the widget. */
	auto *dock = new IsolatedRecordDock();
	obs_frontend_add_dock_by_id("isolated_record_dock", obs_module_text("IsolatedRecord"), dock);

	blog(LOG_INFO, "[isolated-record] loaded version %s", PROJECT_VERSION);
	return true;
}

void obs_module_unload(void)
{
	air::shutdown_all();
	blog(LOG_INFO, "[isolated-record] unloaded");
}
