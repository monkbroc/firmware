// Automatic test runner:
// user/tests/unit$ rerun -x -p "**/*.{cpp,h}" -d .,../../../services "make runner && obj/runner [eeprom]"
#include "catch.hpp"
#include <string>
#include <sstream>
#include "eeprom_emulation.h"
#include "flash_storage.h"

const size_t TestSectorSize = 0x4000;
const uint8_t TestSectorCount = 2;
const uintptr_t TestBase = 0xC000;

/* Simulate 2 Flash sectors of different sizes used for EEPROM emulation */
const uintptr_t SectorBase1 = TestBase;
const size_t SectorSize1 = TestSectorSize;
const uintptr_t SectorBase2 = TestBase + TestSectorSize;
const size_t SectorSize2 = TestSectorSize / 4;

using TestStore = RAMFlashStorage<TestBase, TestSectorCount, TestSectorSize>;
using TestEEPROM = EEPROMEmulation<TestStore, SectorBase1, SectorSize1, SectorBase2, SectorSize2>;

// Alias some constants, otherwise the linker is having issues when
// those are used inside REQUIRE() tests
auto NoSector = TestEEPROM::LogicalSector::NoSector;
auto Sector1 = TestEEPROM::LogicalSector::Sector1;
auto Sector2 = TestEEPROM::LogicalSector::Sector2;

