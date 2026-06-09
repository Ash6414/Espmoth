#pragma once

#include <Arduino.h>
#include "Config.h"

extern int mothUartRxPin;

struct ProbeEdges {
  int lastBusy = -1;
  int lastRx = -1;
  uint32_t busyRising = 0;
  uint32_t busyFalling = 0;
  uint32_t rxRising = 0;
  uint32_t rxFalling = 0;
};

inline void updateProbeEdges(ProbeEdges &edges, uint32_t elapsedMs) {
  int busy = digitalRead(PIN_MOTH_BUSY);
  int rx = digitalRead(mothUartRxPin);

  if (edges.lastBusy == -1) {
    edges.lastBusy = busy;
  } else if (busy != edges.lastBusy) {
    if (busy == HIGH) edges.busyRising += 1;
    else edges.busyFalling += 1;
    Serial.printf("EDGE %lu ms: BUSY %d->%d\n", (unsigned long)elapsedMs, edges.lastBusy, busy);
    edges.lastBusy = busy;
  }

  if (edges.lastRx == -1) {
    edges.lastRx = rx;
  } else if (rx != edges.lastRx) {
    if (rx == HIGH) edges.rxRising += 1;
    else edges.rxFalling += 1;
    Serial.printf("EDGE %lu ms: UART_RX %d->%d\n", (unsigned long)elapsedMs, edges.lastRx, rx);
    edges.lastRx = rx;
  }
}
