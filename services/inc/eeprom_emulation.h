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

// FIXME: Remove this comment implementation is done
// terminology: use sector, not page or block
//              use offset, not address
//              use uintptr_t for offset, size_t for sector sizes

// TODO: use the return value of store.write() to verify that data was
// written properly?

// TODO: think about alignment of records, and the impact
// FLASH_ProgramHalfWord / FLASH_ProgramByte on the robustness

template <typename Store, uintptr_t SectorBase1, size_t SectorSize1, uintptr_t SectorBase2, size_t SectorSize2>
class EEPROMEmulation
{
public:
    static constexpr size_t Capacity = (SectorSize1 < SectorSize2) ? SectorSize1 : SectorSize2;

    enum class LogicalSector
    {
        NoSector,
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

        static const uint16_t LEGACY_ACTIVE = 0x0000;

        uint16_t status;
    };

    struct __attribute__((packed)) Header
    {
        static const uint16_t EMPTY = 0xFFFF;
        static const uint16_t INVALID = 0x0FFF;
        static const uint16_t VALID = 0x00FF;
        static const uint16_t REMOVED = 0x000F;

        static const uint16_t EMPTY_LENGTH = 0xFFFF;

        uint16_t status;
        uint16_t id;
        uint16_t length;
    };

    /* Public API */

    // Initialize the EEPROM sectors
    // Call at boot
    void init()
    {
        if(getActiveSector() == LogicalSector::NoSector)
        {
            clear();
        }

        // If there's a pending erase after a sector swap, do it at boot
        performPendingErase();
    }

