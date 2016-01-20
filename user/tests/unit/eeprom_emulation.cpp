// Automatic test runner:
// user/tests/unit$ rerun -x -p "**/*.{cpp,h}" -d .,../../../services "make runner && obj/runner [eeprom]"
#include "catch.hpp"
#include <string>
#include <sstream>
#include "eeprom_emulation.h"
#include "flash_storage.h"

const int TestSectorSize = 16000;
const int TestSectorCount = 2;
const int TestBase = 0xC000;

using TestStore = RAMFlashStorage<TestBase, TestSectorCount, TestSectorSize>;
using TestEEPROM = EEPROMEmulation<TestStore, TestBase, TestSectorSize, TestBase + TestSectorSize, TestSectorSize>;

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
        store.eraseSector(TestBase);
        store.eraseSector(TestBase + TestSectorSize);
    }

    // Interrupted record write 1: status written as invalid, but id,
    // length, data not written
    template <typename T>
    uintptr_t writeInvalidRecord(uintptr_t offset, uint16_t id, const T& record)
    {
        uint16_t status = TestEEPROM::Header::INVALID;
        store.write(offset, &status, sizeof(status));
        // Next record should be written directly after the invalid header
        return offset + sizeof(TestEEPROM::Header);
    }

    // Interrupted record write 2: status written as invalid, id, length
    // written, but data not written
    template <typename T>
    uintptr_t writeRecordHeader(uintptr_t offset, uint16_t id, const T& record)
    {
        TestEEPROM::Header header = {
            TestEEPROM::Header::INVALID,
            id,
            sizeof(record)
        };

        store.write(offset, &header, sizeof(header));
        // Next record should be written after the invalid header and partial data
        return offset + sizeof(header) + sizeof(record);
    }

    // Interrupted record write 3: status written as invalid, id, length
    // written, data partially written
    template <typename T>
    uintptr_t writePartialRecord(uintptr_t offset, uint16_t id, const T& record)
    {
        TestEEPROM::Header header = {
            TestEEPROM::Header::INVALID,
            id,
            sizeof(record)
        };

        store.write(offset, &header, sizeof(header));
        offset += sizeof(header);
        // write only 1 byte
        store.write(offset, &record, 1);

        // Next record should be written after the invalid header and partial data
        return offset + sizeof(record);
    }

    // Completely written record
    template <typename T>
    uintptr_t writeRecord(uintptr_t offset, uint16_t id, const T& record)
    {
        TestEEPROM::Header header = {
            TestEEPROM::Header::VALID,
            id,
            sizeof(record)
        };

        store.write(offset, &header, sizeof(header));
        offset += sizeof(header);
        store.write(offset, &record, sizeof(record));

        // Next record should be written after the header and data
        return offset + sizeof(record);
    }

    // Validates that a specific record was correctly written at the specified offset
    template <typename T>
    uintptr_t requireValidRecord(uintptr_t offset, uint16_t id, const T& expected)
    {
        uint16_t status = TestEEPROM::Header::VALID;
        REQUIRE(std::memcmp(store.dataAt(offset), &status, sizeof(status)) == 0);
        offset += sizeof(status);

        REQUIRE(std::memcmp(store.dataAt(offset), &id, sizeof(id)) == 0);
        offset += sizeof(id);

        uint16_t length = sizeof(expected);
        REQUIRE(std::memcmp(store.dataAt(offset), &length, sizeof(length)) == 0);
        offset += sizeof(length);

        REQUIRE(std::memcmp(store.dataAt(offset), &expected, sizeof(expected)) == 0);
        return offset + sizeof(expected);
    }

    // Test debugging helper to view the storage contents
    // Usage:
    // WARN(store.dumpStorage(TestBase, 30));
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

TEST_CASE("Get record", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    store.eraseAll();
    uint32_t offset = TestBase + 2;

    SECTION("The record doesn't exist")
    {
        uint16_t recordId = 999;
        SECTION("No other records")
        {

            THEN("get returns false")
            {
                uint8_t value;
                REQUIRE(eeprom.get(recordId, value) == false);
            }
        }

        SECTION("With bad records")
        {
            uint8_t badRecord[] = { 0xCC, 0xDD };
            offset = store.writeInvalidRecord(offset, recordId, badRecord);
            offset = store.writeRecordHeader(offset, recordId, badRecord);
            offset = store.writePartialRecord(offset, recordId, badRecord);

            THEN("get returns false")
            {
                uint8_t value;
                REQUIRE(eeprom.get(recordId, value) == false);
            }
        }
    }

    SECTION("The record exists")
    {
        uint16_t recordId = 0;
        uint8_t record = 0xCC;

        SECTION("No other records")
        {
            offset = store.writeRecord(offset, recordId, record);

            THEN("get returns true and extracts the value")
            {
                uint8_t value;
                REQUIRE(eeprom.get(recordId, value) == true);
                REQUIRE(value == 0xCC);
            }
        }

        SECTION("With bad records")
        {
            uint8_t badRecord[] = { 0xCC, 0xDD };
            offset = store.writeInvalidRecord(offset, recordId, badRecord);
            offset = store.writeRecordHeader(offset, recordId, badRecord);
            offset = store.writePartialRecord(offset, recordId, badRecord);

            offset = store.writeRecord(offset, recordId, record);

            THEN("get returns true and extracts the value")
            {
                uint8_t value;
                REQUIRE(eeprom.get(recordId, value) == true);
                REQUIRE(value == 0xCC);
            }
        }
    }
}

