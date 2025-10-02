#include "config.h"
#include "type.h"
#include <Arduino.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <SD.h>
#include <SPI.h>
#include <SerialController.h>
#include <Wire.h>

SerialController serial;

AudioFile audioFiles[MAX_FILES];
int fileCount = 0;
int currentFileIndex = 0;

// Audio objects - persistent for reuse
AudioFileSourceSD *source = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;
AudioGeneratorWAV *wav = nullptr;
AudioOutputI2S *output = nullptr;

PlayerState playerState = STOPPED;

// Performance tracking
uint32_t lastSwitchTime = 0;
const uint32_t SWITCH_TIMEOUT_MS = 3000;

// Track current volume
float currentGain = 0.9;

// BLE objects
BLEServer *pServer = nullptr;
BLECharacteristic *pTxCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;
String bleInputBuffer = "";

ButtonState buttons[4];

// Forward declarations
void handleCommand(char cmd, const String &arg, bool fromBLE);
void sendBLE(const String &message);
void sendOutput(const String &message);
void sendOutputln(const String &message);

// ===== BLE OUTPUT FUNCTIONS =====

void sendBLE(const String &message) {
  if (deviceConnected && pTxCharacteristic) {
    int len = message.length();
    int offset = 0;

    while (offset < len) {
      int chunkSize = min(20, len - offset);
      String chunk = message.substring(offset, offset + chunkSize);
      pTxCharacteristic->setValue(chunk.c_str());
      pTxCharacteristic->notify();
      offset += chunkSize;
      delay(10);
    }
  }
}

void sendOutput(const String &message) {
  serial.print(message);
  if (deviceConnected) {
    sendBLE(message);
  }
}

void sendOutputln(const String &message) {
  serial.println(message);
  if (deviceConnected) {
    sendBLE(message + "\n");
  }
}

void processBLECommand(const String &input) {
  String trimmed = input;
  trimmed.trim();

  if (trimmed.length() > 0) {
    char cmd = trimmed[0];
    String arg = trimmed.substring(1);
    arg.trim();
    handleCommand(cmd, arg, true);
  }
}

// ===== BLE CALLBACKS (MUST BE BEFORE initBLE) =====

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    deviceConnected = true;
    serial.println("BLE Client Connected");

    BLEDevice::stopAdvertising();
    delay(100);

    sendBLE("Connected to ESP32 Audio Player\n");
    sendBLE("Type 'h' for help\n");
  }

  void onDisconnect(BLEServer *pServer) {
    deviceConnected = false;
    serial.println("BLE Client Disconnected");
    delay(500);
    BLEDevice::startAdvertising();
    serial.println("Restarting BLE advertising");
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      String input = String(rxValue.c_str());
      input.trim();
      if (input.length() > 0) {
        processBLECommand(input);
      }
    }
  }
};

// ===== FILE CACHE MANAGEMENT =====

bool savePlaylistCache() {
  sendOutputln("Saving playlist cache...");

  File cacheFile = SD.open(CACHE_FILE, FILE_WRITE);
  if (!cacheFile) {
    sendOutputln("Failed to create cache file");
    return false;
  }

  cacheFile.println("V1");
  cacheFile.println(fileCount);

  for (int i = 0; i < fileCount; i++) {
    cacheFile.print(audioFiles[i].path);
    cacheFile.print("|");
    cacheFile.print(audioFiles[i].isMP3 ? "1" : "0");
    cacheFile.print("|");
    cacheFile.println(audioFiles[i].size);
  }

  cacheFile.close();
  sendOutputln("Cache saved successfully");
  return true;
}

