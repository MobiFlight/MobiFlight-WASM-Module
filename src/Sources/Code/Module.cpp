#include <MSFS\MSFS.h>
#include <MSFS\MSFS_WindowsTypes.h>
#include <SimConnect.h>
#include <MSFS\Legacy\gauges.h>
#include <vector>
#include <list>
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include "Module.h"

HANDLE g_hSimConnect;
const char* version = "1.0.0";

const char* ClientName = "MobiFlightWasmModule";
const char* MobiFlightEventPrefix = "MobiFlight.";
const char* FileEventsMobiFlight = "modules/events.txt";
const char* FileEventsUser = "modules/events.user.txt";

std::vector<std::pair<std::string, std::string>> CodeEvents;

const char* MOBIFLIGHT_CLIENT_DATA_NAME = "MobiFlight";
const char* CLIENT_DATA_NAME_POSTFIX_SIMVAR = ".LVars";
const char* CLIENT_DATA_NAME_POSTFIX_STRINGVAR = ".StringVars";
const char* CLIENT_DATA_NAME_POSTFIX_COMMAND = ".Command";
const char* CLIENT_DATA_NAME_POSTFIX_RESPONSE = ".Response";

const int MOBIFLIGHT_MESSAGE_SIZE = 1024;

// This is an offset for the dynamically registered SimVars
// to avoid any conflicts with base IDs
constexpr uint16_t SIMVAR_OFFSET = 1000;

// For each registered client can 10000 data definition ids are reserved
constexpr uint16_t CLIENT_DATA_DEF_ID_SIMVAR_RANGE = 10000;
constexpr uint16_t CLIENT_DATA_DEF_ID_STRINGVAR_RANGE = 10000;

// Maximum number of variables that are read from sim per frame, Default: 30
// Can be set to different value via config command
uint16_t MOBIFLIGHT_MAX_VARS_PER_FRAME = 30;

// Max length of a string variable. This will affect the maximum amount of string variables
// due to the maximum client-data-array-size (SIMCONNECT_CLIENTDATA_MAX_SIZE) of 8kB!
constexpr uint16_t MOBIFLIGHT_STRING_SIMVAR_VALUE_MAX_LEN = 128;

// data struct for dynamically registered SimVars
struct SimVar {
	int ID;
	int Offset;
	std::string Name;
	float Value;
};

struct StringSimVar {
	int ID;
	int Offset;
	std::string Name;
	std::string Value;
};

// data struct for client accessing SimVars
struct Client {
	int ID;
	std::string Name;
	std::string DataAreaNameSimVar;
	std::string DataAreaNameResponse;
	std::string DataAreaNameCommand;
	std::string DataAreaNameStringVar;
	SIMCONNECT_CLIENT_DATA_ID DataAreaIDSimvar;
	SIMCONNECT_CLIENT_DATA_ID DataAreaIDResponse;
	SIMCONNECT_CLIENT_DATA_ID DataAreaIDCommand;
	SIMCONNECT_CLIENT_DATA_ID DataAreaIDStringSimVar;
	SIMCONNECT_CLIENT_DATA_DEFINITION_ID DataDefinitionIDStringResponse;
	SIMCONNECT_CLIENT_DATA_DEFINITION_ID DataDefinitionIDStringCommand;
	std::vector<SimVar> SimVars;
	std::vector<StringSimVar> StringSimVars;

	SIMCONNECT_CLIENT_DATA_DEFINITION_ID DataDefinitionIdSimVarsStart;
	SIMCONNECT_CLIENT_DATA_DEFINITION_ID DataDefinitionIdStringVarsStart;
	// This is an optimization to be able to re-use already defined data definition IDs & request IDs
	// after resetting registered SimVars
	uint16_t MaxClientDataDefinition = 0;
	// Runtime Rolling CLient Data reading Index
	//std::vector<SimVar>::iterator RollingClientDataReadIndex;
	uint16_t RollingClientDataReadIndex;

};

// The list of currently registered clients
std::vector<Client*> RegisteredClients;

// The list of currently available LVars
std::vector<std::string> lVarList;

// Data struct to read messages coming from clients
struct StringValue {
	char value[MOBIFLIGHT_MESSAGE_SIZE];
};

