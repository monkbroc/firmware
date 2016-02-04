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

#include <cstring>
#include <memory>

/* EEPROM Emulation using Flash memory
 *
 * EEPROM provides reads and writes for single bytes, with a default
 * value of 0xFF for unprogrammed cells.
 *
 * Two pages (sectors) of Flash memory with potentially different sizes
 * are used to store records each containing the value of 1 byte of
 * emulated EEPROM.
 *
 * Each record contain an index (EEPROM cell virtual address), a data
 * byte and a status byte (valid, invalid, erased).
 *
 * The maximum number of bytes that can be written is the smallest page
 * size divided by the record size.
 *
 * Since erased Flash starts at 0xFF and bits can only be written as 0,
 * writing a new value of an EEPROM byte involves appending a new record
 * to the list of current records in the active page.
 *
 * Reading involves going through the list of valid records in the
 * active page looking for the last record with a specified index.
 *
 * When writing a new value and there is no more room in the current
 * page to append new records, a page swap occurs as follows:
 * - The alternate page is erased if necessary
 * - Records for all values except the  ones being written are copied to
 *   the alternate page
 * - Records for the changed records are written to the alternate page.
 * - The alternate page is marked active and becomes the new active page
 * - The old active page is marked inactive
 *
 * Any of these steps can be interrupted by a reset and the data will
 * remain consistent because the old page will be used until the very
 * last step (old active page is marked inactive).
 *
 * In order to make application programming easier, it is possible to
 * write multiple bytes in an atomic fashion: either all bytes written
 * will be read back or none will be read back, even in the presence of
 * power failure/controller reset.
 *
 * Atomic writes are implemented as follows:
 * - If any invalid records exist, do a page swap (which is atomic)
 * - Write records with an invalid status for all changed bytes
 * - Going backwards from the end, write a valid status for all invalid records
 * - If any of the writes failed, do a page swap
 *
 * It is possible for a write to fail verification (reading back the
 * value). This is because of previous marginal writes or marginal
 * erases (reset during writing or erase that leaves Flash cells reading
 * back as 1 but with a true state between 0 and 1).  To protect against
 * this, if a write doesn't read back correctly, a page swap will be
 * done.
 *
 * On the STM32 microcontroller, the Flash memory cannot be read while
 * being programmed which means the application is frozen while writing
 * or erasing the Flash (no interrupts are serviced). Flash writes are
 * pretty fast, but erases take 200ms or more (depending on the sector
 * size). To avoid intermittent pauses in the user application due to
 * page erases during the page swap, the hasPendingErase() and
 * performPendingErase() APIs exist to allow the user application to
 * schedule when an old page can be erased. If the user application does
 * not call performPendingErase() before the next page swap, the
 * alternate page will be erased just before the page swap.
 *
 */

// What does this mean?
// when unpacking from the combined image, we have the default application image stored
// in the eeprom region.

