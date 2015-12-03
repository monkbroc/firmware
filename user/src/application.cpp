#include "application.h"

SYSTEM_MODE(SEMI_AUTOMATIC);

int updateFrequency(String value) {
  uint16_t freq = value.toInt();
  analogWrite(A4, 50, freq);
  return HAL_PWM_Get_Frequency(A4);
}

void setup()
{
  pinMode(A4, OUTPUT);
  analogWrite(A4, 50);
  Particle.connect();
  Particle.function("freq", updateFrequency);
}
