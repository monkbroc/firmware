// Automatic test runner:
// user/tests/unit$ rerun -x -p "**/*.{cpp,h}" -d .,../../../services "make runner && obj/runner [eeprom]"
#include "catch.hpp"
#include <string>
#include <sstream>
#include "eeprom_emulation.h"
#include "flash_storage.h"

const size_t TestPageSize = 0x4000;
const uint8_t TestPageCount = 2;
const uintptr_t TestBase = 0xC000;

/* Simulate 2 Flash pages of different sizes used for EEPROM emulation */
const uintptr_t PageBase1 = TestBase;
const size_t PageSize1 = TestPageSize;
const uintptr_t PageBase2 = TestBase + TestPageSize;
const size_t PageSize2 = TestPageSize / 4;

using TestStore = RAMFlashStorage<TestBase, TestPageCount, TestPageSize>;
using TestEEPROM = EEPROMEmulation<TestStore, PageBase1, PageSize1, PageBase2, PageSize2>;

// Alias some constants, otherwise the linker is having issues when
// those are used inside REQUIRE() tests
auto NoPage = TestEEPROM::LogicalPage::NoPage;
auto Page1 = TestEEPROM::LogicalPage::Page1;
auto Page2 = TestEEPROM::LogicalPage::Page2;

auto PAGE_ERASED = TestEEPROM::PageHeader::ERASED;
auto PAGE_COPY = TestEEPROM::PageHeader::COPY;
auto PAGE_ACTIVE = TestEEPROM::PageHeader::ACTIVE;

// Decorator class for RAMFlashStorage to pre-write EEPROM records or
// validate written records
class StoreManipulator
{
public:
    StoreManipulator(TestStore &store) : store(store)
    {
    }

    TestStore &store;

    void eraseAll()
    {
        store.eraseSector(PageBase1);
        store.eraseSector(PageBase2);
    }

    uintptr_t recordAddress(uintptr_t baseAddress, uint16_t offset)
    {
        // Page header is 2 bytes, each record is 4 bytes
        return baseAddress + 2 + 4 * offset;
    }

    void writePageStatus(uintptr_t address, uint16_t status)
    {
        store.write(address, &status, sizeof(status));
    }

    uint16_t readPageStatus(uintptr_t address)
    {
        uint16_t status;
        store.read(address, &status, sizeof(status));
        return status;
    }

    void requirePageStatus(uintptr_t address, uint16_t expectedStatus)
    {
        REQUIRE(readPageStatus(address) == expectedStatus);
    }

    // Interrupted record write 1: invalid status, offset and data written
    uintptr_t writeInvalidRecord(uintptr_t address, uint16_t offset, uint8_t data)
    {
        TestEEPROM::Record record(TestEEPROM::Record::INVALID, offset, data);
        store.write(address, &record, sizeof(record));

        return address + sizeof(record);
    }

    // Completely written record
    uintptr_t writeRecord(uintptr_t address, uint16_t offset, uint8_t data)
    {
        TestEEPROM::Record record(TestEEPROM::Record::VALID, offset, data);
        store.write(address, &record, sizeof(record));

        return address + sizeof(record);
    }

    // Validates that a specific record was correctly written at the specified address
    uintptr_t requireValidRecord(uintptr_t address, uint16_t offset, uint8_t expectedData)
    {
        TestEEPROM::Record record;
        store.read(address, &record, sizeof(record));

        auto status = TestEEPROM::Record::VALID;
        REQUIRE(record.status == status);
        REQUIRE(record.offset == offset);
        REQUIRE(record.data == expectedData);

        return address + sizeof(record);
    }

    // Validate that a specific address has no record (erased space)
    uintptr_t requireEmptyRecord(uintptr_t address)
    {
        TestEEPROM::Record erasedRecord, record;
        store.read(address, &record, sizeof(record));
        REQUIRE(std::memcmp(&record, &erasedRecord, sizeof(record)) == 0);

        return address + sizeof(record);
    }

