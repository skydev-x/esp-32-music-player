#include <Arduino.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

// SD Card Pin
#define SD_CS 5

// File cache configuration
#define CACHE_FILE "/playlist.cache"
#define MAX_FILES 300

// Audio file storage with metadata
struct AudioFile {
  char path[128]; // Increased for long filenames
  bool isMP3;
  uint32_t size;
};
AudioFile audioFiles[MAX_FILES];
int fileCount = 0;
int currentFileIndex = 0;

// Audio objects - persistent for reuse
AudioFileSourceSD *source = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;
AudioGeneratorWAV *wav = nullptr;
AudioOutputI2S *output = nullptr;

// System state management
enum PlayerState { STOPPED, PLAYING, SWITCHING, ERROR_STATE };
PlayerState playerState = STOPPED;

// Performance tracking
uint32_t lastSwitchTime = 0;
const uint32_t SWITCH_TIMEOUT_MS = 3000;

// Track current volume
float currentGain = 0.9;

// ===== Buttons =====
#define BTN_UP 32
#define BTN_DOWN 33
#define BTN_PLAY 25
#define BTN_PAUSE 26

// Button state management
struct ButtonState {
  uint32_t lastPress;
  bool wasPressed;
};
ButtonState buttons[4];

// ===== FILE CACHE MANAGEMENT =====

// Save playlist to cache file
bool savePlaylistCache() {
  Serial.println("Saving playlist cache...");

  File cacheFile = SD.open(CACHE_FILE, FILE_WRITE);
  if (!cacheFile) {
    Serial.println("Failed to create cache file");
    return false;
  }

  // Write header: version and file count
  cacheFile.println("V1");
  cacheFile.println(fileCount);

  // Write each file entry: path|isMP3|size
  for (int i = 0; i < fileCount; i++) {
    cacheFile.print(audioFiles[i].path);
    cacheFile.print("|");
    cacheFile.print(audioFiles[i].isMP3 ? "1" : "0");
    cacheFile.print("|");
    cacheFile.println(audioFiles[i].size);
  }

  cacheFile.close();
  Serial.println("Cache saved successfully");
  return true;
}

// Load playlist from cache file
bool loadPlaylistCache() {
  if (!SD.exists(CACHE_FILE)) {
    Serial.println("No cache file found");
    return false;
  }

  Serial.println("Loading playlist cache...");
  File cacheFile = SD.open(CACHE_FILE, FILE_READ);
  if (!cacheFile) {
    Serial.println("Failed to open cache file");
    return false;
  }

  // Read and verify version
  String version = cacheFile.readStringUntil('\n');
  version.trim();
  if (version != "V1") {
    Serial.println("Invalid cache version");
    cacheFile.close();
    return false;
  }

  // Read file count
  String countStr = cacheFile.readStringUntil('\n');
  countStr.trim();
  int cachedCount = countStr.toInt();

  if (cachedCount <= 0 || cachedCount > MAX_FILES) {
    Serial.println("Invalid file count in cache");
    cacheFile.close();
    return false;
  }

  // Read each file entry
  fileCount = 0;
  while (cacheFile.available() && fileCount < cachedCount) {
    String line = cacheFile.readStringUntil('\n');
    line.trim();

    if (line.length() == 0)
      continue;

    // Parse: path|isMP3|size
    int firstPipe = line.indexOf('|');
    int secondPipe = line.lastIndexOf('|');

    if (firstPipe == -1 || secondPipe == -1 || firstPipe == secondPipe) {
      Serial.print("Malformed cache line: ");
      Serial.println(line);
      continue;
    }

    String path = line.substring(0, firstPipe);
    String isMP3Str = line.substring(firstPipe + 1, secondPipe);
    String sizeStr = line.substring(secondPipe + 1);

    // Verify file still exists
    if (!SD.exists(path)) {
      Serial.print("Cached file missing: ");
      Serial.println(path);
      continue;
    }

    // Store in array
    strncpy(audioFiles[fileCount].path, path.c_str(), 127);
    audioFiles[fileCount].path[127] = '\0';
    audioFiles[fileCount].isMP3 = (isMP3Str == "1");
    audioFiles[fileCount].size = sizeStr.toInt();
    fileCount++;
  }

  cacheFile.close();

  Serial.print("Loaded ");
  Serial.print(fileCount);
  Serial.println(" files from cache");

  return fileCount > 0;
}

