

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>  // Include this header for accurate time measurement
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

uint32_t crc32(const char* s, size_t n);

int main(int argc, char* argv[])
{
	// parse command line
	enum Mode
	{
		Client,
		Server
	};

	Mode mode = Client;
	Address address;
	const char* fileName = nullptr;

	if (argc >= 3)
	{
		int a, b, c, d;
#pragma warning(suppress : 4996)
		if (sscanf(argv[1], "%d.%d.%d.%d", &a, &b, &c, &d))
		{
			mode = Client;
			address = Address(a, b, c, d, ServerPort);
			fileName = argv[2]; // Getting the file Name
		}
	}
	else if (argc == 1) {
		mode = Server;
	}
	else {
		printf("Usage: <IP ADDRESS> <FILE NAME>\n");
		return 1;
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

	// Add a variable to track the start time of the transfer
	std::chrono::high_resolution_clock::time_point transferStartTime;
	size_t totalFileSize = 0;  // To store the total file size

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

		if (mode == Client) {
			// Open file for reading
			ifstream file(fileName, ios::binary | ios::ate);
			if (!file) {
				cerr << "Error: Cannot open file.\n";
				return 1;
			}

			size_t fileSize = file.tellg();
			file.seekg(0, ios::beg);
			size_t totalPackets = (fileSize / PacketSize) + ((fileSize % PacketSize) ? 1 : 0);

			// Record the start time when the file starts transmitting
			transferStartTime = std::chrono::high_resolution_clock::now();

			// Send first packet (File Metadata)
			unsigned char metadataPacket[PacketSize];
			memset(metadataPacket, 0, PacketSize);
			snprintf((char*)metadataPacket, PacketSize, "File|%zu|%s", totalPackets, fileName);
			connection.SendPacket(metadataPacket, PacketSize);

			cout << "Sending file: " << fileName << " (" << fileSize << " bytes) in " << totalPackets << " packets.\n";

			char buffer[PacketSize];
			size_t packetIndex = 0;

			while (true) {
				sendAccumulator += DeltaTime;

				while (sendAccumulator > 1.0f / sendRate && packetIndex < totalPackets) {
					memset(buffer, 0, PacketSize);
					file.read(buffer, PacketSize);
					connection.SendPacket((unsigned char*)buffer, PacketSize);

					packetIndex++;
					sendAccumulator -= 1.0f / sendRate;
				}

				if (packetIndex >= totalPackets) {
					cout << "File transmission complete. Waiting for server acknowledgment...\n";
					break;
				}

				connection.Update(DeltaTime);
				net::wait(DeltaTime);
			}
			file.close();

			// Reopen file for CRC32 calculation (reset file pointer)
			file.open(fileName, ios::binary);
			if (!file) {
				cerr << "Error: Cannot reopen file for CRC calculation.\n";
				return 1;
			}

			// If file opened successfully, calculate the CRC32
			unsigned long crc = 0xFFFFFFFF;
			unsigned char fileBuffer[PacketSize];

			while (file.read(reinterpret_cast<char*>(fileBuffer), PacketSize)) {
				crc = crc32(reinterpret_cast<char*>(fileBuffer), file.gcount());
			}

			// If the file has remaining bytes (less than PacketSize)
			if (file.gcount() > 0) {
				crc = crc32(reinterpret_cast<char*>(fileBuffer), file.gcount());
			}

			// Send the CRC32 checksum to the server (final CRC value)
			char crcPacket[PacketSize];
			snprintf(crcPacket, PacketSize, "CRC32|%08lX", crc);
			connection.SendPacket((unsigned char*)crcPacket, strlen(crcPacket) + 1);

			cout << "File transmission complete. CRC32 sent: " << std::hex << crc << endl;
		}


		// SERVER
		while (true)
		{
			unsigned char packet[PacketSize];
			int bytes_read = connection.ReceivePacket(packet, sizeof(packet));
			if (bytes_read == 0)
				break;

			// Validate the received packet
			printf("Received packet: %s\n", packet);

			static string clientCrc;
			unsigned long serverCrc = 0xFFFFFFFF;  // Initial CRC value for CRC32
			static vector<unsigned char> fileData; // To store the received file data

			if (strncmp((char*)packet, "File|", 5) == 0)
			{
				printf("Received file metadata. Sending ACK.\n");
				string ack = "ACK_FILE_INFO"; // Send ACK to client that file successfully 
				connection.SendPacket((unsigned char*)ack.c_str(), ack.size() + 1);
			}
			else if (strncmp((char*)packet, "CRC32|", 6) == 0)
			{
				// Extract the CRC32 from the packet
				clientCrc = string((char*)packet);
				clientCrc = clientCrc.substr(6); // Extract the CRC32 value (remove "CRC32|" prefix)
				printf("Received file CRC32: %s\n", clientCrc.c_str());

				// Calculate CRC32 on the server-side from accumulated file data
				serverCrc = crc32((const char*)fileData.data(), fileData.size()); // Correct CRC calculation
			}
			else
			{
				// Accumulate file data
				fileData.insert(fileData.end(), packet, packet + bytes_read); // Store the received file data
			}

			// Final comparison between client and server CRC32
			if (!clientCrc.empty()) {
				printf("Server CRC32: %08lX\n", serverCrc);
				if (clientCrc == std::to_string(serverCrc)) {
					printf("File transfer successful! CRC32 matched.\n");
				}
				else {
					printf("File transfer failed! CRC32 mismatch.\n");
				}
			}
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

uint32_t crc32(const char* s, size_t n) {
	uint32_t crc = 0xFFFFFFFF;

	for (size_t i = 0;i < n;i++) {
		char ch = s[i];
		for (size_t j = 0;j < 8;j++) {
			uint32_t b = (ch ^ crc) & 1;
			crc >>= 1;
			if (b) crc = crc ^ 0xEDB88320;
			ch >>= 1;
		}
	}

	return ~crc;
}
