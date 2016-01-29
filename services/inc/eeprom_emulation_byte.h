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

#include <cstddef>
#include <cstring>
#include <memory>

// version 2 notes
//
// - Failure mode of reset during page erase after page swap remains
// - TODO: restrict id to SmallestPageSize - sizeof(PageHeader) - 1
// - TODO: cache findEmptyOffset()
// - TODO: Mark a page as inactive using a page footer or by changing
// the value of PageHeader::ACTIVE to allow another state PageHeader::INACTIVE
// - TODO: implement remove as writing 0xFF
// - TODO: page swap optimization: don't copy records with 0xFF to a new page


// FIXME: Remove this comment when implementation is done
// terminology: use page, not page or block
//              use offset, not address
//              use uintptr_t for offset, size_t for page sizes
//
// FIXME: Different address space

// For discussion
// - Flash programming failure mode: marginal write errors (reset while
// erasing or writing, reads as 1 but true state is between 0 and 1) ->
// reprogram with same value works, but different value may fail.
//
// - Present record layout
// - Present incomplete record write recovery strategy
// - Present incomplete page swap recovery strategy
// - Present pending page erase
// - store.read instead of store.dataAt because of misaligned data
//   (misalignment of records, and use of FLASH_ProgramWord / FLASH_ProgramByte has no impact on the robustness)
// - cache capacity because it's O(n^2) to calculate (don't do it before each put)
// - impact of different page sizes
// - testing strategy with writing partial records/validating raw EEPROM
// - Plan for migration: leave old page alone and start writing on new page
// - Expanded API: get, put, clear, remove, countRecords, listRecords (maybe also recordSize), capacity, pending erase
// - Current Wiring implementation calls HAL_EEPROM_Init on first usage (great!)
// - Question: expectations/contract of the Wiring EEPROM API (100% backwards compatible with earlier Arduino core releases): iterator, length, record id vs logical address, return type of get/put
// - TODO: migration, on-device integration tests, dynalib, Wiring API, user documentation, simple endurance benchmarks

// - TODO: get returns the size of the data read or -1 if not found. Zero in input struct before reading

template <typename Store, uintptr_t PageBase1, size_t PageSize1, uintptr_t PageBase2, size_t PageSize2>
class EEPROMEmulationByte
{
public:
    static constexpr size_t SmallestPageSize = (PageSize1 < PageSize2) ? PageSize1 : PageSize2;

    enum class LogicalPage
    {
        NoPage,
        Page1,
        Page2
    };

    static const uint8_t FLASH_ERASED = 0xFF;

    // Struct used to store the state of a page of emulated EEPROM
    //
    // Do not change the size of struct or order of elements since
    // instances of this struct are persisted in the flash memory
    struct __attribute__((packed)) PageHeader
    {
        static const uint16_t ERASED = 0xFFFF;
        static const uint16_t COPY = 0xEEEE;
        static const uint16_t ACTIVE = 0x0000;

        uint16_t status;

        PageHeader(uint16_t status = ERASED) : status(status)
        {
        }
    };

    // Struct used to store the value of 1 byte in the emulated EEPROM
    //
    // Do not change the size of struct or order of elements since
    // instances of this struct are persisted in the flash memory
    struct __attribute__((packed)) Record
    {
        static const uint8_t EMPTY = 0xFF;
        static const uint8_t INVALID = 0x0F;
        static const uint8_t REMOVED = 0x07;
        static const uint8_t VALID = 0x00;

        static const uint16_t EMPTY_ID = 0xFFFF;

        uint16_t id;
        uint8_t status;
        uint8_t data;

        Record(uint16_t status = EMPTY, uint16_t id = EMPTY_ID, uint8_t data = FLASH_ERASED)
            : id(id), status(status), data(data)
        {
        }
    };


    /* Public API */

    // Initialize the EEPROM pages
    // Call at boot
    void init()
    {
        updateActivePage();

        if(getActivePage() == LogicalPage::NoPage)
        {
            clear();
        }
    }

    // Read the latest value of a record
    // Writes 0xFF into data if the value was not programmed
    bool get(uint16_t id, uint8_t &data)
    {
        readRange(id, &data, sizeof(data));

        // TODO: remove return value
        return true;
    }

    void get(uint16_t startAddress, uint8_t *data, size_t length)
    {
        readRange(startAddress, data, length);
    }

    // Writes a new value for a record
    // Performs a page swap (move all valid records to a new page)
    // if the current page is full
    // Returns false if there is not enough capacity to write this
    // record (even after a page swap), true if record was written
    bool put(uint16_t id, uint8_t data)
    {
        writeRange(id, &data, sizeof(data));

        // TODO: remove return value
        return true;
    }

