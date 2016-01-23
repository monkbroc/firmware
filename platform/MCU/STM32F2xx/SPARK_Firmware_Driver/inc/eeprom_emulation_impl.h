/*
 ******************************************************************************
 *  Copyright (c) 2016 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */

#include "eeprom_emulation.h"
#include "flash_storage_impl.h"

constexpr uintptr_t EEPROM_SectorBase1 = 0x0800C000;
constexpr uintptr_t EEPROM_SectorBase2 = 0x08010000;

constexpr size_t EEPROM_SectorSize1 = 16*1024;
constexpr size_t EEPROM_SectorSize2 = 64*1024;

using FlashEEPROM = EEPROMEmulation<InternalFlashStore, EEPROM_SectorBase1, EEPROM_SectorSize1, EEPROM_SectorBase2, EEPROM_SectorSize2>;

FlashEEPROM flashEEPROM;


extern "C" {

void HAL_EEPROM_Init(void)
{
  flashEEPROM.init();
}

uint8_t HAL_EEPROM_Read(uint32_t address)
{
  uint8_t value = 0xFF;
  flashEEPROM.get(address, value);
  return value;
}

void HAL_EEPROM_Write(uint32_t address, uint8_t data)
{
  flashEEPROM.put(address, data);
}

size_t HAL_EEPROM_Length()
{
  return flashEEPROM.totalCapacity();
}

// TODO add additional EEPROM APIs

}
