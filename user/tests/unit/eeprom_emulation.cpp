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
using TestEEPROM = EEPROMEmulationByte<TestStore, PageBase1, PageSize1, PageBase2, PageSize2>;

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

    uintptr_t recordOffset(uintptr_t baseOffset, uint16_t recordId)
    {
        // Page header is 2 bytes, each record is 4 bytes
        return baseOffset + 2 + 4 * recordId;
    }

    void writePageStatus(uintptr_t offset, uint16_t status)
    {
        store.write(offset, &status, sizeof(status));
    }

    uint16_t readPageStatus(uintptr_t offset)
    {
        uint16_t status;
        store.read(offset, &status, sizeof(status));
        return status;
    }

    void requirePageStatus(uintptr_t offset, uint16_t expectedStatus)
    {
        REQUIRE(readPageStatus(offset) == expectedStatus);
    }

    // Interrupted record write 1: id, invalid status and data written
    uintptr_t writeInvalidRecord(uintptr_t offset, uint16_t id, uint8_t data)
    {
        TestEEPROM::Record record(TestEEPROM::Record::INVALID, id, data);
        store.write(offset, &record, sizeof(record));

        return offset + sizeof(record);
    }

    // Completely written record
    uintptr_t writeRecord(uintptr_t offset, uint16_t id, uint8_t data)
    {
        TestEEPROM::Record record(TestEEPROM::Record::VALID, id, data);
        store.write(offset, &record, sizeof(record));

        return offset + sizeof(record);
    }

    // Validates that a specific record was correctly written at the specified offset
    uintptr_t requireValidRecord(uintptr_t offset, uint16_t id, uint8_t expected)
    {
        TestEEPROM::Record record;
        store.read(offset, &record, sizeof(record));

        auto status = TestEEPROM::Record::VALID;
        REQUIRE(record.status == status);
        REQUIRE(record.id == id);
        REQUIRE(record.data == expected);

        return offset + sizeof(record);
    }

    // Validate that a specific offset has no record (erased space)
    uintptr_t requireEmptyRecord(uintptr_t offset)
    {
        TestEEPROM::Record erasedRecord, record;
        store.read(offset, &record, sizeof(record));
        REQUIRE(std::memcmp(&record, &erasedRecord, sizeof(record)) == 0);

        return offset + sizeof(record);
    }

    // Debugging helper to view the storage contents
    // Usage:
    // WARN(store.dumpStorage(PageBase1, 30));
    std::string dumpStorage(uintptr_t offset, uint16_t length)
    {
        std::stringstream ss;
        const uint8_t *begin = store.dataAt(offset);
        const uint8_t *end = &begin[length];

        ss << std::hex << offset << ": ";
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
    uint16_t recordId = 10;

    SECTION("The address was not programmed")
    {
        SECTION("No other records")
        {
            THEN("get returns the value as erased")
            {
                eeprom.get(recordId, value);
                REQUIRE(value == 0xFF);
            }
        }

        SECTION("With other records")
        {
            eeprom.put(0, 0xAA);

            THEN("get returns the value as erased")
            {
                eeprom.get(recordId, value);
                REQUIRE(value == 0xFF);
            }
        }

        SECTION("With a partially written record")
        {
            eeprom.store.discardWritesAfter(1, [&] {
                eeprom.put(recordId, 0xEE);
            });

            THEN("get returns the value as erased")
            {
                eeprom.get(recordId, value);
                REQUIRE(value == 0xFF);
            }
        }
    }

    SECTION("The address was programmed")
    {
        eeprom.put(recordId, 0xCC);

        SECTION("No other records")
        {
            THEN("get extracts the value")
            {
                eeprom.get(recordId, value);
                REQUIRE(value == 0xCC);
            }
        }

        SECTION("Followed by a partially written record")
        {
            eeprom.store.discardWritesAfter(1, [&] {
                eeprom.put(recordId, 0xEE);
            });

            THEN("get extracts the last valid value")
            {
                eeprom.get(recordId, value);
                REQUIRE(value == 0xCC);
            }
        }

        SECTION("Followed by a fully written record")
        {
            eeprom.put(recordId, 0xEE);

            THEN("get extracts the new valid value")
            {
                eeprom.get(recordId, value);
                REQUIRE(value == 0xEE);
            }
        }

    }

    SECTION("The address was programmed by a multi-byte put")
    {
        uint16_t recordId = 0;
        uint8_t values[] = { 1, 2, 3 };

        eeprom.put(recordId, values, sizeof(values));

        THEN("get extracts the value")
        {
            eeprom.get(recordId, value);
            REQUIRE(value == 1);
        }
    }

    SECTION("The address is out of range")
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

    uint16_t recordId = 10;
    uint8_t values[3];
    auto requireValues = [&](uint8_t v1, uint8_t v2, uint8_t v3)
    {
        REQUIRE(values[0] == v1);
        REQUIRE(values[1] == v2);
        REQUIRE(values[2] == v3);
    };

    SECTION("The addresses were not programmed")
    {
        SECTION("No other records")
        {
            THEN("get returns the values as erased")
            {
                eeprom.get(recordId, values, sizeof(values));
                requireValues(0xFF, 0xFF, 0xFF);
            }
        }

        SECTION("With other records")
        {
            eeprom.put(0, 0xAA);

            THEN("get returns the values as erased")
            {
                eeprom.get(recordId, values, sizeof(values));
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
                eeprom.put(recordId, partialValues, sizeof(partialValues));
            });

            THEN("get returns the values as erased")
            {
                eeprom.get(recordId, values, sizeof(values));
                requireValues(0xFF, 0xFF, 0xFF);
            }
        }

        SECTION("With a partially validated block of records")
        {
            // It takes 6 writes to write the 3 data records, followed
            // by the 3 valid statuses, so discard the 6th write
            eeprom.store.discardWritesAfter(5, [&] {
                uint8_t partialValues[] = { 1, 2, 3 };
                eeprom.put(recordId, partialValues, sizeof(partialValues));
            });

            THEN("get returns the values as erased")
            {
                eeprom.get(recordId, values, sizeof(values));
                requireValues(0xFF, 0xFF, 0xFF);
            }
        }
    }

    SECTION("The addresses were programmed")
    {
        uint8_t previousValues[] = { 10, 20, 30 };

        eeprom.put(recordId, previousValues, sizeof(values));

        SECTION("No other records")
        {
            THEN("get returns the values as previously programmed")
            {
                eeprom.get(recordId, values, sizeof(values));
                requireValues(10, 20, 30);
            }
        }

        SECTION("With other records")
        {
            eeprom.put(0, 0xAA);

            THEN("get returns the values as previously programmed")
            {
                eeprom.get(recordId, values, sizeof(values));
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
                eeprom.put(recordId + 1, partialValues, sizeof(partialValues));
            });

            THEN("get returns the values as previously programmed")
            {
                eeprom.get(recordId, values, sizeof(values));
                requireValues(10, 20, 30);
            }
        }

        SECTION("With a partially validated block of records")
        {
            // It takes 4 writes to write the 2 data records, followed
            // by the 2 valid statuses, so discard the 4th write
            eeprom.store.discardWritesAfter(3, [&] {
                uint8_t partialValues[] = { 2, 3 };
                eeprom.put(recordId + 1, partialValues, sizeof(partialValues));
            });

            THEN("get returns the values as previously programmed")
            {
                eeprom.get(recordId, values, sizeof(values));
                requireValues(10, 20, 30);
            }
        }

        SECTION("With a fully written block of records")
        {
            uint8_t newValues[] = { 2, 3 };
            eeprom.put(recordId + 1, newValues, sizeof(newValues));

            THEN("get returns the new values")
            {
                eeprom.get(recordId, values, sizeof(values));
                requireValues(10, 2, 3);
            }
        }
    }
}

