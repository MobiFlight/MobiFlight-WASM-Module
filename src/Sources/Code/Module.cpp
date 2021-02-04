#include <MSFS\MSFS.h>
#include <MSFS\MSFS_WindowsTypes.h>
#include <SimConnect.h>
#include <MSFS\Legacy\gauges.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include "Module.h"

HANDLE g_hSimConnect;
const char* version = "0.2.50			";
const char* CustomEventPrefix = "MobiFlight.";

std::vector<std::string> Events;

enum MOBIFLIGHT_GROUP
{
	DEFAULT
};

enum eEvents
{
	EVENT_FLIGHT_LOADED
};

void CALLBACK MyDispatchProc(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext);

void LoadEventDefinitions() {
	std::ifstream file("modules/events.txt");
	std::string line;

	while (std::getline(file, line)) {
		if (line.find("//") != std::string::npos) continue;
		Events.push_back(line);
	}

	file.close();
}

void RegisterEvents() {
	DWORD eventID = 0;
	for (const auto& value : Events) {

		std::string eventCommand = value;
		std::string eventName = std::string(CustomEventPrefix) + eventCommand;

		HRESULT hr = SimConnect_MapClientEventToSimEvent(g_hSimConnect, eventID, eventName.c_str());
		hr = SimConnect_AddClientEventToNotificationGroup(g_hSimConnect, MOBIFLIGHT_GROUP::DEFAULT, eventID, false);

#if DEBUG
		if (hr != S_OK) fprintf(stderr, "MobiFlight: Error on registering Event %s with ID %u for command (>H:%s)", eventName.c_str(), eventID, eventCommand.c_str());
		else fprintf(stderr, "MobiFlight: Success on registering Event %s with ID %u for command (>H:%s)", eventName.c_str(), eventID, eventCommand.c_str());
#endif
		eventID++;
	}

	SimConnect_SetNotificationGroupPriority(g_hSimConnect, MOBIFLIGHT_GROUP::DEFAULT, SIMCONNECT_GROUP_PRIORITY_HIGHEST);
}

extern "C" MSFS_CALLBACK void module_init(void)
{
	// load defintions
	LoadEventDefinitions();
	
	g_hSimConnect = 0;
	HRESULT hr = SimConnect_Open(&g_hSimConnect, "Standalone Module", (HWND) NULL, 0, 0, 0);
	if (hr != S_OK)
	{
		fprintf(stderr, "Could not open SimConnect connection.\n");
		return;
	}
	hr = SimConnect_SubscribeToSystemEvent(g_hSimConnect, EVENT_FLIGHT_LOADED, "FlightLoaded");
	if (hr != S_OK)
	{
		fprintf(stderr, "Could not subscribe to \"FlightLoaded\" system event.\n");
		return;
	}
	
	RegisterEvents();

	hr = SimConnect_CallDispatch(g_hSimConnect, MyDispatchProc, NULL);
	if (hr != S_OK)
	{
		fprintf(stderr, "Could not set dispatch proc.\n");
		return;
	}

	fprintf(stderr, "MobiFlight: Module Init Complete. Version: %s", version);
	fprintf(stderr, "MobiFlight: Loaded %u event defintions.", Events.size());
}

extern "C" MSFS_CALLBACK void module_deinit(void)
{

	if (!g_hSimConnect)
		return;
	HRESULT hr = SimConnect_Close(g_hSimConnect);
	if (hr != S_OK)
	{
		fprintf(stderr, "Could not close SimConnect connection.\n");
		return;
	}

}

void CALLBACK MyDispatchProc(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext)
{
	switch (pData->dwID)
	{
	case SIMCONNECT_RECV_ID_EVENT: {
		SIMCONNECT_RECV_EVENT* evt = (SIMCONNECT_RECV_EVENT*)pData;
		int eventID = evt->uEventID;

		if (eventID >= Events.size()) {
			fprintf(stderr, "MobiFlight: OOF! - EventID out of range:%u\n", eventID);
			break;
		}

		std::string command = std::string("(>H:") + std::string(Events[eventID]) + std::string(")");
		execute_calculator_code(command.c_str(), nullptr, nullptr, nullptr);
		fprintf(stderr, "%s\n", command.c_str());
		
		break;
	}
	default:
		break;
	}
}
