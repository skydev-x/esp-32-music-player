#pragma once
#include "config.h"
#include "type.h"
#include <SD.h>

class FileManager {
public:
  FileManager();

  bool begin();
  bool scanFiles();
  bool loadCache();
  bool saveCache();
  int getFileCount() const { return fileCount; }
  const AudioFile &getFile(int index) const { return audioFiles[index]; }

private:
  AudioFile audioFiles[MAX_FILES];
  int fileCount;

  bool isAudioFile(const char *filename);

  friend class AudioPlayer;
};
