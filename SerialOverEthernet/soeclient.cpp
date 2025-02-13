/*
 * soeclient.cpp
 *
 *  Created on: 04.02.2025
 *      Author: marvi
 */

#include <corecrt.h>
#include <stdio.h>
#include <string.h>
#include <stdexcept>
#include <string>
#include "soeimpl.h"

using namespace std;

// Initializes a new client handler for the supplied network socket
SOEClient::SOEClient(Socket &socket) {
	this->socket = &socket;
	this->ports = map<string, SOEPort*>();
	this->thread_rx = thread([this]() -> void {
		this->handleClientRX();
	});
	this->thread_tx = thread([this]() -> void {
		this->handleClientTX();
	});
}

// Shuts down the client handler and frees all resources (including ports opened by the client)
SOEClient::~SOEClient() {
	this->socket->close();
	{ unique_lock<mutex> lock(this->tx_waitm); }
	this->tx_waitc.notify_all();
	this->thread_rx.join();
	this->thread_tx.join();
	delete this->socket;
}

// Returns true if the clients network connection is still open
bool SOEClient::isActive() {
	return this->socket->isOpen();
}

void SOEClient::handleClientTX() {

	// Start tx loop
	while (this->socket->isOpen()) {

		bool dataAvailable = false;
		for (auto entry = this->ports.begin(); entry != this->ports.end(); entry++) {

			// Request data from stack
			unsigned int rxid = 0;
			const char* payload = 0;
			size_t length = 0;
			if (!entry->second->read(&rxid, &payload, &length))
				continue;

			// Send payload stream frame
			if (!sendStream(entry->first.c_str(), rxid, payload, length)) {
				sendError(entry->first.c_str(), "failed to transmit STREAM frame, close port");

				// Close port
				delete entry->second;
				this->ports.erase(entry->first);
				sendClaimStatus(false, entry->first.c_str());
			}

			dataAvailable = true;
		}

		// If no data available, wait for more
		if (!dataAvailable) {
			unique_lock<mutex> lock(this->tx_waitm);
			this->tx_waitc.wait(lock);
		}

	}

}

void SOEClient::notifySerialData() {
	{ unique_lock<mutex> lock(this->tx_waitm); }
	this->tx_waitc.notify_all();
}