    // Debugging helper to view the storage contents
    // Usage:
    // WARN(store.dumpStorage(PageBase1, 30));
    std::string dumpStorage(uintptr_t address, uint16_t length)
    {
        std::stringstream ss;
        const uint8_t *begin = store.dataAt(address);
        const uint8_t *end = &begin[length];

        ss << std::hex << address << ": ";
        while(begin < end)
        {
            ss << std::hex << std::setw(2) << std::setfill('0');
            ss << (int)*begin++ << " ";
        }
        return ss.str();
    }
};

TEST_CASE("Get byte", "[eeprom]")
{
    TestEEPROM eeprom;

    eeprom.init();

    uint8_t value;
    uint16_t eepromOffset = 10;

    SECTION("The offset was not programmed")
    {
        SECTION("No other records")
        {
            THEN("get returns the value as erased")
            {
                eeprom.get(eepromOffset, value);
                REQUIRE(value == 0xFF);
            }
        }

        SECTION("With other records")
        {
            eeprom.put(0, 0xAA);

            THEN("get returns the value as erased")
            {
                eeprom.get(eepromOffset, value);
                REQUIRE(value == 0xFF);
            }
        }

        SECTION("With a partially written record")
        {
            eeprom.store.discardWritesAfter(1, [&] {
                eeprom.put(eepromOffset, 0xEE);
            });

            THEN("get returns the value as erased")
            {
                eeprom.get(eepromOffset, value);
                REQUIRE(value == 0xFF);
            }
        }
    }

    SECTION("The offset was programmed")
    {
        eeprom.put(eepromOffset, 0xCC);

        SECTION("No other records")
        {
            THEN("get extracts the value")
            {
                eeprom.get(eepromOffset, value);
                REQUIRE(value == 0xCC);
            }
        }

        SECTION("Followed by a partially written record")
        {
            eeprom.store.discardWritesAfter(1, [&] {
                eeprom.put(eepromOffset, 0xEE);
            });

            THEN("get extracts the value of the last valid record")
            {
                eeprom.get(eepromOffset, value);
                REQUIRE(value == 0xCC);
            }
        }

        SECTION("Followed by a fully written record")
        {
            eeprom.put(eepromOffset, 0xEE);

            THEN("get extracts the value of the new valid record")
            {
                eeprom.get(eepromOffset, value);
                REQUIRE(value == 0xEE);
            }
        }

    }

    SECTION("The offset was programmed by a multi-byte put")
    {
        uint16_t eepromOffset = 0;
        uint8_t values[] = { 1, 2, 3 };

        eeprom.put(eepromOffset, values, sizeof(values));

        THEN("get extracts each value")
        {
            eeprom.get(eepromOffset, value);
            REQUIRE(value == 1);

            eeprom.get(eepromOffset + 1, value);
            REQUIRE(value == 2);

            eeprom.get(eepromOffset + 2, value);
            REQUIRE(value == 3);
        }
    }

    SECTION("The offset is out of range")
    {
        THEN("get returns the value as erased")
        {
            eeprom.get(65000, value);
            REQUIRE(value == 0xFF);
        }
    }
}

