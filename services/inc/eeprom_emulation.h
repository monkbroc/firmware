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
// - TODO: cache findEmptyAddress()
// - TODO: Mark a page as inactive using a page footer or by changing
// the value of PageHeader::ACTIVE to allow another state PageHeader::INACTIVE

// What does this mean?
// when unpacking from the combined image, we have the default application image stored
// in the eeprom region.


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
class EEPROMEmulation
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
    // WARNING: Do not change the size of struct or order of elements since
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
    // WARNING: Do not change the size of struct or order of elements since
    // instances of this struct are persisted in the flash memory
    struct __attribute__((packed)) Record
    {
        static const uint8_t EMPTY = 0xFF;
        static const uint8_t INVALID = 0x0F;
        static const uint8_t VALID = 0x00;

        static const uint16_t EMPTY_OFFSET = 0xFFFF;

        uint16_t offset;
        uint8_t status;
        uint8_t data;

        Record(uint16_t status = EMPTY, uint16_t offset = EMPTY_OFFSET, uint8_t data = FLASH_ERASED)
            : offset(offset), status(status), data(data)
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

    // Read the latest value of a byte of EEPROM
    // Writes 0xFF into data if the value was not programmed
    void get(uint16_t offset, uint8_t &data)
    {
        readRange(offset, &data, sizeof(data));
    }

    // Reads the latest value of a block of EEPROM
    // Fills data with 0xFF if values were not programmed
    void get(uint16_t offset, uint8_t *data, size_t length)
    {
        readRange(offset, data, length);
    }

    // Writes a new value for a byte of EEPROM
    // Performs a page swap (move all valid records to a new page)
    // if the current page is full
    void put(uint16_t offset, uint8_t data)
    {
        writeRange(offset, &data, sizeof(data));
    }

    // Writes new values for a block of EEPROM
    // The write of all values will be atomic even if a reset occurs
    // during the write
    //
    // Performs a page swap (move all valid records to a new page)
    // if the current page is full
    void put(uint16_t offset, uint8_t *data, size_t length)
    {
        writeRange(offset, data, length);
    }

    // Destroys all the data 💣
    void clear()
    {
        erasePage(LogicalPage::Page1);
        erasePage(LogicalPage::Page2);
        writePageStatus(LogicalPage::Page1, PageHeader::ACTIVE);

        updateActivePage();
    }

    // Returns number of bytes that can be stored in EEPROM
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
    uintptr_t findEmptyAddress(LogicalPage page)
    {
        uintptr_t emptyAddress = getPageEnd(page);
        forEachRecord(page, [&](uintptr_t address, const Record &record)
        {
            if(record.status == Record::EMPTY)
            {
                emptyAddress = address;
            }
        });
        return emptyAddress;
    }

    // Write a record to the first empty space available in a page
    //
    // Returns false when write was unsuccessful to protect against
    // marginal erase, true on proper write
    bool writeRecord(LogicalPage page, uint16_t offset, uint8_t data, uint16_t status = Record::VALID)
    {
        // FIXME: get rid of findEmptyAddress every time
        uintptr_t address = findEmptyAddress(page);
        size_t spaceRemaining = getPageEnd(page) - address;

        // No more room for record
        if(spaceRemaining < sizeof(Record))
        {
            return false;
        }

        // Write record and return true when write is verified successfully
        Record record(status, offset, data);
        return (store.write(address, &record, sizeof(record)) >= 0);
    }

    // Write final valid status on a partially written record
    //
    // Returns false when write was unsuccessful to protect against
    // marginal erase, true on proper write
    bool writeRecordStatus(uintptr_t address, uint16_t status)
    {
        Record record(status);
        uintptr_t statusAddress = address + offsetof(Record, status);
        return (store.write(statusAddress, &record.status, sizeof(record.status)) >= 0);
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
    void readRange(uint16_t startOffset, uint8_t *data, uint16_t length)
    {
        std::memset(data, FLASH_ERASED, length);

        uint16_t endOffset = startOffset + length;
        forEachValidRecord(getActivePage(), [&](uintptr_t address, const Record &record)
        {
            if(record.offset >= startOffset && record.offset <= endOffset)
            {
                data[record.offset - startOffset] = record.data;
            }
        });
    }

    // Write the new value of each byte in the range if it has changed.
    //
    // Write new records as invalid in increasing order of address, then
    // go back and write records as valid in decreasing order of
    // address. This ensures data consistency if writeRange is
    // interrupted by a reset.
    void writeRange(uint16_t startOffset, uint8_t *data, uint16_t length)
    {
        // don't write anything if address is out of range
        if(startOffset + length >= capacity())
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
        readRange(startOffset, existingData.get(), length);

        // Make sure there are no previous invalid records before
        // starting to write
        bool success = !hasInvalidRecords(getActivePage());

        // Write all changed values as invalid records
        for(uint16_t i = 0; i < length && success; i++)
        {
            if(existingData[i] != data[i])
            {
                uint16_t offset = startOffset + i;
                success = success && writeRecord(getActivePage(), offset, data[i], Record::INVALID);
            }
        }

        // If all writes succeeded, mark all invalid records active
        if(success)
        {
            forEachInvalidRecord(getActivePage(), [&](uintptr_t address, const Record &record)
            {
                success = success && writeRecordStatus(address, Record::VALID);
            });
        }

        // If any writes failed because the page was full or a marginal
        // write error occured, do a page swap then write all the
        // records
        if(!success)
        {
            swapPagesAndWrite(startOffset, data, length);
        }
    }

    // Iterate through a page and yield each record, including valid
    // and invalid records, and the empty record at the end (if there is
    // room)
    template <typename Func>
    void forEachRecord(LogicalPage page, Func f)
    {
        uintptr_t address = getPageStart(page);
        uintptr_t endAddress = getPageEnd(page);

        // Skip page header
        address += sizeof(PageHeader);

        // Walk through record list
        while(address < endAddress)
        {
            const Record &record = *(const Record *) store.dataAt(address);

            // Yield record
            f(address, record);

            // End of data
            if(record.status == Record::EMPTY)
            {
                return;
            }

            // Skip over record
            address += sizeof(record);
        }
    }

    // TODO: document
    template <typename Func>
    void forEachInvalidRecord(LogicalPage page, Func f)
    {
        uintptr_t address = findLastInvalidAddress(page);
        uintptr_t startAddress = getPageStart(page);

        // Walk through record list
        while(address > startAddress)
        {
            const Record &record = *(const Record *) store.dataAt(address);

            if(record.status == Record::INVALID)
            {
                // Yield record
                f(address, record);
            }
            else
            {
                // End of invalid records
                return;
            }

            // Skip backwards over record
            address -= sizeof(record);
        }
    }

    // TODO: document
    uintptr_t findLastInvalidAddress(LogicalPage page)
    {
        uintptr_t lastInvalidAddress = getPageStart(page);
        forEachRecord(page, [&](uintptr_t address, const Record &record)
        {
            if(record.status == Record::INVALID)
            {
                lastInvalidAddress = address;
            }
        });
        return lastInvalidAddress;
    }

    bool hasInvalidRecords(LogicalPage page)
    {
        return findLastInvalidAddress(page) != getPageStart(page);
    }

    // Iterate through a page and yield each valid record,
    // ignoring any records after the first invalid one
    template <typename Func>
    void forEachValidRecord(LogicalPage page, Func f)
    {
        bool foundInvalid = false;
        forEachRecord(page, [&](uintptr_t address, const Record &record)
        {
            if(!foundInvalid && record.status == Record::VALID)
            {
                f(address, record);
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
        uint16_t currentOffset;
        uint8_t currentData;
        int32_t previousOffset = -1;
        bool nextRecordFound;

        do
        {
            nextRecordFound = false;
            currentOffset = UINT16_MAX;
            forEachValidRecord(page, [&](uintptr_t address, const Record &record)
            {
                if(record.offset <= currentOffset && (int32_t)record.offset > previousOffset)
                {
                    currentOffset = record.offset;
                    currentData = record.data;
                    nextRecordFound = true;
                }
            });

            if(nextRecordFound)
            {
                // Yield record
                f(currentOffset, currentData);
                previousOffset = currentOffset;
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
    bool swapPagesAndWrite(uint16_t startOffset, const uint8_t *data, uint16_t length)
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

            // Write alternate page as destination for copy
            success = success && writePageStatus(destinationPage, PageHeader::COPY);

            // Copy records from source to destination
            success = success && copyAllRecordsToPageExcept(sourcePage, destinationPage, startOffset, startOffset + length);

            // FIXME: simulate a marginal write error. This would be
            // hard to do automatically from the unit test...
            //if(tries == 0)
            //{
            //    uint32_t garbage = 0xDEADBEEF;
            //    store.write(getPageStart(destinationPage) + 10, &garbage, sizeof(garbage));
            //    success = false;
            //}

            // Write new records to destination directly
            for(uint16_t i = 0; i < length && success; i++)
            {
                // Don't bother writing records that are 0xFF
                if(data[i] != FLASH_ERASED)
                {
                    uint16_t offset = startOffset + i;
                    success = success && writeRecord(destinationPage, offset, data[i]);
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
            uint16_t exceptOffsetStart,
            uint16_t exceptOffsetEnd)
    {
        bool success = true;
        forEachSortedValidRecord(sourcePage, [&](uint16_t offset, uint8_t data)
        {
            if(offset < exceptOffsetStart || offset > exceptOffsetEnd)
            {
                // Don't bother writing records that are 0xFF
                if(data != FLASH_ERASED)
                {
                    success = success && writeRecord(destinationPage, offset, data);
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