// Enum for notification groups
enum MOBIFLIGHT_GROUP
{
	DEFAULT
};

// Enum for SimConnect Event Types that we are registering for
enum eEvents
{
	EVENT_FLIGHT_LOADED,
	EVENT_FRAME
};

void CALLBACK MyDispatchProc(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext);

// Helper method to split up the lines from config file
// into Pairs
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

// Read the event defitinions from file
// Providing a file with these definitions allows legacy SimConnect clients
// to trigger MobiFlight events transparently
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

// Register all Events with SimConnect that have been defined
void RegisterEvents() {
	DWORD eventID = 0;

	for (const auto& value : CodeEvents) {
		std::string eventCommand = value.second;
		std::string eventName = std::string(MobiFlightEventPrefix) + value.first;

		HRESULT hr = SimConnect_MapClientEventToSimEvent(g_hSimConnect, eventID, eventName.c_str());
		hr = SimConnect_AddClientEventToNotificationGroup(g_hSimConnect, MOBIFLIGHT_GROUP::DEFAULT, eventID, false);

#if _DEBUG
		if (hr != S_OK) {
			fprintf(stderr, "MobiFlight: Error on registering Event %s with ID %lu for code %s", eventName.c_str(), eventID, eventCommand.c_str());
		}
		else {
			std::cout << "MobiFlight: Success on registering Event " << eventName.c_str();
			std::cout << " with ID " << eventID << " for code " << eventCommand.c_str() << std::endl;
		}
#endif

		eventID++;
	}

	SimConnect_SetNotificationGroupPriority(g_hSimConnect, MOBIFLIGHT_GROUP::DEFAULT, SIMCONNECT_GROUP_PRIORITY_HIGHEST);
}

void LoadEventDefinitions() {
	CodeEvents.clear();

	LoadEventDefinitions(FileEventsMobiFlight);
	int eventDefinition = CodeEvents.size();
	LoadEventDefinitions(FileEventsUser);

	std::cout << "MobiFlight: Loaded " << CodeEvents.size() << " event definitions in total." << std::endl;
	std::cout << "MobiFlight: Loaded " << eventDefinition << " built-in event definitions." << std::endl;
	std::cout << "MobiFlight: Loaded " << CodeEvents.size() - eventDefinition << " user event definitions." << std::endl;
}

void SendResponse(const char * message, Client* client) {
	SimConnect_SetClientData(
		g_hSimConnect,
		client->DataAreaIDResponse,
		client->DataDefinitionIDStringResponse,
		SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
		0,
		MOBIFLIGHT_MESSAGE_SIZE,
		(void*) message
	);
}

// Sends information that new client data areas are created.
void SendNewClientResponse(Client* client, Client* nc) {
	std::ostringstream oss;
	oss << "MF.Clients.Add." << nc->Name << ".Finished";
	std::string data = oss.str();
	std::cout << "MobiFlight[" << client->Name.c_str() << "]: SendNewClientData > " << data.c_str() << std::endl;
	SendResponse(data.c_str(), client);
}

// List all available LVars for the currently loaded flight
// and send them to the SimConnect client
void ListLVars(Client* client) {
	lVarList.clear();

	for (int i = 0; i != 1000; i++) {
		const char * lVarName = get_name_of_named_variable(i);
		if (lVarName == NULLPTR) break;
		lVarList.push_back(std::string(lVarName));
	}

	std::sort(lVarList.begin(), lVarList.end());

	for (const auto& lVar : lVarList) {
		SendResponse(lVar.c_str(), client);
#if _DEBUG
		std::cout << "MobiFlight[" << client->Name.c_str() << "]: Available LVar > " << lVar.c_str() << std::endl;
#endif
	}
}

// Overloaded write function for string SimVars
void WriteSimVar(StringSimVar& simVar, Client* client)
{
	HRESULT hr = SimConnect_SetClientData(
		g_hSimConnect,
		client->DataAreaIDStringSimVar,
		simVar.ID,
		SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
		0,
		MOBIFLIGHT_STRING_SIMVAR_VALUE_MAX_LEN,
		(void*)simVar.Value.c_str()
	);

	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight[%s]: Error on Setting String Client Data. %lu, SimVar: %s (String-ID: %ul)\n", client->Name.c_str(), hr, simVar.Name.c_str(), simVar.ID);
	}
