/**
 ******************************************************************************
 * @file    eeprom_emulation.h
 * @author  Julien Vanier
 ******************************************************************************
  Copyright (c) 2016 Particle Industries, Inc.  All rights reserved.

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

#include <stddef.h>
#include <cstring>
#include "catch.hpp"

template <typename Store, uintptr_t Sector1, size_t Size1, uintptr_t Sector2, size_t Size2>
class EEPROMEmulation
{
public:
    struct BlockHeader {
        static const uint16_t BLOCK_ERASED = 0xFFFF;
        static const uint16_t BLOCK_VERIFIED = 0x0FFF;
        static const uint16_t BLOCK_COPY = 0x00FF;
        static const uint16_t BLOCK_ACTIVE = 0x0000;

        uint16_t status;
    };

    struct __attribute__((packed)) Header
    {
        static const uint32_t EMPTY = 0xFFFF;
        static const uint32_t INVALID = 0x00FF;
        static const uint32_t VALID = 0x0000;

        static const uint32_t EMPTY_LENGTH = 0xFFFF;

        uint16_t status;
        uint16_t id;
        uint16_t length;
    };

    template <typename T>
    bool get(uint16_t id, T& output)
    {
        uint16_t length;
        uintptr_t data;
        if(findRecord(id, length, data))
        {
            if(length == sizeof(output))
            {
                store.read(&output, data, sizeof(output));
                return true;
            }
        }
        return false;
    }

    template <typename T>
    bool put(uint16_t id, const T& input)
    {
        bool written = false;
        forEachRecord([&](uint32_t offset, const Header &header)
        {
            if(header.status == Header::EMPTY)
            {
                // TODO check for enough space
                Header header = {
                    Header::INVALID,
                    id,
                    sizeof(input)
                };

                // Write header
                store.write(offset, &header, sizeof(header));

                // Write data
                store.write(offset + sizeof(header), &input, sizeof(input));

                // Write final valid status
                header.status = Header::VALID;
                store.write(offset, &header, sizeof(header.status));

                written = true;
            }
            // TODO page swap when full
        });

        return written;
    }

    uint8_t getActivePage()
    {
        // TODO
        return 1;
    }

    bool findRecord(uint16_t id, uint16_t &length, uintptr_t &data)
    {
        bool found = false;
        forEachRecord([&](uint32_t offset, const Header &header)
        {
            if(header.id == id)
            {
                length = header.length;
                data = offset + sizeof(header);
                found = true;
            }
        });
        return found;
    }

    template <typename Func>
    void forEachRecord(Func f)
    {
        uint32_t currentAddress;
        uint32_t lastAddress;

        switch(getActivePage())
        {
            case 1:
                currentAddress = Sector1;
                lastAddress = Sector1 + Size1;
                break;
            case 2:
                currentAddress = Sector2;
                lastAddress = Sector2 + Size2;
                break;
            default:
                // TODO: what's the correct error behavior?
                return;
        }

        // Skip block header
        currentAddress += sizeof(BlockHeader);

        // Walk through record list
        while(currentAddress < lastAddress)
        {
            Header header;
            store.read(&header, currentAddress, sizeof(Header));
            f(currentAddress, header);

            // End of data
            if(header.status == Header::EMPTY)
            {
                return;
            }

            // Skip over data if the header was properly written
            if(header.length != Header::EMPTY_LENGTH)
            {
                //WARN("Skipping length " << header.length);
                currentAddress += header.length;
            }

            // Skip over record header
            //WARN("Skipping header " << sizeof(header));
            currentAddress += sizeof(header);
        }
    }

    Store store;
};
