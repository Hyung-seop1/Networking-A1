/*
	Reliability and Flow Control Example
	From "Networking for Game Programmers" - http://www.gaffer.org/networking-for-game-programmers
	Author: Glenn Fiedler <gaffer@gaffer.org>
*/

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "Net.h"
#pragma warning(disable: 4996)

//#define SHOW_ACKS

using namespace std;
using namespace net;

const int ServerPort = 30000;
const int ClientPort = 30001;
const int ProtocolId = 0x11223344;
const float DeltaTime = 1.0f / 30.0f;
const float SendRate = 1.0f / 30.0f;
const float TimeOut = 10.0f;
const int PacketSize = 256;

class FlowControl
{
public:

	FlowControl()
	{
		printf("flow control initialized\n");
		Reset();
	}

	void Reset()
	{
		mode = Bad;
		penalty_time = 4.0f;
		good_conditions_time = 0.0f;
		penalty_reduction_accumulator = 0.0f;
	}

	void Update(float deltaTime, float rtt)
	{
		const float RTT_Threshold = 250.0f;

		if (mode == Good)
		{
			if (rtt > RTT_Threshold)
			{
				printf("*** dropping to bad mode ***\n");
				mode = Bad;
				if (good_conditions_time < 10.0f && penalty_time < 60.0f)
				{
					penalty_time *= 2.0f;
					if (penalty_time > 60.0f)
						penalty_time = 60.0f;
					printf("penalty time increased to %.1f\n", penalty_time);
				}
				good_conditions_time = 0.0f;
				penalty_reduction_accumulator = 0.0f;
				return;
			}

			good_conditions_time += deltaTime;
			penalty_reduction_accumulator += deltaTime;

			if (penalty_reduction_accumulator > 10.0f && penalty_time > 1.0f)
			{
				penalty_time /= 2.0f;
				if (penalty_time < 1.0f)
					penalty_time = 1.0f;
				printf("penalty time reduced to %.1f\n", penalty_time);
				penalty_reduction_accumulator = 0.0f;
			}
		}

		if (mode == Bad)
		{
			if (rtt <= RTT_Threshold)
				good_conditions_time += deltaTime;
			else
				good_conditions_time = 0.0f;

			if (good_conditions_time > penalty_time)
			{
				printf("*** upgrading to good mode ***\n");
				good_conditions_time = 0.0f;
				penalty_reduction_accumulator = 0.0f;
				mode = Good;
				return;
			}
		}
	}

	float GetSendRate()
	{
		return mode == Good ? 30.0f : 10.0f;
	}

private:

	enum Mode
	{
		Good,
		Bad
	};

	Mode mode;
	float penalty_time;
	float good_conditions_time;
	float penalty_reduction_accumulator;
};