#if _DEBUG
	std::cout << "MobiFlight[" << client->Name.c_str() << "]: Written String-SimVar " << simVar.Name.c_str();
	std::cout << " with String-ID " << simVar.ID << " has value " << simVar.Value.c_str() << std::endl;
#endif
}

// Overloaded write function for float SimVars
void WriteSimVar(SimVar& simVar, Client* client) {
	HRESULT hr = SimConnect_SetClientData(
		g_hSimConnect,
		client->DataAreaIDSimvar,
		simVar.ID,
		SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
		0,
		sizeof(simVar.Value),
		&simVar.Value
	);

		if (hr != S_OK) {
			fprintf(stderr, "MobiFlight[%s]: Error on Setting Client Data. %lu, SimVar: %s (ID: %u)", client->Name.c_str(), hr, simVar.Name.c_str(), simVar.ID);
		}
#if _DEBUG
	std::cout << "MobiFlight[" << client->Name.c_str() << "]: Written SimVar " << simVar.Name.c_str();
	std::cout << " with ID " << simVar.ID << " has value " << simVar.Value << std::endl;
#endif
}

// Register a single Float-SimVar and send the current value to SimConnect Clients
void RegisterFloatSimVar(const std::string code, Client* client) {
	std::vector<SimVar>* SimVars = &(client->SimVars);
	std::vector<StringSimVar>* StringSimVars = &(client->StringSimVars);
	SimVar newSimVar;
	HRESULT hr;

	newSimVar.Name = code;
	newSimVar.ID = SimVars->size() + client->DataDefinitionIdSimVarsStart;
	newSimVar.Offset = SimVars->size() * (sizeof(float));
	newSimVar.Value = 0.0F;
	SimVars->push_back(newSimVar);

	if (client->MaxClientDataDefinition < (SimVars->size() + StringSimVars->size())) {
		hr = SimConnect_AddToClientDataDefinition(
			g_hSimConnect,
			newSimVar.ID,
			newSimVar.Offset,
			sizeof(float),
			0
		);

		if (hr != S_OK) {
			fprintf(stderr, "MobiFlight[%s]: Error on adding Client Data \"%s\" with ID: %u, Offset: %u and Size: %lu\n", client->Name.c_str(), newSimVar.Name.c_str(), newSimVar.ID, newSimVar.Offset, sizeof(float));
		}
#if _DEBUG
		else {
			std::cout << "MobiFlight[" << client->Name.c_str() << "]: Added SimVar > " << newSimVar.Name.c_str();
			std::cout << " with ID: " << newSimVar.ID << ", Offset: " << newSimVar.Offset << " and Size: " << sizeof(float) << std::endl;
		}
		std::cout << "MobiFlight[" << client->Name.c_str() << "]: RegisterFloatSimVar SimVars Size: " << SimVars->size() << std::endl;
#endif
		client->MaxClientDataDefinition = (SimVars->size() + StringSimVars->size());
	}

	FLOAT64 floatVal = 0;
	execute_calculator_code(std::string(code).c_str(), &floatVal, nullptr, nullptr);
	newSimVar.Value = floatVal;
	WriteSimVar(newSimVar, client);
#if _DEBUG
	std::cout << "MobiFlight[" << client->Name.c_str() << "]: RegisterFloatSimVar > " << newSimVar.Name.c_str();
	std::cout << " ID [" << newSimVar.ID << "] : Offset(" << newSimVar.Offset << ") : Value(" << newSimVar.Value << ")"  << std::endl;
#endif
}

