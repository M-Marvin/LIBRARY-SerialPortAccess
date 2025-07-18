/*
 * soeport.cpp
 *
 *  Created on: 04.02.2025
 *      Author: marvi
 */

#include <cstring>
#include <stdexcept>
#include "soeimpl.hpp"
#include "dbgprintf.h"

SerialOverEthernet::SOEPortHandler::SOEPortHandler(SerialAccess::SerialPort* port, std::function<void(void)> newDataCallback, std::function<void(unsigned int)> txConfirmCallback) {
	this->port.reset(port);
	this->new_data = newDataCallback;
	this->tx_confirm = txConfirmCallback;

	this->thread_tx = std::thread([this]() -> void {
		handlePortTX();
	});
	this->thread_rx = std::thread([this]() -> void {
		handlePortRX();
	});
}

SerialOverEthernet::SOEPortHandler::~SOEPortHandler() {
	this->port->closePort();
	{ std::unique_lock<std::mutex> lock(this->tx_stackm); }
	this->tx_waitc.notify_all();
	{ std::unique_lock<std::mutex> lock(this->rx_stackm); }
	this->rx_waitc.notify_all();
	this->thread_tx.join();
	this->thread_rx.join();
}

bool SerialOverEthernet::SOEPortHandler::isOpen() {
	return this->port->isOpen();
}

bool SerialOverEthernet::SOEPortHandler::send(unsigned int txid, const char* buffer, unsigned long length) {

	// Do not insert any data if this port is shutting down
	if (!this->port->isOpen()) return false;

	// Ignore packages which txid is "in the past", just confirm that it has already been received
	if (txid < this->next_txid && this->next_txid - txid < 0x7FFFFFFF) return true;

	// If tx stack has reached limit, abort operation, only make an exception if this is the next package that should be send
	if (this->tx_stack.size() >= SERIAL_RX_STACK_LIMIT && txid != this->next_txid) {
		std::lock_guard<std::mutex> lock(this->tx_stackm);
		this->tx_waitc.notify_all();
		return false;
	}

	// Copy payload bytes and insert in tx stack at txid
	char* stackBuffer = new char[length];
	memcpy(stackBuffer, buffer, length);
	{
		std::lock_guard<std::mutex> lock(this->tx_stackm);
		this->tx_stack[txid] = {length, std::unique_ptr<char>(stackBuffer)};
		dbgprintf("[DBG] serial <- [tx stack] <- |network| : [tx %u] size %lu len: %lu\n", txid, this->tx_stack.size(), length);
		this->tx_waitc.notify_all();
	}

	return true;

}

bool SerialOverEthernet::SOEPortHandler::read(unsigned int* rxid, const char** buffer, unsigned long* length) {

	// Nothing to send if port is closed
	if (!this->port->isOpen()) return false;

	std::lock_guard<std::mutex> lock(this->rx_stackm);

	// Look for packages which's reception might have failed
	for (unsigned int id = this->last_transmitted_rxid; id < this->next_transmit_rxid; id++) {
		if (this->rx_stack.count(id)) {
			rx_entry& stackEntry = this->rx_stack.at(id);
			if (stackEntry.rx_confirmed || stackEntry.time_to_resend >= std::chrono::steady_clock::now()) continue;
			*rxid = id;
			*buffer = stackEntry.payload.get();
			*length = stackEntry.length;

			// Update time to resend the package in case reception is not confirmed first
			stackEntry.time_to_resend = std::chrono::steady_clock::now() + std::chrono::milliseconds(INET_TX_REP_INTERVAL);

			return true;
		}
	}

	// Get element from rx stack and increment next free rxid to write data to
	if (this->rx_stack.count(this->next_transmit_rxid)) {
		rx_entry& stackEntry = this->rx_stack.at(this->next_transmit_rxid);
		if (stackEntry.length > 0) {
			*rxid = this->next_transmit_rxid++;
			*buffer = stackEntry.payload.get();
			*length = stackEntry.length;

			// Update time to resend the package in case reception is not confirmed first
			stackEntry.time_to_resend = std::chrono::steady_clock::now() + std::chrono::milliseconds(INET_TX_REP_INTERVAL);

			if (this->next_free_rxid < this->next_transmit_rxid)
				this->next_free_rxid = this->next_transmit_rxid;
			return true;
		}
	}

	return false; // No more data

}