int main(int argc, char* argv[])
{
	
	/*File type determined if ASCII or image file for efficient fragementation and reconstruction
		- Determine file meta data(name, size, type)
		- A fixed - sized header can be for metadata
		- Reciever should ack receipt of metadata before proceeding with data reception(reordering, checksum, etc.)

		- Large files fragmented into buffers or chunks

		- Sender buffers unacknowledged packets with their metadata

		- Instead of waiting for ACK to be sent for each packet before sending next one, Implement "SLIDING WINDOW"
		- Multiple packets sent before waiting for ACK
		- Reciever buffers out - of - order packets for reordering, if mismatch, request retransmission of corrupted packet
		- Reciever computes checksum on recieved file,

		-Checksum(CRC32) on entire file one last packets of last buffer sent.
		- Send as part of metadata packet.*/
	
	// parse command line

	enum Mode
	{
		Client,
		Server
	};

	Mode mode = Server;
	Address address;

	if (argc >= 2)
	{
		int a, b, c, d;
#pragma warning(suppress : 4996)
		if (sscanf(argv[1], "%d.%d.%d.%d", &a, &b, &c, &d))
		{
			mode = Client;
			address = Address(a, b, c, d, ServerPort);
		}
	}

	// initialize
	if (!InitializeSockets())
	{
		printf("failed to initialize sockets\n");
		return 1;
	}

	ReliableConnection connection(ProtocolId, TimeOut);
	const int port = mode == Server ? ServerPort : ClientPort;

	if (!connection.Start(port))
	{
		printf("could not start connection on port %d\n", port);
		return 1;
	}

	if (mode == Client)
		connection.Connect(address);
	else
		// If mode is "server" listen
		connection.Listen();

	bool connected = false;
	float sendAccumulator = 0.0f;
	float statsAccumulator = 0.0f;

	FlowControl flowControl;

	while (true)
	{
		// update flow control
		if (connection.IsConnected())
			flowControl.Update(DeltaTime, connection.GetReliabilitySystem().GetRoundTripTime() * 1000.0f);

		const float sendRate = flowControl.GetSendRate();

		// detect changes in connection state

		if (mode == Server && connected && !connection.IsConnected())
		{
			flowControl.Reset();
			printf("reset flow control\n");
			connected = false;
		}

		if (!connected && connection.IsConnected())
		{
			printf("client connected to server\n");
			connected = true;
		}

		if (!connected && connection.ConnectFailed())
		{
			printf("connection failed\n");
			break;
		}
	
		// send and receive packets
		sendAccumulator += DeltaTime;
		int sendCount = 0;

		// client
		while (sendAccumulator > 1.0f / sendRate)
		{
			unsigned char packet[PacketSize];
			memset(packet, 0, sizeof(packet));

			// Open file to determine metadata
			FILE* file = fopen("testfile.bin", "rb"); 
			if (!file)
			{
				printf("Error opening file\n");
				break;
			}

			// Determine file size
			fseek(file, 0, SEEK_END);
			unsigned long fileSize = ftell(file);
			rewind(file);

			// Determine file type (ASCII or Binary check)
			int isBinary = 0;
			unsigned char sample[256] = { 0 };
			size_t readBytes = fread(sample, 1, sizeof(sample), file);
			for (size_t i = 0; i < readBytes; i++)
			{
				if (sample[i] == 0) // Presence of NULL byte indicates binary file
				{
					isBinary = 1;
					break;
				}
			}

			fclose(file);

			// Construct metadata packet (filename, size, type)
			unsigned char metaPacket[PacketSize] = { 0 };
			int metaIndex = 0;

			// File name (limited to 50 bytes max, padded with null bytes)
			const char* fileName = "testfile.bin";
			for (int i = 0; i < 50 && fileName[i] != '\0'; i++)
			{
				metaPacket[metaIndex++] = fileName[i];
			}
			while (metaIndex < 50) // Ensure 50 bytes are always sent
			{
				metaPacket[metaIndex++] = '\0';
			}

			// Append file size (4-byte big-endian)
			metaPacket[metaIndex++] = (fileSize >> 24) & 0xFF;
			metaPacket[metaIndex++] = (fileSize >> 16) & 0xFF;
			metaPacket[metaIndex++] = (fileSize >> 8) & 0xFF;
			metaPacket[metaIndex++] = fileSize & 0xFF;

			// Append file type ('B' for Binary, 'A' for ASCII)
			metaPacket[metaIndex++] = isBinary ? 'B' : 'A';

			// Send metadata packet
			connection.SendPacket(metaPacket, metaIndex);

			// Continue with the existing "Hello World" packet logic
			snprintf((char*)packet, sizeof(packet), "Hello World <<%d>>", sendCount);
			sendCount++;

			connection.SendPacket(packet, strlen((char*)packet + 1));
			sendAccumulator -= 1.0f / sendRate;
		}


		//server
		while (true)
		{
			unsigned char packet[PacketSize];
			int bytes_read = connection.ReceivePacket(packet, sizeof(packet));
			if (bytes_read == 0)
				break;

			// Validate the received packet
			printf("Received packet: %s\n", packet);

			// Send acknowledgment back to the client
			string response = "ACK";
			connection.SendPacket((unsigned char*)response.c_str(), response.size() + 1);
		}

		// show packets that were acked this frame

#ifdef SHOW_ACKS
		unsigned int* acks = NULL;
		int ack_count = 0;
		connection.GetReliabilitySystem().GetAcks(&acks, ack_count);
		if (ack_count > 0)
		{
			printf("acks: %d", acks[0]);
			for (int i = 1; i < ack_count; ++i)
				printf(",%d", acks[i]);
			printf("\n");
		}
#endif

		// update connection

		connection.Update(DeltaTime);

		// show connection stats

		statsAccumulator += DeltaTime;

		while (statsAccumulator >= 0.25f && connection.IsConnected())
		{
			float rtt = connection.GetReliabilitySystem().GetRoundTripTime();

			unsigned int sent_packets = connection.GetReliabilitySystem().GetSentPackets();
			unsigned int acked_packets = connection.GetReliabilitySystem().GetAckedPackets();
			unsigned int lost_packets = connection.GetReliabilitySystem().GetLostPackets();

			float sent_bandwidth = connection.GetReliabilitySystem().GetSentBandwidth();
			float acked_bandwidth = connection.GetReliabilitySystem().GetAckedBandwidth();

			printf("rtt %.1fms, sent %d, acked %d, lost %d (%.1f%%), sent bandwidth = %.1fkbps, acked bandwidth = %.1fkbps\n",
				rtt * 1000.0f, sent_packets, acked_packets, lost_packets,
				sent_packets > 0.0f ? (float)lost_packets / (float)sent_packets * 100.0f : 0.0f,
				sent_bandwidth, acked_bandwidth);

			statsAccumulator -= 0.25f;
		}

		net::wait(DeltaTime);
	}

	return 0;
}
