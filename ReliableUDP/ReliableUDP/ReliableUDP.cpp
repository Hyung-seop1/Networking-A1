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

	Mode mode = Client;
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

		/* Receiving the file metadata
		*
		* Process with incoming metadata packets
		* declaring the new variable in char maybe called metaPacket[]
		* use connection.ReceivePacket(metaPacket , sizeof(metaPacket)) and declare to byte read variable
		* Parse metadata from received packets and initialize file receiving process and store the information
		*
		* // Step 1: Receiving file metadata
			unsigned char metaPacket[256]; // Buffer to store incoming metadata
			int bytes_read = connection.ReceivePacket(metaPacket, sizeof(metaPacket)); // Read metadata packet

			if (bytes_read > 0) {
				metaPacket[bytes_read] = '\0'; // Ensure null termination for string parsing

				// Parse metadata (assuming format: "filename|filesize")
				string metadata((char*)metaPacket, bytes_read);
				size_t separator = metadata.find('|');

				if (separator != string::npos) {
					string filename = metadata.substr(0, separator);
					int fileSize = stoi(metadata.substr(separator + 1));

					// Initialize file receiving process
					ofstream file(filename, ios::binary); // Open file for writing
					if (file.is_open()) {
						printf("Receiving file: %s, Size: %d bytes\n", filename.c_str(), fileSize);
					} else {
						printf("Error: Could not open file for writing\n");
					}
				} else {
					printf("Error: Invalid metadata format\n");
				}
			}
		*/

		// send and receive packets
		sendAccumulator += DeltaTime;
		int sendCount = 0;

		//client
		while (sendAccumulator > 1.0f / sendRate)
		{
			unsigned char packet[PacketSize];
			memset(packet, 0, sizeof(packet));

			snprintf((char*)packet, sizeof(packet), "Hello World <<%d>>", sendCount);
			sendCount++;

			connection.SendPacket(packet, strlen((char*)packet + 1));
			sendAccumulator -= 1.0f / sendRate;


			/* Receiving the file pieces
			*
			* This would involve receiving and assembling file pieces based on metadata
			* Receiving the actual file data in pieces, typically after the metadata is exchanged. These chunks will be written to disk in sequence.
			* After handling the metadata, include another loop to process the file pieces sent by the other side.
			*
			*/
		}

		//server
		while (true)
		{
			unsigned char packet[256];
			int bytes_read = connection.ReceivePacket(packet, sizeof(packet));
			if (bytes_read == 0)
				break;

			printf("Hello World : %s\n", packet);
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

	ShutdownSockets();

	return 0;
}