auto SECTOR_ERASED = TestEEPROM::SectorHeader::ERASED;
auto SECTOR_COPY = TestEEPROM::SectorHeader::COPY;
auto SECTOR_ACTIVE = TestEEPROM::SectorHeader::ACTIVE;
auto SECTOR_INACTIVE = TestEEPROM::SectorHeader::INACTIVE;

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
        store.eraseSector(SectorBase1);
        store.eraseSector(SectorBase2);
    }

    void writeSectorStatus(uintptr_t offset, uint16_t status)
    {
        store.write(offset, &status, sizeof(status));
    }

    uint16_t readSectorStatus(uintptr_t offset)
    {
        uint16_t status;
        store.read(offset, &status, sizeof(status));
        return status;
    }

    void requireSectorStatus(uintptr_t offset, uint16_t expectedStatus)
    {
        REQUIRE(readSectorStatus(offset) == expectedStatus);
    }

    // Interrupted record write 1: status written as invalid, but id,
    // length, data not written
    template <typename T>
    uintptr_t writeInvalidRecord(uintptr_t offset, uint16_t id, const T& record)
    {
        TestEEPROM::Header header = {
            TestEEPROM::Header::INVALID,
            0,
            0
        };

        store.write(offset, &header, sizeof(header.status));
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
            sizeof(record),
            id
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
            sizeof(record),
            id
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
            sizeof(record),
            id
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
        auto status = TestEEPROM::Header::VALID;
        REQUIRE(std::memcmp(store.dataAt(offset), &status, sizeof(status)) == 0);
        offset += sizeof(status);

        uint16_t length = sizeof(expected);
        REQUIRE(std::memcmp(store.dataAt(offset), &length, sizeof(length)) == 0);
        offset += sizeof(length);

        REQUIRE(std::memcmp(store.dataAt(offset), &id, sizeof(id)) == 0);
        offset += sizeof(id);

        REQUIRE(std::memcmp(store.dataAt(offset), &expected, sizeof(expected)) == 0);
        return offset + sizeof(expected);
    }

    // Debugging helper to view the storage contents
    // Usage:
    // WARN(store.dumpStorage(SectorBase1, 30));
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

    eeprom.init();

    uint32_t offset = SectorBase1 + 2;
    uint8_t value;

    SECTION("The record doesn't exist")
    {
        uint16_t recordId = 999;
        SECTION("No other records")
        {
            THEN("get returns false")
            {
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
                REQUIRE(eeprom.get(recordId, value) == false);
            }
        }
    }

    SECTION("A bad record exists")
    {
        uint16_t recordId = 0;
        uint8_t badRecord = 0xCC;

        SECTION("invalid record")
        {
            offset = store.writeInvalidRecord(offset, recordId, badRecord);

            THEN("get returns false")
            {
                REQUIRE(eeprom.get(recordId, value) == false);
            }
        }

        SECTION("missing data")
        {
            offset = store.writeRecordHeader(offset, recordId, badRecord);

            THEN("get returns false")
            {
                REQUIRE(eeprom.get(recordId, value) == false);
            }
        }

        SECTION("partial data")
        {
            offset = store.writePartialRecord(offset, recordId, badRecord);

            THEN("get returns false")
            {
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
                REQUIRE(eeprom.get(recordId, value) == true);
                REQUIRE(value == 0xCC);
            }
        }

        SECTION("With bad records")
        {
            uint8_t badRecord[] = { 0xDD, 0xEE };
            offset = store.writeInvalidRecord(offset, recordId, badRecord);
            offset = store.writeRecordHeader(offset, recordId, badRecord);
            offset = store.writePartialRecord(offset, recordId, badRecord);

            offset = store.writeRecord(offset, recordId, record);

            THEN("get returns true and extracts the value")
            {
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

    eeprom.init();

    uint32_t offset = SectorBase1 + 2;

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

    SECTION("A bad record exists")
    {
        uint16_t recordId = 0;
        uint8_t badRecord = 0xEE;
        offset = store.writeRecordHeader(offset, recordId, badRecord);

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

    SECTION("The new record value is the same as current one")
    {
        uint16_t recordId = 0;
        uint8_t record = 0xCC;

        eeprom.put(recordId, record);

        uintptr_t originalEmptyOffset = eeprom.findEmptyOffset(eeprom.getActiveSector());

        THEN("put returns true but doesn't create a new copy of the record")
        {
            REQUIRE(eeprom.put(recordId, record) == true);

            uintptr_t emptyOffset = eeprom.findEmptyOffset(eeprom.getActiveSector());
            REQUIRE(emptyOffset == originalEmptyOffset);
        }
    }

    SECTION("EEPROM capacity is reached")
    {
        uint32_t data[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        uint16_t recordSize = sizeof(data) + sizeof(TestEEPROM::Header);

        // Write many large records until capacity is reached
        for(uint16_t id = 0; id < 200 && eeprom.remainingCapacity() >= recordSize; id++)
        {
            REQUIRE(eeprom.put(id, data) == true);
        }

        THEN("Additional records cannot be added if they wouldn't fit in the smallest sector")
        {
            REQUIRE(eeprom.put(1000, data) == false);
        }
    }

    SECTION("Sector swap is required")
    {
        uint16_t recordSize = sizeof(uint32_t) + sizeof(TestEEPROM::Header);
        uint16_t writesToFillSector1 = SectorSize1 / recordSize;

        uint16_t id = 100;

        // Write multiple copies of the same record until sector 1 is full
        for(uint32_t i = 0; i < writesToFillSector1; i++)
        {
            REQUIRE(eeprom.put(id, i) == true);
        }

        REQUIRE(eeprom.getActiveSector() == Sector1);

        THEN("The next write performs a sector swap")
        {
            uint32_t record = 0xDEADBEEF;
            REQUIRE(eeprom.put(id, record) == true);

            REQUIRE(eeprom.getActiveSector() == Sector2);
        }
    }

    SECTION("EEPROM is near capacity and overwriting an existing record would only fit after a sector swap")
    {
        // Sector2 is smaller than sector1 in this test so it's easier
        // to fill sector2
        store.eraseAll();
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        eeprom.init();

        uint32_t data[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        uint16_t recordSize = sizeof(data) + sizeof(TestEEPROM::Header);

        // Write many large records until there is no room for another record
        for(uint16_t id = 0; id < 200 && eeprom.remainingCapacity() >= recordSize; id++)
        {
            REQUIRE(eeprom.put(id, data) == true);
        }

        REQUIRE(eeprom.getActiveSector() == Sector2);

        THEN("Overwritten record is added after sector swap")
        {
            uint32_t data2[] = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
            uint16_t recordId = 0;
            REQUIRE(eeprom.put(recordId, data2) == true);
            REQUIRE(eeprom.getActiveSector() == Sector1);
        }
    }
}


TEST_CASE("Remove record", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);
    eeprom.init();

    SECTION("The record doesn't exist")
    {
        uint16_t recordId = 0;

        REQUIRE(eeprom.remove(recordId) == false);
    }

    SECTION("The record exists")
    {
        uint16_t recordId = 0;
        uint8_t record = 0xCC;

        eeprom.put(recordId, record);

        THEN("remove returns true")
        {
            REQUIRE(eeprom.remove(recordId) == true);
        }

        THEN("get doesn't return the removed record")
        {
            eeprom.remove(recordId);

            REQUIRE(eeprom.get(recordId, record) == false);
        }
    }
}

TEST_CASE("Total capacity", "[eeprom]")
{
    TestEEPROM eeprom;
    size_t smallestSectorSize = TestSectorSize / 4 - sizeof(TestEEPROM::SectorHeader);

    REQUIRE(eeprom.totalCapacity() == smallestSectorSize);
}

TEST_CASE("Used capacity", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    // Bad records should be ignored
    store.writePartialRecord(SectorBase1 + 2, 100, 0xAA);
    // Simulate reset during writing
    eeprom.init();

    uint8_t dummy = 0xCC;
    uint8_t data[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };

    for(uint16_t i = 0; i < 20; i++)
    {
        // Old records should be ignored
        eeprom.put(i, dummy);

        // Latest records should be counted
        eeprom.put(i, data);
    }

    THEN("Measures capacity for valid records")
    {
        size_t expectedCapacity = 20 * (sizeof(TestEEPROM::Header) + sizeof(data));

        REQUIRE(eeprom.usedCapacity() == expectedCapacity);
    }
}

TEST_CASE("Count records", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    // Bad records should be ignored
    store.writePartialRecord(SectorBase1 + 2, 100, 0xAA);

    uint8_t dummy = 0xCC;
    uint8_t data[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };

    for(uint16_t i = 0; i < 20; i++)
    {
        // Duplicate records should be only counted once
        eeprom.put(i, dummy);
        eeprom.put(i, data);
    }

    REQUIRE(eeprom.countRecords() == 20);
}


TEST_CASE("List records", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    // Bad records should be ignored
    store.writePartialRecord(SectorBase1 + 2, 100, 0xAA);

    uint8_t dummy = 0xCC;
    uint8_t data[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };

    for(uint16_t i = 0; i < 3; i++)
    {
        // Duplicate records should be only counted once
        eeprom.put(i * 2, dummy);
        eeprom.put(i * 2, data);
    }

    uint16_t ids[5];
    REQUIRE(eeprom.listRecords(ids, 5) == 3);

    REQUIRE(ids[0] == 0);
    REQUIRE(ids[1] == 2);
    REQUIRE(ids[2] == 4);
}

TEST_CASE("Initialize EEPROM", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    SECTION("Random flash")
    {
        eeprom.init();

        THEN("Sector 1 is active, sector 2 is erased")
        {
            store.requireSectorStatus(SectorBase1, SECTOR_ACTIVE);
            store.requireSectorStatus(SectorBase2, SECTOR_ERASED);
        }
    }

    SECTION("Erased flash")
    {
        store.eraseAll();

        eeprom.init();

        THEN("Sector 1 is active, sector 2 is erased")
        {
            store.requireSectorStatus(SectorBase1, SECTOR_ACTIVE);
            store.requireSectorStatus(SectorBase2, SECTOR_ERASED);
        }
    }

    SECTION("Sector 1 active")
    {
        store.eraseAll();
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);

        eeprom.init();

        THEN("Sector 1 remains active, sector 2 remains erased")
        {
            store.requireSectorStatus(SectorBase1, SECTOR_ACTIVE);
            store.requireSectorStatus(SectorBase2, SECTOR_ERASED);
        }
    }

    SECTION("Sector 2 active")
    {
        store.eraseAll();
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);

        eeprom.init();

        THEN("Sector 1 remains erased, sector 2 remains active")
        {
            store.requireSectorStatus(SectorBase1, SECTOR_ERASED);
            store.requireSectorStatus(SectorBase2, SECTOR_ACTIVE);
        }
    }

    SECTION("Pending erase on sector 1")
    {
        store.eraseAll();
        store.writeSectorStatus(SectorBase1, SECTOR_INACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);

        eeprom.init();

        THEN("Sector 1 is erased, sector 2 remains active")
        {
            store.requireSectorStatus(SectorBase1, SECTOR_ERASED);
            store.requireSectorStatus(SectorBase2, SECTOR_ACTIVE);
        }
    }

}

TEST_CASE("Clear", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();
    eeprom.clear();

    THEN("Sector 1 is active, sector 2 is erased")
    {
        store.requireSectorStatus(SectorBase1, SECTOR_ACTIVE);
        store.requireSectorStatus(SectorBase2, SECTOR_ERASED);
    }
}

TEST_CASE("Verify sector", "[eeprom]")
{
    TestEEPROM eeprom;

    SECTION("random flash")
    {
        REQUIRE(eeprom.verifySector(Sector1) == false);
    }

    SECTION("erased flash")
    {
        eeprom.store.eraseSector(SectorBase1);
        REQUIRE(eeprom.verifySector(Sector1) == true);
    }

    SECTION("partially erased flash")
    {
        eeprom.store.eraseSector(SectorBase1);

        uint8_t garbage = 0xCC;
        eeprom.store.write(SectorBase1 + 100, &garbage, sizeof(garbage));

        REQUIRE(eeprom.verifySector(Sector1) == false);
    }
}

TEST_CASE("Active sector", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    store.eraseAll();

    // No valid sector

    SECTION("Sector 1 erased, sector 2 erased (blank flash)")
    {
        eeprom.updateActiveSector();
        REQUIRE(eeprom.getActiveSector() == NoSector);
    }

    SECTION("Sector 1 garbage, sector 2 garbage (invalid state)")
    {
        store.writeSectorStatus(SectorBase1, 999);
        store.writeSectorStatus(SectorBase2, 999);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getActiveSector() == NoSector);
    }

    // Sector 1 valid

    SECTION("Sector 1 active, sector 2 erased (normal case)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getActiveSector() == Sector1);
    }

    // Steps of swap from sector 1 to sector 2

    SECTION("Sector 1 active, sector 2 copy (interrupted swap)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_COPY);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getActiveSector() == Sector1);
    }

    SECTION("Sector 1 active, sector 2 active (almost completed swap)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getActiveSector() == Sector1);
    }

    SECTION("Sector 1 inactive, sector 2 active (completed swap, pending erase)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_INACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getActiveSector() == Sector2);
    }

    // Sector 2 valid

    SECTION("Sector 1 erased, sector 2 active (normal case)")
    {
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getActiveSector() == Sector2);
    }

    // Steps of swap from sector 2 to sector 1

    SECTION("Sector 1 copy, sector 2 active (interrupted swap)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_COPY);
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getActiveSector() == Sector2);
    }

    SECTION("Sector 1 active, sector 2 active (almost completed swap)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getActiveSector() == Sector1);
    }

    SECTION("Sector 1 active, sector 2 inactive (completed swap, pending erase)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_INACTIVE);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getActiveSector() == Sector1);
    }
}

