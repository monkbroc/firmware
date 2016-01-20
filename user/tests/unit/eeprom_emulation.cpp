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

// Interrupted write 1: status written as invalid, but id, length, data
// not written
template <typename T>
uintptr_t writeInvalidRecord(TestStore &store, uintptr_t offset, uint16_t id, const T& record)
{
    uint16_t status = TestEEPROM::Header::INVALID;
    store.write(offset, &status, sizeof(status));
    // Next record should be written directly after the invalid header
    return offset + sizeof(TestEEPROM::Header);
}

// Interrupted write 2: status written as invalid, id, length written, but data
// not written
template <typename T>
uintptr_t writeRecordHeader(TestStore &store, uintptr_t offset, uint16_t id, const T& record)
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

// Interrupted write 3: status written as invalid, id, length written, data
// partially written
template <typename T>
uintptr_t writePartialRecord(TestStore &store, uintptr_t offset, uint16_t id, const T& record)
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

template <typename T>
uintptr_t writeRecord(TestStore &store, uintptr_t offset, uint16_t id, const T& record)
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

template <typename T>
void readValidRecord(TestStore &store, uintptr_t offset, uint16_t expectedId, const T& expected)
{
    uint16_t status;
    uint16_t expectedStatus = TestEEPROM::Header::VALID;
    store.read(&status, offset, sizeof(status));
    offset += sizeof(status);
    REQUIRE(status == expectedStatus);

    uint16_t id;
    store.read(&id, offset, sizeof(id));
    offset += sizeof(id);
    REQUIRE(id == expectedId);

    uint16_t length;
    store.read(&length, offset, sizeof(length));
    offset += sizeof(length);
    REQUIRE(length == sizeof(expected));

    T value;
    store.read(&value, offset, sizeof(expected));
    offset += sizeof(expected);
    REQUIRE(value == expected);
}

// Display helper for Header struct
std::ostream& operator << ( std::ostream& os, TestEEPROM::Header const& value ) {
    std::stringstream ss;
    ss << "Header" << std::hex <<
        " status=0x" << value.status <<
        " id=0x" << value.id <<
        " length=0x" << value.length;
    os << ss.str();
    return os;
}


TEST_CASE("Get record", "[eeprom]")
{
    TestEEPROM eeprom;

    // Start with erased flash
    eeprom.store.eraseSector(TestBase);
    eeprom.store.eraseSector(TestBase + TestSectorSize);
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
            offset = writeInvalidRecord(eeprom.store, offset, recordId, badRecord);
            offset = writeRecordHeader(eeprom.store, offset, recordId, badRecord);
            offset = writePartialRecord(eeprom.store, offset, recordId, badRecord);

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
            offset = writeRecord(eeprom.store, offset, recordId, record);

            THEN("get returns true and extracts the value")
            {
                uint8_t value;
                REQUIRE(eeprom.get(recordId, value) == true);
                REQUIRE(value == record);
            }
        }

        SECTION("With bad records")
        {
            uint8_t badRecord[] = { 0xCC, 0xDD };
            offset = writeInvalidRecord(eeprom.store, offset, recordId, badRecord);
            offset = writeRecordHeader(eeprom.store, offset, recordId, badRecord);
            offset = writePartialRecord(eeprom.store, offset, recordId, badRecord);

            offset = writeRecord(eeprom.store, offset, recordId, record);

            THEN("get returns true and extracts the value")
            {
                uint8_t value;
                REQUIRE(eeprom.get(recordId, value) == true);
                REQUIRE(value == record);
            }
        }
    }
}

