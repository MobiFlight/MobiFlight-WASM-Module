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
const char* version = "0.3.11";
const char* MobiFlightEventPrefix = "MobiFlight.";
const char* FileEventsMobiFlight = "modules/events.txt";
const char* FileEventsUser = "modules/events.user.txt";

std::vector<std::pair<std::string, std::string>> CodeEvents;

const SIMCONNECT_CLIENT_DATA_ID MOBIFLIGHT_CLIENT_DATA_ID_SIMVAR = 0;
const SIMCONNECT_CLIENT_DATA_ID MOBIFLIGHT_CLIENT_DATA_ID_COMMAND = 1;
const SIMCONNECT_CLIENT_DATA_ID MOBIFLIGHT_CLIENT_DATA_ID_RESPONSE = 2;

const SIMCONNECT_CLIENT_DATA_DEFINITION_ID MOBIFLIGHT_DATA_DEFINITION_ID_STRING_RESPONSE = 0;
const SIMCONNECT_CLIENT_DATA_DEFINITION_ID MOBIFLIGHT_DATA_DEFINITION_ID_STRING_COMMAND = 1;

uint16_t MaxClientDataDefinition = 0;

//uint32_t SimVarIdCounter = 1;
uint16_t SimVarOffset = 1000;

struct SimVar {
	int ID;
	int Offset;
	std::string Name;
	float Value;
};

std::vector<SimVar> SimVars;

struct StringValue {
	char value[255];
};

enum MOBIFLIGHT_GROUP
{
	DEFAULT
};

enum eEvents
{
	EVENT_FLIGHT_LOADED,
	EVENT_FRAME
};

void CALLBACK MyDispatchProc(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext);

std::pair<std::string, std::string> splitIntoPair(std::string value, char delimiter) {
	auto index = value.find(delimiter);
	std::pair<std::string, std::string> result;
	if (index != std::string::npos) {

		// Split around ':' character
		result = std::make_pair(
			value.substr(0, index),
			value.substr(index + 1)
		);

		// Trim any leading ' ' in the value part
		// (you may wish to add further conditions, such as '\t')
		while (!result.second.empty() && result.second.front() == ' ') {
			result.second.erase(0, 1);
		}
	}
	else {
		// Split around ':' character
		result = std::make_pair(
			value,
			std::string("(>H:" + value + ")")
		);
	}

	return result;
}

void LoadEventDefinitions(const char * fileName) {
	std::ifstream file(fileName);
	std::string line;

	while (std::getline(file, line)) {
		if (line.find("//") != std::string::npos) continue;

		std::pair<std::string, std::string> codeEvent = splitIntoPair(line, '#');
		CodeEvents.push_back(codeEvent);
	}

	file.close();
}

void RegisterEvents() {
	DWORD eventID = 0;

	for (const auto& value : CodeEvents) {
		std::string eventCommand = value.second;
		std::string eventName = std::string(MobiFlightEventPrefix) + value.first;

		HRESULT hr = SimConnect_MapClientEventToSimEvent(g_hSimConnect, eventID, eventName.c_str());
		hr = SimConnect_AddClientEventToNotificationGroup(g_hSimConnect, MOBIFLIGHT_GROUP::DEFAULT, eventID, false);

#if _DEBUG
		if (hr != S_OK) fprintf(stderr, "MobiFlight: Error on registering Event %s with ID %u for code %s", eventName.c_str(), eventID, eventCommand.c_str());
		else fprintf(stderr, "MobiFlight: Success on registering Event %s with ID %u for code %s", eventName.c_str(), eventID, eventCommand.c_str());
#endif

		eventID++;
	}

	SimConnect_SetNotificationGroupPriority(g_hSimConnect, MOBIFLIGHT_GROUP::DEFAULT, SIMCONNECT_GROUP_PRIORITY_HIGHEST);
}

void ListLVars() {
	int lVarId = 0;

	std::vector<std::string> lVarList;
	for (int i = 0; i != 1000; i++) {
		const char * lVarName = get_name_of_named_variable(i);
		if (lVarName == NULLPTR) break;

		lVarList.push_back(std::string(lVarName));
	}

	std::sort(lVarList.begin(), lVarList.end());

	for (const auto& lVar : lVarList) {
		StringValue buffer;
		strcpy(&buffer.value[0], lVar.c_str());
		
		SimConnect_SetClientData(
			g_hSimConnect,
			MOBIFLIGHT_CLIENT_DATA_ID_RESPONSE,
			MOBIFLIGHT_DATA_DEFINITION_ID_STRING_RESPONSE,
			SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
			0,
			sizeof(StringValue),
			&buffer.value[0]
		);
		fprintf(stderr, "MobiFlight: Available LVar > %s", buffer.value);
	}
}