TEST_CASE("Get multi-byte", "[eeprom]")
{
    TestEEPROM eeprom;
    eeprom.init();

    uint16_t eepromOffset = 10;
    uint8_t values[3];
    auto requireValues = [&](uint8_t v1, uint8_t v2, uint8_t v3)
    {
        REQUIRE(values[0] == v1);
        REQUIRE(values[1] == v2);
        REQUIRE(values[2] == v3);
    };

    SECTION("The offsets were not programmed")
    {
        SECTION("No other records")
        {
            THEN("get returns the values as erased")
            {
                eeprom.get(eepromOffset, values, sizeof(values));
                requireValues(0xFF, 0xFF, 0xFF);
            }
        }

        SECTION("With other records")
        {
            eeprom.put(0, 0xAA);

            THEN("get returns the values as erased")
            {
                eeprom.get(eepromOffset, values, sizeof(values));
                requireValues(0xFF, 0xFF, 0xFF);
            }
        }

        SECTION("With a partially written block of records")
        {
            // It takes 6 writes to write the 3 data records, followed
            // by the 3 valid statuses, so discard everything after the
            // first invalid record write
            eeprom.store.discardWritesAfter(1, [&] {
                uint8_t partialValues[] = { 1, 2, 3 };
                eeprom.put(eepromOffset, partialValues, sizeof(partialValues));
            });

            THEN("get returns the values as erased")
            {
                eeprom.get(eepromOffset, values, sizeof(values));
                requireValues(0xFF, 0xFF, 0xFF);
            }
        }

        SECTION("With a partially validated block of records")
        {
            // It takes 6 writes to write the 3 data records, followed
            // by the 3 valid statuses, so discard the 6th write
            eeprom.store.discardWritesAfter(5, [&] {
                uint8_t partialValues[] = { 1, 2, 3 };
                eeprom.put(eepromOffset, partialValues, sizeof(partialValues));
            });

            THEN("get returns the values as erased")
            {
                eeprom.get(eepromOffset, values, sizeof(values));
                requireValues(0xFF, 0xFF, 0xFF);
            }
        }
    }

    SECTION("The offsets were programmed")
    {
        uint8_t previousValues[] = { 10, 20, 30 };

        eeprom.put(eepromOffset, previousValues, sizeof(previousValues));

        SECTION("No other records")
        {
            THEN("get returns the values as previously programmed")
            {
                eeprom.get(eepromOffset, values, sizeof(values));
                requireValues(10, 20, 30);
            }
        }

        SECTION("With other records")
        {
            eeprom.put(0, 0xAA);

            THEN("get returns the values as previously programmed")
            {
                eeprom.get(eepromOffset, values, sizeof(values));
                requireValues(10, 20, 30);
            }
        }

        SECTION("With a partially written block of records")
        {
            // It takes 4 writes to write the 2 data records, followed
            // by the 2 valid statuses, so discard everything after the
            // first invalid record write
            eeprom.store.discardWritesAfter(1, [&] {
                uint8_t partialValues[] = { 2, 3 };
                eeprom.put(eepromOffset + 1, partialValues, sizeof(partialValues));
            });

            THEN("get returns the values as previously programmed")
            {
                eeprom.get(eepromOffset, values, sizeof(values));
                requireValues(10, 20, 30);
            }
        }

        SECTION("With a partially validated block of records")
        {
            // It takes 4 writes to write the 2 data records, followed
            // by the 2 valid statuses, so discard the 4th write
            eeprom.store.discardWritesAfter(3, [&] {
                uint8_t partialValues[] = { 2, 3 };
                eeprom.put(eepromOffset + 1, partialValues, sizeof(partialValues));
            });

            THEN("get returns the values as previously programmed")
            {
                eeprom.get(eepromOffset, values, sizeof(values));
                requireValues(10, 20, 30);
            }
        }

        SECTION("With a fully written block of records")
        {
            uint8_t newValues[] = { 2, 3 };
            eeprom.put(eepromOffset + 1, newValues, sizeof(newValues));

            THEN("get returns the new values")
            {
                eeprom.get(eepromOffset, values, sizeof(values));
                requireValues(10, 2, 3);
            }
        }
    }

    SECTION("Some offsets were programmed")
    {
        uint8_t previousValues[] = { 10, 20 };

        eeprom.put(eepromOffset, previousValues, sizeof(previousValues));

        THEN("get returns the values as previously programmed and erase for missing values")
        {
            eeprom.get(eepromOffset, values, sizeof(values));
            requireValues(10, 20, 0xFF);
        }
    }
}