TEST_CASE("Put record", "[eeprom]")
{
    TestEEPROM eeprom;

    // Start with erased flash
    eeprom.store.eraseSector(TestBase);
    eeprom.store.eraseSector(TestBase + TestSectorSize);
    uint32_t offset = TestBase + 2;

    SECTION("The record doesn't exist")
    {
        uint16_t recordId = 0;
        uint8_t record = 0xDD;

        THEN("put returns true and creates the record")
        {
            REQUIRE(eeprom.put(recordId, record) == true);
            readValidRecord(eeprom.store, offset, recordId, record);
        }
    }

    //SECTION("The record exists")
    //{
    //    uint16_t recordId = 0;
    //    uint8_t record = 0xCC;

    //    SECTION("No other records")
    //    {
    //        offset = writeRecord(eeprom.store, offset, recordId, record);

    //        THEN("get returns true and extracts the value")
    //        {
    //            uint8_t value;
    //            REQUIRE(eeprom.get(recordId, value) == true);
    //            REQUIRE(value == record);
    //        }
    //    }

    //    SECTION("With bad records")
    //    {
    //        uint8_t badRecord[] = { 0xCC, 0xDD };
    //        offset = writeInvalidRecord(eeprom.store, offset, recordId, badRecord);
    //        offset = writeRecordHeader(eeprom.store, offset, recordId, badRecord);
    //        offset = writePartialRecord(eeprom.store, offset, recordId, badRecord);

    //        offset = writeRecord(eeprom.store, offset, recordId, record);

    //        THEN("get returns true and extracts the value")
    //        {
    //            uint8_t value;
    //            REQUIRE(eeprom.get(recordId, value) == true);
    //            REQUIRE(value == record);
    //        }
    //    }
    //}
}