// Handles the incoming network requests
void SOEClient::handleClientRX() {

	// Setup rx variables
	this->op_code = -1;
	this->pckg_buf = 0;
	this->pckg_len = 0;
	this->pckg_recv = 0;

	// Start rx loop
	char rxbuf[INET_RX_BUF];
	unsigned int received = 0;
	while (this->socket->isOpen()) {

		// If no more data remaining, wait for new data from the network
		if (received == 0 && !this->socket->receive(rxbuf, INET_RX_BUF, &received)) {
			printf("FRAME ERROR: failed to receive data from client socket!\n");
			break;
		}

		// Exit if the socket was closed
		if (!socket->isOpen()) break;

		// If no frame is currently being received, start new frame
		if (this->op_code == -1) {

			// Check for header size
			if (received < 1) {
				printf("FRAME ERROR: received incomplete SOE header!\n");
				break;
			}

			// Decode header
			this->op_code = rxbuf[0] & 0x7;
			this->pckg_len = (rxbuf[0] & 0xF8) >> 3;
			if (this->pckg_len == 31) {

				// Check for additional header size
				if (received < 5) {
					printf("FRAME ERROR: received incomplete SOE header!\n");
					break;
				}

				// Decode additional length field
				this->pckg_len = 0;
				this->pckg_len |= (rxbuf[4] & 0xFF) << 0;
				this->pckg_len |= (rxbuf[3] & 0xFF) << 8;
				this->pckg_len |= (rxbuf[2] & 0xFF) << 16;
				this->pckg_len |= (rxbuf[1] & 0xFF) << 24;
			}

			// Allocate buffer for payload
			this->pckg_buf = new char[this->pckg_len]; //(char*) malloc(this->pckg_len);
			if (this->pckg_buf == 0) {
				printf("FRAME ERROR: package to large, could not allocate!");
				break;
			}

			// Subtract header from remaining payload
			unsigned int headerLen = (this->pckg_len > 30 ? 5 : 1);

			// Calculate length to copy
			unsigned int rempcklen = this->pckg_len;
			if (rempcklen > received - headerLen)
				rempcklen = received - headerLen;

			// Copy received payload
			memcpy(this->pckg_buf, rxbuf + (this->pckg_len > 30 ? 5 : 1), rempcklen);
			this->pckg_recv = rempcklen;

			// Data of next frame remaining, move to front and process later
			unsigned int reminder = received - rempcklen - headerLen;
			if (reminder > 0) {
				memcpy(rxbuf, rxbuf + rempcklen + headerLen, reminder);
			}
			received = reminder;

		// Otherwise, add received data to current frame payload buffer
		} else {

			// Check payload length
			unsigned int newLen = this->pckg_recv + received;
			if (newLen > this->pckg_len || newLen < this->pckg_recv) {
				printf("FRAME ERROR: payload length does not match header!\n");
				break;
			}

			// Calculate length to copy
			unsigned int rempcklen = this->pckg_len - this->pckg_recv;
			if (rempcklen > received)
				rempcklen = received;

			// Copy received payload
			memcpy(this->pckg_buf + this->pckg_recv, rxbuf, rempcklen);
			this->pckg_recv += rempcklen;
			received -= rempcklen;

			// Data of next frame remaining, move to front and process later
			unsigned int reminder = received - rempcklen;
			if (reminder > 0) {
				memcpy(rxbuf, rxbuf + rempcklen, reminder);
			}
			received = reminder;

		}

		// Check if all payload was received
		if (this->pckg_recv >= this->pckg_len) {

			switch (this->op_code) {
			case OPC_OPEN: {

				// Check payload length
				if (this->pckg_len < 6) {
					sendError(NULL, "received incomplete OPEN control frame");
					break;
				}

				// Decode baud
				unsigned int portBaud =
						(this->pckg_buf[0] & 0xFF) << 24 |
						(this->pckg_buf[1] & 0xFF) << 16 |
						(this->pckg_buf[2] & 0xFF) << 8 |
						(this->pckg_buf[3] & 0xFF) << 0;

				// Decode port name length
				unsigned short portStrLen =
						(this->pckg_buf[4] & 0xFF) << 8 |
						(this->pckg_buf[5] & 0xFF) << 0;

				// Check port name length
				if (portStrLen > this->pckg_recv) {
					sendError(NULL, "received invalid OPEN payload");
					break;
				}

				// Decode port name string
				char portName[portStrLen + 1] = {0};
				memcpy(portName, this->pckg_buf + 6, portStrLen);

				// Attempt to open port
				SerialPort* port = new SerialPort(portName);
				port->setBaud(portBaud);
				if (!port->openPort()) {
					sendError(portName, "failed to claim port");
					delete port;
					break;
				}
				port->setTimeouts(SERIAL_RX_TIMEOUT, SERIAL_TX_TIMEOUT);
				SOEPort* portHandler = new SOEPort(this, *port, portName);
				this->ports[string(portName)] = portHandler;

				// Confirm that the port has been opened, close port if this fails, to avoid unused open ports
				if (!sendClaimStatus(true, portName)) {
					sendError(portName, "failed to complete OPENED confirmation, close port");
					delete port;
					this->ports.erase(string(portName));
				}

				break;
			}
			case OPC_CLOSE: {

				// Check payload length
				if (this->pckg_len < 2) {
					sendError(NULL, "received incomplete CLOSE control frame");
					break;
				}

				// Decode port name length
				unsigned short portStrLen =
						(this->pckg_buf[0] & 0xFF) << 8 |
						(this->pckg_buf[1] & 0xFF) << 0;

				// Check port name length
				if (portStrLen > this->pckg_recv) {
					sendError(NULL, "received invalid CLOSE payload");
					break;
				}

				// Decode port name string
				char portName[portStrLen + 1] = {0};
				memcpy(portName, this->pckg_buf + 2, portStrLen);

				// Attempt to close port
				string sPortName = string(portName);
				try {
					SOEPort* portHandler = this->ports.at(sPortName);
					delete portHandler;
					this->ports.erase(sPortName);
				} catch (out_of_range &e) {
					sendError(portName, "port not claimed");
					break;
				}

				// Confirm that the port has been closed
				if (!sendClaimStatus(false, portName)) {
					// If the error report fails too ... don't care at this point ...
					sendError(portName, "failed to transmit CLOSE confirmation");
				}

				break;
			}
			case OPC_STREAM: {

				// Check payload length
				if (this->pckg_len < 6) {
					sendError(NULL, "received incomplete STREAM control frame");
					break;
				}

				// Decode port name length
				unsigned short portStrLen =
						(this->pckg_buf[0] & 0xFF) << 8 |
						(this->pckg_buf[1] & 0xFF) << 0;

				// Check port name length
				if (portStrLen > this->pckg_recv) {
					sendError(NULL, "received invalid STREAM payload");
					break;
				}

				// Decode port name string
				char portName[portStrLen + 1] = {0};
				memcpy(portName, this->pckg_buf + 2, portStrLen);

				// Decode transmission id
				unsigned int txid =
						(this->pckg_buf[2 + portStrLen] & 0xFF) << 24 |
						(this->pckg_buf[3 + portStrLen] & 0xFF) << 16 |
						(this->pckg_buf[4 + portStrLen] & 0xFF) << 8 |
						(this->pckg_buf[5 + portStrLen] & 0xFF) << 0;

				// Attempt to get port
				SOEPort* portHandler = 0;
				try {
					portHandler = this->ports.at(string(portName));
				} catch (const std::out_of_range& e) {
					sendError(portName, "port not claimed");
					break;
				}

				// Check if port was closed unexpectedly
				if (!portHandler->isOpen()) {
					sendError(portName, "port is already closed");

					// Close port
					delete portHandler;
					this->ports.erase(string(portName));
					sendClaimStatus(false, portName);

					break;
				}

				// Put remaining payload on transmission stack
				const char* payload = this->pckg_buf + 6 + portStrLen;
				size_t payloadLen = this->pckg_recv - 6 - portStrLen;
				if (!portHandler->send(txid, payload, payloadLen)) {
					sendError(portName, "invalid transmission data");
					break;
				}

#ifdef DEBUG_PRINTS
				printf("DEBUG: queued payload: %s [tx %d] %llu\n", portName, txid, payloadLen);
#endif

				// TX CONFIRM is send after the data is transfered from the stack to the serial port!
				break;
			}
			case OPC_RX_CONFIRM: {

				// Check payload length
				if (this->pckg_len < 6) {
					sendError(NULL, "received incomplete RX_CONFIRM control frame");
					break;
				}

				// Decode port name length
				unsigned short portStrLen =
						(this->pckg_buf[0] & 0xFF) << 8 |
						(this->pckg_buf[1] & 0xFF) << 0;

				// Check port name length
				if (portStrLen > this->pckg_recv) {
					sendError(NULL, "received invalid RX_CONFIRM payload");
					break;
				}

				// Decode port name string
				char portName[portStrLen + 1] = {0};
				memcpy(portName, this->pckg_buf + 2, portStrLen);

				// Decode transmission id
				unsigned int rxid =
						(this->pckg_buf[2 + portStrLen] & 0xFF) << 24 |
						(this->pckg_buf[3 + portStrLen] & 0xFF) << 16 |
						(this->pckg_buf[4 + portStrLen] & 0xFF) << 8 |
						(this->pckg_buf[5 + portStrLen] & 0xFF) << 0;

				// Attempt to get port
				SOEPort* portHandler = 0;
				try {
					portHandler = this->ports.at(string(portName));
				} catch (const std::out_of_range& e) {
					sendError(portName, "port not claimed");
					break;
				}

				// Confirm reception
				portHandler->confirmReception(rxid);

#ifdef DEBUG_PRINTS
				printf("DEBUG: rx confirm: %s [rx %d]\n", portName, rxid);
#endif

				break;
			}
			default:
				sendError(NULL, "received invalid control frame");
			}

			// Free payload buffer and reset frame, if data of an another frame remained, move it to the front and process in next loop
			delete[] this->pckg_buf; //free(this->pckg_buf);
			this->pckg_buf = 0;
			this->pckg_recv = 0;
			this->pckg_len = 0;
			this->op_code = -1;

		}

	}

	// Release all ports
	for (auto entry = this->ports.begin(); entry != this->ports.end(); entry++) {
		printf("auto close port: %s\n", entry->first.c_str());
		delete entry->second;
	}
	this->ports.clear();

	// Free payload buffer
	if (this->pckg_buf != 0) {
		delete[] this->pckg_buf; // free(this->pckg_buf);
		this->pckg_buf = 0;
	}

	// Reset frame state
	this->op_code = -1;
	this->pckg_len = 0;
	this->pckg_recv = 0;
}

