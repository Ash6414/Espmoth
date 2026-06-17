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
  uint32_t localFileId;
};

struct MothSdInfo {
  bool valid;
  uint32_t totalKb;
  uint32_t freeKb;
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
  MothSdInfo sd;
};

struct UploadSession {
  bool ok;
  bool alreadyComplete;
  String uploadId;
  uint32_t fileId;
  uint32_t chunkSize;
  uint32_t totalChunks;
  uint32_t resumeOffset;
};

struct DeleteCandidate {
  uint32_t fileId;
  uint32_t localFileId;
  String filename;
  String path;
  bool deleted;
  String error;
};
