/*
 * soeport.cpp
 *
 *  Created on: 04.02.2025
 *      Author: marvi
 */

#include <stdexcept>
#include "soeimpl.hpp"

using namespace std;

SOEPortHandler::SOEPortHandler(SerialPort* port, function<void(void)> newDataCallback, function<void(unsigned int)> txConfirmCallback) {
	this->port.reset(port);
	this->new_data = newDataCallback;
	this->tx_confirm = txConfirmCallback;

	this->thread_tx = thread([this]() -> void {
		handlePortTX();
	});
	this->thread_rx = thread([this]() -> void {
		handlePortRX();
	});
}

SOEPortHandler::~SOEPortHandler() {
	this->port->closePort();
	{ unique_lock<mutex> lock(this->tx_stackm); }
	this->tx_waitc.notify_all();
	{ unique_lock<mutex> lock(this->rx_stackm); }
	this->rx_waitc.notify_all();
	this->thread_tx.join();
	this->thread_rx.join();
}

bool SOEPortHandler::isOpen() {
	return this->port->isOpen();
}

bool SOEPortHandler::send(unsigned int txid, const char* buffer, unsigned long length) {

	// Do not insert any data if this port is shutting down
	if (!this->port->isOpen()) return false;

	// Ignore packages which txid is "in the past", just confirm that it has already been received
	if (txid < this->next_txid && this->next_txid - txid < 0x7FFFFFFF) return true;

	// If tx stack has reached limit, abort operation, only make an exception if this is the next package that should be send
	if (this->tx_stack.size() >= SERIAL_RX_STACK_LIMIT && txid != this->next_txid) return false;

	// Copy payload bytes and insert in tx stack at txid
	char* stackBuffer = new char[length];
	memcpy(stackBuffer, buffer, length);
	{
		lock_guard<mutex> lock(this->tx_stackm);
		this->tx_stack[txid] = {length, unique_ptr<char>(stackBuffer)};

#ifdef DEBUG_PRINTS
		printf("DEBUG: serial <- [tx stack] <- |network| : [tx %u] size %llu len: %lu\n", txid, this->tx_stack.size(), length);
#endif

		this->tx_waitc.notify_all();
	}

	return true;

}

bool SOEPortHandler::read(unsigned int* rxid, const char** buffer, unsigned long* length) {

	// Nothing to send if port is closed
	if (!this->port->isOpen()) return false;

	lock_guard<mutex> lock(this->rx_stackm);

	// Get element from rx stack and increment next free rxid to write data to
	if (this->rx_stack.count(this->next_transmit_rxid)) {
		rx_entry& stackEntry = this->rx_stack.at(this->next_transmit_rxid);
		if (stackEntry.length > 0) {
			*rxid = this->next_transmit_rxid++;
			*buffer = stackEntry.payload.get();
			*length = stackEntry.length;

			// Update time to resend the package in case reception is not confirmed first
			stackEntry.time_to_resend = chrono::steady_clock::now() + chrono::milliseconds(INET_TX_REP_INTERVAL);

			if (this->next_free_rxid < this->next_transmit_rxid)
				this->next_free_rxid = this->next_transmit_rxid;
			return true;
		}
	}

	// If currently no new data available, look for packages whichs reception might have failed
	for (unsigned int id = this->last_transmitted_rxid; id < this->next_transmit_rxid; id++) {
		if (this->rx_stack.count(id)) {
			rx_entry& stackEntry = this->rx_stack.at(id);
			if (stackEntry.rx_confirmed || stackEntry.time_to_resend >= chrono::steady_clock::now()) continue;
			*rxid = id;
			*buffer = stackEntry.payload.get();
			*length = stackEntry.length;

			// Update time to resend the package in case reception is not confirmed first
			stackEntry.time_to_resend = chrono::steady_clock::now() + chrono::milliseconds(INET_TX_REP_INTERVAL);

			return true;
		}
	}
	return false; // No more data

}

void SOEPortHandler::confirmReception(unsigned int rxid) {

	// Receiving a recption confirmation for an package that was not yet transmitted makes no sense
	if (rxid >= this->next_free_rxid) return;

	// Get entry from stack and mark as received
	try {
		lock_guard<mutex> lock(this->rx_stackm);
		rx_entry& stackEntry = this->rx_stack.at(rxid);

		// Mark package reception confirmed
		stackEntry.rx_confirmed = true;

#ifdef DEBUG_PRINTS
		printf("DEBUG: serial -> |rx stack| -> [network] -> serial : [rx %u] size %llu len: %lu\n", rxid, this->rx_stack.size(), stackEntry.length);
#endif

	} catch (std::out_of_range& e) {} // The entry did not exist, ignore

}