// Register a single String-SimVar and send the current value to SimConnect Clients
void RegisterStringSimVar(const std::string code, Client* client) {
	std::vector<SimVar>* SimVars = &(client->SimVars);
	std::vector<StringSimVar>* StringSimVars = &(client->StringSimVars);
	StringSimVar newStringSimVar;
	HRESULT hr;

	newStringSimVar.Name = code;
	newStringSimVar.ID = StringSimVars->size() + client->DataDefinitionIdStringVarsStart;
	newStringSimVar.Offset = StringSimVars->size() * MOBIFLIGHT_STRING_SIMVAR_VALUE_MAX_LEN;
	newStringSimVar.Value.empty();
	StringSimVars->push_back(newStringSimVar);

	if (client->MaxClientDataDefinition < (SimVars->size() + StringSimVars->size())) {
		hr = SimConnect_AddToClientDataDefinition(
			g_hSimConnect,
			newStringSimVar.ID,
			newStringSimVar.Offset,
			MOBIFLIGHT_STRING_SIMVAR_VALUE_MAX_LEN,
			0
		);

		if (hr != S_OK) {
			fprintf(stderr, "MobiFlight[%s]: Error on adding Client Data \"%s\" with String-ID: %u, String-Offset: %u and Size: %u\n", client->Name.c_str(), newStringSimVar.Name.c_str(), newStringSimVar.ID, newStringSimVar.Offset, MOBIFLIGHT_STRING_SIMVAR_VALUE_MAX_LEN);
		}
#if _DEBUG
		else {
			std::cout << "MobiFlight[" << client->Name.c_str() << "]: Added String-SimVar > " << newStringSimVar.Name.c_str();
			std::cout << " with String-ID: " << newStringSimVar.ID << ", String-Offset: " << newStringSimVar.Offset << " and Size: " << MOBIFLIGHT_STRING_SIMVAR_VALUE_MAX_LEN << std::endl;
		}
		std::cout << "MobiFlight[" << client->Name.c_str() << "]: RegisterStringSimVar StringSimVars Size: " << StringSimVars->size() << std::endl;
#endif
		client->MaxClientDataDefinition = (SimVars->size() + StringSimVars->size());
	}

	PCSTRINGZ charVal = nullptr;
	execute_calculator_code(std::string(code).c_str(), nullptr, nullptr, &charVal);
	newStringSimVar.Value = std::string(charVal, strnlen(charVal, MOBIFLIGHT_STRING_SIMVAR_VALUE_MAX_LEN));
	WriteSimVar(newStringSimVar, client);
#if _DEBUG
	std::cout << "MobiFlight[" << client->Name.c_str() << "]: RegisterStringSimVar > " << newStringSimVar.Name.c_str();
	std::cout << " ID [" << newStringSimVar.ID << "] : Offset(" << newStringSimVar.Offset << ") : Value(" << newStringSimVar.Value << ")"  << std::endl;
#endif
}

// Clear the list of currently tracked SimVars
void ClearSimVars(Client* client) {
	client->SimVars.clear();
	client->StringSimVars.clear();
	std::cout << "MobiFlight[" << client->Name.c_str() << "]: Cleared SimVar tracking." << std::endl;
	//client->RollingClientDataReadIndex = client->SimVars.begin();
	client->RollingClientDataReadIndex = 0;
}

// Read a single SimVar and send the current value to SimConnect Clients (overloaded for float SimVars)
void ReadSimVar(SimVar &simVar, Client* client) {
	FLOAT64 floatVal = 0;

	execute_calculator_code(std::string(simVar.Name).c_str(), &floatVal, nullptr, nullptr);

	if (simVar.Value == floatVal) return;
	simVar.Value = floatVal;

	WriteSimVar(simVar, client);

#if _DEBUG
	std::cout << "MobiFlight[" << client->Name.c_str() << "]: SimVar " << simVar.Name.c_str();
	std::cout << " with ID " << simVar.ID << " has value " << simVar.Value << std::endl;
#endif
}

// Read a single SimVar and send the current value to SimConnect Clients (overloaded for string SimVars)
void ReadSimVar(StringSimVar &simVar, Client* client) {
	PCSTRINGZ charVal = nullptr;

	execute_calculator_code(std::string(simVar.Name).c_str(), nullptr, nullptr, &charVal);
	std::string stringVal = std::string(charVal, strnlen(charVal, MOBIFLIGHT_STRING_SIMVAR_VALUE_MAX_LEN));
	if (simVar.Value == stringVal) return;
	simVar.Value = stringVal;

	WriteSimVar(simVar, client);

#if _DEBUG
	std::cout << "MobiFlight[" << client->Name.c_str() << "]: StringSimVar " << simVar.Name.c_str();
	std::cout << " with ID " << simVar.ID << " has value " << simVar.Value << std::endl;
#endif
}

