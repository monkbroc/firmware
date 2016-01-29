// Automatic test runner:
// user/tests/unit$ rerun -x -p "**/*.{cpp,h}" -d .,../../../services "make runner && obj/runner [eeprom_byte]"
#include "catch.hpp"
#include <string>
#include <sstream>
#include "eeprom_emulation_byte.h"
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

TEST_CASE("NODUP Get byte", "[eeprom_byte]")
{
    TestEEPROM eeprom;

    eeprom.init();

    uint8_t value;

    SECTION("The address was not programmed")
    {
        uint16_t recordId = 10;
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
        uint16_t recordId = 0;
        uint8_t record = 0xCC;

        eeprom.put(recordId, record);

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

TEST_CASE("NODUP Get multi-byte", "[eeprom_byte]")
{
    TestEEPROM eeprom;
    eeprom.init();

    uint8_t values[3];
    auto requireValues = [&](uint8_t v1, uint8_t v2, uint8_t v3)
    {
        REQUIRE(values[0] == v1);
        REQUIRE(values[1] == v2);
        REQUIRE(values[2] == v3);
    };

    SECTION("The addresses were not programmed")
    {
        uint16_t recordId = 10;
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
        uint16_t recordId = 10;
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

TEST_CASE("NODUP Put record", "[eeprom_byte]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    uint32_t offset = PageBase1 + 2;

    SECTION("The record doesn't exist")
    {
        uint16_t recordId = 0;
        uint8_t record = 0xDD;

        THEN("put creates the record")
        {
            eeprom.put(recordId, record);

            store.requireValidRecord(offset, recordId, record);
        }

        THEN("get returns the put record")
        {
            eeprom.put(recordId, record);

            uint8_t newRecord;
            eeprom.get(recordId, newRecord);

            REQUIRE(newRecord == 0xDD);
        }
    }

    //SECTION("A bad record exists")
    //{
    //    // --------> this should trigger a page swap
    //
    //    uint16_t recordId = 0;
    //    uint8_t badRecord = 0xEE;
    //    offset = store.writeRecordHeader(offset, recordId, badRecord);

    //    uint8_t record = 0xDD;

    //    THEN("put returns true and creates the record")
    //    {
    //        REQUIRE(eeprom.put(recordId, record) == true);
    //        store.requireValidRecord(offset, recordId, record);
    //    }

    //    THEN("get returns the put record")
    //    {
    //        eeprom.put(recordId, record);
    //        uint8_t newRecord;
    //        REQUIRE(eeprom.get(recordId, newRecord) == true);
    //        REQUIRE(newRecord == 0xDD);
    //    }
    //}

    SECTION("The record exists")
    {
        uint16_t recordId = 0;
        uint8_t previousRecord = 0xCC;

        offset = store.writeRecord(offset, recordId, previousRecord);

        uint8_t record = 0xDD;

        THEN("put creates a new copy of the record")
        {
            eeprom.put(recordId, record);

            store.requireValidRecord(offset, recordId, record);
        }

        THEN("get returns the put record")
        {
            eeprom.put(recordId, record);

            uint8_t newRecord;
            eeprom.get(recordId, newRecord);

            REQUIRE(newRecord == 0xDD);
        }
    }

    SECTION("The new record value is the same as the current one")
    {
        uint16_t recordId = 0;
        uint8_t record = 0xCC;

        eeprom.put(recordId, record);

        uintptr_t originalEmptyOffset = eeprom.findEmptyOffset(eeprom.getActivePage());

        THEN("put doesn't create a new copy of the record")
        {
            eeprom.put(recordId, record);

            uintptr_t emptyOffset = eeprom.findEmptyOffset(eeprom.getActivePage());
            REQUIRE(emptyOffset == originalEmptyOffset);
        }
    }

    SECTION("The address is out of range")
    {
        uint8_t record = 0xEE;
        uintptr_t originalEmptyOffset = eeprom.findEmptyOffset(eeprom.getActivePage());

        THEN("put doesn't create a new record")
        {
            eeprom.put(65000, record);

            uintptr_t emptyOffset = eeprom.findEmptyOffset(eeprom.getActivePage());
            REQUIRE(emptyOffset == originalEmptyOffset);
        }
    }

    //SECTION("Page swap is required")
    //{
    //    uint16_t recordSize = sizeof(uint32_t) + sizeof(TestEEPROM::Record);
    //    uint16_t writesToFillPage1 = PageSize1 / recordSize;

    //    uint16_t id = 100;

    //    // Write multiple copies of the same record until page 1 is full
    //    for(uint32_t i = 0; i < writesToFillPage1; i++)
    //    {
    //        REQUIRE(eeprom.put(id, i) == true);
    //    }

    //    REQUIRE(eeprom.getActivePage() == Page1);

    //    THEN("The next write performs a page swap")
    //    {
    //        uint32_t record = 0xDEADBEEF;
    //        REQUIRE(eeprom.put(id, record) == true);

    //        REQUIRE(eeprom.getActivePage() == Page2);
    //    }
    //}
}


//TEST_CASE("NODUP Remove record", "[eeprom_byte]")
//{
//    TestEEPROM eeprom;
//    StoreManipulator store(eeprom.store);
//    eeprom.init();
//
//    SECTION("The record doesn't exist")
//    {
//        uint16_t recordId = 0;
//
//        REQUIRE(eeprom.remove(recordId) == false);
//    }
//
//    SECTION("The record exists")
//    {
//        uint16_t recordId = 0;
//        uint8_t record = 0xCC;
//
//        eeprom.put(recordId, record);
//
//        THEN("remove returns true")
//        {
//            eeprom.remove(recordId);
//            REQUIRE(eeprom.remove(recordId) == true);
//        }
//
//        THEN("get doesn't return the removed record")
//        {
//            eeprom.remove(recordId);
//
//            uint8_t newRecord = 0;
//            REQUIRE(eeprom.get(recordId, newRecord) == false);
//            REQUIRE(newRecord != record);
//        }
//    }
//}

TEST_CASE("NODUP Capacity", "[eeprom_byte]")
{
    TestEEPROM eeprom;
    // Each record is 4 bytes, and some space is used by the page header
    size_t expectedByteCapacity = PageSize2 / 4 - 1;

    REQUIRE(eeprom.capacity() == expectedByteCapacity);
}

TEST_CASE("NODUP Initialize EEPROM", "[eeprom_byte]")
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

TEST_CASE("NODUP Clear", "[eeprom_byte]")
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

TEST_CASE("NODUP Verify page", "[eeprom_byte]")
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

//TEST_CASE("NODUP Active page", "[eeprom_byte]")
//{
//    TestEEPROM eeprom;
//    StoreManipulator store(eeprom.store);
//
//    store.eraseAll();
//
//    // No valid page
//
//    SECTION("Page 1 erased, page 2 erased (blank flash)")
//    {
//        eeprom.updateActivePage();
//        REQUIRE(eeprom.getActivePage() == NoPage);
//    }
//
//    SECTION("Page 1 garbage, page 2 garbage (invalid state)")
//    {
//        store.writePageStatus(PageBase1, 999);
//        store.writePageStatus(PageBase2, 999);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getActivePage() == NoPage);
//    }
//
//    // Page 1 valid
//
//    SECTION("Page 1 active, page 2 erased (normal case)")
//    {
//        store.writePageStatus(PageBase1, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getActivePage() == Page1);
//    }
//
//    // Steps of swap from page 1 to page 2
//
//    SECTION("Page 1 active, page 2 copy (interrupted swap)")
//    {
//        store.writePageStatus(PageBase1, PAGE_ACTIVE);
//        store.writePageStatus(PageBase2, PAGE_COPY);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getActivePage() == Page1);
//    }
//
//    SECTION("Page 1 active, page 2 active (almost completed swap)")
//    {
//        store.writePageStatus(PageBase1, PAGE_ACTIVE);
//        store.writePageStatus(PageBase2, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getActivePage() == Page1);
//    }
//
//    SECTION("Page 1 inactive, page 2 active (completed swap, pending erase)")
//    {
//        store.writePageStatus(PageBase1, PAGE_INACTIVE);
//        store.writePageStatus(PageBase2, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getActivePage() == Page2);
//    }
//
//    // Page 2 valid
//
//    SECTION("Page 1 erased, page 2 active (normal case)")
//    {
//        store.writePageStatus(PageBase2, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getActivePage() == Page2);
//    }
//
//    // Steps of swap from page 2 to page 1
//
//    SECTION("Page 1 copy, page 2 active (interrupted swap)")
//    {
//        store.writePageStatus(PageBase1, PAGE_COPY);
//        store.writePageStatus(PageBase2, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getActivePage() == Page2);
//    }
//
//    SECTION("Page 1 active, page 2 active (almost completed swap)")
//    {
//        store.writePageStatus(PageBase1, PAGE_ACTIVE);
//        store.writePageStatus(PageBase2, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getActivePage() == Page1);
//    }
//
//    SECTION("Page 1 active, page 2 inactive (completed swap, pending erase)")
//    {
//        store.writePageStatus(PageBase1, PAGE_ACTIVE);
//        store.writePageStatus(PageBase2, PAGE_INACTIVE);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getActivePage() == Page1);
//    }
//}
//
//TEST_CASE("NODUP Alternate page", "[eeprom_byte]")
//{
//    TestEEPROM eeprom;
//    StoreManipulator store(eeprom.store);
//
//    store.eraseAll();
//
//    SECTION("Page 1 is active")
//    {
//        store.writePageStatus(PageBase1, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getAlternatePage() == Page2);
//    }
//
//    SECTION("Page 2 is active")
//    {
//        store.writePageStatus(PageBase2, PAGE_ACTIVE);
//        eeprom.updateActivePage();
//
//        REQUIRE(eeprom.getAlternatePage() == Page1);
//    }
//
//    // Not necessary to test when no page is active since that
//    // condition will be fixed at boot in init()
//}

TEST_CASE("NODUP Copy records to page", "[eeprom_byte]")
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

    SECTION("Single record")
    {
        uint16_t recordId = 100;
        uint8_t record = 0xBB;
        eeprom.put(recordId, record);

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The record is copied")
        {
            store.requireValidRecord(alternateOffset, recordId, record);
        }
    }

    SECTION("Multiple copies of a record")
    {
        uint16_t recordId = 100;
        uint8_t record = 0xBB;
        eeprom.put(recordId, record);
        
        uint8_t newRecord = 0xCC;
        eeprom.put(recordId, newRecord);

        eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

        THEN("The latest record is copied")
        {
            store.requireValidRecord(alternateOffset, recordId, newRecord);
        }
    }
    
    // TODO: removed record

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

    SECTION("Except a specified record")
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

        THEN("The specified record is not copied")
        {
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[0], record);
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[2], record);
        }
    }

    SECTION("With invalid records")
    {
        // Write one valid record
        uint16_t recordId = 100;
        uint8_t record = 0xAA;
        eeprom.put(recordId, record);

        // Write one invalid record
        uint16_t invalidRecordId = 200;
        uint8_t invalidRecord = 0xCC;
        eeprom.store.setWriteCount(1);
        eeprom.put(recordId, record);
        eeprom.store.setWriteCount(INT_MAX);

        THEN("Records up to the invalid record are copied")
        {
            eeprom.copyAllRecordsToPageExcept(fromPage, toPage, exceptRecordIdStart, exceptRecordIdEnd);

            alternateOffset = store.requireValidRecord(alternateOffset, recordId, record);

            // The copied record is followed by empty space
            REQUIRE(eeprom.findEmptyOffset(toPage) == alternateOffset);
        }
    }
}

