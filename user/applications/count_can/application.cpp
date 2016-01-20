#include "application.h"
#include <map>

STARTUP(WiFi.selectAntenna(ANT_AUTO));

SYSTEM_THREAD(ENABLED);

void receiveMessages();
void updateCount();

CANChannel can(CAN_D1_D2);

std::map<uint32_t, uint32_t> messageCount;
String messageCountStr;

void setup() {
  can.begin(500000);
  Particle.variable("messages", messageCountStr);

  pinMode(D0, OUTPUT);
  digitalWrite(D0, HIGH);
}

void loop() {
  receiveMessages();
  updateCount();
}

void receiveMessages() {
  CANMessage message;
  while(can.receive(message)) {
    messageCount[message.id] += 1;
  }
}

void updateCount() {
  CriticalSection cs;

  messageCountStr = "Messages: ";
  for(auto count : messageCount) {
    messageCountStr += "0x";
    messageCountStr += String(count.first, HEX);
    messageCountStr += ": ";
    messageCountStr += String(count.second, DEC);
    messageCountStr += ", ";
  }
}