//SCENARIO("RAMFlashStore is initially random", "[ramflash]")
//{
//    TestStore store;
//    int fingerprint = sum(store, TestBase, TestSectorSize*TestSectorCount);
//    REQUIRE(fingerprint != 0);      // not all 0's
//    REQUIRE(fingerprint != (TestSectorSize*TestSectorCount)*0xFF);
//}
//
//SCENARIO("RAMFlashStore can be erased","[ramflash]")
//{
//    TestStore store;
//    REQUIRE_FALSE(store.eraseSector(TestBase+100+TestSectorSize));
//
//    const uint8_t* data = store.dataAt(TestBase+TestSectorSize);
//    for (unsigned i=0; i<TestSectorSize; i++) {
//        CAPTURE(i);
//        CHECK(data[i] == 0xFF);
//    }
//
//    int fingerprint = sum(store, TestBase+TestSectorSize, TestSectorSize);   // 2nd sector
//    REQUIRE(fingerprint != 0);      // not all 0's
//    REQUIRE(fingerprint == (TestSectorSize)*0xFF);
//}
//
//SCENARIO("RAMFlashStore can store data", "[ramflash]")
//{
//    TestStore store;
//    REQUIRE_FALSE(store.eraseSector(TestBase));
//    REQUIRE_FALSE(store.write(TestBase+3, (const uint8_t*)"batman", 7));
//
//    const char* expected = "\xFF\xFF\xFF" "batman" "\x00\xFF\xFF";
//    const char* actual = (const char*)store.dataAt(TestBase);
//    REQUIRE(string(actual,12) == string(expected,12));
//}
//
//SCENARIO("RAMFlashStore emulates NAND flash", "[ramflash]")
//{
//    TestStore store;
//    REQUIRE_FALSE(store.eraseSector(TestBase));
//    REQUIRE_FALSE(store.write(TestBase+3, (const uint8_t*)"batman", 7));
//    REQUIRE_FALSE(store.write(TestBase+0, (const uint8_t*)"\xA8\xFF\x00", 3));
//
//    const char* actual = (const char*)store.dataAt(TestBase);
//
//    const char* expected = "\xA8\xFF\x00" "batman" "\x00\xFF\xFF";
//    REQUIRE(string(actual,12) == string(expected,12));
//
//    // no change to flash storage
//    REQUIRE_FALSE(store.write(TestBase, (const uint8_t*)"\xF7\x80\x00\xFF", 3));
//    expected = "\xA0\x80\0batman\x00\xFF\xFF";
//    REQUIRE(string(actual,12) == string(expected,12));
//}
//
//
//// DCD Tests
//
//
//SCENARIO("DCD initialized returns 0xFF", "[dcd]")
//{
//    TestDCD dcd;
//
//    const uint8_t* data = dcd.read(0);
//    for (unsigned i=0; i<dcd.Length; i++)
//    {
//        CAPTURE( i );
//        REQUIRE(data[i] == 0xFFu);
//    }
//}
//
//SCENARIO("DCD Length is SectorSize minus 8", "[dcd]")
//{
//    TestDCD dcd;
//    REQUIRE(dcd.Length == TestSectorSize-8);
//}
//
//SCENARIO("DCD can save data", "[dcd]")
//{
//    TestDCD dcd;
//
//    uint8_t expected[dcd.Length];
//    memset(expected, 0xFF, sizeof(expected));
//    memcpy(expected+23, "batman", 6);
//
//    REQUIRE_FALSE(dcd.write(23, "batman", 6));
//
//    const uint8_t* data = dcd.read(0);
//    assertMemoryEqual(data, expected, dcd.Length);
//}
//
//SCENARIO("DCD can write whole sector", "[dcd]")
//{
//    TestDCD dcd;
//
//    uint8_t expected[dcd.Length];
//    for (unsigned i=0; i<dcd.Length; i++)
//        expected[i] = rand();
//
//    dcd.write(0, expected, dcd.Length);
//    const uint8_t* data = dcd.read(0);
//    assertMemoryEqual(data, expected, dcd.Length);
//}
//
//SCENARIO("DCD can overwrite data", "[dcd]")
//{
//    TestDCD dcd;
//
//    uint8_t expected[dcd.Length];
//    for (unsigned i=0; i<dcd.Length; i++)
//        expected[i] = 0xFF;
//    memmove(expected+23, "bbatman", 7);
//
//    // overwrite data swapping a b to an a and vice versa
//    REQUIRE_FALSE(dcd.write(23, "batman", 6));
//    REQUIRE_FALSE(dcd.write(24, "batman", 6));
//
//    const uint8_t* data = dcd.read(0);
//    assertMemoryEqual(data, expected, dcd.Length);
//}
//
//SCENARIO("DCD uses 2nd sector if both are valid", "[dcd]")
//{
//    TestDCD dcd;
//    TestStore& store = dcd.store;
//
//    TestDCD::Header header;
//    header.make_valid();
//
//    // directly manipulate the flash to create desired state
//    store.eraseSector(TestBase);
//    store.eraseSector(TestBase+TestSectorSize);
//    store.write(TestBase, &header, sizeof(header));
//    store.write(TestBase+sizeof(header), "abcd", 4);
//    store.write(TestBase+TestSectorSize, &header, sizeof(header));
//    store.write(TestBase+TestSectorSize+sizeof(header), "1234", 4);
//
//    const uint8_t* result = dcd.read(0);
//    assertMemoryEqual(result, (const uint8_t*)"1234", 4);
//}
//
//
//SCENARIO("DCD write is atomic if partial failure", "[dcd]")
//{
//    for (int write_count=1; write_count<5; write_count++)
//    {
//        TestDCD dcd;
//        REQUIRE_FALSE(dcd.write(23, "abcdef", 6));
//        REQUIRE_FALSE(dcd.write(23, "batman", 6));
//
//        assertMemoryEqual(dcd.read(23), (const uint8_t*)"batman", 6);
//
//        // mock a power failure after a certain number of writes
//        dcd.store.setWriteCount(write_count);
//        CAPTURE(write_count);
//
//        // write should fail
//        REQUIRE(dcd.write(23, "7890-!", 6));
//
//        // last write is unsuccessful
//        assertMemoryEqual(dcd.read(23), (const uint8_t*)"batman", 6);
//    }
//}