void RegisterSimVar(const std::string code) {
	SimVar var1;
	var1.Name = code;
	var1.ID = SimVars.size() + SimVarOffset;
	var1.Offset = (SimVars.size()) * sizeof(float);

	SimVars.push_back(var1);
	HRESULT hr;

	if (MaxClientDataDefinition < SimVars.size())
	{
		hr = SimConnect_AddToClientDataDefinition(
			g_hSimConnect,
			var1.ID,
			var1.Offset,
			sizeof(float),
			0
		);

		MaxClientDataDefinition = SimVars.size();
	}

	FLOAT64 val;
	execute_calculator_code(std::string(code).c_str(), &val, NULL, NULL);

	hr = SimConnect_SetClientData(
		g_hSimConnect,
		MOBIFLIGHT_CLIENT_DATA_ID_SIMVAR,
		var1.ID,
		SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
		0,
		sizeof(var1.Value),
		&var1.Value
	);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on Setting Client Data. %u, SimVar: %s (ID: %u)", hr, var1.Name.c_str(), var1.ID);
	}

	var1.Value = val;

	hr = SimConnect_SetClientData(
		g_hSimConnect,
		MOBIFLIGHT_CLIENT_DATA_ID_SIMVAR,
		var1.ID,
		SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
		0,
		sizeof(var1.Value),
		&var1.Value
	);

	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on Setting Client Data. %u, SimVar: %s (ID: %u)", hr, var1.Name.c_str(), var1.ID);
	}

	fprintf(stderr, "MobiFlight: RegisterSimVar > %s ID [%u] : Offset(%u) : Value(%f)", var1.Name.c_str(), var1.ID, var1.Offset, var1.Value);
}

void ClearSimVars() {
	/*
	for (auto& value : SimVars) {
		HRESULT hr = SimConnect_ClearClientDataDefinition(
			g_hSimConnect,
			value.ID
		);
		if (hr != S_OK) {
			fprintf(stderr, "MobiFlight: Error on Clearing Client Data Definition. %u, SimVar: %s (ID: %u)", hr, value.Name.c_str(), value.ID);
		}
	}
	*/
	SimVars.clear();

	fprintf(stderr, "MobiFlight: Cleared SimVar tracking.");
}

void ReadSimVar(SimVar &simVar) {
	FLOAT64 val = 0;
	execute_calculator_code(std::string(simVar.Name).c_str(), &val, NULL, NULL);
	if (simVar.Value == val) return;

	simVar.Value = val;

	SimConnect_SetClientData(
		g_hSimConnect,
		MOBIFLIGHT_CLIENT_DATA_ID_SIMVAR,
		simVar.ID,
		SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
		0,
		sizeof(simVar.Value),
		&simVar.Value
	);
#if _DEBUG
	fprintf(stderr, "MobiFlight: SimVar %s with ID %u has value %f", simVar.Name.c_str(), simVar.ID, simVar.Value);
#endif
}

void ReadSimVars() {
	for (auto& value : SimVars) {
		ReadSimVar(value);
	}
}

