// Copy this file to src/app_config.h and fill in your values.
// src/app_config.h is in .gitignore so credentials never end up on GitHub.
#pragma once

#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-password"
#define BRIDGE_URL "http://192.168.1.50:5000/api/samples"
#define DEVICE_ID "m5-01"

// M5Stack Core: Port A (Grove, red) is SDA=21, SCL=22
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