TEST_CASE("Put record", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    uint16_t recordId = 0;

    SECTION("The record doesn't exist")
    {
        THEN("put creates the record")
        {
            eeprom.put(recordId, 0xCC);

            uint32_t offset = store.recordOffset(PageBase1, 0);
            store.requireValidRecord(offset, recordId, 0xCC);
        }

        THEN("get returns the put record")
        {
            eeprom.put(recordId, 0xCC);

            uint8_t newRecord;
            eeprom.get(recordId, newRecord);

            REQUIRE(newRecord == 0xCC);
        }
    }

    SECTION("A bad record exists")
    {
        eeprom.store.discardWritesAfter(1, [&] {
            eeprom.put(recordId, 0xEE);
        });

        THEN("put triggers a page swap")
        {
            REQUIRE(eeprom.getActivePage() == Page1);

            eeprom.put(recordId, 0xCC);

            REQUIRE(eeprom.getActivePage() == Page2);
        }

        THEN("put creates the record")
        {
            eeprom.put(recordId, 0xCC);

            uint8_t newRecord;
            eeprom.get(recordId, newRecord);

            REQUIRE(newRecord == 0xCC);
        }
    }

    SECTION("The record exists")
    {
        eeprom.put(recordId, 0xCC);

        THEN("put creates a new copy of the record")
        {
            eeprom.put(recordId, 0xDD);

            uint32_t offset = store.recordOffset(PageBase1, 1);
            store.requireValidRecord(offset, recordId, 0xDD);
        }

        THEN("get returns the put record")
        {
            eeprom.put(recordId, 0xDD);

            uint8_t newRecord;
            eeprom.get(recordId, newRecord);

            REQUIRE(newRecord == 0xDD);
        }
    }

    SECTION("The new record value is the same as the current one")
    {
        eeprom.put(recordId, 0xCC);

        uintptr_t originalEmptyOffset = eeprom.findEmptyOffset(eeprom.getActivePage());

        THEN("put doesn't create a new copy of the record")
        {
            eeprom.put(recordId, 0xCC);

            uintptr_t emptyOffset = eeprom.findEmptyOffset(eeprom.getActivePage());
            REQUIRE(emptyOffset == originalEmptyOffset);
        }
    }

    SECTION("The address is out of range")
    {
        uintptr_t originalEmptyOffset = eeprom.findEmptyOffset(eeprom.getActivePage());

        THEN("put doesn't create a new record")
        {
            eeprom.put(65000, 0xEE);

            uintptr_t emptyOffset = eeprom.findEmptyOffset(eeprom.getActivePage());
            REQUIRE(emptyOffset == originalEmptyOffset);
        }
    }

    SECTION("Page swap is required")
    {
        uint16_t writesToFillPage1 = PageSize1 / sizeof(TestEEPROM::Record) - 1;

        // Write multiple copies of the same record until page 1 is full
        for(uint32_t i = 0; i < writesToFillPage1; i++)
        {
            eeprom.put(recordId, (uint8_t)i);
        }

        REQUIRE(eeprom.getActivePage() == Page1);

        THEN("The next write performs a page swap")
        {
            uint8_t newRecord = 0;
            eeprom.put(recordId, newRecord);

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

    // No valid page

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

    // Page 1 valid

    SECTION("Page 1 active, page 2 erased (normal case)")
    {
        store.writePageStatus(PageBase1, PAGE_ACTIVE);
        eeprom.updateActivePage();

        REQUIRE(eeprom.getActivePage() == Page1);
    }

    // Steps of swap from page 1 to page 2

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

    // Page 2 valid

    SECTION("Page 1 erased, page 2 active (normal case)")
    {
        store.writePageStatus(PageBase2, PAGE_ACTIVE);
        eeprom.updateActivePage();

        REQUIRE(eeprom.getActivePage() == Page2);
    }

    // Steps of swap from page 2 to page 1

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

    // Not necessary to test when no page is active since that
    // condition will be fixed at boot in init()
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

    uint16_t recordId = 100;

    SECTION("Single record")
    {
        eeprom.put(recordId, 0xBB);

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The record is copied")
        {
            store.requireValidRecord(alternateOffset, recordId, 0xBB);
        }
    }

    SECTION("Multiple copies of a record")
    {
        eeprom.put(recordId, 0xBB);
        eeprom.put(recordId, 0xCC);

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The last record is copied, followed by empty space")
        {
            store.requireValidRecord(alternateOffset, recordId, 0xCC);
        }
    }

    SECTION("Multiple copies of a record followed by an invalid record")
    {
        eeprom.put(recordId, 0xBB);
        eeprom.put(recordId, 0xCC);
        eeprom.store.discardWritesAfter(1, [&] {
            eeprom.put(recordId, 0xEE);
        });

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The last valid record is copied")
        {
            alternateOffset = store.requireValidRecord(alternateOffset, recordId, 0xCC);
            alternateOffset = store.requireEmptyRecord(alternateOffset);
        }
    }

    SECTION("Record with 0xFF value")
    {
        eeprom.put(recordId, 0xBB);
        eeprom.put(recordId, 0xFF);

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The record is not copied")
        {
            alternateOffset = store.requireEmptyRecord(alternateOffset);
        }
    }

    SECTION("Multiple records")
    {
        uint16_t recordIds[] = { 30, 10, 40 };
        uint8_t record = 0xAA;

        for(auto id : recordIds)
        {
            eeprom.put(id, record);
        }

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The records are copied from small ids to large ids")
        {
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[1], record);
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[0], record);
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[2], record);
        }
    }

    SECTION("Except specified records")
    {
        uint16_t recordIds[] = { 30, 10, 40 };
        uint8_t record = 0xAA;

        for(auto id : recordIds)
        {
            eeprom.put(id, record);
        }

        exceptRecordIdStart = 10;
        exceptRecordIdEnd = 10;

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The specified records are not copied")
        {
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[0], record);
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[2], record);
            alternateOffset = store.requireEmptyRecord(alternateOffset);
        }
    }

    SECTION("With invalid records")
    {
        eeprom.put(recordId, 0xAA);
        eeprom.store.discardWritesAfter(1, [&] {
            eeprom.put(200, 0xEE);
        });

        THEN("Records up to the invalid record are copied")
        {
            eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

            // The copied record is followed by empty space
            alternateOffset = store.requireValidRecord(alternateOffset, recordId, 0xAA);
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
    uint16_t recordId = 0;
    uint8_t data[] = { 1, 2, 3 };
    eeprom.put(recordId, data, sizeof(data));

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
//
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
