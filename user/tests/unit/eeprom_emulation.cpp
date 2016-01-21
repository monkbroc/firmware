// Automatic test runner:
// user/tests/unit$ rerun -x -p "**/*.{cpp,h}" -d .,../../../services "make runner && obj/runner [eeprom]"
#include "catch.hpp"
#include <string>
#include <sstream>
#include "eeprom_emulation.h"
#include "flash_storage.h"

const int TestSectorSize = 0x4000;
const int TestSectorCount = 2;
const int TestBase = 0xC000;

const int SectorBase1 = TestBase;
const int SectorBase2 = TestBase + TestSectorSize;

using TestStore = RAMFlashStorage<TestBase, TestSectorCount, TestSectorSize>;
using TestEEPROM = EEPROMEmulation<TestStore, SectorBase1, TestSectorSize, SectorBase2, TestSectorSize>;

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
        REQUIRE(eeprom.getActiveSector() == NoSector);
    }

    SECTION("Sector 1 garbage, sector 2 garbage (invalid state)")
    {
        store.writeSectorStatus(SectorBase1, 999);
        store.writeSectorStatus(SectorBase2, 999);
        REQUIRE(eeprom.getActiveSector() == NoSector);
    }

    // Sector 1 valid

    SECTION("Sector 1 active, sector 2 erased (normal case)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);
        REQUIRE(eeprom.getActiveSector() == Sector1);
    }

    // Steps of swap from sector 1 to sector 2

    SECTION("Sector 1 active, sector 2 copy (interrupted swap)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_COPY);
        REQUIRE(eeprom.getActiveSector() == Sector1);
    }

    SECTION("Sector 1 inactive, sector 2 copy (almost completed swap)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_INACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_COPY);
        REQUIRE(eeprom.getActiveSector() == Sector2);
    }

    SECTION("Sector 1 active, sector 2 inactive (completed swap, pending erase)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_ACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_INACTIVE);
        REQUIRE(eeprom.getActiveSector() == Sector1);
    }

    // Sector 2 valid

    SECTION("Sector 1 erased, sector 2 active (normal case)")
    {
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        REQUIRE(eeprom.getActiveSector() == Sector2);
    }

    // Steps of swap from sector 2 to sector 1

    SECTION("Sector 1 copy, sector 2 active (interrupted swap)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_COPY);
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        REQUIRE(eeprom.getActiveSector() == Sector2);
    }

    SECTION("Sector 1 copy, sector 2 inactive (almost completed swap)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_COPY);
        store.writeSectorStatus(SectorBase2, SECTOR_INACTIVE);
        REQUIRE(eeprom.getActiveSector() == Sector1);
    }

    SECTION("Sector 1 inactive, sector 2 active (completed swap, pending erase)")
    {
        store.writeSectorStatus(SectorBase1, SECTOR_INACTIVE);
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);
        REQUIRE(eeprom.getActiveSector() == Sector2);
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

        REQUIRE(eeprom.getAlternateSector() == Sector2);
    }

    SECTION("Sector 2 is active")
    {
        store.writeSectorStatus(SectorBase2, SECTOR_ACTIVE);

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

    SECTION("Single record")
    {
        uint16_t recordId = 100;
        uint8_t record = 0xBB;
        activeOffset = store.writeRecord(activeOffset, recordId, record);

        eeprom.copyAllRecordsToSector(fromSector, toSector);

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

        eeprom.copyAllRecordsToSector(fromSector, toSector);

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

        eeprom.copyAllRecordsToSector(fromSector, toSector);

        THEN("The records are copied from small ids to large ids")
        {
            alternateOffset = store.requireValidRecord(alternateOffset, recordIds[1], record);
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
    store.writeRecord(activeOffset + 2, recordId, record);

    SECTION("Interrupted sector swap 1: during erase")
    {
        store.writeSectorStatus(alternateOffset, SECTOR_INACTIVE);
        eeprom.store.setWriteCount(0);
        eeprom.swapSectors();

        // Verify that the alternate sector is not yet erased
        store.requireSectorStatus(alternateOffset, SECTOR_INACTIVE);

        THEN("Redoing the sector swap works")
        {
            eeprom.store.setWriteCount(INT_MAX);
            eeprom.swapSectors();

            store.requireSectorStatus(alternateOffset, SECTOR_ACTIVE);
            store.requireValidRecord(alternateOffset + 2, recordId, record);
        }
    }

    SECTION("Interrupted sector swap 2: during copy")
    {
        eeprom.store.setWriteCount(2);
        eeprom.swapSectors();

        // Verify that the alternate sector is still copy
        store.requireSectorStatus(alternateOffset, SECTOR_COPY);

        THEN("Redoing the sector swap works")
        {
            eeprom.store.setWriteCount(INT_MAX);
            eeprom.swapSectors();

            store.requireSectorStatus(alternateOffset, SECTOR_ACTIVE);
            store.requireValidRecord(alternateOffset + 2, recordId, record);
        }
    }

    SECTION("Interrupted sector swap 3: before new sector activation")
    {
        eeprom.store.setWriteCount(5);
        eeprom.swapSectors();

        // Verify that the alternate sector is still copy and active sector is already inactive
        store.requireSectorStatus(alternateOffset, SECTOR_COPY);
        store.requireSectorStatus(activeOffset, SECTOR_INACTIVE);

        THEN("Swapped sector becomes the active sector")
        {
            eeprom.store.setWriteCount(INT_MAX);

            REQUIRE(eeprom.getActiveSector() == Sector2);

            store.requireSectorStatus(alternateOffset, SECTOR_ACTIVE);
            store.requireValidRecord(alternateOffset + 2, recordId, record);
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

        THEN("No sector needs to be erased")
        {
            REQUIRE(eeprom.getErasableSector() == NoSector);
            REQUIRE(eeprom.hasErasableSector() == false);
        }
    }

    SECTION("One active sector, one inactive sector")
    {
        store.writeSectorStatus(activeOffset, SECTOR_ACTIVE);
        store.writeSectorStatus(alternateOffset, SECTOR_INACTIVE);

        THEN("The old sector needs to be erased")
        {
            REQUIRE(eeprom.getErasableSector() == Sector2);
            REQUIRE(eeprom.hasErasableSector() == true);
        }

        THEN("Erasing the old sector clear it")
        {
            eeprom.eraseErasableSector();

            REQUIRE(eeprom.getErasableSector() == NoSector);
            REQUIRE(eeprom.hasErasableSector() == false);
        }
    }

    SECTION("One copy sector, one inactive sector")
    {
        store.writeSectorStatus(activeOffset, SECTOR_COPY);
        store.writeSectorStatus(alternateOffset, TestEEPROM::SectorHeader::INACTIVE);

        THEN("The copy sector is marked active and the other sector needs to be erased")
        {
            REQUIRE(eeprom.getErasableSector() == Sector2);
            REQUIRE(eeprom.hasErasableSector() == true);
        }
    }
}
