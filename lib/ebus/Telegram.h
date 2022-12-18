/*
 * Copyright (C) Roland Jax 2012-2022 <roland.jax@liwest.at>
 *
 * This file is part of ebus.
 *
 * ebus is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ebus is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ebus. If not, see http://www.gnu.org/licenses/.
 */

#ifndef EBUS_TELEGRAM_H
#define EBUS_TELEGRAM_H

#include <cstddef>
#include <string>
#include <vector>

#include "Sequence.h"

namespace ebus
{

#define SEQ_EMPTY         99 // sequence is empty

#define SEQ_OK             0 // success

#define SEQ_ERR_SHORT     -1 // sequence is too short
#define SEQ_ERR_LONG      -2 // sequence is too long
#define SEQ_ERR_NN        -3 // data byte number is invalid
#define SEQ_ERR_CRC       -4 // sequence has a CRC error
#define SEQ_ERR_ACK       -5 // acknowledge byte is invalid
#define SEQ_ERR_QQ        -6 // source address is invalid
#define SEQ_ERR_ZZ        -7 // target address is invalid
#define SEQ_ERR_ACK_MISS  -8 // acknowledge byte is missing
#define SEQ_ERR_INVALID   -9 // sequence is invalid

enum class Type
{
	undefined,           // default value
	BC,                  // broadcast
	MM,                  // master master
	MS                   // master slave
};

static const uint8_t seq_ack = uint8_t(0x00);   // positive acknowledge
static const uint8_t seq_nak = uint8_t(0xff);   // negative acknowledge
static const uint8_t seq_broad = uint8_t(0xfe); // broadcast destination address

static const int seq_max_bytes = 16;                // 16 maximum data bytes

class Telegram
{

public:
	Telegram() = default;
	explicit Telegram(Sequence &seq);

	void parse(Sequence &seq);

	void createMaster(const uint8_t src, const std::vector<uint8_t> &vec);
	void createMaster(Sequence &seq);

	void createSlave(const std::vector<uint8_t> &vec);
	void createSlave(Sequence &seq);

	void clear();

	uint8_t getMasterQQ() const;

	Sequence getMaster() const;
	uint8_t getMasterCRC() const;
	int getMasterState() const;

	void setSlaveACK(const uint8_t byte);

	Sequence getSlave() const;
	uint8_t getSlaveCRC() const;
	int getSlaveState() const;

	void setMasterACK(const uint8_t byte);

	ebus::Type get_type() const;

	bool isValid() const;

	const std::string to_string();
	const std::string toStringMaster();
	const std::string toStringSlave();

	static bool isMaster(const uint8_t byte);
	static bool isSlave(const uint8_t byte);
	static uint8_t slaveAddress(const uint8_t address);

private:
	Type m_type = Type::undefined;

	Sequence m_master;
	size_t m_masterNN = 0;
	uint8_t m_masterCRC = seq_zero;
	int m_masterState = SEQ_EMPTY;

	uint8_t m_slaveACK = seq_zero;

	Sequence m_slave;
	size_t m_slaveNN = 0;
	uint8_t m_slaveCRC = seq_zero;
	int m_slaveState = SEQ_EMPTY;

	uint8_t m_masterACK = seq_zero;

	const std::string errorText(const int error);

	const std::string toStringMasterError();
	const std::string toStringSlaveError();

	void set_type(const uint8_t byte);
	bool isAddressValid(const uint8_t byte);
	int checkMasterSequence(Sequence &seq);
	int checkSlaveSequence(Sequence &seq);
};

} // namespace ebus

#endif // EBUS_TELEGRAM_H

