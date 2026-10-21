// How the web side talks to the rest of the program.
//
// Web requests are handled in the network task, while loop() owns the screen, the scanner
// and the connection. Changing those from two tasks at once would corrupt them. So a web
// request never changes anything itself: it drops a Command into a FreeRTOS queue, and
// loop() applies the queued commands at the start of every frame. One owner, no races.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct Command {
  enum class Type : uint8_t { SetView, Scan, SetAutoScan, SetBacklight, SetLed, ShowMessage, SaveWifi, ForgetWifi, Reboot };
  Type type;
  int32_t value = 0;     // view index, on/off, backlight level
  uint8_t r = 0, g = 0, b = 0;
  char text[97] = {};    // message text, or SSID for SaveWifi
  char secret[65] = {};  // password for SaveWifi (never logged)
};

class CommandQueue {
 public:
  void begin() { queue_ = xQueueCreate(16, sizeof(Command)); }

  // From any task. Returns false when the queue is full (the request then answers 503).
  bool push(const Command &c) { return queue_ != nullptr && xQueueSend(queue_, &c, 0) == pdTRUE; }

  // From loop() only.
  bool pop(Command &c) { return queue_ != nullptr && xQueueReceive(queue_, &c, 0) == pdTRUE; }

 private:
  QueueHandle_t queue_ = nullptr;
};