// Read all dynamically registered SimVars
void ReadSimVars() {
	for (auto& client : RegisteredClients) {
		std::vector<SimVar>* SimVars = &(client->SimVars);
		std::vector<StringSimVar>* StringSimVars = &(client->StringSimVars);

		int totalSimVars = SimVars->size() + StringSimVars->size();
		int maxVarsPerFrame = (totalSimVars < MOBIFLIGHT_MAX_VARS_PER_FRAME) ? totalSimVars : MOBIFLIGHT_MAX_VARS_PER_FRAME;

		for (int i=0; i < maxVarsPerFrame; ++i) {
			if(client->RollingClientDataReadIndex < SimVars->size() ) {
				ReadSimVar(SimVars->at(client->RollingClientDataReadIndex), client);
			}
			else {
				ReadSimVar(StringSimVars->at(client->RollingClientDataReadIndex - SimVars->size()), client);
			}
			client->RollingClientDataReadIndex++;
			if (client->RollingClientDataReadIndex >= totalSimVars)
				client->RollingClientDataReadIndex = 0;
		}
	}
}

// Basic initialization of all required data areas
// "ClientName.LVars" -> All LVars are updated here, and all variables are floats
// "ClientName.Response" -> All responses are provided back to clients, the data is string with max length 255
// "ClientName.Command" -> SimConnect clients can send Commands via this data area
void RegisterClientDataArea(Client* client) {
	HRESULT hr = SimConnect_MapClientDataNameToID(g_hSimConnect, client->DataAreaNameSimVar.c_str(), client->DataAreaIDSimvar);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %lu", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, client->DataAreaIDSimvar, 4096, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	hr = SimConnect_MapClientDataNameToID(g_hSimConnect, client->DataAreaNameResponse.c_str(), client->DataAreaIDResponse);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %lu", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, client->DataAreaIDResponse, MOBIFLIGHT_MESSAGE_SIZE, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	hr = SimConnect_MapClientDataNameToID(g_hSimConnect, client->DataAreaNameCommand.c_str(), client->DataAreaIDCommand);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %lu", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, client->DataAreaIDCommand, MOBIFLIGHT_MESSAGE_SIZE, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	hr = SimConnect_MapClientDataNameToID(g_hSimConnect, client->DataAreaNameStringVar.c_str(), client->DataAreaIDStringSimVar);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %lu", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, client->DataAreaIDStringSimVar, SIMCONNECT_CLIENTDATA_MAX_SIZE, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	DWORD dataAreaOffset = 0;
	hr = SimConnect_AddToClientDataDefinition(
		g_hSimConnect,
		client->DataDefinitionIDStringResponse,
		dataAreaOffset,
		MOBIFLIGHT_MESSAGE_SIZE,
		0
	);

	hr = SimConnect_AddToClientDataDefinition(
		g_hSimConnect,
		client->DataDefinitionIDStringCommand,
		dataAreaOffset,
		MOBIFLIGHT_MESSAGE_SIZE,
		0
	);

	SimConnect_RequestClientData(g_hSimConnect,
		client->DataAreaIDCommand,
		client->ID, //RequestID
		client->DataDefinitionIDStringCommand,
		SIMCONNECT_CLIENT_DATA_PERIOD_ON_SET,
		SIMCONNECT_CLIENT_DATA_REQUEST_FLAG_CHANGED,
		0,
		0,
		0);
}