void SerialOverEthernet::SOEPortHandler::confirmReception(unsigned int rxid) {

	// Receiving a recption confirmation for an package that was not yet transmitted makes no sense
	if (rxid >= this->next_free_rxid) return;

	// Get entry from stack and mark as received
	try {
		std::lock_guard<std::mutex> lock(this->rx_stackm);
		rx_entry& stackEntry = this->rx_stack.at(rxid);

		// Mark package reception confirmed
		stackEntry.rx_confirmed = true;

		dbgprintf("[DBG] serial -> |rx stack| -> [network] -> serial : [rx %u] size %lu len: %lu\n", rxid, this->rx_stack.size(), stackEntry.length);
	} catch (std::out_of_range& e) {} // The entry did not exist, ignore

}

void SerialOverEthernet::SOEPortHandler::confirmTransmission(unsigned int rxid) {

	// Ignore packages which rxid is "in the future"
	if (rxid > this->last_transmitted_rxid && rxid - this->last_transmitted_rxid < 0x7FFFFFFF) return;

	std::lock_guard<std::mutex> lock(this->rx_stackm);

	// Remove all packages before and including rxid, this ensures even packges who's tx confirm might be lost are cleared from the stack
	for (unsigned int i = this->last_transmitted_rxid; i != rxid + 1; i++) {
		this->rx_stack.erase(i);
		dbgprintf("[DBG] serial -> rx stack -> |network| -> [serial] : [rx %u] size %lu\n", i, this->rx_stack.size());
	}
	this->last_transmitted_rxid = rxid + 1;

	// Resume reception, in case it was paused
	this->rx_waitc.notify_all();

}

void SerialOverEthernet::SOEPortHandler::handlePortTX() {
	// Init tx variables
	this->tx_stack = std::map<unsigned int, tx_entry>();
	this->next_txid = 0;

	// Start tx loop
	while (this->port->isOpen()) {

		// If not available, wait for more data
		std::unique_lock<std::mutex> lock(this->tx_stackm);
		if (!this->tx_stack.count(this->next_txid)) {
			this->tx_waitc.wait(lock, [this] {
				bool ready = this->tx_stack.count(this->next_txid) || !this->port->isOpen();
				if (!ready) dbgprintf("[DBG] waiting for txid: %u\n", this->next_txid);
				return ready;
			});
			if (!this->port->isOpen()) continue;
		}

		// Get next element from tx stack
		tx_entry* stackEntry = &this->tx_stack.at(this->next_txid);
		lock.unlock();
		dbgprintf("[DBG] [serial] <- |tx stack| <- network : [tx %u] size %lu len: %lu >%.*s\n", this->next_txid, this->tx_stack.size(), stackEntry->length, (int) stackEntry->length, stackEntry->payload.get());

		// Transmit data over serial
		unsigned long transmitted = 0;
		while (transmitted < stackEntry->length && this->port->isOpen()) {
			transmitted += this->port->writeBytes(stackEntry->payload.get() + transmitted, stackEntry->length - transmitted);
		}

		// Send transmission confirmation
		this->tx_confirm(this->next_txid);

		// Remove entry from tx stack and increment last txid
		lock.lock();
		this->tx_stack.erase(next_txid);
		this->next_txid++;

	}

	// Delete all remaining tx stack entries
	this->tx_stack.clear();
}

