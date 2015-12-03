#include "application.h"

SYSTEM_MODE(SEMI_AUTOMATIC);

int updateFrequency(String value) {
  HAL_PWM_Set_Frequency(A4, value.toInt());
  pinMode(A4, OUTPUT);
  analogWrite(A4, 50);
  return HAL_PWM_Get_Frequency(A4);
}

void setup()
{
  pinMode(A4, OUTPUT);
  analogWrite(A4, 50);
  Particle.connect();
  Particle.function("freq", updateFrequency);
}
