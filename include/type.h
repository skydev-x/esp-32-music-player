#pragma once
#include <stdint.h>

// Audio file storage with metadata
struct AudioFile {
  char path[128];
  bool isMP3;
  uint32_t size;
};

// System state management
enum PlayerState { STOPPED, PLAYING, SWITCHING, ERROR_STATE };

// Button state management
struct ButtonState {
  uint32_t lastPress;
  bool wasPressed;
};
