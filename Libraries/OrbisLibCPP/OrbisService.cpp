#include "stdafx.hpp"
#include "OrbisService.hpp"

enum ClientCommands
{
	CMD_CLIENT_CONNECT,
	CMD_CLIENT_DISCONNECT,
	CMD_CLIENT_HEARTBEAT,
	CMD_CLIENT_CHANGE_PRINT_PORT,
	CMD_CLIENT_CHANGE_COM_PORT
};

struct CommandPacket_s
{
	int CommandIndex;
	union
	{
		int Index;
		short PrintPort;
		char COMPort[0x20];
	};
};

OrbisService::OrbisService(OrbisLib* orbisLib)
{
	this->orbisLib = orbisLib;

	//Set Bool to let threads stop when we do clean up.
	this->IsRunning = true;

	//Connect to Windows Service.
	while (!Connect())
	{
		Sleep(1000);
	}
}

OrbisService::~OrbisService()
{
	//Signal Threads to stop.
	this->IsRunning = false;

	//Disconnect from windows Service.
	Disconnect();
}

VOID OrbisService::ServiceCallback(LPVOID lpParameter, Sockets* Socket)
{
	
	OrbisService* orbisService = (OrbisService*)lpParameter;

	//Allocate space to recieve the packet from the Target Console
	TargetCommandPacket_s* Packet = (TargetCommandPacket_s*)malloc(sizeof(TargetCommandPacket_s));

	//Recieve the Targets command packet.
	if (!Socket->Receive((char*)Packet, sizeof(TargetCommandPacket_s)))
	{
		printf("Failed to recv TargetCommandPacket\n");

		return;
	}

	switch (Packet->CommandIndex)
	{
	case CMD_PRINT:
		if (orbisService->Target_Print)
			orbisService->Target_Print(Packet->IPAddr, Packet->Print.Sender, Packet->Print.Type, Packet->Print.Data);
		break;


	case CMD_INTERCEPT:
		if (orbisService->Proc_Intercept)
			orbisService->Proc_Intercept(Packet->IPAddr, Packet->Break.Reason, &Packet->Break.Registers);
		break;

	case CMD_CONTINUE:
		if (orbisService->Proc_Continue)
			orbisService->Proc_Continue(Packet->IPAddr);
		break;


	case CMD_PROC_DIE:
		if (orbisService->Proc_Die)
			orbisService->Proc_Die(Packet->IPAddr);
		break;

	case CMD_PROC_ATTACH:
		if (orbisService->Proc_Attach)
			orbisService->Proc_Attach(Packet->IPAddr, (char*)&Packet->ProcName);
		break;

	case CMD_PROC_DETACH:
		if (orbisService->Proc_Detach)
			orbisService->Proc_Detach(Packet->IPAddr);
		break;


	case CMD_TARGET_SUSPEND:
		if (orbisService->Target_Suspend)
			orbisService->Target_Suspend(Packet->IPAddr);
		break;

	case CMD_TARGET_RESUME:
		if (orbisService->Target_Resume)
			orbisService->Target_Resume(Packet->IPAddr);
		break;

	case CMD_TARGET_SHUTDOWN:
		if (orbisService->Target_Shutdown)
			orbisService->Target_Shutdown(Packet->IPAddr);
		break;

	case CMD_TARGET_NEWTITLE:
		if (orbisService->Target_NewTitle)
			orbisService->Target_NewTitle(Packet->IPAddr, (char*)&Packet->TitleChange.TitleID);
		break;


	case CMD_DB_TOUCHED:
		orbisService->orbisLib->Settings->UpdateSettings();
		orbisService->orbisLib->TargetManagement->UpdateTargets();

		if (orbisService->DB_Touched)
			orbisService->DB_Touched();
		break;

	case CMD_TARGET_AVAILABILITY:
		orbisService->orbisLib->TargetManagement->UpdateTargets();

		if(orbisService->Target_Availability)
			orbisService->Target_Availability(Packet->Target.Available, (char*)&Packet->Target.TargetName);
		break;
	}

	//Clean up.
	free(Packet);
}