    void put(uint16_t startAddress, uint8_t *data, size_t length)
    {
        writeRange(startAddress, data, length);
    }

    // Destroys all the data 💣
    void clear()
    {
        erasePage(LogicalPage::Page1);
        erasePage(LogicalPage::Page2);
        writePageStatus(LogicalPage::Page1, PageHeader::ACTIVE);

        updateActivePage();
    }

    // Mark a record as removed to free up some capacity at next page swap
    // Return false if the record was not found, true if it was removed.
    bool remove(uint16_t id)
    {
        // TODO: implement this as writing 0xFF
        return true;
    }

    // The number of bytes that can be stored
    constexpr size_t capacity()
    {
        return (SmallestPageSize - sizeof(PageHeader)) / sizeof(Record);
    }

    // Since erasing a page prevents the bus accessing the Flash memory
    // thus freezing the application code, provide an API for the user
    // application to figure out if a page needs to be erased.
    // If the user application doesn't call performPendingErase(), then
    // at the next reboot or next page swap the page will be erased anyway
    bool hasPendingErase()
    {
        return getPendingErasePage() != LogicalPage::NoPage;
    }

    // Erases the old page after a page swap, if necessary
    void performPendingErase()
    {
        if(hasPendingErase())
        {
            erasePage(getPendingErasePage());
        }
    }

    /* Implementation */

    uintptr_t getPageStart(LogicalPage page)
    {
        switch(page)
        {
            case LogicalPage::Page1: return PageBase1;
            case LogicalPage::Page2: return PageBase2;
            default: return 0;
        }
    }

    uintptr_t getPageEnd(LogicalPage page)
    {
        switch(page)
        {
            case LogicalPage::Page1: return PageBase1 + PageSize1;
            case LogicalPage::Page2: return PageBase2 + PageSize2;
            default: return 0;
        }
    }

    uintptr_t getPageSize(LogicalPage page)
    {
        switch(page)
        {
            case LogicalPage::Page1: return PageSize1;
            case LogicalPage::Page2: return PageSize2;
            default: return 0;
        }
    }

    // The offset to the first empty record, or the end of the page if
    // no records are empty
    uintptr_t findEmptyOffset(LogicalPage page)
    {
        uintptr_t freeOffset = getPageEnd(page);
        forEachRecord(page, [&](uintptr_t offset, const Record &record)
        {
            if(record.status == Record::EMPTY)
            {
                freeOffset = offset;
            }
        });
        return freeOffset;
    }

    // Write a record to the first empty space available in a page
    //
    // Returns false when write was unsuccessful to protect against
    // marginal erase, true on proper write
    bool writeRecord(LogicalPage page, uint16_t id, uint8_t data, uint16_t status = Record::VALID)
    {
        // FIXME: get rid of findEmptyOffset every time
        uintptr_t offset = findEmptyOffset(page);
        size_t spaceRemaining = getPageEnd(page) - offset;

        // No more room for record
        if(spaceRemaining < sizeof(Record))
        {
            return false;
        }

        // Write record and return true when write is verified successfully
        Record record(status, id, data);
        return (store.write(offset, &record, sizeof(record)) >= 0);
    }

    // Write final valid status on a partially written record
    //
    // Returns false when write was unsuccessful to protect against
    // marginal erase, true on proper write
    bool writeRecordStatus(uintptr_t offset, uint16_t status)
    {
        Record record(status);
        uintptr_t statusOffset = offset + offsetof(Record, status);
        return (store.write(statusOffset, &record.status, sizeof(record.status)) >= 0);
    }


    // Figure out which page should currently be read from/written to
    // and which one should be used as the target of the page swap
    void updateActivePage()
    {
        uint16_t status1 = readPageStatus(LogicalPage::Page1);
        uint16_t status2 = readPageStatus(LogicalPage::Page2);

        // Pick the first active page
        if(status1 == PageHeader::ACTIVE)
        {
            activePage = LogicalPage::Page1;
            alternatePage = LogicalPage::Page2;
        }
        else if(status2 == PageHeader::ACTIVE)
        {
            activePage = LogicalPage::Page2;
            alternatePage = LogicalPage::Page1;
        }
        else
        {
            activePage = LogicalPage::NoPage;
            alternatePage = LogicalPage::NoPage;
        }
    }

    // Which page should currently be read from/written to
    LogicalPage getActivePage()
    {
        return activePage;
    }

    // Which page should be used as the target for the next swap
    LogicalPage getAlternatePage()
    {
        return alternatePage;
    }

