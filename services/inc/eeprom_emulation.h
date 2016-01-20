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
#include <utility>
#include "catch.hpp"

// TODO: harmonize terminology: sector/page/block
//                              offset/address
// TODO: harmonize types: uintptr_t/uint32_t

template <typename Store, uintptr_t Sector1, size_t Size1, uintptr_t Sector2, size_t Size2>
class EEPROMEmulation
{
public:
    using SectorSpan = std::pair<uintptr_t, size_t>;

    static const size_t Capacity = std::min(Size1, Size2);

    static const uint8_t FLASH_ERASED = 0xFF;

    struct SectorHeader {
        static const uint16_t ERASED = 0xFFFF;
        static const uint16_t VERIFIED = 0x0FFF;
        static const uint16_t COPY = 0x00FF;
        static const uint16_t ACTIVE = 0x0000;

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
                store.read(data, &output, sizeof(output));
                return true;
            }
        }
        return false;
    }

    template <typename T>
    bool put(uint16_t id, const T& input)
    {
        return writeRecord(getActiveSector(), id, &input, sizeof(input));
    }

    bool writeRecord(uint8_t sector, uint16_t id, const void *data, uint16_t length)
    {
        bool written = false;
        forEachRecord(sector, [&](uint32_t offset, const Header &header)
        {
            if(header.status == Header::EMPTY)
            {
                // TODO check for enough space
                Header header = {
                    Header::INVALID,
                    id,
                    length
                };

                // Write header
                uint32_t headerOffset = offset;
                store.write(headerOffset, &header, sizeof(header));

                // Write data
                uint32_t dataOffset = headerOffset + sizeof(header);
                store.write(dataOffset, data, length);

                // Write final valid status
                header.status = Header::VALID;
                store.write(offset, &header, sizeof(header.status));

                written = true;
            }
            // TODO sector swap when full
        });

        return written;
    }

    uint8_t getActiveSector()
    {
        // TODO
        return 1;
    }

    uint8_t getAlternateSector()
    {
        switch(getActiveSector())
        {
            case 1:
                return 2;
            default:
                return 1;
        }
    }


    bool findRecord(uint16_t id, uint16_t &length, uintptr_t &data)
    {
        bool found = false;
        forEachRecord(getActiveSector(), [&](uint32_t offset, const Header &header)
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
    void forEachRecord(uint8_t sector, Func f)
    {
        SectorSpan span = getSectorSpan(sector);
        uint32_t currentAddress = span.first;
        uint32_t lastAddress = span.second;

        // Skip sector header
        currentAddress += sizeof(SectorHeader);

        // Walk through record list
        while(currentAddress < lastAddress)
        {
            Header header;
            store.read(currentAddress, &header, sizeof(Header));
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
   
    SectorSpan getSectorSpan(uint8_t sector)
    {
        switch(sector)
        {
            case 1:
                return SectorSpan { Sector1, Sector1 + Size1 };
            case 2:
                return SectorSpan { Sector2, Sector2 + Size2 };
            default:
                // TODO: what's the correct error behavior?
                return SectorSpan { };
        }
    }

    // Verify that the entire sector is erased to protect against resets
    // during sector erase
    bool verifySector(uint8_t sector)
    {
        SectorSpan span = getSectorSpan(sector);

        SectorHeader header;
        store.read(span.first, &header, sizeof(header));

        if(header.status == SectorHeader::VERIFIED)
        {
            return true;
        }

        for(uintptr_t offset = span.first; offset < span.second; offset++)
        {
            if(*store.dataAt(offset) != FLASH_ERASED)
            {
                return false;
            }
        }
        
        header.status = SectorHeader::VERIFIED;
        store.write(span.first, &header, sizeof(header));
        return true;
    }

    void eraseSector(uint8_t sector)
    {
        SectorSpan span = getSectorSpan(sector);
        store.eraseSector(span.first);
    }

    bool swapSectors()
    {
        uint8_t activeSector = getActiveSector();
        uint8_t alternateSector = getAlternateSector();

        if(!verifySector(alternateSector))
        {
            eraseSector(alternateSector);
        }

        return false;
    }

    void copyAllRecordsToSector(uint8_t fromSector, uint8_t toSector, int32_t exceptRecordId = -1)
    {
        uint16_t currentLength = 0;
        const void *currentData = nullptr;
        uint16_t currentId;
        int32_t lastId = -1;
        bool newRecordFound;

        do
        {
            newRecordFound = false;
            currentId = UINT16_MAX;
            forEachRecord(fromSector, [&](uint32_t offset, const Header &header)
            {
                if(header.status == Header::VALID && header.id <= currentId && (int32_t)header.id > lastId && (int32_t)header.id != exceptRecordId)
                {
                    currentId = header.id;
                    currentLength = header.length;
                    currentData = store.dataAt(offset + sizeof(header));
                    newRecordFound = true;
                }
            });

            if(newRecordFound)
            {
                writeRecord(toSector, currentId, currentData, currentLength);
                lastId = currentId;
            }
        } while(newRecordFound);
    }

    Store store;
};