bool loadPlaylistCache() {
  if (!SD.exists(CACHE_FILE)) {
    sendOutputln("No cache file found");
    return false;
  }

  sendOutputln("Loading playlist cache...");
  File cacheFile = SD.open(CACHE_FILE, FILE_READ);
  if (!cacheFile) {
    sendOutputln("Failed to open cache file");
    return false;
  }

  String version = cacheFile.readStringUntil('\n');
  version.trim();
  if (version != "V1") {
    sendOutputln("Invalid cache version");
    cacheFile.close();
    return false;
  }

  String countStr = cacheFile.readStringUntil('\n');
  countStr.trim();
  int cachedCount = countStr.toInt();

  if (cachedCount <= 0 || cachedCount > MAX_FILES) {
    sendOutputln("Invalid file count in cache");
    cacheFile.close();
    return false;
  }

  fileCount = 0;
  while (cacheFile.available() && fileCount < cachedCount) {
    String line = cacheFile.readStringUntil('\n');
    line.trim();

    if (line.length() == 0)
      continue;

    int firstPipe = line.indexOf('|');
    int secondPipe = line.lastIndexOf('|');

    if (firstPipe == -1 || secondPipe == -1 || firstPipe == secondPipe) {
      sendOutput("Malformed cache line: ");
      sendOutputln(line);
      continue;
    }

    String path = line.substring(0, firstPipe);
    String isMP3Str = line.substring(firstPipe + 1, secondPipe);
    String sizeStr = line.substring(secondPipe + 1);

    if (!SD.exists(path)) {
      sendOutput("Cached file missing: ");
      sendOutputln(path);
      continue;
    }

    strncpy(audioFiles[fileCount].path, path.c_str(), 127);
    audioFiles[fileCount].path[127] = '\0';
    audioFiles[fileCount].isMP3 = (isMP3Str == "1");
    audioFiles[fileCount].size = sizeStr.toInt();
    fileCount++;
  }

  cacheFile.close();

  sendOutput("Loaded ");
  sendOutput(String(fileCount));
  sendOutputln(" files from cache");

  return fileCount > 0;
}

// ===== UTILITY FUNCTIONS =====

bool isAudioFile(const char *filename) {
  int len = strlen(filename);
  if (len < 4)
    return false;

  const char *ext = filename + len - 4;
  return (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0);
}

bool isMP3File(const char *filename) {
  int len = strlen(filename);
  if (len < 4)
    return false;

  const char *ext = filename + len - 4;
  return (strcasecmp(ext, ".mp3") == 0);
}

