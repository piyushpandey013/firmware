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

#include <cstdint>
#include <cstddef>
#include "protocol_defs.h"
#include "coap.h"

namespace particle
{
namespace protocol
{


class Message
{
	template<size_t max, size_t prefix, size_t suffix>
	friend class BufferMessageChannel;

	uint8_t* buffer;
	size_t buffer_length;
	size_t message_length;
    int id;                     // if < 0 then not-defined.

	size_t trim_capacity()
	{
		size_t trimmed = buffer_length-message_length;
		buffer_length = message_length;
		return trimmed;
	}

	size_t buffer_available() const { return buffer_length-message_length; }

	bool splinter(Message& target, size_t size_required, size_t offset)
	{
		size_t available = buffer_available();
		if (available<(size_required+offset))
			return false;

		int excess = trim_capacity();
		target.set_buffer(buf()+length()+offset, excess);
		return true;
	}

public:
	Message() : Message(nullptr, 0, 0) {}

	Message(uint8_t* buf, size_t buflen, size_t msglen) : buffer(buf), buffer_length(buflen), message_length(msglen), id(-1) {}

	void clear() { id = -1; }

	size_t capacity() const { return buffer_length; }
	uint8_t* buf() const { return buffer; }
	size_t length() const { return message_length; }

	void set_length(size_t length) { if (length<=buffer_length) message_length = length; }
	void set_buffer(uint8_t* buffer, size_t length) { this->buffer = buffer; buffer_length = length; message_length = 0; }

    void set_id(message_id_t id) { this->id = id; }
    bool has_id() { return id>=0; }
    message_id_t get_id() { return message_id_t(id); }

    CoAPType::Enum get_type()
    {
    		return length() ? CoAP::type(buf()): CoAPType::ERROR;
    }

    bool decode_id()
    {
    		bool decode = (length()>=4);
    		if (decode)
    			set_id(CoAP::message_id(buf()));
    		return decode;
    }
};

/**
 * A message channel represents a way to send and receive messages with an endpoint.
 *
 * Note that the implementation may use a shared message buffer for all
 * message operations. The only operation that does not invalidate an existing
 * message is MessageChannel::response() since this allocates the new message at the end of the existing one.
 *
 */
class MessageChannel
{

public:
	virtual ~MessageChannel() {}

	virtual bool is_unreliable()=0;

	virtual ProtocolError establish()=0;

	/**
	 * Retrieves a new message object containing the message buffer.
	 */
	virtual ProtocolError create(Message& message, size_t minimum_size=0)=0;

	/**
	 * Fetch the next message from the channel.
	 * If no message is ready, a message of size 0 is returned.
	 *
	 * @return an error value !=0 on error.
	 */
	virtual ProtocolError receive(Message& message)=0;

	/**
	 * Send the given message to the endpoint
	 * @return an error value !=0 on error.
	 */
	virtual ProtocolError send(Message& msg)=0;

	/**
	 * Fill out a message struct to contain storage for a response.
	 */
	virtual ProtocolError response(Message& original, Message& response, size_t required)=0;
};

class AbstractMessageChannel : public MessageChannel
{

};


}}