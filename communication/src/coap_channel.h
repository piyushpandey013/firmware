/**
 ******************************************************************************
  Copyright (c) 2013-2015 Particle Industries, Inc.  All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */
#pragma once

#include "message_channel.h"
#include "coap.h"
#include "timer_hal.h"
#include "stdlib.h"

namespace particle
{
namespace protocol
{


template <typename T>
class CoAPChannel : public T
{
	message_id_t message_id;
	using base = T;

protected:

	message_id_t next_message_id()
	{
		return ++message_id;
	}

public:
	CoAPChannel(message_id_t msg_seed=0) : message_id(msg_seed)
	{
	}

	ProtocolError send(Message& msg) override
	{
		message_id_t id;
		if (msg.has_id())
		{
			id = msg.get_id();
		}
		else
		{
			id = next_message_id();
		}

		uint8_t* buf = msg.buf();
		buf[2] = id >> 8;
		buf[3] = id & 0xFF;

		return T::send(msg);
	}
};

/**
 * A CoAP message that is available for (re-)transmission.
 */
class __attribute__((packed)) CoAPMessage
{
	/**
	 * Messages are stored as a singly-linked list.
	 * This pointer is the next message in the list, or nullptr if this is the last message in the list.
	 */
	CoAPMessage* next;

	/**
	 * The time when the system will resend this message or give up sending
	 * when the maximum number of transmits has been reached.
	 */
	system_tick_t timeout;

	/**
	 * The unique 16-bit ID for this message.
	 */
	message_id_t id;

	/**
	 * The number of times this message has been transmitted.
	 * 0 means the message has not been sent yet.
	 */
	uint8_t transmit_count;

	// padding
	// uint8_t reserved;


	/**
	 * How many data bytes follow.
	 */
	uint16_t data_len;

	/**
	 * The CoAPMessage is dynamically allocated as a single chunk combining both the fields above and the message data.
	 */
	uint8_t data[0];


	static uint16_t message_count;
public:

	static const uint16_t ACK_TIMEOUT = 2000;
	static const uint16_t ACK_RANDOM_FACTOR = 1500;
	static const uint8_t MAX_RETRANSMIT = 4;


	/**
	 * The number of outstanding messages allowed.
	 */
	static const uint8_t NSTART = 1;


	CoAPMessage(message_id_t id_) : next(nullptr), timeout(0), id(id_), transmit_count(0), data_len(0) {
		message_count++;
	}

	/**
	 * Create a new CoAPMessage from the given Message instance. The returned CoAPMessage is dynamically allocated
	 * and has an independent lifetime from the Message
	 * instance. When no longer required, `delete` the CoAPMessage..
	 */
	static CoAPMessage* create(Message& msg)
	{
		size_t len = msg.length();
		uint8_t* memory = new uint8_t[sizeof(CoAPMessage)+len];
		if (memory) {
			CoAPMessage* coapmsg = new (memory)CoAPMessage(msg.get_id());		// in-place new
			coapmsg->set_data(msg.buf(), len);
			return coapmsg;
		}
		return nullptr;
	}

	~CoAPMessage()
	{
		message_count--;
	}

	static uint16_t messages() { return message_count; }

	inline CoAPMessage* get_next() const { return next; }
	inline void set_next(CoAPMessage* next) { this->next = next; }
	inline bool matches(message_id_t id) const { return this->id==id; }
	inline message_id_t get_id() const { return id; }
	inline void removed() { next = nullptr; }

	/**
	 * Prepares to retransmit this message after a timeout.
	 * @return false if the message cannot be retransmitted.
	 */
	bool prepare_retransmit(system_tick_t now)
	{
		timeout = now + transmit_timeout(transmit_count);
		transmit_count++;
		return transmit_count <= MAX_RETRANSMIT;
	}

	static inline system_tick_t transmit_timeout(uint8_t transmit_count)
	{
		system_tick_t timeout = (ACK_TIMEOUT << transmit_count);
		timeout += ((timeout * rand()%256)>>9);
		return timeout;
	}

	inline CoAPType::Enum type() const
	{
		return data_len>0 ? CoAP::type(data) : CoAPType::ERROR;
	}

	ProtocolError set_data(const uint8_t* data, size_t data_len)
	{
		if (data_len>1500)
			return IO_ERROR;
		memcpy(this->data, data, data_len);
		this->data_len = data_len;
		return NO_ERROR;
	}

	const uint8_t* get_data() const { return data; }
	uint16_t get_data_length() const { return data_len; }

};

inline bool time_has_passed(system_tick_t now, system_tick_t tick)
{
	static_assert(sizeof(system_tick_t)==4, "system_tick_t should be 4 bytes");
	if (now>=tick)
	{
		return now-tick <= 0x7FFFFFFF;
	}
	else
	{
		return tick-now >= 0x80000000;
	}
}



/**
 * A mix-in class that provides message resending for reliable delivery of messages.
 */
class CoAPMessageStore
{
	/**
	 * The head of the list of messages.
	 */
	CoAPMessage* head;

