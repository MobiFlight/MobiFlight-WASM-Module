#include <MSFS\MSFS.h>
#include <MSFS\MSFS_WindowsTypes.h>
#include <SimConnect.h>
#include <MSFS\Legacy\gauges.h>
#include <vector>
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include "Module.h"

HANDLE g_hSimConnect;
const char* version = "0.3.44";
const char* MobiFlightEventPrefix = "MobiFlight.";
const char* FileEventsMobiFlight = "modules/events.txt";
const char* FileEventsUser = "modules/events.user.txt";

std::vector<std::pair<std::string, std::string>> CodeEvents;

const char* MOBIFLIGHT_CLIENT_DATA_NAME = "MobiFlight";
const char* CLIENT_DATA_NAME_POSTFIX_SIMVAR = ".LVars";
const char* CLIENT_DATA_NAME_POSTFIX_COMMAND = ".Command";
const char* CLIENT_DATA_NAME_POSTFIX_RESPONSE = ".Response";

uint16_t CommandResponseDataAreaSize = 256;

// This is an offset for the dynamically registered SimVars 
// to avoid any conflicts with base IDs
uint16_t SimVarOffset = 1000;

// For each registered client can 10000 data definition ids are reserved
uint16_t ClientDataDefinitionIdSimVarsRange = 10000;

// data struct for dynamically registered SimVars
struct SimVar {
	int ID;
	int Offset;
	std::string Name;
	float Value;
};

// data struct for client accessing SimVars
struct Client {
	int ID;
	std::string Name;
	std::string DataAreaNameSimVar;
	std::string DataAreaNameResponse;
	std::string DataAreaNameCommand;
	SIMCONNECT_CLIENT_DATA_ID DataAreaIDSimvar;
	SIMCONNECT_CLIENT_DATA_ID DataAreaIDResponse;
	SIMCONNECT_CLIENT_DATA_ID DataAreaIDCommand;
	SIMCONNECT_CLIENT_DATA_DEFINITION_ID DataDefinitionIDStringResponse;
	SIMCONNECT_CLIENT_DATA_DEFINITION_ID DataDefinitionIDStringCommand;
	std::vector<SimVar> SimVars;
	SIMCONNECT_CLIENT_DATA_DEFINITION_ID DataDefinitionIdSimVarsStart;
	// This is an optimization to be able to re-use already defined data definition IDs & request IDs
	// after resetting registered SimVars 
	uint16_t MaxClientDataDefinition = 0;
};

// The list of currently registered clients
std::vector<Client*> RegisteredClients;

// The list of currently available LVars
std::vector<std::string> lVarList;

// Data struct to read messages coming from clients
struct StringValue {
	char value[255];
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
		if (hr != S_OK) fprintf(stderr, "MobiFlight: Error on registering Event %s with ID %u for code %s", eventName.c_str(), eventID, eventCommand.c_str());
		else fprintf(stderr, "MobiFlight: Success on registering Event %s with ID %u for code %s", eventName.c_str(), eventID, eventCommand.c_str());
#endif

		eventID++;
	}

	SimConnect_SetNotificationGroupPriority(g_hSimConnect, MOBIFLIGHT_GROUP::DEFAULT, SIMCONNECT_GROUP_PRIORITY_HIGHEST);
}

void SendResponse(const char * message, Client* client) {
	SimConnect_SetClientData(
		g_hSimConnect,
		client->DataAreaIDResponse,
		client->DataDefinitionIDStringResponse,
		SIMCONNECT_CLIENT_DATA_SET_FLAG_DEFAULT,
		0,
		CommandResponseDataAreaSize,
		(void*) message
	);
}

// Sends information about new client data areas as json string.
// e.g. {"Name": "ClientName", "SimVars": 3, "Command": 4, "Response": 5}
void SendNewClientResponse(Client* client, Client* nc) {
	std::ostringstream oss;
	oss << "{\"Name\": \"" << nc->Name << "\", \"SimVars\": "  << nc->DataAreaIDSimvar;
	oss << ", \"Command\": " << nc->DataAreaIDCommand << ", \"Response\": " << nc->DataAreaIDResponse << "}";
	std::string data = oss.str();
	fprintf(stderr, "MobiFlight: SendNewClientData > %s", data.c_str());
	SendResponse(data.c_str(), client);
}