// ===== OPTIMIZED UTILITY FUNCTIONS =====

// Fast file type detection - no String conversion
bool isAudioFile(const char *filename) {
  int len = strlen(filename);
  if (len < 4)
    return false;

  // Check last 4 characters for .mp3 or .wav (case insensitive)
  const char *ext = filename + len - 4;

  return (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0);
}

// Check if filename is MP3
bool isMP3File(const char *filename) {
  int len = strlen(filename);
  if (len < 4)
    return false;

  const char *ext = filename + len - 4;
  return (strcasecmp(ext, ".mp3") == 0);
}

// Scan SD card and build file list
bool scanSDCard() {
  Serial.println("Scanning SD card...");
  fileCount = 0;

  File root = SD.open("/");
  if (!root) {
    Serial.println("Failed to open root directory");
    return false;
  }

  File file = root.openNextFile();
  while (file && fileCount < MAX_FILES) {
    if (!file.isDirectory()) {
      const char *fileName = file.name();

      if (isAudioFile(fileName)) {
        // Build path
        if (fileName[0] == '/') {
          strncpy(audioFiles[fileCount].path, fileName, 127);
        } else {
          snprintf(audioFiles[fileCount].path, 128, "/%s", fileName);
        }
        audioFiles[fileCount].path[127] = '\0';

        audioFiles[fileCount].size = file.size();
        audioFiles[fileCount].isMP3 = isMP3File(fileName);
        fileCount++;
      }
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();

  Serial.print("Found ");
  Serial.print(fileCount);
  Serial.println(" audio files");

  return fileCount > 0;
}

// ===== AUDIO SYSTEM MANAGEMENT =====

// Initialize audio system once
bool initializeAudioSystem() {
  if (!output) {
    output = new AudioOutputI2S();
    output->SetPinout(12, 27, 14);
    output->SetOutputModeMono(true);
    output->SetGain(currentGain);
  }

  if (!source) {
    source = new AudioFileSourceSD();
  }

  if (!mp3) {
    mp3 = new AudioGeneratorMP3();
  }

  if (!wav) {
    wav = new AudioGeneratorWAV();
  }

  return (output && source && mp3 && wav);
}

// Safe cleanup without deletion
void safeStopPlayback() {
  if (playerState == SWITCHING)
    return; // Already switching

  playerState = SWITCHING;

  if (mp3 && mp3->isRunning()) {
    mp3->stop();
  }
  if (wav && wav->isRunning()) {
    wav->stop();
  }

  if (source) {
    source->close();
  }

  playerState = STOPPED;
}

// Robust file playing with error recovery
bool playFileRobust(int index, int maxRetries = 3) {
  if (index < 0 || index >= fileCount) {
    Serial.println("Invalid file index");
    return false;
  }

  // Timeout protection
  uint32_t startTime = millis();
  if (startTime - lastSwitchTime < 100) {
    Serial.println("Switch too fast, skipping");
    return false;
  }
  lastSwitchTime = startTime;

  // Safe cleanup
  safeStopPlayback();

  AudioFile &audioFile = audioFiles[index];
  Serial.print("Loading [");
  Serial.print(index + 1);
  Serial.print("]: ");
  Serial.println(audioFile.path);

  int retryCount = 0;
  while (retryCount < maxRetries) {
    // Open file
    if (!source->open(audioFile.path)) {
      Serial.print("File open failed, retry ");
      Serial.println(retryCount + 1);
      retryCount++;
      delay(100);
      continue;
    }

    // Initialize appropriate generator
    bool success = false;
    if (audioFile.isMP3) {
      success = mp3->begin(source, output);
    } else {
      success = wav->begin(source, output);
    }

    if (success) {
      currentFileIndex = index;
      playerState = PLAYING;
      Serial.println("Playback started");
      return true;
    }

    Serial.print("Generator failed, retry ");
    Serial.println(retryCount + 1);
    source->close();
    retryCount++;
    delay(100);
  }

  // All retries failed
  playerState = ERROR_STATE;
  Serial.println("File load failed completely");
  return false;
}

// Smart next song with fallback
void playNext() {
  if (fileCount == 0)
    return;

  int nextIndex = (currentFileIndex + 1) % fileCount;

  if (!playFileRobust(nextIndex)) {
    Serial.println("Next song failed, stopping playback");
    playerState = STOPPED;
  }
}

// Sequential previous with error handling (FIXED modulo bug)
void playPrevious() {
  if (fileCount == 0)
    return;

  // Fixed: Proper handling of negative modulo
  int prevIndex = (currentFileIndex - 1 + fileCount) % fileCount;

  if (!playFileRobust(prevIndex)) {
    Serial.println("Previous song failed, staying on current");
  }
}

// ===== SYSTEM INITIALIZATION =====

void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial time to initialize

  Serial.println("\n\n=== ESP32 Audio Player ===");

  randomSeed(analogRead(0) + millis());
  SPI.begin();

  // Initialize SD card
  Serial.println("Initializing SD card...");
  for (int retry = 0; retry < 5; retry++) {
    if (SD.begin(SD_CS)) {
      Serial.println("SD Card ready");
      break;
    }
    Serial.print("SD retry ");
    Serial.println(retry + 1);
    delay(1000);

    if (retry == 4) {
      Serial.println("SD Card failed completely");
      while (1)
        delay(1000);
    }
  }

  // Try to load from cache first
  bool cacheLoaded = loadPlaylistCache();

  // If cache failed or user wants rescan, do full scan
  if (!cacheLoaded) {
    Serial.println("Performing full SD scan...");
    if (!scanSDCard() || fileCount == 0) {
      Serial.println("No audio files found");
      while (1)
        delay(1000);
    }

    // Save the new playlist cache
    savePlaylistCache();
  }

  // Initialize buttons
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_PLAY, INPUT_PULLUP);
  pinMode(BTN_PAUSE, INPUT_PULLUP);

  // Initialize audio system
  if (!initializeAudioSystem()) {
    Serial.println("Audio system initialization failed");
    while (1)
      delay(1000);
  }

  Serial.println("System ready!");
  Serial.println("Type 'h' for help");

  // Start with first song (or configured index)
  delay(500);
  playFileRobust(3);
}