    // Iterate through a page to extract the latest value of each address
    void readRange(uint16_t startAddress, uint8_t *data, uint16_t length)
    {
        std::memset(data, FLASH_ERASED, length);

        uint16_t endAddress = startAddress + length;
        forEachValidRecord(getActivePage(), [&](uintptr_t offset, const Record &record)
        {
            if(record.id >= startAddress && record.id <= endAddress)
            {
                data[record.id - startAddress] = record.data;
            }
        });
    }

    // Write the new value of each byte in the range if it has changed.
    //
    // Write new records as invalid in increasing order of address, then
    // go back and write records as valid in decreasing order of
    // address. This ensures data consistency if writeRange is
    // interrupted by a reset.
    void writeRange(uint16_t startAddress, uint8_t *data, uint16_t length)
    {
        // don't write anything if address is out of range
        if(startAddress + length >= capacity())
        {
            return;
        }

        // Read existing values for range
        std::unique_ptr<uint8_t[]> existingData(new uint8_t[length]);
        // don't write anything if memory is full
        if(!existingData)
        {
            return;
        }
        readRange(startAddress, existingData.get(), length);

        // Make sure there are no previous invalid records before
        // starting to write
        bool success = !hasInvalidRecords(getActivePage());

        // Write all changed values as invalid records
        for(uint16_t i = 0; i < length && success; i++)
        {
            if(existingData[i] != data[i])
            {
                uint16_t address = startAddress + i;
                success = success && writeRecord(getActivePage(), address, data[i], Record::INVALID);
            }
        }

        // If all writes succeeded, mark all invalid records active
        if(success)
        {
            forEachInvalidRecord(getActivePage(), [&](uintptr_t offset, const Record &record)
            {
                success = success && writeRecordStatus(offset, Record::VALID);
            });
        }

        // If any writes failed because the page was full or a marginal
        // write error occured, do a page swap then write all the
        // records
        if(!success)
        {
            swapPagesAndWrite(startAddress, data, length);
        }
    }

    // Iterate through a page to find the latest valid record with a
    // specified id
    // Returns true if a record is found and puts length and data in the
    // parameters references
    bool findRecord(uint16_t id, uint8_t &data)
    {
        bool found = false;
        forEachRecord(getActivePage(), [&](uintptr_t offset, const Record &record)
        {
            if(record.status == Record::VALID && record.id == id)
            {
                data = record.data;
                found = true;
            }
        });
        return found;
    }

    // Iterate through a page and yield each record, including valid
    // and invalid records, and the empty record at the end (if there is
    // room)
    template <typename Func>
    void forEachRecord(LogicalPage page, Func f)
    {
        uintptr_t currentOffset = getPageStart(page);
        uintptr_t lastOffset = getPageEnd(page);

        // Skip page header
        currentOffset += sizeof(PageHeader);

        // Walk through record list
        while(currentOffset < lastOffset)
        {
            const Record &record = *(const Record *) store.dataAt(currentOffset);

            // Yield record
            f(currentOffset, record);

            // End of data
            if(record.status == Record::EMPTY)
            {
                return;
            }

            // Skip over record
            currentOffset += sizeof(record);
        }
    }

    // TODO: document
    template <typename Func>
    void forEachInvalidRecord(LogicalPage page, Func f)
    {
        uintptr_t currentOffset = findLastInvalidOffset(page);
        uintptr_t startOffset = getPageStart(page);

        // Walk through record list
        while(currentOffset > startOffset)
        {
            const Record &record = *(const Record *) store.dataAt(currentOffset);

            if(record.status == Record::INVALID)
            {
                // Yield record
                f(currentOffset, record);
            }
            else
            {
                // End of invalid records
                return;
            }

            // Skip backwards over record
            currentOffset -= sizeof(record);
        }
    }

    // TODO: document
    uintptr_t findLastInvalidOffset(LogicalPage page)
    {
        uintptr_t invalidOffset = getPageStart(page);
        forEachRecord(page, [&](uintptr_t offset, const Record &record)
        {
            if(record.status == Record::INVALID)
            {
                invalidOffset = offset;
            }
        });
        return invalidOffset;
    }

    bool hasInvalidRecords(LogicalPage page)
    {
        return findLastInvalidOffset(page) != getPageStart(page);
    }

    // Iterate through a page and yield each valid record,
    // ignoring any records after the first invalid one
    template <typename Func>
    void forEachValidRecord(LogicalPage page, Func f)
    {
        bool foundInvalid = false;
        forEachRecord(page, [&](uintptr_t offset, const Record &record)
        {
            if(!foundInvalid && record.status == Record::VALID)
            {
                f(offset, record);
            }
            else
            {
                foundInvalid = true;
            }
        });
    }

