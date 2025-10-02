#pragma once

/* ---------- SD ---------- */
#define SD_CS 5
#define CACHE_FILE "/playlist.cache"
#define MAX_FILES 300

/* ---------- BLE ---------- */
#define BLE_DEVICE_NAME "ESP32_AudioPlayer"
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UUID_RX "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_UUID_TX "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define BLE_MTU_SIZE 512

/* ---------- Buttons ---------- */
#define BTN_UP 32
#define BTN_DOWN 33
#define BTN_PLAY 25
#define BTN_PAUSE 26