TEST_CASE("Alternate sector", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    store.eraseAll();

    SECTION("Sector 1 is active")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getAlternateSector() == Sector2);
    }

    SECTION("Sector 2 is active")
    {
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        REQUIRE(eeprom.getAlternateSector() == Sector1);
    }

    // Not necessary to test when no sector is active since that
    // condition will be fixed at boot in init()
}

TEST_CASE("Copy records to sector", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    uint32_t activeOffset = SectorBase1 + 2;
    uint32_t alternateOffset = SectorBase2 + 2;
    auto fromSector = Sector1;
    auto toSector = Sector2;
    uint16_t exceptRecordId = 0xFFFF;

    SECTION("Single record")
    {
        uint16_t recordId = 100;
        uint8_t record = 0xBB;
        activeOffset = store.writeRecord(activeOffset, recordId, record);

        eeprom.copyAllRecordsToSector(fromSector, toSector, exceptRecordId);

        THEN("The record is copied")
        {
            store.requireValidRecord(alternateOffset, recordId, record);
        }
    }

    SECTION("Multiple copies of a record")
    {
        uint16_t recordId = 100;
        uint8_t record = 0xBB;
        activeOffset = store.writeRecord(activeOffset, recordId, record);

        uint8_t newRecord = 0xCC;
        activeOffset = store.writeRecord(activeOffset, recordId, newRecord);

        eeprom.copyAllRecordsToSector(fromSector, toSector, exceptRecordId);

        THEN("The latest record is copied")
        {
            store.requireValidRecord(alternateOffset, recordId, newRecord);
        }
    }

    SECTION("Multiple records")
    {
        uint16_t recordIds[] = { 30, 10, 40 };
        uint32_t record = 0xDEADBEEF;

        activeOffset = store.writeRecord(activeOffset, recordIds[0], record);
        activeOffset = store.writeRecord(activeOffset, recordIds[1], record);
        activeOffset = store.writeRecord(activeOffset, recordIds[2], record);

        eeprom.copyAllRecordsToSector(fromSector, toSector, exceptRecordId);

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
        uint32_t record = 0xDEADBEEF;

        activeOffset = store.writeRecord(activeOffset, recordIds[0], record);
        activeOffset = store.writeRecord(activeOffset, recordIds[1], record);
        activeOffset = store.writeRecord(activeOffset, recordIds[2], record);

        exceptRecordId = 10;

        eeprom.copyAllRecordsToSector(fromSector, toSector, exceptRecordId);

        THEN("The specified record is not copied")
        {
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[0], record);
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[2], record);
        }
    }
}

