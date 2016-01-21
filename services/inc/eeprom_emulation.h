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

template <typename Store, uintptr_t SectorBase1, size_t SectorSize1, uintptr_t SectorBase2, size_t SectorSize2>
class EEPROMEmulation
{
public:
    using SectorSpan = std::pair<uintptr_t, size_t>;

    static const size_t Capacity = std::min(SectorSize1, SectorSize2);

    enum class LogicalSector
    {
        None,
        Sector1,
        Sector2
    };

    static const uint8_t FLASH_ERASED = 0xFF;

    struct __attribute__((packed)) SectorHeader
    {
        static const uint16_t ERASED = 0xFFFF;
        static const uint16_t COPY = 0x0FFF;
        static const uint16_t ACTIVE = 0x00FF;
        static const uint16_t INACTIVE = 0x000F;

        static const uint16_t LEGACY_VALID = 0x0000;

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

    bool writeRecord(LogicalSector sector, uint16_t id, const void *data, uint16_t length)
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

    LogicalSector getActiveSector()
    {
        uint16_t status1 = readSectorStatus(LogicalSector::Sector1);
        uint16_t status2 = readSectorStatus(LogicalSector::Sector2);

        // Pick the first active sector
        if(status1 == SectorHeader::ACTIVE)
        {
            return LogicalSector::Sector1;
        }
        else if(status2 == SectorHeader::ACTIVE)
        {
            return LogicalSector::Sector2;
        }
        // If the sector swap was interrupted just before the new sector
        // was marked active, all the data was written properly so use
        // the copy sector as the active sector
        else if(status1 == SectorHeader::COPY && status2 == SectorHeader::INACTIVE)
        {
            writeSectorStatus(LogicalSector::Sector1, SectorHeader::ACTIVE);
            return LogicalSector::Sector1;
        }
        else if(status1 == SectorHeader::INACTIVE && status2 == SectorHeader::COPY)
        {
            writeSectorStatus(LogicalSector::Sector2, SectorHeader::ACTIVE);
            return LogicalSector::Sector2;
        }
        else
        {
            return LogicalSector::None;
        }
    }

    LogicalSector getAlternateSector()
    {
        switch(getActiveSector())
        {
            case LogicalSector::Sector1:
                return LogicalSector::Sector2;
            default:
                return LogicalSector::Sector1;
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

    // Iterate through a sector and yield each record, including valid
    // and invalid records, and the empty record at the end (if there is
    // room)
    // TODO: what if there's no room for the empty record?
    template <typename Func>
    void forEachRecord(LogicalSector sector, Func f)
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

    // Iterate through a sector and yield each valid record, in
    // increasing order of id
    template <typename Func>
    void forEachValidRecord(LogicalSector sector, Func f)
    {
        uint16_t currentId;
        uint32_t currentAddress;
        int32_t lastId = -1;
        bool nextRecordFound;

        do
        {
            nextRecordFound = false;
            currentId = UINT16_MAX;
            forEachRecord(sector, [&](uint32_t offset, const Header &header)
            {
                if(header.status == Header::VALID && header.id <= currentId && (int32_t)header.id > lastId)
                {
                    currentId = header.id;
                    currentAddress = offset;
                    nextRecordFound = true;
                }
            });

            if(nextRecordFound)
            {
                Header header;
                store.read(currentAddress, &header, sizeof(Header));
                f(currentAddress, header);
                lastId = currentId;
            }
        } while(nextRecordFound);
    }
   
    SectorSpan getSectorSpan(LogicalSector sector)
    {
        switch(sector)
        {
            case LogicalSector::Sector1:
                return SectorSpan { SectorBase1, SectorBase1 + SectorSize1 };
            case LogicalSector::Sector2:
                return SectorSpan { SectorBase2, SectorBase2 + SectorSize2 };
            default:
                // TODO: what's the correct error behavior?
                return SectorSpan { };
        }
    }

    // Verify that the entire sector is erased to protect against resets
    // during sector erase
    bool verifySector(LogicalSector sector)
    {
        SectorSpan span = getSectorSpan(sector);

        for(uintptr_t offset = span.first; offset < span.second; offset++)
        {
            if(*store.dataAt(offset) != FLASH_ERASED)
            {
                return false;
            }
        }

        return true;
    }

    void eraseSector(LogicalSector sector)
    {
        SectorSpan span = getSectorSpan(sector);
        store.eraseSector(span.first);
    }

    uint16_t readSectorStatus(LogicalSector sector)
    {
        SectorSpan span = getSectorSpan(sector);
        SectorHeader header;
        store.read(span.first, &header, sizeof(header));
        return header.status;
    }

    void writeSectorStatus(LogicalSector sector, uint16_t status)
    {
        SectorSpan span = getSectorSpan(sector);
        SectorHeader header = { status };
        store.write(span.first, &header, sizeof(header));
    }

    void swapSectors()
    {
        LogicalSector activeSector = getActiveSector();
        LogicalSector alternateSector = getAlternateSector();

        if(!verifySector(alternateSector))
        {
            eraseSector(alternateSector);
        }

        writeSectorStatus(alternateSector, SectorHeader::COPY);

        copyAllRecordsToSector(activeSector, alternateSector);

        writeSectorStatus(activeSector, SectorHeader::INACTIVE);
        writeSectorStatus(alternateSector, SectorHeader::ACTIVE);
    }

    void copyAllRecordsToSector(LogicalSector fromSector, LogicalSector toSector, int32_t exceptRecordId = -1)
    {
        forEachValidRecord(fromSector, [&](uint32_t offset, const Header &header)
        {
            if((int32_t)header.id != exceptRecordId)
            {
                const void *currentData = store.dataAt(offset + sizeof(header));
                writeRecord(toSector, header.id, currentData, header.length);
            }
        });
    }

    // Since erasing a sector prevents the bus accessing the Flash
    // thus freezing the application code, provide an API for the user
    // application to figure out if a sector needs to be erased.
    // If the user application doesn't call eraseErasableSector(), then
    // at the next sector swap the page will be erase anyway
    LogicalSector getErasableSector()
    {
        LogicalSector alternateSector = getAlternateSector();
        if(readSectorStatus(alternateSector) != SectorHeader::ERASED)
        {
            return alternateSector;
        }
        else
        {
            return LogicalSector::None;
        }
    }

    bool hasErasableSector()
    {
        return getErasableSector() != LogicalSector::None;
    }

    void eraseErasableSector()
    {
        if(hasErasableSector())
        {
            eraseSector(getErasableSector());
        }
    }

    Store store;
};