TEST_CASE("Put record", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    uint16_t eepromOffset = 0;

    SECTION("The record doesn't exist")
    {
        THEN("put creates the record")
        {
            eeprom.put(eepromOffset, 0xCC);

            uint32_t address = store.recordAddress(PageBase1, 0);
            store.requireValidRecord(address, eepromOffset, 0xCC);
        }

        THEN("get returns the put record")
        {
            eeprom.put(eepromOffset, 0xCC);

            uint8_t newRecord;
            eeprom.get(eepromOffset, newRecord);

            REQUIRE(newRecord == 0xCC);
        }
    }

    SECTION("A bad record exists")
    {
        eeprom.store.discardWritesAfter(1, [&] {
            eeprom.put(eepromOffset, 0xEE);
        });

        THEN("put triggers a page swap")
        {
            REQUIRE(eeprom.getActivePage() == Page1);

            eeprom.put(eepromOffset, 0xCC);

            REQUIRE(eeprom.getActivePage() == Page2);
        }

        THEN("put creates the record")
        {
            eeprom.put(eepromOffset, 0xCC);

            uint8_t newRecord;
            eeprom.get(eepromOffset, newRecord);

            REQUIRE(newRecord == 0xCC);
        }
    }

    SECTION("The record exists")
    {
        eeprom.put(eepromOffset, 0xCC);

        THEN("put creates a new copy of the record")
        {
            eeprom.put(eepromOffset, 0xDD);

            uint32_t address = store.recordAddress(PageBase1, 1);
            store.requireValidRecord(address, eepromOffset, 0xDD);
        }

        THEN("get returns the put record")
        {
            eeprom.put(eepromOffset, 0xDD);

            uint8_t newRecord;
            eeprom.get(eepromOffset, newRecord);

            REQUIRE(newRecord == 0xDD);
        }
    }

    SECTION("The new record value is the same as the current one")
    {
        eeprom.put(eepromOffset, 0xCC);

        uintptr_t originalEmptyAddress = eeprom.findEmptyAddress(eeprom.getActivePage());

        THEN("put doesn't create a new copy of the record")
        {
            eeprom.put(eepromOffset, 0xCC);

            uintptr_t emptyAddress = eeprom.findEmptyAddress(eeprom.getActivePage());
            REQUIRE(emptyAddress == originalEmptyAddress);
        }
    }

    SECTION("The address is out of range")
    {
        uintptr_t originalEmptyAddress = eeprom.findEmptyAddress(eeprom.getActivePage());

        THEN("put doesn't create a new record")
        {
            eeprom.put(65000, 0xEE);

            uintptr_t emptyAddress = eeprom.findEmptyAddress(eeprom.getActivePage());
            REQUIRE(emptyAddress == originalEmptyAddress);
        }
    }

    SECTION("Page swap is required")
    {
        uint16_t writesToFillPage1 = PageSize1 / sizeof(TestEEPROM::Record) - 1;

        // Write multiple copies of the same record until page 1 is full
        for(uint32_t i = 0; i < writesToFillPage1; i++)
        {
            eeprom.put(eepromOffset, (uint8_t)i);
        }

        REQUIRE(eeprom.getActivePage() == Page1);

        THEN("The next write performs a page swap")
        {
            uint8_t newRecord = 0;
            eeprom.put(eepromOffset, newRecord);

            REQUIRE(eeprom.getActivePage() == Page2);
        }
    }
}

TEST_CASE("Capacity", "[eeprom]")
{
    TestEEPROM eeprom;
    // Each record is 4 bytes, and some space is used by the page header
    size_t expectedByteCapacity = PageSize2 / 4 - 1;

    REQUIRE(eeprom.capacity() == expectedByteCapacity);
}

TEST_CASE("Initialize EEPROM", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    SECTION("Random flash")
    {
        eeprom.init();

        THEN("Page 1 is active, page 2 is erased")
        {
            store.requirePageStatus(PageBase1, PAGE_ACTIVE);
            store.requirePageStatus(PageBase2, PAGE_ERASED);
        }
    }

    SECTION("Erased flash")
    {
        store.eraseAll();

        eeprom.init();

        THEN("Page 1 is active, page 2 is erased")
        {
            store.requirePageStatus(PageBase1, PAGE_ACTIVE);
            store.requirePageStatus(PageBase2, PAGE_ERASED);
        }
    }

    SECTION("Page 1 active")
    {
        store.eraseAll();
        store.writePageStatus(PageBase1, PAGE_ACTIVE);

        eeprom.init();

        THEN("Page 1 remains active, page 2 remains erased")
        {
            store.requirePageStatus(PageBase1, PAGE_ACTIVE);
            store.requirePageStatus(PageBase2, PAGE_ERASED);
        }
    }

    SECTION("Page 2 active")
    {
        store.eraseAll();
        store.writePageStatus(PageBase2, PAGE_ACTIVE);

        eeprom.init();

        THEN("Page 1 remains erased, page 2 remains active")
        {
            store.requirePageStatus(PageBase1, PAGE_ERASED);
            store.requirePageStatus(PageBase2, PAGE_ACTIVE);
        }
    }
}