// Sends an response frame
bool SOEClient::sendFrame(char opc, const char* payload, unsigned int length) {

	if (!this->socket->isOpen()) return false;

	// Make frame start
	char frameStart = (opc & 0x7) | (length > 30 ? 31 : length) << 3;
	size_t frameLen = (length > 30 ? 5 : 1) + length;

	// Make additonal package length
	char buffer[frameLen] = {0};
	buffer[0] = frameStart;
	if (length > 30) {
		buffer[4] = (length >> 0) & 0xFF;
		buffer[3] = (length >> 8) & 0xFF;
		buffer[2] = (length >> 16) & 0xFF;
		buffer[1] = (length >> 24) & 0xFF;
	}

	// Copy payload
	memcpy(buffer + (length > 30 ? 5 : 1), payload, length);

	// Transmit frame
	if (!this->socket->send(buffer, frameLen)) {
		printf("FRAME ERROR: failed to transmit frame!\n");
		return false;
	}

	return true;
}

// Sends an error message response frame
void SOEClient::sendError(const char* port, const char* msg) {

	if (!this->socket->isOpen()) return;

	// Get message and port name length
	unsigned int portLen = port != 0 ? strlen(port) : 0;
	unsigned int msgLen = msg != 0 ? strlen(msg) : 0;
	if (portLen > 0xFFFF) portLen = 0xFFFF;
	if (msgLen > 0xFFFF) msgLen = 0xFFFF;

	// Allocate payload buffer
	unsigned int payloadLen = (portLen > 0 ? 2 + portLen : 0) + (msgLen > 0 ? 2 + msgLen : 0);
	char buffer[payloadLen] = {0};

	// Encode port name
	if (portLen > 0) {
		buffer[0] = (portLen >> 8) & 0xFF;
		buffer[1] = (portLen >> 0) & 0xFF;
		memcpy(buffer + 2, port, portLen);
		portLen += 2; // This allows us to make the next statements simpler
	}

	// Encode message
	if (msgLen > 0) {
		buffer[0 + portLen] = (msgLen >> 8) & 0xFF;
		buffer[1 + portLen] = (msgLen >> 0) & 0xFF;
		memcpy(buffer + 2 + portLen, msg, msgLen);
	}

	// Transmit payload in ERROR frame, ignore result, we don't care about an error-error ...
	sendFrame(OPC_ERROR, buffer, payloadLen);

}

