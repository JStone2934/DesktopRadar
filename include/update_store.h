#pragma once

#include <stdint.h>

#include "update_protocol.h"

bool updateStoreBegin();
bool updateStoreLoadManifest(UpdateManifestRecord* out);
bool updateStoreSaveManifest(UpdateManifestRecord* record);
bool updateStoreLoadJournal(UpdateJournal* out);
bool updateStoreSaveJournal(UpdateJournal* journal);
bool updateStoreGetTime(const char* key, uint64_t* value);
bool updateStoreSetTime(const char* key, uint64_t value);
bool updateStoreGetLastError(UpdateError* error);
bool updateStoreSetLastError(UpdateError error);
bool updateStoreProbe();