// Register new client and initialize data areas if necessary
Client* RegisterNewClient(const std::string clientName) {
	Client* newClient;
	bool clientFound = false;

	// Check if client already exists, for example due to client reconnect
	for (auto& client : RegisteredClients) {
		if (client->Name == clientName) {
			newClient = client;
			clientFound = true;
		}
	}

	if (!clientFound) {
		newClient = new Client();
		newClient->Name = clientName;
		newClient->ID = RegisteredClients.size();
		newClient->DataAreaIDSimvar = 4 * newClient->ID;
		newClient->DataAreaIDCommand = newClient->DataAreaIDSimvar + 1;
		newClient->DataAreaIDResponse = newClient->DataAreaIDCommand + 1;
		newClient->DataAreaIDStringSimVar = newClient->DataAreaIDResponse + 1;
		newClient->DataAreaNameSimVar = newClient->Name + std::string(CLIENT_DATA_NAME_POSTFIX_SIMVAR);
		newClient->DataAreaNameResponse = newClient->Name + std::string(CLIENT_DATA_NAME_POSTFIX_RESPONSE);
		newClient->DataAreaNameCommand = newClient->Name + std::string(CLIENT_DATA_NAME_POSTFIX_COMMAND);
		newClient->DataAreaNameStringVar = newClient->Name + std::string(CLIENT_DATA_NAME_POSTFIX_STRINGVAR);
		newClient->DataDefinitionIDStringResponse = 2 * newClient->ID; // 500 Clients possible until offset 1000 is reached
		newClient->DataDefinitionIDStringCommand = newClient->DataDefinitionIDStringResponse + 1;
		newClient->SimVars = std::vector<SimVar>();
		newClient->StringSimVars = std::vector<StringSimVar>();
		//newClient->RollingClientDataReadIndex = newClient->SimVars.begin();
		newClient->RollingClientDataReadIndex = 0;
		newClient->DataDefinitionIdSimVarsStart = SIMVAR_OFFSET + (newClient->ID * (CLIENT_DATA_DEF_ID_SIMVAR_RANGE + CLIENT_DATA_DEF_ID_STRINGVAR_RANGE));
		newClient->DataDefinitionIdStringVarsStart = newClient->DataDefinitionIdSimVarsStart + CLIENT_DATA_DEF_ID_SIMVAR_RANGE;

		RegisteredClients.push_back(newClient);

		// Create the new client data areas
		RegisterClientDataArea(newClient);
	}

#if _DEBUG
		std::cout << "MobiFlight: NewClient Name: " << newClient->Name.c_str() << std::endl;
		std::cout << "MobiFlight: NewClient ID: " << newClient->ID << std::endl;
		std::cout << "MobiFlight: NewClient DataAreaIDSimvar: " << newClient->DataAreaIDSimvar << std::endl;
		std::cout << "MobiFlight: NewClient DataAreaIDResponse: " << newClient->DataAreaIDResponse << std::endl;
		std::cout << "MobiFlight: NewClient DataAreaIDCommand: " << newClient->DataAreaIDCommand << std::endl;
		std::cout << "MobiFlight: NewClient DataAreaIDStringSimVar: " << newClient->DataAreaIDStringSimVar << std::endl;
		std::cout << "MobiFlight: NewClient DataAreaNameSimVar: " << newClient->DataAreaNameSimVar.c_str() << std::endl;
		std::cout << "MobiFlight: NewClient DataAreaNameResponse: " << newClient->DataAreaNameResponse.c_str() << std::endl;
		std::cout << "MobiFlight: NewClient DataAreaNameCommand: " << newClient->DataAreaNameCommand.c_str() << std::endl;
		std::cout << "MobiFlight: NewClient DataAreaNameStringVar: " << newClient->DataAreaNameStringVar.c_str() << std::endl;
		std::cout << "MobiFlight: NewClient DataDefinitionIDStringResponse: " << newClient->DataDefinitionIDStringResponse << std::endl;
		std::cout << "MobiFlight: NewClient DataDefinitionIDStringCommand: " << newClient->DataDefinitionIDStringCommand << std::endl;
		std::cout << "MobiFlight: NewClient DataDefinitionIdSimVarsStart: " << newClient->DataDefinitionIdSimVarsStart << std::endl;
		std::cout << "MobiFlight: NewClient DataDefinitionIdStringVarsStart: " << newClient->DataDefinitionIdStringVarsStart << std::endl;
#endif

	return newClient;
}