bool OrbisService::Connect()
{
	//Shouldnt happen but for safety we have this to make sure we dont connect twice.
	if (this->IsConnectedtoService)
	{
		printf("ServiceClient already connected!\n");

		return false;
	}

	//Set up socket for local host on the command server port
	Sockets* Socket = new Sockets("127.0.0.1", PORT_COMMANDSERVER);

	//connect to the command server.
	if (!Socket->Connect())
	{
		printf("ServiceClient failed to connect. OrbisService failed to connect to the windows service. Is the windows service running?\n");

		return false;
	}

	//Set up the packet for our command.
	CommandPacket_s CommandPacket;
	CommandPacket.CommandIndex = CMD_CLIENT_CONNECT;

	if (!Socket->Send((char*)&CommandPacket, sizeof(CommandPacket_s))) 
	{
		printf("ServiceClient failed to connect. Failed to send packet data.\n");

		free(Socket);

		return false;
	}

	//Reset the client index value and recieve the data.
	this->ClientIndex = -1;
	if (!Socket->Receive((char*)&this->ClientIndex, sizeof(int)))
	{
		printf("ServiceClient failed to connect. Failed to recieve client index from windows service.\n");

		return false;
	}

	//Make sure we have a client index.
	if (this->ClientIndex <= -1 || this->ClientIndex > MAX_CLIENTS)
	{
		printf("ServiceClient failed to connect. %d is not a valid Client Index.\n", this->ClientIndex);

		free(Socket);

		return false;
	}

	printf("ServiceClient Connected with index %d\n", this->ClientIndex);

	//Set our status as connected.
	this->IsConnectedtoService = true;

	//Start up heart beat thread.
	CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)HeartBeatThread, this, 3, NULL);

	//Start up listener.
	ServiceListener = new SocketListener(this->ServiceCallback, this, PORT_START + this->ClientIndex);

	free(Socket);

	return true;
}

DWORD WINAPI OrbisService::HeartBeatThread(LPVOID Params)
{
	OrbisService* orbisService = (OrbisService*)Params;

	while (orbisService->IsRunning && orbisService->IsConnectedtoService)
	{
		if (orbisService->SendHeartBeat())
		{
			Sleep(8000);
		}
		else
		{
			//Reset our connection status.
			orbisService->IsConnectedtoService = false;

			//Destroy Listener.
			delete orbisService->ServiceListener;

			//Attempt to reconnect to the Windows Service.
			while (!orbisService->Connect())
			{
				Sleep(1000);
			}

			printf("Reconnection to Service Sucessful.\n");

			break;
		}
		
	}
	
	DWORD Thr_Exit = 0;
	ExitThread(Thr_Exit);
}

void OrbisService::Disconnect()
{
	//Make sure we are actually connected before trying to disconnect.
	if (!this->IsConnectedtoService)
	{
		printf("ServiceClient not connected!\n");

		return;
	}

	//Set up socket for local host on the command server port
	Sockets* Socket = new Sockets("127.0.0.1", PORT_COMMANDSERVER);

	//connect to the command server.
	if (!Socket->Connect())
	{
		printf("ServiceClient failed to Disconnect. OrbisService failed to connect to the windows service. Is the windows service running?\n");

		return;
	}

	//Set up the packet for our command.
	CommandPacket_s CommandPacket;
	CommandPacket.CommandIndex = CMD_CLIENT_DISCONNECT;
	CommandPacket.Index = this->ClientIndex;

	//Send the packet.
	if (!Socket->Send((char*)&CommandPacket, sizeof(CommandPacket_s)))
	{
		printf("ServiceClient failed to Disconnect. Failed to send packet data.\n");

		free(Socket);

		return;
	}

	//Reset our connection status.
	this->IsConnectedtoService = false;

	//Destroy Listener
	delete this->ServiceListener;

	free(Socket);
}

bool OrbisService::SendHeartBeat()
{
	//Make sure we are actually connected before trying to disconnect.
	if (!this->IsConnectedtoService)
	{
		printf("ServiceClient not connected!\n");

		return false;
	}

	//Set up socket for local host on the command server port
	Sockets* Socket = new Sockets("127.0.0.1", PORT_COMMANDSERVER);

	//connect to the command server.
	if (!Socket->Connect())
	{
		printf("ServiceClient failed to SendHeartBeat. OrbisService failed to connect to the windows service. Is the windows service running?\n");

		return false;
	}

	//Set up the packet for our command.
	CommandPacket_s CommandPacket;
	CommandPacket.CommandIndex = CMD_CLIENT_HEARTBEAT;
	CommandPacket.Index = this->ClientIndex;

	printf("Sending Heart beat packet...\n");

	//Send the packet.
	if (!Socket->Send((char*)&CommandPacket, sizeof(CommandPacket_s)))
	{
		printf("ServiceClient failed to SendHeartBeat. Failed to send packet data.\n");

		free(Socket);

		return false;
	}

	int Status = 0;
	if (!Socket->Receive((char*)&Status, sizeof(int))) {
		Socket->Close();
		return false;
	}

	if (Status <= 1)
	{
		printf("Lost comms to Service. Reconnecting...\n");

		return false;
	}

	printf("Socket Done.\n");

	free(Socket);

	return true;
}