void SerialOverEthernet::SOEPortHandler::handlePortRX() {

	// Init rx variables
	this->next_free_rxid = 0;
	this->next_transmit_rxid = 0;
	this->last_transmitted_rxid = 0;
	this->rx_stack = std::map<unsigned int, rx_entry>();

	char receptionBuffer[SERIAL_RX_ENTRY_LEN];
	unsigned int inBuffer = 0;

	// Start rx loop
	while (this->port->isOpen()) {

		// Try read payload from serial, append on next free rx stack entry
		{
			std::unique_lock<std::mutex> lock(this->rx_stackm);

			// If next free entry does not yet exist, create
			if (!this->rx_stack.count(this->next_free_rxid)) {
				this->rx_stack[this->next_free_rxid] = {0UL, std::unique_ptr<char>(new char[SERIAL_RX_ENTRY_LEN] {0}), std::chrono::steady_clock::now(), false};
			// Else, if the element has reached its limit or can not hold data from buffer, create new entry
			} else if (this->rx_stack[this->next_free_rxid].length >= SERIAL_RX_ENTRY_LEN || inBuffer > (SERIAL_RX_ENTRY_LEN - this->rx_stack[this->next_free_rxid].length)) {
				// If the RX STACK has reached its limit, hold reception
				if (this->rx_stack.size() >= SERIAL_RX_STACK_LIMIT) {
					dbgprintf("[DBG] rx stack limit reached, reception hold: %lu entries\n", this->rx_stack.size());
					this->rx_waitc.wait(lock, [this]() { return this->rx_stack.size() < SERIAL_RX_STACK_LIMIT || !this->port->isOpen(); });
				}
				this->rx_stack[++this->next_free_rxid] = {0UL, std::unique_ptr<char>(new char[SERIAL_RX_ENTRY_LEN] {0}), std::chrono::steady_clock::now(), false};
			}

			// Check if data is already available in reception buffer and write to entry
			rx_entry& stackEntry = this->rx_stack[this->next_free_rxid];
			if (inBuffer > 0) {
				unsigned int entryCapacity = SERIAL_RX_ENTRY_LEN - stackEntry.length;
				if (entryCapacity < inBuffer) {
					continue; // redo loop the acquire new entry with existing buffer content
					printf("[!] internal error: rx empty entry payload overflow!\n");
					inBuffer = 0;
					// this should normally not happen since an fresh entry should have ben created above
					// we clear the buffer (and discard the received data) to prevent an total deadlock
				}
				memcpy(stackEntry.payload.get() + stackEntry.length, receptionBuffer, inBuffer);
				if (inBuffer > 0) dbgprintf("[DBG] |serial| -> [rx stack] -> network -> serial : [rx %u] size %lu len: %lu + %u >%.*s\n", this->next_free_rxid, this->rx_stack.size(), stackEntry.length, inBuffer, (int) (inBuffer + stackEntry.length), stackEntry.payload.get());
				stackEntry.length += inBuffer;
				inBuffer = 0;
			}

			// In case the reception buffer content filled up the new entry completely, redo loop to acquire new entry
			if (stackEntry.length == SERIAL_RX_ENTRY_LEN) continue;

			// Receive new data into reception buffer
			lock.unlock(); // ! Free the RX STACK to prevent it being blocked while waiting for data
			inBuffer = this->port->readBytesConsecutive(receptionBuffer, SERIAL_RX_ENTRY_LEN - stackEntry.length, SERIAL_RX_TIMEOUT_CONSEC, SERIAL_RX_TIMEOUT_CONSEC);

			lock.lock();
			// Ensure previous entry has not yet been send while waiting for data, if so, redo loop
			if (!this->rx_stack.count(this->next_free_rxid)) continue;

			// Copy data from reception buffer into the top entry
			if (inBuffer > SERIAL_RX_ENTRY_LEN - stackEntry.length) {
				printf("[!] internal error: rx entry payload overflow!\n");
				continue; // redo loop to acquire new entry with existing buffer content
				// this should normally not happen since only as much data is received as could fit in the free entry
			}
			memcpy(stackEntry.payload.get() + stackEntry.length, receptionBuffer, inBuffer);
			if (inBuffer > 0) dbgprintf("[DBG] |serial| -> [rx stack] -> network -> serial : [rx %u] size %lu len: %lu + %u >%.*s\n", this->next_free_rxid, this->rx_stack.size(), stackEntry.length, inBuffer, (int) (inBuffer + stackEntry.length), stackEntry.payload.get());
			stackEntry.length += inBuffer;
			inBuffer = 0;

		}

		// Notify client TX thread that new data is available
		this->new_data();

	}

	// Delete rx stack
	this->rx_stack.clear();
}