// - TODO: on-device integration tests, dynalib, Wiring API, user documentation, simple performance benchmarks

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

    // Stores the status of a page of emulated EEPROM
    //
    // WARNING: Do not change the size of struct or order of elements since
    // instances of this struct are persisted in the flash memory
    struct __attribute__((packed)) PageHeader
    {
        static const uint16_t ERASED = 0xFFFF;
        static const uint16_t COPY = 0x0FFF;
        static const uint16_t ACTIVE = 0x00FF;
        static const uint16_t INACTIVE = 0x000F;

        // The previous implementation used 0 as an active status, but
        // we need inactive to be writable after active
        static const uint16_t LEGACY_ACTIVE = 0x0000;

        uint16_t status;

        PageHeader(uint16_t status = ERASED) : status(status)
        {
        }
    };

    // A record stores the value of 1 byte in the emulated EEPROM
    //
    // WARNING: Do not change the size of struct or order of elements since
    // instances of this struct are persisted in the flash memory
    struct __attribute__((packed)) Record
    {
        static const uint8_t EMPTY = 0xFF;
        static const uint8_t INVALID = 0x0F;
        static const uint8_t VALID = 0x00;

        static const uint16_t EMPTY_INDEX = 0xFFFF;

        uint16_t index;
        uint8_t status;
        uint8_t data;

        Record(uint16_t status = EMPTY, uint16_t index = EMPTY_INDEX, uint8_t data = FLASH_ERASED)
            : index(index), status(status), data(data)
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
    void get(uint16_t index, uint8_t &data)
    {
        readRange(index, &data, sizeof(data));
    }

    // Reads the latest valid values of a block of EEPROM
    // Fills data with 0xFF if values were not programmed
    void get(uint16_t index, void *data, size_t length)
    {
        readRange(index, (uint8_t *)data, length);
    }

    // Writes a new value for a byte of EEPROM
    // Performs a page swap (move all valid records to a new page)
    // if the current page is full
    void put(uint16_t index, uint8_t data)
    {
        writeRange(index, &data, sizeof(data));
    }

    // Writes new values for a block of EEPROM
    // The write will be atomic (all or nothing) even if a reset occurs
    // during the write
    //
    // Performs a page swap (move all valid records to a new page)
    // if the current page is full
    void put(uint16_t index, const void *data, size_t length)
    {
        writeRange(index, (uint8_t *)data, length);
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

    // Check if the old page needs to be erased
    bool hasPendingErase()
    {
        return getPendingErasePage() != LogicalPage::NoPage;
    }

    // Erases the old page after a page swap, if necessary
    // Let the user application call this when convenient since erasing
    // Flash freezes the application for several 100ms.
    void performPendingErase()
    {
        if(hasPendingErase())
        {
            erasePage(getPendingErasePage());
        }
    }

    /* Implementation */

    // Start address of the page
    uintptr_t getPageStart(LogicalPage page)
    {
        switch(page)
        {
            case LogicalPage::Page1: return PageBase1;
            case LogicalPage::Page2: return PageBase2;
            default: return 0;
        }
    }

    // End address (1 past the end) of the page
    uintptr_t getPageEnd(LogicalPage page)
    {
        switch(page)
        {
            case LogicalPage::Page1: return PageBase1 + PageSize1;
            case LogicalPage::Page2: return PageBase2 + PageSize2;
            default: return 0;
        }
    }

    // Number of bytes in the page
    size_t getPageSize(LogicalPage page)
    {
        switch(page)
        {
            case LogicalPage::Page1: return PageSize1;
            case LogicalPage::Page2: return PageSize2;
            default: return 0;
        }
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
        // If no page is active, use a legagy page
        else if(status1 == PageHeader::LEGACY_ACTIVE)
        {
            activePage = LogicalPage::Page1;
            alternatePage = LogicalPage::Page2;
        }
        else if(status2 == PageHeader::LEGACY_ACTIVE)
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

    // Get the current status of a page (empty, active, being copied, ...)
    uint16_t readPageStatus(LogicalPage page)
    {
        PageHeader *header = (PageHeader *) store.dataAt(getPageStart(page));
        return header->status;
    }

    // Update the status of a page
    bool writePageStatus(LogicalPage page, uint16_t status)
    {
        PageHeader header = { status };
        return store.write(getPageStart(page), &header, sizeof(header)) == 0;
    }

    // Iterate through a page to extract the latest value of each address
    void readRange(uint16_t startIndex, uint8_t *data, uint16_t length)
    {
        std::memset(data, FLASH_ERASED, length);

        uint16_t endIndex = startIndex + length;
        forEachValidRecord(getActivePage(), [&](uintptr_t address, const Record &record)
        {
            if(record.index >= startIndex && record.index <= endIndex)
            {
                data[record.index - startIndex] = record.data;
            }
        });
    }

    // Write each byte in the range if its value has changed.
    //
    // Write new records as invalid in increasing order of index, then
    // go back and write records as valid in decreasing order of
    // index. This ensures data consistency if writeRange is
    // interrupted by a reset.
    void writeRange(uint16_t startIndex, const uint8_t *data, uint16_t length)
    {
        // don't write anything if index is out of range
        if(startIndex + length >= capacity())
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
        readRange(startIndex, existingData.get(), length);

        // Make sure there are no previous invalid records before
        // starting to write
        bool success = !hasInvalidRecords(getActivePage());

        // Write all changed values as invalid records
        for(uint16_t i = 0; i < length && success; i++)
        {
            if(existingData[i] != data[i])
            {
                uint16_t index = startIndex + i;
                success = success && writeRecord(getActivePage(), index, data[i], Record::INVALID);
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
            swapPagesAndWrite(startIndex, data, length);
        }
    }

    // The address to the first empty record, or the end of the page if
    // no records are empty
    uintptr_t findEmptyAddress(LogicalPage page)
    {
        uintptr_t emptyAddress = getPageEnd(page);
        forEachRecord(page, [&](uintptr_t address, const Record &record) -> bool
        {
            if(record.status == Record::EMPTY)
            {
                emptyAddress = address;
                return true;
            }
            else
            {
                return false;
            }
        });
        return emptyAddress;
    }

    // Checks if there are any invalid records in a page
    bool hasInvalidRecords(LogicalPage page)
    {
        return findLastInvalidAddress(page) != getPageStart(page);
    }

    // Write a record to the first empty space available in a page
    //
    // Returns false when write was unsuccessful to protect against
    // marginal erase, true on proper write
    bool writeRecord(LogicalPage page, uint16_t index, uint8_t data, uint16_t status = Record::VALID)
    {
        uintptr_t address = findEmptyAddress(page);
        size_t spaceRemaining = getPageEnd(page) - address;

        // No more room for record
        if(spaceRemaining < sizeof(Record))
        {
            return false;
        }

        // Write record and return true when write is verified successfully
        Record record(status, index, data);
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

            // Yield record and potentially break early
            if(f(address, record))
            {
                return;
            }

            // Skip over record
            address += sizeof(record);
        }
    }

    // Iterate through a page and yield each invalid record, starting
    // with the last invalid record going backwards towards the first
    // invalid record
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

    // The address to the last invalid record, or the beginning of the
    // page if no records are invalid
    uintptr_t findLastInvalidAddress(LogicalPage page)
    {
        uintptr_t lastInvalidAddress = getPageStart(page);
        forEachRecord(page, [&](uintptr_t address, const Record &record) -> bool
        {
            if(record.status == Record::EMPTY)
            {
                return true;
            }

            if(record.status == Record::INVALID)
            {
                lastInvalidAddress = address;
            }

            return false;
        });
        return lastInvalidAddress;
    }

    // Iterate through a page and yield each valid record,
    // ignoring any records after the first invalid one
    template <typename Func>
    void forEachValidRecord(LogicalPage page, Func f)
    {
        forEachRecord(page, [=](uintptr_t address, const Record &record)
        {
            if(record.status == Record::VALID)
            {
                f(address, record);
                return false;
            }
            else
            {
                return true;
            }
        });
    }

    // Iterate through a page and yield each valid record, in
    // increasing order of id
    template <typename Func>
    void forEachSortedValidRecord(LogicalPage page, Func f)
    {
        uintptr_t currentAddress;
        uint16_t currentIndex;
        int32_t previousIndex = -1;
        bool nextRecordFound;

        do
        {
            nextRecordFound = false;
            currentIndex = UINT16_MAX;
            forEachValidRecord(page, [&](uintptr_t address, const Record &record)
            {
                if(record.index <= currentIndex && (int32_t)record.index > previousIndex)
                {
                    currentAddress = address;
                    currentIndex = record.index;
                    nextRecordFound = true;
                }
            });

            if(nextRecordFound)
            {
                const Record &record = *(const Record *) store.dataAt(currentAddress);

                // Yield record
                f(currentAddress, record);
                previousIndex = currentIndex;
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

    // Write all valid records from the active page to the alternate
    // page. Erase the alternate page if it is not already erased.
    // Then write the new record to the alternate page.
    // Then erase the old active page
    bool swapPagesAndWrite(uint16_t startIndex, const uint8_t *data, uint16_t length)
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
            success = success && copyAllRecordsToPageExcept(sourcePage, destinationPage, startIndex, startIndex + length);

            // Uncomment to simulate a marginal write error. This would be
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
                    uint16_t index = startIndex + i;
                    success = success && writeRecord(destinationPage, index, data[i]);
                }
            }

            success = success && writePageStatus(destinationPage, PageHeader::ACTIVE);
            success = success && writePageStatus(sourcePage, PageHeader::INACTIVE);

            if(success)
            {
                updateActivePage();
                return true;
            }
        }

        return false;
    }

    // Perform the actual copy of records during page swap
    bool copyAllRecordsToPageExcept(LogicalPage sourcePage,
            LogicalPage destinationPage,
            uint16_t exceptIndexStart,
            uint16_t exceptIndexEnd)
    {
        bool success = true;
        forEachSortedValidRecord(sourcePage, [&](uintptr_t address, const Record &record)
        {
            if(record.index < exceptIndexStart || record.index > exceptIndexEnd)
            {
                // Don't bother writing records that are 0xFF
                if(record.data != FLASH_ERASED)
                {
                    success = success && writeRecord(destinationPage, record.index, record.data);
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