// Sends an port open/close reponse frame
bool SOEClient::sendClaimStatus(bool claimed, const char* portName) {

	if (!this->socket->isOpen()) return false;

	// Get port name length
	unsigned int portLen = strlen(portName);
	if (portLen > 0xFFFF) portLen = 0xFFFF;

	// Allocate payload buffer
	unsigned int payloadLen = 2 + portLen;
	char buffer[payloadLen] = {0};

	// Encode port name
	buffer[0] = (portLen >> 8) & 0xFF;
	buffer[1] = (portLen >> 0) & 0xFF;
	memcpy(buffer + 2, portName, portLen);

	if (claimed) {
		printf("opened port: %s\n", portName);
 	} else {
 		printf("closed port: %s\n", portName);
 	}

	// Transmit payload in OPENED or ERROR frame
	return sendFrame(claimed ? OPC_OPENED : OPC_CLOSED, buffer, payloadLen);

}

// Sends an transmission confirm frame
bool SOEClient::sendTransmissionConfirm(const char* portName, unsigned int txid) {

	if (!this->socket->isOpen()) return false;

	// Get port name length
	unsigned int portLen = strlen(portName);
	if (portLen > 0xFFFF) portLen = 0xFFFF;

	// Allocate payload buffer
	unsigned int payloadLen = 6 + portLen;
	char buffer[payloadLen] = {0};

	// Encode port name
	buffer[0] = (portLen >> 8) & 0xFF;
	buffer[1] = (portLen >> 0) & 0xFF;
	memcpy(buffer + 2, portName, portLen);

	// Encode txid
	buffer[2 + portLen] = (txid >> 24) & 0xFF;
	buffer[3 + portLen] = (txid >> 16) & 0xFF;
	buffer[4 + portLen] = (txid >> 8) & 0xFF;
	buffer[5 + portLen] = (txid >> 0) & 0xFF;

	// Transmit payload in OPENED or ERROR frame
	return sendFrame(OPC_TX_CONFIRM, buffer, payloadLen);

}

// Send payload stream frame
bool SOEClient::sendStream(const char* portName, unsigned int rxid, const char* payload, size_t length) {

	if (!this->socket->isOpen()) return false;

	// Get port name length
	unsigned int portLen = strlen(portName);
	if (portLen > 0xFFFF) portLen = 0xFFFF;

	// Allocate payload buffer
	unsigned int payloadLen = 6 + portLen + length;
	char buffer[payloadLen] = {0};

	// Encode port name
	buffer[0] = (portLen >> 8) & 0xFF;
	buffer[1] = (portLen >> 0) & 0xFF;
	memcpy(buffer + 2, portName, portLen);

	// Encode rxid
	buffer[2 + portLen] = (rxid >> 24) & 0xFF;
	buffer[3 + portLen] = (rxid >> 16) & 0xFF;
	buffer[4 + portLen] = (rxid >> 8) & 0xFF;
	buffer[5 + portLen] = (rxid >> 0) & 0xFF;

	// Copy payload in buffer
	memcpy(buffer + 6 + portLen, payload, length);

	// Transmit payload in STREAM frame
	return sendFrame(OPC_STREAM, buffer, payloadLen);

}