bool scanSDCard() {
  sendOutputln("Scanning SD card...");
  fileCount = 0;

  File root = SD.open("/");
  if (!root) {
    sendOutputln("Failed to open root directory");
    return false;
  }

  File file = root.openNextFile();
  while (file && fileCount < MAX_FILES) {
    if (!file.isDirectory()) {
      const char *fileName = file.name();

      if (isAudioFile(fileName)) {
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

  sendOutput("Found ");
  sendOutput(String(fileCount));
  sendOutputln(" audio files");

  return fileCount > 0;
}

// ===== AUDIO SYSTEM MANAGEMENT =====

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

void safeStopPlayback() {
  if (playerState == SWITCHING)
    return;

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

bool playFileRobust(int index, int maxRetries = 3) {
  if (index < 0 || index >= fileCount) {
    sendOutputln("Invalid file index");
    return false;
  }

  uint32_t startTime = millis();
  if (startTime - lastSwitchTime < 100) {
    sendOutputln("Switch too fast, skipping");
    return false;
  }
  lastSwitchTime = startTime;

  safeStopPlayback();

  AudioFile &audioFile = audioFiles[index];
  sendOutput("Loading [");
  sendOutput(String(index + 1));
  sendOutput("]: ");
  sendOutputln(audioFile.path);

  int retryCount = 0;
  while (retryCount < maxRetries) {
    if (!source->open(audioFile.path)) {
      sendOutput("File open failed, retry ");
      sendOutputln(String(retryCount + 1));
      retryCount++;
      delay(100);
      continue;
    }

    bool success = false;
    if (audioFile.isMP3) {
      success = mp3->begin(source, output);
    } else {
      success = wav->begin(source, output);
    }

    if (success) {
      currentFileIndex = index;
      playerState = PLAYING;
      sendOutputln("Playback started");
      return true;
    }

    sendOutput("Generator failed, retry ");
    sendOutputln(String(retryCount + 1));
    source->close();
    retryCount++;
    delay(100);
  }

  playerState = ERROR_STATE;
  sendOutputln("File load failed completely");
  return false;
}

void playNext() {
  if (fileCount == 0)
    return;

  int nextIndex = (currentFileIndex + 1) % fileCount;

  if (!playFileRobust(nextIndex)) {
    sendOutputln("Next song failed, stopping playback");
    playerState = STOPPED;
  }
}

void playPrevious() {
  if (fileCount == 0)
    return;

  int prevIndex = (currentFileIndex - 1 + fileCount) % fileCount;

  if (!playFileRobust(prevIndex)) {
    sendOutputln("Previous song failed, staying on current");
  }
}

// ===== BLE INITIALIZATION =====

void initBLE() {
  serial.println("Initializing BLE...");

  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(BLE_MTU_SIZE);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
      CHAR_UUID_TX,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
      CHAR_UUID_RX, BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x12);

  BLEDevice::startAdvertising();

  serial.println("BLE Ready - Waiting for connection...");
  serial.print("Device name: ");
  serial.println(BLE_DEVICE_NAME);
}

// ===== SYSTEM INITIALIZATION =====

void setup() {
  serial.begin(115200);
  delay(1000);

  serial.println("\n\n=== ESP32 Audio Player with BLE ===");

  // Initialize BLE FIRST
  initBLE();

  randomSeed(analogRead(0) + millis());
  SPI.begin();

  // Initialize SD card
  serial.println("Initializing SD card...");
  for (int retry = 0; retry < 5; retry++) {
    if (SD.begin(SD_CS)) {
      serial.println("SD Card ready");
      break;
    }
    serial.print("SD retry ");
    serial.println(retry + 1);
    delay(1000);

    if (retry == 4) {
      serial.println("SD Card failed completely");
      while (1)
        delay(1000);
    }
  }

  bool cacheLoaded = loadPlaylistCache();

  if (!cacheLoaded) {
    serial.println("Performing full SD scan...");
    if (!scanSDCard() || fileCount == 0) {
      serial.println("No audio files found");
      while (1)
        delay(1000);
    }
    savePlaylistCache();
  }

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_PLAY, INPUT_PULLUP);
  pinMode(BTN_PAUSE, INPUT_PULLUP);

  if (!initializeAudioSystem()) {
    serial.println("Audio system initialization failed");
    while (1)
      delay(1000);
  }

  serial.println("System ready!");
  serial.println("Type 'h' for help");

  delay(500);
  if (fileCount > 0) {
    playFileRobust(4);
  }
}

// ===== INPUT HANDLING =====

void handleButtons() {
  static const int btnPins[] = {BTN_UP, BTN_DOWN, BTN_PLAY, BTN_PAUSE};
  uint32_t now = millis();

  for (int i = 0; i < 4; i++) {
    bool currentPressed = !digitalRead(btnPins[i]);

    if (currentPressed && !buttons[i].wasPressed &&
        (now - buttons[i].lastPress > 200)) {

      buttons[i].lastPress = now;

      switch (i) {
      case 0:
        playPrevious();
        break;
      case 1:
        playNext();
        break;
      case 2:
        playFileRobust(currentFileIndex);
        break;
      case 3:
        safeStopPlayback();
        break;
      }
    }

    buttons[i].wasPressed = currentPressed;
  }
}

// ===== MAIN LOOP =====

void loop() {
  static uint32_t lastHealthCheck = 0;
  uint32_t now = millis();

  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    serial.println("Restarting BLE advertising");
    oldDeviceConnected = deviceConnected;
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  handleButtons();

  if (playerState == PLAYING) {
    bool isPlaying = false;

    if (audioFiles[currentFileIndex].isMP3 && mp3) {
      isPlaying = mp3->isRunning() && mp3->loop();
    } else if (!audioFiles[currentFileIndex].isMP3 && wav) {
      isPlaying = wav->isRunning() && wav->loop();
    }

    if (!isPlaying) {
      sendOutputln("Song ended, playing next");
      playNext();
    }
  }

  if (now - lastHealthCheck > 10000) {
    if (playerState == ERROR_STATE) {
      sendOutputln("Recovering from error state");
      playerState = STOPPED;
      delay(500);
    }
    lastHealthCheck = now;
  }

  if (serial.available()) {
    String input = serial.readStringUntil('\n');
    input.trim();

    if (input.length() > 0) {
      char cmd = input[0];
      String arg = input.substring(1);
      arg.trim();
      handleCommand(cmd, arg, false);
    }

    while (serial.available())
      serial.read();
  }

  yield();
}

void handleCommand(char cmd, const String &arg, bool fromBLE) {
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
    sendOutputln("Stopped");
    break;

  case 'r':
  case 'R':
    playFileRobust(currentFileIndex);
    break;

  case 'c':
  case 'C':
    sendOutputln("Rescanning SD card...");
    if (scanSDCard()) {
      savePlaylistCache();
      sendOutputln("Cache updated");
    }
    break;

  case 'l':
  case 'L': {
    sendOutputln("\n=== Playlist ===");
    int displayCount = min(fileCount, 30);
    for (int i = 0; i < displayCount; i++) {
      String line = "";
      line += (i == currentFileIndex ? "> " : "  ");
      line += String(i + 1);
      line += ". ";
      line += audioFiles[i].path;
      sendOutputln(line);
    }
    if (fileCount > 30) {
      String more = "... and ";
      more += String(fileCount - 30);
      more += " more";
      sendOutputln(more);
    }
    sendOutputln("");
    break;
  }

  case '+':
    currentGain = min(1.0f, currentGain + 0.1f);
    if (output)
      output->SetGain(currentGain);
    sendOutput("Volume: ");
    sendOutputln(String((int)(currentGain * 100)));
    break;

  case '-':
    currentGain = max(0.0f, currentGain - 0.1f);
    if (output)
      output->SetGain(currentGain);
    sendOutput("Volume: ");
    sendOutputln(String((int)(currentGain * 100)));
    break;

  case 'g':
    if (arg.length() > 0) {
      int targetIndex = arg.toInt() - 1;
      if (targetIndex >= 0 && targetIndex < fileCount) {
        playFileRobust(targetIndex);
      } else {
        sendOutputln("Invalid track number");
      }
    }
    break;

  case 'i': {
    sendOutputln("\n=== Status ===");
    sendOutput("State: ");
    sendOutputln(playerState == PLAYING     ? "PLAYING"
                 : playerState == STOPPED   ? "STOPPED"
                 : playerState == SWITCHING ? "SWITCHING"
                                            : "ERROR");
    sendOutput("Track: ");
    sendOutput(String(currentFileIndex + 1));
    sendOutput("/");
    sendOutputln(String(fileCount));
    sendOutput("File: ");
    sendOutputln(audioFiles[currentFileIndex].path);
    sendOutput("Volume: ");
    sendOutput(String((int)(currentGain * 100)));
    sendOutputln("%");
    sendOutput("BLE: ");
    sendOutputln(deviceConnected ? "Connected" : "Disconnected");
    sendOutputln("");
    break;
  }

  case 'h':
  case 'H':
  case '?':
    sendOutputln("\n=== Commands ===");
    sendOutputln("n - Next track");
    sendOutputln("p - Previous track");
    sendOutputln("s - Stop playback");
    sendOutputln("r - Restart current track");
    sendOutputln("l - List tracks");
    sendOutputln("c - Rescan SD & update cache");
    sendOutputln("+ - Volume up");
    sendOutputln("- - Volume down");
    sendOutputln("g<num> - Go to track (e.g., g5)");
    sendOutputln("i - Show info");
    sendOutputln("h - Show this help");
    sendOutputln("");
    break;

  default:
    sendOutput("Unknown command: ");
    sendOutputln(String(cmd));
    sendOutputln("Type 'h' for help");
    break;
  }
}
