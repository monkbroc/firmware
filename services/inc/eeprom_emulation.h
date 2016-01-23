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

// FIXME: Remove this comment implementation is done
// terminology: use sector, not page or block
//              use offset, not address
//              use uintptr_t for offset, size_t for sector sizes

// TODO: think about alignment of records, and the impact
// FLASH_ProgramHalfWord / FLASH_ProgramByte on the robustness
// ==> Conclusion: No impact.

// TODO: add API to support external iteration through valid records

template <typename Store, uintptr_t SectorBase1, size_t SectorSize1, uintptr_t SectorBase2, size_t SectorSize2>
class EEPROMEmulation
{
public:
    static constexpr size_t SmallestSectorSize = (SectorSize1 < SectorSize2) ? SectorSize1 : SectorSize2;

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

        // TODO: use this for migration
        static const uint16_t LEGACY_ACTIVE = 0x0000;

        uint16_t status;
    };

    struct __attribute__((packed)) Header
    {
        // TODO: does it matter if status is uint8_t or uint16_t? impact
        // on alignment?
        static const uint8_t EMPTY = 0xFF;
        static const uint8_t INVALID = 0x7F;
        static const uint8_t VALID = 0x0F;
        static const uint8_t REMOVED = 0x07;

        static const uint16_t EMPTY_LENGTH = 0xFFFF;

        uint8_t status;
        uint16_t length;
        uint16_t id;
    };

    /* Public API */

    // Initialize the EEPROM sectors
    // Call at boot
    void init()
    {
        updateActiveSector();

        if(getActiveSector() == LogicalSector::NoSector)
        {
            clear();
        }

        // If there's a pending erase after a sector swap, do it at boot
        performPendingErase();

        calculateCapacity();
    }

    // Read the latest value of a record
    // Returns false if the records was not found, true if read
    template <typename T>
    bool get(uint16_t id, T& output)
    {
        uint16_t length;
        uintptr_t dataOffset;
        if(findRecord(id, length, dataOffset))
        {
            if(length == sizeof(output))
            {
                store.read(dataOffset, &output, sizeof(output));
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
        // Don't create a new record if identical to previous record
        uint16_t previousLength = 0;
        uintptr_t dataOffset = 0;
        if(findRecord(id, previousLength, dataOffset))
        {
            if(identicalValues(input, previousLength, dataOffset))
            {
                return true;
            }
            size_t previousRecordSize = sizeof(Header) + previousLength;
            updateCapacity(-previousRecordSize);
        }

        size_t recordSize = sizeof(Header) + sizeof(input);

        // If the new record wouldn't fit even if the current record
        // were to be removed from the storage by a page swap
        if(recordSize > remainingCapacity())
        {
            return false;
        }

        if(!writeRecord(getActiveSector(), id, &input, sizeof(input)))
        {
            return swapSectorsAndWriteRecord(id, &input, sizeof(input));
        }

        updateCapacity(recordSize);
        return true;
    }

    // Destroys all the data 💣
    void clear()
    {
        eraseSector(LogicalSector::Sector1);
        eraseSector(LogicalSector::Sector2);
        writeSectorStatus(LogicalSector::Sector1, SectorHeader::ACTIVE);

        updateActiveSector();
        calculateCapacity();
    }

    // Mark a record as removed to free up some capacity at next sector swap
    // Return false if the record was not found, true if it was removed.
    bool remove(uint16_t id)
    {
        bool removed = false;
        uint16_t currentLength = 0;
        forEachRecord(getActiveSector(), [&](uintptr_t offset, const Header &header)
        {
            if(header.id == id)
            {
                Header header = { Header::REMOVED, 0, 0 };
                store.write(offset, &header, sizeof(header.status));
                removed = true;
                currentLength = header.length;
            }
        });

        updateCapacity(-currentLength);

        return removed;
    }

    // The total space available to write records.
    //
    // Note: The amount of data that fits is actually smaller since each
    // record has a header
    size_t totalCapacity()
    {
        return SmallestSectorSize - sizeof(SectorHeader);
    }

    // The space currently used by all valid records
    size_t usedCapacity()
    {
        return capacity;
    }

    // The space left to write new records
    //
    // Note: The amount of data that fits is actually smaller since each
    // record has a header
    size_t remainingCapacity()
    {
        return totalCapacity() - usedCapacity();
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

    uintptr_t getSectorStart(LogicalSector sector)
    {
        switch(sector)
        {
            case LogicalSector::Sector1: return SectorBase1;
            case LogicalSector::Sector2: return SectorBase2;
            default: return 0;
        }
    }

    uintptr_t getSectorEnd(LogicalSector sector)
    {
        switch(sector)
        {
            case LogicalSector::Sector1: return SectorBase1 + SectorSize1;
            case LogicalSector::Sector2: return SectorBase2 + SectorSize2;
            default: return 0;
        }
    }

    uintptr_t getSectorSize(LogicalSector sector)
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
    bool identicalValues(const T& input, uint16_t length, uintptr_t dataOffset)
    {
        bool sameLength = sizeof(input) == length;
        bool sameData = std::memcmp(&input, store.dataAt(dataOffset), sizeof(input)) == 0;
        return sameLength && sameData;
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

    // Write a record to the first empty space available in a sector
    //
    // Returns false when write was unsuccessful to protect against
    // marginal erase, true on proper write
    bool writeRecord(LogicalSector sector, uint16_t id, const void *data, uint16_t length)
    {
        uintptr_t freeOffset = findEmptyOffset(sector);
        size_t spaceRemaining = getSectorEnd(sector) - freeOffset;

        size_t recordSize = sizeof(Header) + length;

        // No more room for record
        if(spaceRemaining < recordSize)
        {
            return false;
        }

        Header header = {
            Header::INVALID,
            length,
            id
        };

        // Write header
        uintptr_t headerOffset = freeOffset;
        if(store.write(headerOffset, &header, sizeof(header)) < 0)
        {
            return false;
        }

        // Write data
        uintptr_t dataOffset = headerOffset + sizeof(header);
        if(store.write(dataOffset, data, length) < 0)
        {
            return false;
        }

        // Write final valid status
        header.status = Header::VALID;
        if(store.write(headerOffset, &header, sizeof(header.status)) < 0)
        {
            return false;
        }

        return true;
    }

    // Figure out which sector should currently be read from/written to
    // and which one should be used as the target of the sector swap
    void updateActiveSector()
    {
        uint16_t status1 = readSectorStatus(LogicalSector::Sector1);
        uint16_t status2 = readSectorStatus(LogicalSector::Sector2);

        // Pick the first active sector
        if(status1 == SectorHeader::ACTIVE)
        {
            activeSector = LogicalSector::Sector1;
            alternateSector = LogicalSector::Sector2;
        }
        else if(status2 == SectorHeader::ACTIVE)
        {
            activeSector = LogicalSector::Sector2;
            alternateSector = LogicalSector::Sector1;
        }
        else
        {
            activeSector = LogicalSector::NoSector;
            alternateSector = LogicalSector::NoSector;
        }
    }

    // Which sector should currently be read from/written to
    LogicalSector getActiveSector()
    {
        return activeSector;
    }

    // Which sector should be used as the target for the next swap
    LogicalSector getAlternateSector()
    {
        return alternateSector;
    }

    // Iterate through a sector to find the latest valid record with a
    // specified id
    // Returns true if a record is found and puts length and data in the
    // parameters references
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
                if(header.status == Header::VALID &&
                        header.id <= currentId &&
                        (int32_t)header.id > previousId)
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

                // Yield record
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
    bool writeSectorStatus(LogicalSector sector, uint16_t status)
    {
        SectorHeader header = { status };
        return store.write(getSectorStart(sector), &header, sizeof(header)) == 0;
    }

    // Write all valid records from the active sector to the alternate
    // sector. Erase the alternate sector if it is not already erased.
    // Then write the new record to the alternate sector.
    bool swapSectorsAndWriteRecord(uint16_t id, const void *data, uint16_t length)
    {
        LogicalSector sourceSector = getActiveSector();
        LogicalSector destinationSector = getAlternateSector();

        // loop protects against marginal erase: if a sector was kind of
        // erased and read back as all 0xFF but when values are written
        // some bits written as 1 actually become 0
        for(int tries = 0; tries < 2; tries++)
        {
            if(!verifySector(destinationSector) || tries > 0)
            {
                eraseSector(destinationSector);
            }

            if(!writeSectorStatus(destinationSector, SectorHeader::COPY))
            {
                continue;
            }

            if(!copyAllRecordsToSector(sourceSector, destinationSector, id))
            {
                continue;
            }

            // FIXME: simulate a marginal write error. This would be
            // hard to do automatically from the unit test...
            //if(tries == 0)
            //{
            //    uint32_t garbage = 0xDEADBEEF;
            //    store.write(getSectorStart(destinationSector) + 10, &garbage, sizeof(garbage));
            //    continue;
            //}

            if(!writeRecord(destinationSector, id, data, length))
            {
                continue;
            }

            if(!writeSectorStatus(destinationSector, SectorHeader::ACTIVE))
            {
                continue;
            }

            if(!writeSectorStatus(sourceSector, SectorHeader::INACTIVE))
            {
                continue;
            }

            // Success!

            updateActiveSector();
            calculateCapacity();
            return true;
        }

        return false;
    }

    // Perform the actual copy of records during sector swap
    bool copyAllRecordsToSector(LogicalSector sourceSector, LogicalSector destinationSector, uint16_t exceptRecordId)
    {
        bool success = true;
        forEachValidRecord(sourceSector, [&](uintptr_t offset, const Header &header)
        {
            if(header.id != exceptRecordId)
            {
                const void *currentData = store.dataAt(offset + sizeof(header));
                success = success && writeRecord(destinationSector, header.id, currentData, header.length);
            }
        });

        return success;
    }

    // The space currently used by all valid records
    // This is an expensive operation if there are a lot of records so
    // cache the result in a member variable
    void calculateCapacity()
    {
        size_t partialCapacity = 0;
        forEachValidRecord(getActiveSector(), [&](uintptr_t offset, const Header &header)
        {
            partialCapacity += sizeof(Header) + header.length;
        });

        capacity = partialCapacity;
    }

    // Update capacity after a put or remove
    void updateCapacity(int32_t capacityChange)
    {
        capacity += capacityChange;
    }

    // Which sector needs to be erased after a sector swap.
    LogicalSector getPendingEraseSector()
    {
        if(readSectorStatus(getAlternateSector()) != SectorHeader::ERASED)
        {
            return getAlternateSector();
        }
        else
        {
            return LogicalSector::NoSector;
        }
    }

    // Hardware-dependent interface to read, erase and program memory
    Store store;

protected:
    LogicalSector activeSector;
    LogicalSector alternateSector;
    size_t capacity;
};