// ===== INPUT HANDLING =====

void handleButtons() {
  static const int btnPins[] = {BTN_UP, BTN_DOWN, BTN_PLAY, BTN_PAUSE};
  uint32_t now = millis();

  for (int i = 0; i < 4; i++) {
    bool currentPressed = !digitalRead(btnPins[i]); // Active LOW

    if (currentPressed && !buttons[i].wasPressed &&
        (now - buttons[i].lastPress > 200)) { // 200ms debounce

      buttons[i].lastPress = now;

      switch (i) {
      case 0: // UP - Previous
        playPrevious();
        break;
      case 1: // DOWN - Next
        playNext();
        break;
      case 2: // PLAY - Restart current
        playFileRobust(currentFileIndex);
        break;
      case 3: // PAUSE - Stop
        safeStopPlayback();
        break;
      }
    }

    buttons[i].wasPressed = currentPressed;
  }
}

void handleSerialCommand(char cmd, const String &arg) {
  switch (cmd) {
  case 'n':
  case 'N':
    playNext();
    break;

  case 'p':
  case 'P':
    playPrevious();
    break;

  case 's':
  case 'S':
    safeStopPlayback();
    Serial.println("Stopped");
    break;

  case 'r':
  case 'R':
    playFileRobust(currentFileIndex);
    break;

  case 'c':
  case 'C': {
    // Rescan SD card and update cache
    Serial.println("Rescanning SD card...");
    if (scanSDCard()) {
      savePlaylistCache();
      Serial.println("Cache updated");
    }
    break;
  }

  case 'l':
  case 'L': {
    Serial.println("\n=== Playlist ===");
    int displayCount = min(fileCount, 30);
    for (int i = 0; i < displayCount; i++) {
      Serial.print(i == currentFileIndex ? "> " : "  ");
      Serial.print(i + 1);
      Serial.print(". ");
      Serial.println(audioFiles[i].path);
    }
    if (fileCount > 30) {
      Serial.print("... and ");
      Serial.print(fileCount - 30);
      Serial.println(" more");
    }
    Serial.println();
    break;
  }

  case '+': {
    currentGain = min(1.0f, currentGain + 0.1f);
    if (output)
      output->SetGain(currentGain);
    Serial.print("Volume: ");
    Serial.println((int)(currentGain * 100));
    break;
  }

  case '-': {
    currentGain = max(0.0f, currentGain - 0.1f);
    if (output)
      output->SetGain(currentGain);
    Serial.print("Volume: ");
    Serial.println((int)(currentGain * 100));
    break;
  }

  case 'g': {
    if (arg.length() > 0) {
      int targetIndex = arg.toInt() - 1; // Convert to 0-based
      if (targetIndex >= 0 && targetIndex < fileCount) {
        playFileRobust(targetIndex);
      } else {
        Serial.println("Invalid track number");
      }
    }
    break;
  }

  case 'i': {
    Serial.println("\n=== Status ===");
    Serial.print("State: ");
    Serial.println(playerState == PLAYING     ? "PLAYING"
                   : playerState == STOPPED   ? "STOPPED"
                   : playerState == SWITCHING ? "SWITCHING"
                                              : "ERROR");
    Serial.print("Track: ");
    Serial.print(currentFileIndex + 1);
    Serial.print("/");
    Serial.println(fileCount);
    Serial.print("File: ");
    Serial.println(audioFiles[currentFileIndex].path);
    Serial.print("Volume: ");
    Serial.print((int)(currentGain * 100));
    Serial.println("%");
    Serial.println();
    break;
  }

  case 'h':
  case 'H':
  case '?': {
    Serial.println("\n=== Commands ===");
    Serial.println("n - Next track");
    Serial.println("p - Previous track");
    Serial.println("s - Stop playback");
    Serial.println("r - Restart current track");
    Serial.println("l - List tracks");
    Serial.println("c - Rescan SD & update cache");
    Serial.println("+ - Volume up");
    Serial.println("- - Volume down");
    Serial.println("g<num> - Go to track (e.g., g5)");
    Serial.println("i - Show info");
    Serial.println("h - Show this help");
    Serial.println();
    break;
  }

  default:
    Serial.print("Unknown command: ");
    Serial.println(cmd);
    Serial.println("Type 'h' for help");
    break;
  }
}