TEST_CASE("Clear", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();
    eeprom.clear();

    THEN("Page 1 is active, page 2 is erased")
    {
        store.requirePageStatus(PageBase1, PAGE_ACTIVE);
        store.requirePageStatus(PageBase2, PAGE_ERASED);
    }
}

TEST_CASE("Verify page", "[eeprom]")
{
    TestEEPROM eeprom;

    SECTION("random flash")
    {
        REQUIRE(eeprom.verifyPage(Page1) == false);
    }

    SECTION("erased flash")
    {
        eeprom.store.eraseSector(PageBase1);
        REQUIRE(eeprom.verifyPage(Page1) == true);
    }

    SECTION("partially erased flash")
    {
        eeprom.store.eraseSector(PageBase1);

        uint8_t garbage = 0xCC;
        eeprom.store.write(PageBase1 + 100, &garbage, sizeof(garbage));

        REQUIRE(eeprom.verifyPage(Page1) == false);
    }
}

TEST_CASE("Active page", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    store.eraseAll();

    SECTION("No valid page")
    {
        SECTION("Page 1 erased, page 2 erased (blank flash)")
        {
            eeprom.updateActivePage();
            REQUIRE(eeprom.getActivePage() == NoPage);
        }

        SECTION("Page 1 garbage, page 2 garbage (invalid state)")
        {
            store.writePageStatus(PageBase1, 999);
            store.writePageStatus(PageBase2, 999);
            eeprom.updateActivePage();

            REQUIRE(eeprom.getActivePage() == NoPage);
        }
    }

    SECTION("Page 1 valid")
    {
        SECTION("Page 1 active, page 2 erased (normal case)")
        {
            store.writePageStatus(PageBase1, PAGE_ACTIVE);
            eeprom.updateActivePage();

            REQUIRE(eeprom.getActivePage() == Page1);
        }
    }

    SECTION("Steps of swap from page 1 to page 2")
    {
        SECTION("Page 1 active, page 2 copy (interrupted swap)")
        {
            store.writePageStatus(PageBase1, PAGE_ACTIVE);
            store.writePageStatus(PageBase2, PAGE_COPY);
            eeprom.updateActivePage();

            REQUIRE(eeprom.getActivePage() == Page1);
        }

        SECTION("Page 1 active, page 2 active (almost completed swap)")
        {
            store.writePageStatus(PageBase1, PAGE_ACTIVE);
            store.writePageStatus(PageBase2, PAGE_ACTIVE);
            eeprom.updateActivePage();

            REQUIRE(eeprom.getActivePage() == Page1);
        }
    }

    SECTION("Page 2 valid")
    {
        SECTION("Page 1 erased, page 2 active (normal case)")
        {
            store.writePageStatus(PageBase2, PAGE_ACTIVE);
            eeprom.updateActivePage();

            REQUIRE(eeprom.getActivePage() == Page2);
        }
    }

    SECTION("Steps of swap from page 2 to page 1")
    {
        SECTION("Page 1 copy, page 2 active (interrupted swap)")
        {
            store.writePageStatus(PageBase1, PAGE_COPY);
            store.writePageStatus(PageBase2, PAGE_ACTIVE);
            eeprom.updateActivePage();

            REQUIRE(eeprom.getActivePage() == Page2);
        }

        SECTION("Page 1 active, page 2 active (almost completed swap)")
        {
            store.writePageStatus(PageBase1, PAGE_ACTIVE);
            store.writePageStatus(PageBase2, PAGE_ACTIVE);
            eeprom.updateActivePage();

            REQUIRE(eeprom.getActivePage() == Page1);
        }
    }
}