    // Read the latest value of a record
    // Returns false if the records was not found, true if read
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
            // TODO
            // else if length == 1 and sizeof(output) != 1
            // ==> convert from legacy (1 byte per address) to new format
        }
        return false;
    }

    // Writes a new value for a record
    // Performs a sector swap (move all valid records to a new sector)
    // if the current sector is full
    // Returns false if there is not enough capacity to write this
    // record (even after a sector swap), true if record was written
    template <typename T>
    bool put(uint16_t id, const T& input)
    {
        // don't create a new record if identical to previous record
        if(verify(id, input))
        {
            return true;
        }

        size_t recordSize = sizeof(Header) + sizeof(input);

        if(recordSize > remainingCapacity(id))
        {
            return false;
        }

        writeRecord(getActiveSector(), id, &input, sizeof(input));
        return true;
    }

    // Destroys all the data 💣
    void clear()
    {
        eraseSector(LogicalSector::Sector1);
        eraseSector(LogicalSector::Sector2);
        writeSectorStatus(LogicalSector::Sector1, SectorHeader::ACTIVE);
    }

    // Mark a record as removed to free up some capacity at next sector swap
    // Return false if the record was not found, true if it was removed.
    bool remove(uint16_t id)
    {
        bool removed = false;
        forEachRecord(getActiveSector(), [&](uintptr_t offset, const Header &header)
        {
            if(header.id == id)
            {
                uint16_t status = Header::REMOVED;
                store.write(offset, &status, sizeof(status));
                removed = true;
            }
        });

        return removed;
    }

    // The total space available to write records.
    //
    // Note: The amount of data that fits is actually smaller since each
    // record has a header
    size_t totalCapacity()
    {
        return Capacity - sizeof(SectorHeader);
    }

    // The space currently used by records if the EEPROM was compacted
    // Optionally don't count record exceptRecordId
    size_t usedCapacity(int32_t exceptRecordId = -1)
    {
        size_t capacity = 0;
        forEachValidRecord(getActiveSector(), [&](uintptr_t offset, const Header &header)
        {
            if((int32_t)header.id != exceptRecordId)
            {
                capacity += sizeof(Header) + header.length;
            }
        });
        return capacity;
    }

    // The space left to write new records
    // Optionally don't count record exceptRecordId
    //
    // Note: The amount of data that fits is actually smaller since each
    // record has a header
    size_t remainingCapacity(int32_t exceptRecordId = -1)
    {
        return totalCapacity() - usedCapacity(exceptRecordId);
    }

    // How many valid records are currently stored
    uint16_t countRecords()
    {
        uint16_t count = 0;
        forEachValidRecord(getActiveSector(), [&](uintptr_t offset, const Header &header)
        {
            count++;
        });
        return count;
    }

    // Get the ids of the valid records currently stored
    // Returns the number of ids written to the recordIds array
    uint16_t listRecords(uint16_t *recordIds, uint16_t maxRecordIds)
    {
        uint16_t count = 0;
        forEachValidRecord(getActiveSector(), [&](uintptr_t offset, const Header &header)
        {
            if(count < maxRecordIds)
            {
                recordIds[count] = header.id;
                count++;
            }
        });
        return count;
    }

    // Since erasing a sector prevents the bus accessing the Flash memory
    // thus freezing the application code, provide an API for the user
    // application to figure out if a sector needs to be erased.
    // If the user application doesn't call performPendingErase(), then
    // at the next reboot or next sector swap the sector will be erased anyway
    bool hasPendingErase()
    {
        return getPendingEraseSector() != LogicalSector::NoSector;
    }

    // Erases the old sector after a sector swap, if necessary
    void performPendingErase()
    {
        if(hasPendingErase())
        {
            eraseSector(getPendingEraseSector());
        }
    }

    /* Implementation */

    constexpr uintptr_t getSectorStart(LogicalSector sector)
    {
        switch(sector)
        {
            case LogicalSector::Sector1: return SectorBase1;
            case LogicalSector::Sector2: return SectorBase2;
            default: return 0;
        }
    }

    constexpr uintptr_t getSectorEnd(LogicalSector sector)
    {
        switch(sector)
        {
            case LogicalSector::Sector1: return SectorBase1 + SectorSize1;
            case LogicalSector::Sector2: return SectorBase2 + SectorSize2;
            default: return 0;
        }
    }

    constexpr uintptr_t getSectorSize(LogicalSector sector)
    {
        switch(sector)
        {
            case LogicalSector::Sector1: return SectorSize1;
            case LogicalSector::Sector2: return SectorSize2;
            default: return 0;
        }
    }

    // Check if the existing value of a record matches the new value
    template <typename T>
    bool verify(uint16_t id, const T& input)
    {
        T existing;
        if(get(id, existing))
        {
            return std::memcmp(&input, &existing, sizeof(input)) == 0;
        }
        else
        {
            return false;
        }
    }

    // The offset to the first empty record, or the end of the sector if
    // no records are empty
    uintptr_t findEmptyOffset(LogicalSector sector)
    {
        uintptr_t freeOffset = getSectorEnd(sector);
        forEachRecord(sector, [&](uintptr_t offset, const Header &header)
        {
            if(header.status == Header::EMPTY)
            {
                freeOffset = offset;
            }
        });
        return freeOffset;
    }

    // TODO: write description
    void writeRecord(LogicalSector sector, uint16_t id, const void *data, uint16_t length)
    {
        uintptr_t freeOffset = findEmptyOffset(sector);
        size_t spaceRemaining = getSectorEnd(sector) - freeOffset;

        size_t recordSize = sizeof(Header) + length;

        // TODO: move this check to avoid possibility of calling swapSectors within swapSectors :-S
        if(spaceRemaining < recordSize)
        {
            swapSectors(id);
            // write record on new active sector
            writeRecord(getActiveSector(), id, data, length);
            return;
        }

        Header header = {
            Header::INVALID,
            id,
            length
        };

        // Write header
        uintptr_t headerOffset = freeOffset;
        store.write(headerOffset, &header, sizeof(header));

        // Write data
        uintptr_t dataOffset = headerOffset + sizeof(header);
        store.write(dataOffset, data, length);

        // Write final valid status
        header.status = Header::VALID;
        store.write(freeOffset, &header, sizeof(header.status));
    }

    // Which sector should currently be read from/written to
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
            return LogicalSector::NoSector;
        }
    }

    // Which sector should be used as the target for the next swap
    LogicalSector getAlternateSector()
    {
        return getActiveSector() == LogicalSector::Sector1 ? LogicalSector::Sector2 : LogicalSector::Sector1;
    }

    // Iterate through a sector to find the latest valid record with a
    // specified id
    bool findRecord(uint16_t id, uint16_t &length, uintptr_t &data)
    {
        bool found = false;
        forEachRecord(getActiveSector(), [&](uintptr_t offset, const Header &header)
        {
            if(header.status == Header::VALID && header.id == id)
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
    template <typename Func>
    void forEachRecord(LogicalSector sector, Func f)
    {
        uintptr_t currentOffset = getSectorStart(sector);
        uintptr_t lastOffset = getSectorEnd(sector);

        // Skip sector header
        currentOffset += sizeof(SectorHeader);

        // Walk through record list
        while(currentOffset < lastOffset)
        {
            Header header;
            store.read(currentOffset, &header, sizeof(Header));

            // Yield record
            f(currentOffset, header);

            // End of data
            if(header.status == Header::EMPTY)
            {
                return;
            }

            // Skip over data if the header was properly written
            if(header.length != Header::EMPTY_LENGTH)
            {
                currentOffset += header.length;
            }

            // Skip over record header
            currentOffset += sizeof(header);
        }
    }

    // Iterate through a sector and yield each valid record, in
    // increasing order of id
    template <typename Func>
    void forEachValidRecord(LogicalSector sector, Func f)
    {
        uint16_t currentId;
        uintptr_t currentOffset;
        int32_t previousId = -1;
        bool nextRecordFound;

        do
        {
            nextRecordFound = false;
            currentId = UINT16_MAX;
            forEachRecord(sector, [&](uintptr_t offset, const Header &header)
            {
                if(header.status == Header::VALID && header.id <= currentId && (int32_t)header.id > previousId)
                {
                    currentId = header.id;
                    currentOffset = offset;
                    nextRecordFound = true;
                }
            });

            if(nextRecordFound)
            {
                Header header;
                store.read(currentOffset, &header, sizeof(Header));
                f(currentOffset, header);
                previousId = currentId;
            }
        } while(nextRecordFound);
    }
   
    // Verify that the entire sector is erased to protect against resets
    // during sector erase
    bool verifySector(LogicalSector sector)
    {
        const uint8_t *begin = store.dataAt(getSectorStart(sector));
        const uint8_t *end = store.dataAt(getSectorEnd(sector));
        while(begin < end)
        {
            if(*begin++ != FLASH_ERASED)
            {
                return false;
            }
        }

        return true;
    }

    // Reset entire sector to 0xFF
    void eraseSector(LogicalSector sector)
    {
        store.eraseSector(getSectorStart(sector));
    }

    // Get the current status of a sector (empty, active, being copied, ...)
    uint16_t readSectorStatus(LogicalSector sector)
    {
        SectorHeader header;
        store.read(getSectorStart(sector), &header, sizeof(header));
        return header.status;
    }

    // Update the status of a sector
    void writeSectorStatus(LogicalSector sector, uint16_t status)
    {
        SectorHeader header = { status };
        store.write(getSectorStart(sector), &header, sizeof(header));
    }

    // Write all valid records from the active sector to the alternate
    // sector. Erase the alternate sector if it is not already erased.
    // 
    // Optionally don't copy record exceptRecordId
    void swapSectors(int32_t exceptRecordId = -1)
    {
        LogicalSector activeSector = getActiveSector();
        LogicalSector alternateSector = getAlternateSector();

        // TODO: add a loop here 2 tries and validation to protect against
        // marginal erase? If a sector was kind of erased and reads back as
        // all 0xFF but when values are written some bits are actually 0.
        if(!verifySector(alternateSector))
        {
            eraseSector(alternateSector);
        }

        writeSectorStatus(alternateSector, SectorHeader::COPY);

        copyAllRecordsToSector(activeSector, alternateSector, exceptRecordId);

        writeSectorStatus(activeSector, SectorHeader::INACTIVE);
        writeSectorStatus(alternateSector, SectorHeader::ACTIVE);
    }

    // Perform the actual copy of records during sector swap
    void copyAllRecordsToSector(LogicalSector fromSector, LogicalSector toSector, int32_t exceptRecordId = -1)
    {
        forEachValidRecord(fromSector, [&](uintptr_t offset, const Header &header)
        {
            if((int32_t)header.id != exceptRecordId)
            {
                const void *currentData = store.dataAt(offset + sizeof(header));
                writeRecord(toSector, header.id, currentData, header.length);
            }
        });
    }

    // Which sector needs to be erased after a sector swap.
    LogicalSector getPendingEraseSector()
    {
        LogicalSector alternateSector = getAlternateSector();
        if(readSectorStatus(alternateSector) != SectorHeader::ERASED)
        {
            return alternateSector;
        }
        else
        {
            return LogicalSector::NoSector;
        }
    }

    // Hardware-dependent interface to read, erase and program memory
    Store store;
};