// List all available LVars for the currently loaded flight
// and send them to the SimConnect client
void ListLVars(Client* client) {
	int lVarId = 0;
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
		fprintf(stderr, "MobiFlight: Available LVar > %s", lVar.c_str());
#endif
	}
}

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
		fprintf(stderr, "MobiFlight: Error on Setting Client Data. %u, SimVar: %s (ID: %u)", hr, simVar.Name.c_str(), simVar.ID);
	}
#if _DEBUG
	fprintf(stderr, "MobiFlight: SimVar %s with ID %u has value %f", simVar.Name.c_str(), simVar.ID, simVar.Value);
#endif
}

// Register a single SimVar and send the current value to SimConnect Clients 
void RegisterSimVar(const std::string code, Client* client) {
	std::vector<SimVar>* SimVars = &(client->SimVars);
	SimVar var1;
	var1.Name = code;
	var1.ID = SimVars->size() + client->DataDefinitionIdSimVarsStart;
	var1.Offset = (SimVars->size()) * sizeof(float);

	SimVars->push_back(var1);
	HRESULT hr;

	if (client->MaxClientDataDefinition < SimVars->size())
	{
		hr = SimConnect_AddToClientDataDefinition(
			g_hSimConnect,
			var1.ID,
			var1.Offset,
			sizeof(float),
			0
		);

		client->MaxClientDataDefinition = SimVars->size();
	}

	fprintf(stderr, "MobiFlight: RegisterSimVar SimVars Size2: %d\n", SimVars->size());

	FLOAT64 val;
	WriteSimVar(var1, client);

	execute_calculator_code(std::string(code).c_str(), &val, NULL, NULL);
	var1.Value = val;
	
	WriteSimVar(var1, client);

	fprintf(stderr, "MobiFlight: RegisterSimVar > %s ID [%u] : Offset(%u) : Value(%f)", var1.Name.c_str(), var1.ID, var1.Offset, var1.Value);
}

// Clear the list of currently tracked SimVars
void ClearSimVars(Client* client) {
	client->SimVars.clear();
	fprintf(stderr, "MobiFlight: Cleared SimVar tracking.");
}

// Read a single SimVar and send the current value to SimConnect Clients
void ReadSimVar(SimVar &simVar, Client* client) {
	FLOAT64 val = 0;
	execute_calculator_code(std::string(simVar.Name).c_str(), &val, NULL, NULL);
	
	if (simVar.Value == val) return;
	simVar.Value = val;

	WriteSimVar(simVar, client);

#if _DEBUG
	fprintf(stderr, "MobiFlight: SimVar %s with ID %u has value %f", simVar.Name.c_str(), simVar.ID, simVar.Value);
#endif
}