TEST_CASE("Swap sectors", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    eeprom.init();

    uint32_t activeOffset = SectorBase1;
    uint32_t alternateOffset = SectorBase2;

    // Write some data
    uint16_t recordId = 100;
    uint8_t record = 0xBB;
    eeprom.put(recordId, record);

    // Have a record to write after the swap
    uint16_t newRecordId = 200;
    uint8_t newRecord = 0xCC;

    auto requireSwapCompleted = [&]()
    {
        store.requireSectorStatus(activeOffset, SECTOR_INACTIVE);
        store.requireSectorStatus(alternateOffset, SECTOR_ACTIVE);
        store.requireValidRecord(alternateOffset + 2, recordId, record);
    };

    auto performSwap = [&]()
    {
        eeprom.swapSectorsAndWriteRecord(newRecordId, &newRecord, sizeof(newRecord));
    };

    SECTION("No interruption")
    {
        performSwap();

        requireSwapCompleted();
    }

    SECTION("Interrupted sector swap 1: during erase")
    {
        store.writeSectorStatus(alternateOffset, SECTOR_INACTIVE);
        eeprom.store.setWriteCount(0);

        performSwap();

        // Verify that the alternate sector is not yet erased
        store.requireSectorStatus(alternateOffset, SECTOR_INACTIVE);

        THEN("Redoing the sector swap works")
        {
            eeprom.store.setWriteCount(INT_MAX);

            performSwap();

            requireSwapCompleted();
        }
    }

    SECTION("Interrupted sector swap 2: during copy")
    {
        eeprom.store.setWriteCount(2);
        eeprom.swapSectorsAndWriteRecord(newRecordId, &newRecord, sizeof(newRecord));

        // Verify that the alternate sector is still copy
        store.requireSectorStatus(alternateOffset, SECTOR_COPY);

        THEN("Redoing the sector swap works")
        {
            eeprom.store.setWriteCount(INT_MAX);

            performSwap();

            requireSwapCompleted();
        }
    }

    SECTION("Interrupted sector swap 3: before old sector becomes inactive")
    {
        eeprom.store.setWriteCount(8);

        performSwap();

        // Verify that both sectors are active
        store.requireSectorStatus(alternateOffset, SECTOR_ACTIVE);
        store.requireSectorStatus(activeOffset, SECTOR_ACTIVE);

        THEN("Sector 1 remains the active sector")
        {
            eeprom.store.setWriteCount(INT_MAX);

            REQUIRE(eeprom.getActiveSector() == Sector1);
        }
    }
}