	/**
	 * Retrieves the message with the given ID and the previous message.
	 * If no message exists with the given id, nullptr is returned.
	 */
	CoAPMessage* for_id(message_id_t id, CoAPMessage*& prev)
	{
		prev = nullptr;
		CoAPMessage* next = head;
		while (next)
		{
			if (next->matches(id))
				return next;
			prev = next;
			next = next->get_next();
		}
		return nullptr;
	}

	/**
	 * Removes a message given the message to remove and the previous entry in the list.
	 */
	void remove(CoAPMessage* message, CoAPMessage* previous)
	{
		if (previous)
			previous->set_next(message->get_next());
		else
			head = message->get_next();
		message->removed();
	}

public:

	CoAPMessageStore() : head(nullptr) {}

	~CoAPMessageStore() {
		clear();
	}

	/**
	 * Retrieves the current confirmable message that is still
	 * waiting acknowledgement.
	 * Returns nullptr if there is no such unconfirmed message.
	 */
	CoAPMessage* from_id(message_id_t id)
	{
		CoAPMessage* prev;
		CoAPMessage* next = for_id(id, prev);
		return next;
	}

	ProtocolError add(CoAPMessage* message)
	{
		return add(*message);
	}

	/**
	 * Adds a message to this message store.
	 */
	ProtocolError add(CoAPMessage& message)
	{
		remove(message);
		if (message.get_next())
			return INVALID_STATE;
		message.set_next(head);
		head = &message;
		return NO_ERROR;
	}

	/**
	 * Removes a message from this message store.
	 * Returns true if the message existed, false otherwise.
	 */
	bool remove(CoAPMessage& msg)
	{
		return remove(msg.get_id())==&msg;
	}

	/**
	 * Removes a message from the store with the given id.
	 * Returns nullptr if the message does not exist. Returns
	 * the removed message otherwise.
	 */
	CoAPMessage* remove(message_id_t msg_id)
	{
		CoAPMessage* prev;
		CoAPMessage* msg = for_id(msg_id, prev);
		if (msg) {
			remove(msg, prev);
		}
		return msg;
	}

	bool is_confirmable(const uint8_t* buf)
	{
		return CoAP::type(buf)==CoAPType::CON;
	}

	/**
	 * Process existing messages, resending any unacknoweldged requests.
	 *
	 */
	void process(system_tick_t time)
	{

	}

	/**
	 * Registers that this message has been sent from the application.
	 */
	ProtocolError send(Message& msg, system_tick_t time)
	{
		if (!msg.has_id())
			return MISSING_MESSAGE_ID;

		if (is_confirmable(msg.buf()))
		{
			// confirmable message, create a CoAPMessage for this
			CoAPMessage* coapmsg = CoAPMessage::create(msg);
			if (coapmsg==nullptr)
				return INSUFFICIENT_STORAGE;
			add(*coapmsg);
		}
		return NO_ERROR;
	}

	/**
	 * Notifies the message store that a message has been received.
	 *
	 */
	ProtocolError receive(Message& msg)
	{
		CoAPType::Enum msgtype= msg.get_type();
		if (msgtype==CoAPType::ACK || msgtype==CoAPType::RESET)
		{
			msg.decode_id();
			message_id_t id = msg.get_id();
			clear_message(id);
		}
		return NO_ERROR;
	}

	void clear_message(message_id_t id)
	{
		delete remove(id);
	}


	/**
	 * Removes all knowledge of any messages.
	 */
	void clear()
	{
		while (head!=nullptr)
		{
			delete remove(head->get_id());
		}
	}

};

template <class T>
class CoAPReliableChannel : public T, CoAPMessageStore
{
	using channel = T;

	system_tick_t millis()
	{
		return HAL_Timer_Get_Milli_Seconds();
	}

public:

	ProtocolError establish() override
	{
		CoAPMessageStore::clear();
		return channel::establish();
	}

	/**
	 * Sends the message reliably. A non-confirmable message
	 * it is sent once. A confirmable message is sent and resent
	 * until an ack is received or the message times out.
	 */
	ProtocolError send(Message& msg) override
	{
		ProtocolError error = CoAPMessageStore::send(msg, millis());
		if (!error)
			error = channel::send(msg);

		// todo - for synchronous send, we can simply ignore all
		// received packets that are not the one we are waiting for
		// (or optionally cache them.)
		return error;
	}

	ProtocolError receive(Message& msg) override
	{
		ProtocolError error = channel::receive(msg);
		if (!error && msg.length())
			CoAPMessageStore::receive(msg);
		else
		{
			system_tick_t now = millis();
			CoAPMessageStore::process(now);
		}
		return error;
	}

};


}}