// ===== MAIN LOOP =====

void loop() {
  static uint32_t lastHealthCheck = 0;
  uint32_t now = millis();

  // Handle buttons
  handleButtons();

  // Audio playback management
  if (playerState == PLAYING) {
    bool isPlaying = false;

    if (audioFiles[currentFileIndex].isMP3 && mp3) {
      isPlaying = mp3->isRunning() && mp3->loop();
    } else if (!audioFiles[currentFileIndex].isMP3 && wav) {
      isPlaying = wav->isRunning() && wav->loop();
    }

    if (!isPlaying) {
      Serial.println("Song ended, playing next");
      playNext();
    }
  }
  // REMOVED: Auto-restart logic (was causing unwanted behavior)

  // Health check and recovery
  if (now - lastHealthCheck > 10000) { // Every 10 seconds
    if (playerState == ERROR_STATE) {
      Serial.println("Recovering from error state");
      playerState = STOPPED;
      delay(500);
      // Don't auto-play, wait for user input
    }
    lastHealthCheck = now;
  }

  // Handle serial commands
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.length() > 0) {
      char cmd = input[0];
      String arg = input.substring(1);
      arg.trim();
      handleSerialCommand(cmd, arg);
    }

    // Clear any remaining buffer
    while (Serial.available())
      Serial.read();
  }

  yield(); // Allow background tasks
}