TEST_CASE("Alternate page", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    store.eraseAll();

    SECTION("Page 1 is active")
    {
        store.writePageStatus(PageBase1, PAGE_ACTIVE);
        eeprom.updateActivePage();

        REQUIRE(eeprom.getAlternatePage() == Page2);
    }

    SECTION("Page 2 is active")
    {
        store.writePageStatus(PageBase2, PAGE_ACTIVE);
        eeprom.updateActivePage();

        REQUIRE(eeprom.getAlternatePage() == Page1);
    }

    SECTION("No page is valid")
    {
        // Not necessary to test when no page is active since that
        // condition will be fixed at boot in init()
    }
}

TEST_CASE("Copy records to page", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    uint32_t activeOffset = PageBase1 + 2;
    uint32_t alternateOffset = PageBase2 + 2;
    auto fromPage = Page1;
    auto toPage = Page2;
    uint16_t exceptRecordIdStart = 0xFFFF;
    uint16_t exceptRecordIdEnd = 0xFFFF;

    uint16_t eepromOffset = 100;

    SECTION("Single record")
    {
        eeprom.put(eepromOffset, 0xBB);

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The record is copied")
        {
            store.requireValidRecord(alternateOffset, eepromOffset, 0xBB);
        }
    }

    SECTION("Multiple copies of a record")
    {
        eeprom.put(eepromOffset, 0xBB);
        eeprom.put(eepromOffset, 0xCC);

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The last record is copied, followed by empty space")
        {
            store.requireValidRecord(alternateOffset, eepromOffset, 0xCC);
        }
    }

    SECTION("Multiple copies of a record followed by an invalid record")
    {
        eeprom.put(eepromOffset, 0xBB);
        eeprom.put(eepromOffset, 0xCC);
        eeprom.store.discardWritesAfter(1, [&] {
            eeprom.put(eepromOffset, 0xEE);
        });

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The last valid record is copied")
        {
            alternateOffset = store.requireValidRecord(alternateOffset, eepromOffset, 0xCC);
            alternateOffset = store.requireEmptyRecord(alternateOffset);
        }
    }

    SECTION("Record with 0xFF value")
    {
        eeprom.put(eepromOffset, 0xBB);
        eeprom.put(eepromOffset, 0xFF);

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The record is not copied")
        {
            alternateOffset = store.requireEmptyRecord(alternateOffset);
        }
    }

    SECTION("Multiple records")
    {
        uint16_t eepromOffsets[] = { 30, 10, 40 };
        uint8_t record = 0xAA;

        for(auto offset : eepromOffsets)
        {
            eeprom.put(offset, record);
        }

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The records are copied from small ids to large ids")
        {
            alternateOffset = store.requireValidRecord(alternateOffset, eepromOffsets[1], record);
            alternateOffset = store.requireValidRecord(alternateOffset, eepromOffsets[0], record);
            alternateOffset = store.requireValidRecord(alternateOffset, eepromOffsets[2], record);
        }
    }

    SECTION("Except specified records")
    {
        uint16_t eepromOffsets[] = { 30, 10, 40 };
        uint8_t record = 0xAA;

        for(auto offset : eepromOffsets)
        {
            eeprom.put(offset, record);
        }

        exceptRecordIdStart = 10;
        exceptRecordIdEnd = 10;

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The specified records are not copied")
        {
            alternateOffset = store.requireValidRecord(alternateOffset, eepromOffsets[0], record);
            alternateOffset = store.requireValidRecord(alternateOffset, eepromOffsets[2], record);
            alternateOffset = store.requireEmptyRecord(alternateOffset);
        }
    }

    SECTION("With invalid records")
    {
        eeprom.put(eepromOffset, 0xAA);
        eeprom.store.discardWritesAfter(1, [&] {
            eeprom.put(200, 0xEE);
        });

        THEN("Records up to the invalid record are copied")
        {
            eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

            // The copied record is followed by empty space
            alternateOffset = store.requireValidRecord(alternateOffset, eepromOffset, 0xAA);
            alternateOffset = store.requireEmptyRecord(alternateOffset);
        }
    }
}

