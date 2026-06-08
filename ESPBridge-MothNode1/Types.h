#pragma once
#include <Arduino.h>

struct PowerState {
  float batteryV;
  float batteryPercent;
  bool charging;
  bool chargeDone;
};

struct MothFile {
  String path;
  uint32_t size;
};

struct ChunkResult {
  bool ok;
  String path;
  uint32_t offset;
  uint32_t length;
  uint32_t crc;
};

enum UploadResultCode {
  UPLOAD_NOT_ATTEMPTED = 0,
  UPLOAD_SUCCESS = 1,
  UPLOAD_SKIPPED_BUSY = 2,
  UPLOAD_SKIPPED_POWER = 3,
  UPLOAD_NO_FILES = 4,
  UPLOAD_SERVER_FAILED = 5,
  UPLOAD_BRIDGE_FAILED = 6
};

struct UploadSummary {
  UploadResultCode code;
  uint16_t filesSeen;
  uint16_t filesUploaded;
  uint16_t filesDeleted;
  String message;
};