TEST_CASE("Erasable sector", "[eeprom]")
{
    TestEEPROM eeprom;
    StoreManipulator store(eeprom.store);

    store.eraseAll();

    uint32_t activeOffset = SectorBase1;
    uint32_t alternateOffset = SectorBase2;

    SECTION("One active sector, one erased sector")
    {
        store.writeSectorStatus(activeOffset, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        THEN("No sector needs to be erased")
        {
            REQUIRE(eeprom.getPendingEraseSector() == NoSector);
            REQUIRE(eeprom.hasPendingErase() == false);
        }
    }

    SECTION("One active sector, one inactive sector")
    {
        store.writeSectorStatus(activeOffset, SECTOR_ACTIVE);
        store.writeSectorStatus(alternateOffset, SECTOR_INACTIVE);
        eeprom.updateActiveSector();

        THEN("The old sector needs to be erased")
        {
            REQUIRE(eeprom.getPendingEraseSector() == Sector2);
            REQUIRE(eeprom.hasPendingErase() == true);
        }

        THEN("Erasing the old sector clear it")
        {
            eeprom.performPendingErase();

            REQUIRE(eeprom.getPendingEraseSector() == NoSector);
            REQUIRE(eeprom.hasPendingErase() == false);
        }
    }

    SECTION("2 active sectors")
    {
        store.writeSectorStatus(activeOffset, SECTOR_ACTIVE);
        store.writeSectorStatus(alternateOffset, SECTOR_ACTIVE);
        eeprom.updateActiveSector();

        THEN("Sector 2 needs to be erased")
        {
            REQUIRE(eeprom.getPendingEraseSector() == Sector2);
            REQUIRE(eeprom.hasPendingErase() == true);
        }
    }
}
