#include <SoftwareSerial.h>

SoftwareSerial mySerial(5, 4); // RX, TX

void setup() {
  // Start the serial communication with a baud rate of 115200
  Serial.begin(115200);
  mySerial.begin(115200);

  // Wait for the serial port to initialize
  while (!Serial) {
    delay(100);
  }

  // Hex string to send
  String hex_to_send = "FDFCFBFA0800120000006400000004030201";
  sendHexData(hex_to_send);
}

void loop() {
  // Read and print serial data
  readSerialData();
}

void sendHexData(String hexString) {
  // Convert hex string to bytes
  int hexStringLength = hexString.length();
  byte hexBytes[hexStringLength / 2];
  for (int i = 0; i < hexStringLength; i += 2) {
    hexBytes[i / 2] = strtoul(hexString.substring(i, i + 2).c_str(), NULL, 16);
  }

  // Send bytes through software serial
  mySerial.write(hexBytes, sizeof(hexBytes));
}

void readSerialData() {
  // Read and print data from software serial
  while (mySerial.available() > 0) {
    char incomingByte = mySerial.read();
    Serial.print(incomingByte);
  }
}