void RegisterClientDataArea() {
	HRESULT hr = SimConnect_MapClientDataNameToID(g_hSimConnect, "MobiFlight.LVars", MOBIFLIGHT_CLIENT_DATA_ID_SIMVAR);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %u", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, MOBIFLIGHT_CLIENT_DATA_ID_SIMVAR, 4096, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	hr = SimConnect_MapClientDataNameToID(g_hSimConnect, "MobiFlight.Response", MOBIFLIGHT_CLIENT_DATA_ID_RESPONSE);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %u", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, MOBIFLIGHT_CLIENT_DATA_ID_RESPONSE, 256, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	hr = SimConnect_MapClientDataNameToID(g_hSimConnect, "MobiFlight.Command", MOBIFLIGHT_CLIENT_DATA_ID_COMMAND);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %u", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, MOBIFLIGHT_CLIENT_DATA_ID_COMMAND, 256, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	hr = SimConnect_AddToClientDataDefinition(
		g_hSimConnect,
		MOBIFLIGHT_DATA_DEFINITION_ID_STRING_RESPONSE,
		0,
		256,
		0
	);

	hr = SimConnect_AddToClientDataDefinition(
		g_hSimConnect,
		MOBIFLIGHT_DATA_DEFINITION_ID_STRING_COMMAND,
		0,
		256,
		0
	);

	SimConnect_RequestClientData(g_hSimConnect,
		MOBIFLIGHT_CLIENT_DATA_ID_COMMAND,
		0,
		MOBIFLIGHT_DATA_DEFINITION_ID_STRING_COMMAND,
		SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET,
		SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_CHANGED,
		0,
		0,
		0);
}

extern "C" MSFS_CALLBACK void module_init(void)
{
	// load defintions
	LoadEventDefinitions(FileEventsMobiFlight);
	int eventDefinition = CodeEvents.size();
	LoadEventDefinitions(FileEventsUser);
	
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

	hr = SimConnect_SubscribeToSystemEvent(g_hSimConnect, EVENT_FRAME, "Frame");
	if (hr != S_OK)
	{
		fprintf(stderr, "Could not subscribe to \"Frame\" system event.\n");
		return;
	}

	hr = SimConnect_CallDispatch(g_hSimConnect, MyDispatchProc, NULL);
	if (hr != S_OK)
	{
		fprintf(stderr, "Could not set dispatch proc.\n");
		return;
	}

	RegisterClientDataArea();
	RegisterEvents();
	ListLVars();
	fprintf(stderr, "MobiFlight: Module Init Complete. Version: %s", version);
	fprintf(stderr, "MobiFlight: Loaded %u event defintions in total.", CodeEvents.size());
	fprintf(stderr, "MobiFlight: Loaded %u built-in event defintions.", eventDefinition);
	fprintf(stderr, "MobiFlight: Loaded %u user event defintions.", CodeEvents.size() - eventDefinition);
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
		case SIMCONNECT_RECV_ID_EVENT_FILENAME: {
			SIMCONNECT_RECV_EVENT_FILENAME* evt = (SIMCONNECT_RECV_EVENT_FILENAME*)pData;
			int eventID = evt->uEventID;

			
			break;
		}

		case SIMCONNECT_RECV_ID_CLIENT_DATA: {
			auto recv_data = static_cast<SIMCONNECT_RECV_CLIENT_DATA*>(pData);
			std::string str = std::string((char*)(&recv_data->dwData));
			fprintf(stderr, "MobiFlight: Received Command: %s\n", str.c_str());


			if (str == "MF.SimVars.Clear") {
				ClearSimVars();
				break;

			} else if (str == "MF.LVars.List") {
				ListLVars();
				break;

			}

			std::shared_ptr<std::string> m_str = std::make_shared<std::string>(str);
			
			if (m_str.get()->find("MF.SimVars.Add.") != std::string::npos) {
				str = m_str.get()->substr(
					std::string("MF.SimVars.Add.").length()
				);
				RegisterSimVar(str);

				fprintf(stderr, "MobiFlight: Received SimVar to register: %s.\n", str.c_str());
			}
			break;
		}

		case SIMCONNECT_RECV_ID_EVENT_FRAME: {
			SIMCONNECT_RECV_EVENT* evt = (SIMCONNECT_RECV_EVENT*)pData;
			int eventID = evt->uEventID;

			ReadSimVars();
			break;
		}

		case SIMCONNECT_RECV_ID_EVENT: {
			SIMCONNECT_RECV_EVENT* evt = (SIMCONNECT_RECV_EVENT*)pData;
			int eventID = evt->uEventID;

			if (eventID < CodeEvents.size()) {
				// We got a Code Event or a User Code Event
				 int CodeEventId = eventID;
				std::string command = std::string(CodeEvents[CodeEventId].second);
				fprintf(stderr, "execute %s\n", command.c_str());
				execute_calculator_code(command.c_str(), nullptr, nullptr, nullptr);
			} 
			else {
				fprintf(stderr, "MobiFlight: OOF! - EventID out of range:%u\n", eventID);
			}
		
			break;
		}
		default:
			break;
	}
}