void SOEPortHandler::confirmTransmission(unsigned int rxid) {

	lock_guard<mutex> lock(this->rx_stackm);

	// Remove all packages before and including rxid, this ensures even packges whos tx confirm might be lost are cleared from the stack
	for (unsigned int i = this->last_transmitted_rxid; i != rxid + 1; i++) {
		this->rx_stack.erase(i);

#ifdef DEBUG_PRINTS
		printf("DEBUG: serial -> rx stack -> |network| -> [serial] : [rx %u] size %llu\n", i, this->rx_stack.size());
#endif

	}
	this->last_transmitted_rxid = rxid + 1;

	// Resume reception, in case it was paused
	this->rx_waitc.notify_all();

}

void SOEPortHandler::handlePortTX() {
	// Init tx variables
	this->tx_stack = map<unsigned int, tx_entry>();
	this->next_txid = 0;

	// Start tx loop
	while (this->port->isOpen()) {

		// If not available, wait for more data
		unique_lock<mutex> lock(this->tx_stackm);
		if (!this->tx_stack.count(this->next_txid)) {
			this->tx_waitc.wait(lock, [this] { return this->tx_stack.count(this->next_txid) || !this->port->isOpen(); });
			if (!this->port->isOpen()) continue;
		}

		// Get next element from tx stack
		tx_entry* stackEntry = &this->tx_stack.at(this->next_txid);
		lock.unlock();

#ifdef DEBUG_PRINTS
		printf("DEBUG: [serial] <- |tx stack| <- network : [tx %u] size %llu len: %lu\n", this->next_txid, this->tx_stack.size(), stackEntry->length);
#endif

		// Transmit data over serial
		unsigned long transmitted = 0;
		while (transmitted < stackEntry->length && this->port->isOpen()) {
			transmitted += this->port->writeBytes(stackEntry->payload.get() + transmitted, stackEntry->length - transmitted);
		}

		// Send transmission confirmation
		this->tx_confirm(this->next_txid);

		// Remove entry from tx stack and increment last txid
		{
			unique_lock<mutex> lock(this->tx_stackm);
			this->tx_stack.erase(next_txid);
			this->next_txid++;
		}

	}

	// Delete all remaining tx stack entries
	this->tx_stack.clear();
}

void SOEPortHandler::handlePortRX() {

	// Init rx variables
	this->next_free_rxid = 0;
	this->next_transmit_rxid = 0;
	this->last_transmitted_rxid = 0;
	this->rx_stack = map<unsigned int, rx_entry>();

	// Start rx loop
	while (this->port->isOpen()) {

		// Try read payload from serial, append on next free rx stack entry
		{
			unique_lock<mutex> lock(this->rx_stackm);

			// If next free entry does not yet exist, create
			if (!this->rx_stack.count(this->next_free_rxid)) {
				this->rx_stack[this->next_free_rxid] = {0UL, unique_ptr<char>(new char[SERIAL_RX_ENTRY_LEN] {0}), chrono::steady_clock::now(), false};
			// Else, if the element has reached its limit, create new entry
			} else if (this->rx_stack[this->next_free_rxid].length >= SERIAL_RX_ENTRY_LEN) {
				// If the RX STACK has reached its limit, hold reception
				if (this->rx_stack.size() >= SERIAL_RX_STACK_LIMIT) {
#ifdef DEBUG_PRINTS
					printf("DEBUG: [!] rx stack limit reached, reception hold: %llu entries\n", this->rx_stack.size());
#endif
					this->rx_waitc.wait(lock, [this]() { return this->rx_stack.size() < SERIAL_RX_STACK_LIMIT || !this->port->isOpen(); });
				}
				this->rx_stack[++this->next_free_rxid] = {0UL, unique_ptr<char>(new char[SERIAL_RX_ENTRY_LEN] {0}), chrono::steady_clock::now(), false};
			}

			// Read from serial into free rx entry, append to existing data
			rx_entry& stackEntry = this->rx_stack[this->next_free_rxid];
			lock.unlock(); // ! Free the RX STACK to prent it being blocked while waiting for data
			unsigned long received = this->port->readBytes(stackEntry.payload.get() + stackEntry.length, SERIAL_RX_ENTRY_LEN - stackEntry.length);
#ifdef DEBUG_PRINTS
			if (received != 0) printf("DEBUG: |serial| -> [rx stack] -> network -> serial : [rx %u] size %llu len: %lu + %lu\n", this->next_free_rxid, this->rx_stack.size(), stackEntry.length, received);
#endif
			stackEntry.length += received;

		}

		// Notify client TX thread that new data is available
		this->new_data();

	}

	// Delete rx stack
	this->rx_stack.clear();
}