//TEST_CASE("NODUP Swap pages", "[eeprom_byte]")
//{
//    TestEEPROM eeprom;
//    StoreManipulator store(eeprom.store);
//
//    eeprom.init();
//
//    uint32_t activeOffset = PageBase1;
//    uint32_t alternateOffset = PageBase2;
//
//    // Write some data
//    uint16_t recordId = 100;
//    uint8_t record = 0xBB;
//    eeprom.put(recordId, record);
//
//    // Have a record to write after the swap
//    uint16_t newRecordId = 200;
//    uint8_t newRecord = 0xCC;
//
//    auto requireSwapCompleted = [&]()
//    {
//        store.requirePageStatus(activeOffset, PAGE_INACTIVE);
//        store.requirePageStatus(alternateOffset, PAGE_ACTIVE);
//        store.requireValidRecord(alternateOffset + 2, recordId, record);
//    };
//
//    auto performSwap = [&]()
//    {
//        eeprom.swapPagesAndWriteRecord(newRecordId, &newRecord, sizeof(newRecord));
//    };
//
//    SECTION("No interruption")
//    {
//        performSwap();
//
//        requireSwapCompleted();
//    }
//
//    SECTION("Interrupted page swap 1: during erase")
//    {
//        store.writePageStatus(alternateOffset, PAGE_INACTIVE);
//        eeprom.store.setWriteCount(0);
//
//        performSwap();
//
//        // Verify that the alternate page is not yet erased
//        store.requirePageStatus(alternateOffset, PAGE_INACTIVE);
//
//        THEN("Redoing the page swap works")
//        {
//            eeprom.store.setWriteCount(INT_MAX);
//
//            performSwap();
//
//            requireSwapCompleted();
//        }
//    }
//
//    SECTION("Interrupted page swap 2: during copy")
//    {
//        eeprom.store.setWriteCount(2);
//        eeprom.swapPagesAndWriteRecord(newRecordId, &newRecord, sizeof(newRecord));
//
//        // Verify that the alternate page is still copy
//        store.requirePageStatus(alternateOffset, PAGE_COPY);
//
//        THEN("Redoing the page swap works")
//        {
//            eeprom.store.setWriteCount(INT_MAX);
//
//            performSwap();
//
//            requireSwapCompleted();
//        }
//    }
//
//    SECTION("Interrupted page swap 3: before old page becomes inactive")
//    {
//        eeprom.store.setWriteCount(8);
//
//        performSwap();
//
//        // Verify that both pages are active
//        store.requirePageStatus(alternateOffset, PAGE_ACTIVE);
//        store.requirePageStatus(activeOffset, PAGE_ACTIVE);
//
//        THEN("Page 1 remains the active page")
//        {
//            eeprom.store.setWriteCount(INT_MAX);
//
//            REQUIRE(eeprom.getActivePage() == Page1);
//        }
//    }
//}
//
//TEST_CASE("NODUP Erasable page", "[eeprom_byte]")
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