TEST_CASE("Put record", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    store.eraseAll();
    uint32_t offset = TestBase + 2;

    SECTION("The record doesn't exist")
    {
        uint16_t recordId = 0;
        uint8_t record = 0xDD;

        THEN("put returns true and creates the record")
        {
            REQUIRE(eeprom.put(recordId, record) == true);
            store.requireValidRecord(offset, recordId, record);
        }

        THEN("get returns the put record")
        {
            eeprom.put(recordId, record);
            uint8_t newRecord;
            REQUIRE(eeprom.get(recordId, newRecord) == true);
            REQUIRE(newRecord == 0xDD);
        }
    }

    SECTION("The record exists")
    {
        uint16_t recordId = 0;
        uint8_t previousRecord = 0xCC;

        offset = store.writeRecord(offset, recordId, previousRecord);

        uint8_t record = 0xDD;

        THEN("put returns true and creates a new copy of the record")
        {
            REQUIRE(eeprom.put(recordId, record) == true);
            store.requireValidRecord(offset, recordId, record);
        }

        THEN("get returns the put record")
        {
            eeprom.put(recordId, record);
            uint8_t newRecord;
            REQUIRE(eeprom.get(recordId, newRecord) == true);
            REQUIRE(newRecord == 0xDD);
        }
    }
}

TEST_CASE("Verify sector", "[eeprom]")
{
    TestEEPROM eeprom;

    SECTION("random flash")
    {
        REQUIRE(eeprom.verifySector(1) == false);
    }

    SECTION("verified flash")
    {
        eeprom.store.eraseSector(TestBase);

        uint16_t status = TestEEPROM::SectorHeader::VERIFIED;
        eeprom.store.write(TestBase, &status, sizeof(status));

        REQUIRE(eeprom.verifySector(1) == true);
    }


    SECTION("erased flash")
    {
        eeprom.store.eraseSector(TestBase);
        REQUIRE(eeprom.verifySector(1) == true);

        uint16_t status = TestEEPROM::SectorHeader::VERIFIED;
        REQUIRE(std::memcmp(eeprom.store.dataAt(TestBase), &status, sizeof(status)) == 0);
    }

    SECTION("partially erased flash")
    {
        eeprom.store.eraseSector(TestBase);
        uint8_t garbage = 0xCC;
        eeprom.store.write(TestBase + 100, &garbage, sizeof(garbage));
        REQUIRE(eeprom.verifySector(1) == false);
    }
}

TEST_CASE("Alternate sector", "[eeprom]")
{
    // TODO

}

TEST_CASE("Copy records to sector", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    // Start with erased flash
    eeprom.store.eraseSector(TestBase);
    eeprom.store.eraseSector(TestBase + TestSectorSize);

    uint32_t fromOffset = TestBase + 2;
    uint32_t toOffset = TestBase + TestSectorSize + 2;
    uint8_t fromSector = 1;
    uint8_t toSector = 2;

    SECTION("Single record")
    {
        uint16_t recordId = 100;
        uint8_t record = 0xBB;
        fromOffset = store.writeRecord(fromOffset, recordId, record);

        eeprom.copyAllRecordsToSector(fromSector, toSector);

        THEN("The record is copied")
        {
            store.requireValidRecord(toOffset, recordId, record);
        }
    }

    SECTION("Multiple copies of a record")
    {
        uint16_t recordId = 100;
        uint8_t record = 0xBB;
        fromOffset = store.writeRecord(fromOffset, recordId, record);

        uint8_t newRecord = 0xCC;
        fromOffset = store.writeRecord(fromOffset, recordId, newRecord);

        eeprom.copyAllRecordsToSector(fromSector, toSector);

        THEN("The latest record is copied")
        {
            store.requireValidRecord(toOffset, recordId, newRecord);
        }
    }

    SECTION("Multiple records")
    {
        uint16_t recordIds[] = { 30, 10, 40 };
        uint32_t record = 0xDEADBEEF;

        fromOffset = store.writeRecord(fromOffset, recordIds[0], record);
        fromOffset = store.writeRecord(fromOffset, recordIds[1], record);
        fromOffset = store.writeRecord(fromOffset, recordIds[2], record);

        eeprom.copyAllRecordsToSector(fromSector, toSector);

        THEN("The records are copied from small ids to large ids")
        {
            toOffset = store.requireValidRecord(toOffset, recordIds[1], record);
            toOffset = store.requireValidRecord(toOffset, recordIds[0], record);
            toOffset = store.requireValidRecord(toOffset, recordIds[2], record);
        }
    }
}