    // Iterate through a page and yield each valid record, in
    // increasing order of id
    template <typename Func>
    void forEachSortedValidRecord(LogicalPage page, Func f)
    {
        uint16_t currentId;
        uint8_t currentData;
        int32_t previousId = -1;
        bool nextRecordFound;

        do
        {
            nextRecordFound = false;
            currentId = UINT16_MAX;
            forEachValidRecord(page, [&](uintptr_t offset, const Record &record)
            {
                if( record.id <= currentId && (int32_t)record.id > previousId)
                {
                    currentId = record.id;
                    currentData = record.data;
                    nextRecordFound = true;
                }
            });

            if(nextRecordFound)
            {
                // Yield record
                f(currentId, currentData);
                previousId = currentId;
            }
        } while(nextRecordFound);
    }

    // Verify that the entire page is erased to protect against resets
    // during page erase
    bool verifyPage(LogicalPage page)
    {
        const uint8_t *begin = store.dataAt(getPageStart(page));
        const uint8_t *end = store.dataAt(getPageEnd(page));
        while(begin < end)
        {
            if(*begin++ != FLASH_ERASED)
            {
                return false;
            }
        }

        return true;
    }

    // Reset entire page to 0xFF
    void erasePage(LogicalPage page)
    {
        store.eraseSector(getPageStart(page));
    }

    // Get the current status of a page (empty, active, being copied, ...)
    uint16_t readPageStatus(LogicalPage page)
    {
        PageHeader header;
        store.read(getPageStart(page), &header, sizeof(header));
        return header.status;
    }

    // Update the status of a page
    bool writePageStatus(LogicalPage page, uint16_t status)
    {
        PageHeader header = { status };
        return store.write(getPageStart(page), &header, sizeof(header)) == 0;
    }

    // Write all valid records from the active page to the alternate
    // page. Erase the alternate page if it is not already erased.
    // Then write the new record to the alternate page.
    // Then erase the old active page
    bool swapPagesAndWrite(uint16_t id, const uint8_t *data, uint16_t length)
    {
        LogicalPage sourcePage = getActivePage();
        LogicalPage destinationPage = getAlternatePage();

        // loop protects against marginal erase: if a page was kind of
        // erased and read back as all 0xFF but when values are written
        // some bits written as 1 actually become 0
        for(int tries = 0; tries < 2; tries++)
        {
            bool success = true;
            if(!verifyPage(destinationPage) || tries > 0)
            {
                erasePage(destinationPage);
            }

            success = success && writePageStatus(destinationPage, PageHeader::COPY);

            success = success && copyAllRecordsToPageExcept(sourcePage, destinationPage, id, id + length);

            // FIXME: simulate a marginal write error. This would be
            // hard to do automatically from the unit test...
            //if(tries == 0)
            //{
            //    uint32_t garbage = 0xDEADBEEF;
            //    store.write(getPageStart(destinationPage) + 10, &garbage, sizeof(garbage));
            //    success = false;
            //}

            for(uint16_t i = 0; i < length && success; i++)
            {
                // Don't bother writing records that are 0xFF
                if(data[i] != FLASH_ERASED)
                {
                    success = success && writeRecord(destinationPage, id, data[i]);
                }
            }

            success = success && writePageStatus(destinationPage, PageHeader::ACTIVE);

            if(success)
            {
                erasePage(sourcePage);
                updateActivePage();
                return true;
            }
        }

        return false;
    }

    // Perform the actual copy of records during page swap
    bool copyAllRecordsToPageExcept(LogicalPage sourcePage,
            LogicalPage destinationPage,
            uint16_t exceptRecordIdStart,
            uint16_t exceptRecordIdEnd)
    {
        bool success = true;
        forEachSortedValidRecord(sourcePage, [&](uint16_t id, uint8_t data)
        {
            if(id < exceptRecordIdStart || id > exceptRecordIdEnd)
            {
                // Don't bother writing records that are 0xFF
                if(data != FLASH_ERASED)
                {
                    success = success && writeRecord(destinationPage, id, data);
                }
            }
        });

        return success;
    }

    // Which page needs to be erased after a page swap.
    LogicalPage getPendingErasePage()
    {
        if(readPageStatus(getAlternatePage()) != PageHeader::ERASED)
        {
            return getAlternatePage();
        }
        else
        {
            return LogicalPage::NoPage;
        }
    }

    // Hardware-dependent interface to read, erase and program memory
    Store store;

protected:
    LogicalPage activePage;
    LogicalPage alternatePage;
};