extern "C" MSFS_CALLBACK void module_init(void)
{
	g_hSimConnect = 0;
	HRESULT hr = SimConnect_Open(&g_hSimConnect, ClientName, (HWND) NULL, 0, 0, 0);
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

	// load defintions
	LoadEventDefinitions();
	// Register Mobiflight default client
	Client* client = RegisterNewClient(std::string(MOBIFLIGHT_CLIENT_DATA_NAME));
	RegisterEvents();
	ListLVars(client);

	std::cout << "MobiFlight: Max Message size is " << MOBIFLIGHT_MESSAGE_SIZE << std::endl;
	std::cout << "MobiFlight: Module Init Complete.Version: " << version << std::endl;
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
			break;
		}

		case SIMCONNECT_RECV_ID_CLIENT_DATA: {
			auto recv_data = static_cast<SIMCONNECT_RECV_CLIENT_DATA*>(pData);
			std::string str = std::string((char*)(&recv_data->dwData));
			DWORD clientID = (DWORD)recv_data->dwRequestID;
#if _DEBUG
				std::cout << "MobiFlight: Received Command: " << str.c_str() << std::endl;
				std::cout << "MobiFlight: Received ClientId: " << clientID << std::endl;
#endif

			Client* client = RegisteredClients[clientID];
			if (str == "MF.Ping") {
				SendResponse("MF.Pong", client);
				std::cout << "MobiFlight[" << client->Name.c_str() << "]: Received ping" << std::endl;
			}

			else if (str == "MF.SimVars.Clear") {
				ClearSimVars(client);
				break;

			}
			else if (str == "MF.LVars.List") {
				SendResponse("MF.LVars.List.Start", client);
				ListLVars(client);
				SendResponse("MF.LVars.List.End", client);
				break;

			}
			else if (str == "MF.Version.Get")
			{
				std::string v = "MF.Version." + std::string(version);
				SendResponse(v.c_str(), client);
				std::cout << "MobiFlight[" << client->Name.c_str() << "]: Received get version" << std::endl;
				break;

			}
			// MF.SimVars.Set(5 (>L:MyVar))
			else if (str.find("MF.SimVars.Set.") != std::string::npos) {
				std::string prefix = "MF.SimVars.Set.";
				str = str.substr(prefix.length());
#if _DEBUG
				std::cout << "MobiFlight[" << client->Name.c_str() << "]: Executing Code: " << str.c_str() << std::endl;
#endif
				execute_calculator_code(str.c_str(), 0, nullptr, nullptr);
				break;
			}

			std::shared_ptr<std::string> m_str = std::make_shared<std::string>(str);

			if (m_str.get()->find("MF.SimVars.Add.") != std::string::npos) {
				std::string prefix = "MF.SimVars.Add.";
				str = m_str.get()->substr(prefix.length());
				RegisterFloatSimVar(str, client);
				std::cout << "MobiFlight[" << client->Name.c_str() << "]: Received SimVar to register: " << str.c_str() << std::endl;
				break;
			}

			if (m_str.get()->find("MF.SimVars.AddString.") != std::string::npos) {
				std::string prefix = "MF.SimVars.AddString.";
				str = m_str.get()->substr(prefix.length());
				RegisterStringSimVar(str, client);
				std::cout << "MobiFlight[" << client->Name.c_str() << "]: Received StringSimVar to register: " << str.c_str() << std::endl;
				break;
			}

			if (m_str.get()->find("MF.Clients.Add.") != std::string::npos) {
				std::string prefix = "MF.Clients.Add.";
				str= m_str.get()->substr(prefix.length());
				Client* newClient = RegisterNewClient(str);
				SendNewClientResponse(client, newClient);
				std::cout << "MobiFlight[" << client->Name.c_str() << "]: Received Client to register: " << str.c_str() << std::endl;
			}

			if (m_str.get()->find("MF.Config.MAX_VARS_PER_FRAME.Set.") != std::string::npos) {
				std::string prefix = "MF.Config.MAX_VARS_PER_FRAME.Set.";
				str = m_str.get()->substr(prefix.length());
				uint16_t value = static_cast<uint16_t>(std::stoi(str));
				MOBIFLIGHT_MAX_VARS_PER_FRAME = value;
				std::cout << "MobiFlight: Set MF.Config.MAX_VARS_PER_FRAME to " << value << std::endl;
			}
			break;
		}

		case SIMCONNECT_RECV_ID_EVENT_FRAME: {
			SIMCONNECT_RECV_EVENT* evt = (SIMCONNECT_RECV_EVENT*)pData;
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
#if _DEBUG
				std::cout << "MobiFlight execute " << command.c_str() << std::endl;
#endif
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