TEST_CASE("Swap pages", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    uint32_t activeOffset = PageBase1;
    uint32_t alternateOffset = PageBase2;

    // Write some data
    uint16_t eepromOffset = 0;
    uint8_t data[] = { 1, 2, 3 };
    eeprom.put(eepromOffset, data, sizeof(data));

    // Have a record to write after the swap
    uint16_t newRecordId = 1;
    uint8_t newData[] = { 20, 30 };

    auto requireSwapCompleted = [&]()
    {
        store.requirePageStatus(activeOffset, PAGE_ERASED);
        store.requirePageStatus(alternateOffset, PAGE_ACTIVE);

        uintptr_t dataOffset = alternateOffset + 2;
        dataOffset = store.requireValidRecord(dataOffset, 0, 1);
        dataOffset = store.requireValidRecord(dataOffset, 1, 20);
        dataOffset = store.requireValidRecord(dataOffset, 2, 30);
    };

    auto performSwap = [&]()
    {
        eeprom.swapPagesAndWrite(newRecordId, newData, sizeof(newData));
    };

    SECTION("No interruption")
    {
        performSwap();

        requireSwapCompleted();
    }

    SECTION("Interrupted page swap 1: during erase")
    {
        // Garbage status
        store.writePageStatus(alternateOffset, 999);
        eeprom.store.setWriteCount(0);

        performSwap();

        // Verify that the alternate page is not yet erased
        store.requirePageStatus(alternateOffset, 999);

        THEN("Redoing the page swap works")
        {
            eeprom.store.setWriteCount(INT_MAX);

            performSwap();

            requireSwapCompleted();
        }
    }

    SECTION("Interrupted page swap 2: during copy")
    {
        eeprom.store.setWriteCount(2);
        performSwap();

        // Verify that the alternate page is still copy
        store.requirePageStatus(alternateOffset, PAGE_COPY);

        THEN("Redoing the page swap works")
        {
            eeprom.store.setWriteCount(INT_MAX);

            performSwap();

            requireSwapCompleted();
        }
    }

    SECTION("Interrupted page swap 3: before old page gets erased")
    {
        eeprom.store.setWriteCount(5);

        performSwap();

        // Verify that both pages are active
        store.requirePageStatus(alternateOffset, PAGE_ACTIVE);
        store.requirePageStatus(activeOffset, PAGE_ACTIVE);

        THEN("Page 1 remains the active page")
        {
            eeprom.store.setWriteCount(INT_MAX);

            REQUIRE(eeprom.getActivePage() == Page1);
        }
    }
}

//TEST_CASE("Erasable page", "[eeprom]")
//{
//    TestEEPROM eeprom;
//    StoreManipulator store(eeprom.store);
//
//    store.eraseAll();
//
//    uint32_t activeOffset = PageBase1;
//    uint32_t alternateOffset = PageBase2;
//
//    SECTION("One active page, one erased page")
//    {
//        store.writePageStatus(activeOffset, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        THEN("No page needs to be erased")
//        {
//            REQUIRE(eeprom.getPendingErasePage() == NoPage);
//            REQUIRE(eeprom.hasPendingErase() == false);
//        }
//    }
//
//    SECTION("One active page, one inactive page")
//    {
//        store.writePageStatus(activeOffset, PAGE_ACTIVE);
//        store.writePageStatus(alternateOffset, PAGE_INACTIVE);
//        eeprom.updateActivePage();
//
//        THEN("The old page needs to be erased")
//        {
//            REQUIRE(eeprom.getPendingErasePage() == Page2);
//            REQUIRE(eeprom.hasPendingErase() == true);
//        }
//
//        THEN("Erasing the old page clear it")
//        {
//            eeprom.performPendingErase();
//
//            REQUIRE(eeprom.getPendingErasePage() == NoPage);
//            REQUIRE(eeprom.hasPendingErase() == false);
//        }
//    }
//
//    SECTION("2 active pages")
//    {
//        store.writePageStatus(activeOffset, PAGE_ACTIVE);
//        store.writePageStatus(alternateOffset, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        THEN("Page 2 needs to be erased")
//        {
//            REQUIRE(eeprom.getPendingErasePage() == Page2);
//            REQUIRE(eeprom.hasPendingErase() == true);
//        }
//    }
//}