// Read all dynamically registered SimVars
void ReadSimVars() {
	for (auto& client : RegisteredClients) {
		std::vector<SimVar>* SimVars = &(client->SimVars);
		for (auto& value : *SimVars) {
			ReadSimVar(value, client);
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
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %u", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, client->DataAreaIDSimvar, 4096, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	hr = SimConnect_MapClientDataNameToID(g_hSimConnect, client->DataAreaNameResponse.c_str(), client->DataAreaIDResponse);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %u", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, client->DataAreaIDResponse, CommandResponseDataAreaSize, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	hr = SimConnect_MapClientDataNameToID(g_hSimConnect, client->DataAreaNameCommand.c_str(), client->DataAreaIDCommand);
	if (hr != S_OK) {
		fprintf(stderr, "MobiFlight: Error on creating Client Data Area. %u", hr);
		return;
	}
	SimConnect_CreateClientData(g_hSimConnect, client->DataAreaIDCommand, CommandResponseDataAreaSize, SIMCONNECT_CREATE_CLIENT_DATA_FLAG_DEFAULT);

	DWORD dataAreaOffset = 0;
	hr = SimConnect_AddToClientDataDefinition(
		g_hSimConnect,
		client->DataDefinitionIDStringResponse,
		dataAreaOffset,
		CommandResponseDataAreaSize,
		0
	);

	hr = SimConnect_AddToClientDataDefinition(
		g_hSimConnect,
		client->DataDefinitionIDStringCommand,
		dataAreaOffset,
		CommandResponseDataAreaSize,
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
		newClient->DataAreaIDSimvar = 3 * newClient->ID;
		newClient->DataAreaIDResponse = newClient->DataAreaIDSimvar + 1;
		newClient->DataAreaIDCommand = newClient->DataAreaIDResponse + 1;
		newClient->DataAreaNameSimVar = newClient->Name + std::string(CLIENT_DATA_NAME_POSTFIX_SIMVAR);
		newClient->DataAreaNameResponse = newClient->Name + std::string(CLIENT_DATA_NAME_POSTFIX_RESPONSE);
		newClient->DataAreaNameCommand = newClient->Name + std::string(CLIENT_DATA_NAME_POSTFIX_COMMAND);
		newClient->DataDefinitionIDStringResponse = 2 * newClient->ID; // 500 Clients possible until offset 1000 is reached
		newClient->DataDefinitionIDStringCommand = newClient->DataDefinitionIDStringResponse + 1;
		newClient->SimVars = std::vector<SimVar>();
		newClient->DataDefinitionIdSimVarsStart = SimVarOffset + (newClient->ID * ClientDataDefinitionIdSimVarsRange);

		RegisteredClients.push_back(newClient);

		// Create the new client data areas
		RegisterClientDataArea(newClient);
	}

#if _DEBUG
		fprintf(stderr, "MobiFlight: NewClient Name: %s\n", newClient->Name.c_str());
		fprintf(stderr, "MobiFlight: NewClient ID: %d\n", newClient->ID);
		fprintf(stderr, "MobiFlight: NewClient DataAreaIDSimvar: %d\n", newClient->DataAreaIDSimvar);
		fprintf(stderr, "MobiFlight: NewClient DataAreaIDResponse: %d\n", newClient->DataAreaIDResponse);
		fprintf(stderr, "MobiFlight: NewClient DataAreaIDCommand: %d\n", newClient->DataAreaIDCommand);
		fprintf(stderr, "MobiFlight: NewClient DataAreaNameSimVar: %s\n", newClient->DataAreaNameSimVar.c_str());
		fprintf(stderr, "MobiFlight: NewClient DataAreaNameResponse: %s\n", newClient->DataAreaNameResponse.c_str());
		fprintf(stderr, "MobiFlight: NewClient DataAreaNameCommand: %s\n", newClient->DataAreaNameCommand.c_str());
		fprintf(stderr, "MobiFlight: NewClient DataDefinitionIDStringResponse: %d\n", newClient->DataDefinitionIDStringResponse);
		fprintf(stderr, "MobiFlight: NewClient DataDefinitionIDStringCommand: %d\n", newClient->DataDefinitionIDStringCommand);
		fprintf(stderr, "MobiFlight: NewClient DataDefinitionIdSimVarsStart: %d\n", newClient->DataDefinitionIdSimVarsStart);
#endif

	return newClient;
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

	// Register Mobiflight default client
	Client* client = RegisterNewClient(std::string(MOBIFLIGHT_CLIENT_DATA_NAME));
	RegisterEvents();
	ListLVars(client);
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
			DWORD clientID = (DWORD)recv_data->dwRequestID;
#if _DEBUG
				fprintf(stderr, "MobiFlight: Received Command: %s\n", str.c_str());
				fprintf(stderr, "MobiFlight: Received ClientId: %ld\n", clientID);
#endif
			
			Client* client = RegisteredClients[clientID];
			
			if (str == "MF.SimVars.Clear") {
				ClearSimVars(client);
				break;

			} else if (str == "MF.LVars.List") {
				SendResponse("MF.LVars.List.Start", client);
				ListLVars(client);
				SendResponse("MF.LVars.List.End", client);
				break;

			}
			// MF.SimVars.Set(5 (>L:MyVar))
			else if (str.find("MF.SimVars.Set.") != std::string::npos) {
				std::string prefix = "MF.SimVars.Set.";
				str = str.substr(prefix.length());
				execute_calculator_code(str.c_str(), nullptr, nullptr, nullptr);
				break;
			}
		
			std::shared_ptr<std::string> m_str = std::make_shared<std::string>(str);
			
			if (m_str.get()->find("MF.SimVars.Add.") != std::string::npos) {
				std::string prefix = "MF.SimVars.Add.";
				str = m_str.get()->substr(prefix.length());
				RegisterSimVar(str, client);
				fprintf(stderr, "MobiFlight: Received SimVar to register: %s.\n", str.c_str());
				break;
			}

			if (m_str.get()->find("MF.Clients.Add.") != std::string::npos) {
				std::string prefix = "MF.Clients.Add.";
				str= m_str.get()->substr(prefix.length());
				Client* newClient = RegisterNewClient(str);
				SendNewClientResponse(client, newClient);
				fprintf(stderr, "MobiFlight: Received Client to register: %s.\n", str.c_str());
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
