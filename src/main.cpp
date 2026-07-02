#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <esp_sleep.h>
#include <esp_pm.h>
#ifdef ESP_IDF_VERSION_MAJOR
    #include "esp_wifi.h"
    #include "esp_err.h"
#endif
#include "WebServer.h"
#include "Scale.h"
#include "WiFiManager.h"
#include "FlowRate.h"
#include "Calibration.h"
#include "BluetoothScale.h"
#include "TouchSensor.h"
#include "BoardConfig.h"
#include "PowerManager.h"
#include "Display.h"
#include "BatteryMonitor.h"
#include "Version.h"

// Board-specific pin configuration
uint8_t dataPin = HX711_DATA_PIN;     // HX711 Data pin
uint8_t clockPin = HX711_CLOCK_PIN;   // HX711 Clock pin  
uint8_t touchPin = TOUCH_TARE_PIN;    // Touch sensor for tare
uint8_t sleepTouchPin = TOUCH_SLEEP_PIN;  // Touch sensor for sleep functionality
uint8_t batteryPin = BATTERY_PIN;     // Battery voltage monitoring
uint8_t sdaPin = I2C_SDA_PIN;         // I2C Data pin for display
uint8_t sclPin = I2C_SCL_PIN;         // I2C Clock pin for display
uint8_t tarePower = TOUCH_TARE_POWER_PIN;
uint8_t scalesPower = SCALES_POWER_PIN;
uint8_t oledPower = OLED_POWER_PIN;

enum wifi_state {
    kInit,
    kInitWait,
    kStarting,
    kStartWait,
    kWaiting,
    kConnected,
    kActive,
    kDisabled,
};

wifi_state wifiState = kInit;

#if defined(BOARD_TYPE_XIAOC6)
    uint8_t antennaPower = ANTENNA_POWER_PIN;
    uint8_t antennaSelect = ANTENNA_SELECT_PIN;
#else
  uint8_t auxPin = AUX_PIN;
#endif

float calibrationFactor = 4195.712891;
Scale scale(dataPin, clockPin, calibrationFactor);
FlowRate flowRate;
BluetoothScale bluetoothScale;
TouchSensor touchSensor(touchPin, &scale);
Display oledDisplay(sdaPin, sclPin, &scale, &flowRate);
PowerManager powerManager(sleepTouchPin, clockPin, &oledDisplay);
BatteryMonitor batteryMonitor(batteryPin);

void setup() {
  unsigned long totalStartupTime = 0;
  unsigned long startTime = 0;

  pinMode(tarePower, OUTPUT);
  digitalWrite(tarePower, HIGH);
  pinMode(scalesPower, OUTPUT);
  digitalWrite(scalesPower, HIGH);
  pinMode(oledPower, OUTPUT);
  digitalWrite(oledPower, HIGH);
  #if defined(BOARD_TYPE_XIAOC6)
    pinMode(antennaPower, OUTPUT);
    digitalWrite(antennaPower, LOW);  // LOW to turn on
    pinMode(antennaSelect, OUTPUT);
    digitalWrite(antennaSelect, LOW); // LOW internal antenna, HIGH external antenna
  #else
    pinMode(auxPin, OUTPUT);
    digitalWrite(auxPin, HIGH);
  #endif


  gpio_hold_dis((gpio_num_t) clockPin);

  Serial.begin(115200);

  /*
  // Debugging
  int serialCount = 0;
  while(!Serial) {
    delay(100); // Wait for serial port to be available
    serialCount++;
    if (serialCount >= 20) { // Wait up to 2 seconds
      Serial.println("Serial port not available - continuing without serial output");
      break;
    }
  }
  delay(1000);*/

  // Set CPU frequency explicitly for power optimization
  #if defined(BOARD_TYPE_XIAOC6)
    //setCpuFrequencyMhz(80); // Reduce CPU frequency to 80MHz for better battery life    
  esp_pm_config_t pm_config = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true
  };
  esp_pm_configure(&pm_config);
  #else
    setCpuFrequencyMhz(80); // Reduce CPU frequency to 80MHz for better battery life    
  #endif
  
  // Link scale and flow rate for tare operation coordination
  scale.setFlowRatePtr(&flowRate);

  // Check for factory reset request (hold touch pin during boot)
  pinMode(touchPin, INPUT_PULLDOWN);
  if (digitalRead(touchPin) == HIGH) {
    Serial.println("FACTORY RESET: Touch pin held during boot - clearing WiFi credentials");
    clearWiFiCredentials();
    delay(1000);
  }

  Serial.printf("CPU frequency set to: %dMHz for power optimization\n", getCpuFrequencyMhz());
  // Version and board identification
  Serial.println("=================================");
  Serial.printf("WeighMyBru² v%s\n", WEIGHMYBRU_VERSION_STRING);
  Serial.printf("Board: %s\n", WEIGHMYBRU_BOARD_NAME);
  Serial.printf("Build: %s %s\n", WEIGHMYBRU_BUILD_DATE, WEIGHMYBRU_BUILD_TIME);
  Serial.printf("Full Version: %s\n", WEIGHMYBRU_FULL_VERSION);
  Serial.printf("Flash Size: %dMB\n", FLASH_SIZE_MB);
  Serial.printf("CPU Frequency: %dMHz (Power Optimized)\n", getCpuFrequencyMhz());
  Serial.println("=================================");
  
  // CRITICAL: Initialize BLE FIRST before WiFi to prevent radio conflicts
  Serial.println("Initializing BLE FIRST for GaggiMate compatibility...");
  Serial.printf("Free heap before BLE init: %u bytes\n", ESP.getFreeHeap());
  #ifdef BOARD_HAS_PSRAM
    Serial.printf("Free PSRAM before BLE init: %u bytes\n", ESP.getFreePsram());
  #endif

  Serial.printf("Timestamp - Before Display: %lums, Total %lums\n",millis() - startTime, millis() - totalStartupTime);
  startTime = millis();

  // start scale while it takes time to start other devices
  scale.begin();
    
  // Initialize display with error handling - don't block if display fails
  Serial.println("Initializing display...");
  bool displayAvailable = oledDisplay.begin();
  
  if (!displayAvailable) {
    Serial.println("WARNING: Display initialization failed!");
    Serial.println("System will continue in headless mode without display.");
    Serial.println("All functionality remains available via web interface.");
  } else {
    Serial.println("Display initialized - ready for visual feedback");
    // Set reduced brightness for power optimization
    oledDisplay.setBrightness(1);  // 50% brightness vs 255 max
    Serial.println("Display brightness set to 50% for power optimization");
  }

  Serial.printf("Timestamp - After Display: %lums, Total %lums\n",millis() - startTime, millis() - totalStartupTime);
  startTime = millis();

  try {
    //#if !defined(BOARD_TYPE_XIAOC6)
        
      bluetoothScale.begin();  // Initialize BLE without scale reference
      Serial.println("BLE initialized successfully - GaggiMate should be able to connect");
    //#endif
    #ifdef BOARD_HAS_PSRAM
      Serial.printf("Free PSRAM after BLE init: %u bytes\n", ESP.getFreePsram());
    #endif
  } catch (...) {
    Serial.println("BLE initialization failed - continuing without Bluetooth");
    Serial.printf("Free heap after BLE fail: %u bytes\n", ESP.getFreeHeap());
  }
  
  Serial.printf("Timestamp - After Bluetooth: %lums, Total %lums\n",millis() - startTime, millis() - totalStartupTime);
  startTime = millis();
  
  // Check wake-up reason and show appropriate message
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wakeup caused by external signal (touch sensor)");
      // Show the same starting message as normal boot for consistency
      //delay(1500);
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("Wakeup caused by external signal using RTC_CNTL");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wakeup caused by timer");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      Serial.println("Wakeup caused by touchpad");
      break;
    default:
      Serial.println("Wakeup was not caused by deep sleep: " + String(wakeup_reason));
      // For normal startup, the begin() method already shows a startup message
      //delay(1000);
      break;
  }
  //Wait for BLE to finish intitalizing before starting WiFi
  delay(100); //1500); 

  // Initialize scale with error handling - don't block web server if HX711 fails
  Serial.println("Initializing scale...");

  if (!scale.init()) {
    Serial.println("WARNING: Scale (HX711) initialization failed!");
    Serial.println("Web server will continue to run, but scale readings will not be available.");
    Serial.println("Check HX711 wiring and connections.");
  } else {
    Serial.println("Scale initialized successfully");
    // Now that scale is ready, set the reference in BluetoothScale
    bluetoothScale.setScale(&scale);
  }

  Serial.printf("Timestamp - After Scale: %lums, Total %lums\n",millis() - startTime, millis() - totalStartupTime);
  startTime = millis();
  
  // BLE was initialized earlier - no need to initialize again
  // bluetoothScale.begin(&scale);
  
  // Set bluetooth reference in display for status indicator (if display available)
  if (oledDisplay.isConnected()) {
    oledDisplay.setBluetoothScale(&bluetoothScale);
  }
  
  // Set display reference in bluetooth for timer control
  bluetoothScale.setDisplay(&oledDisplay);
  
  // Set power manager reference in display for timer state synchronization (if display available)
  if (oledDisplay.isConnected()) {
    oledDisplay.setPowerManager(&powerManager);
  }
  
  // Set battery monitor reference in display for battery status (if display available)
  if (oledDisplay.isConnected()) {
    oledDisplay.setBatteryMonitor(&batteryMonitor);
  }

  // Initialize touch sensor
  touchSensor.begin();

  // Initialize power manager
  powerManager.begin();

  // Initialize battery monitor
  batteryMonitor.begin();

  // Check for low battery - prevent boot if voltage too low
  float batteryVoltage = batteryMonitor.getBatteryVoltage();
  /*if (batteryVoltage < 3.2f && batteryVoltage > 0.1f) { // > 0.1f to avoid false readings
    Serial.printf("CRITICAL: Battery voltage too low (%.2fV) - entering sleep\n", batteryVoltage);
    
    // Show battery low message on display with large, centered formatting
    if (oledDisplay.isConnected()) {
      oledDisplay.showBatteryLowMessage(batteryVoltage, 3000);
    }
    
    delay(3000); // Show message for 3 seconds
    
    // Force clear any display state and sleep immediately
    if (oledDisplay.isConnected()) {
      oledDisplay.clear();
    }
    
    Serial.println("Forcing deep sleep now...");
    
    powerManager.enterDeepSleep();
  }*/
  
  Serial.printf("Battery voltage OK (%.2fV) - continuing boot\n", batteryVoltage);

  // Show IP addresses and welcome message if display is available
  delay(100); // Small delay to ensure WiFi is fully initialized
  if (oledDisplay.isConnected()) {
    oledDisplay.showIPAddresses();
  }

  // Link display to touch sensor for tare feedback (if display available)
  if (oledDisplay.isConnected()) {
    touchSensor.setDisplay(&oledDisplay);
  }
  
  // Link flow rate to touch sensor for averaging reset on tare
  touchSensor.setFlowRate(&flowRate);

  //bluetoothScale.end();

  Serial.printf("Timestamp - End: %lums, Total %lums\n",millis() - startTime, millis() - totalStartupTime);
}

void initWifi() {
  static unsigned long wifiTime = 0;

  switch(wifiState) {
    case kInit:
      // Initialize WiFi power management BEFORE any WiFi operations
      Serial.println("Initializing WiFi power management...");
      
      // CRITICAL: Force WiFi completely off first to ensure clean state
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      wifiState = kStarting;
      wifiTime = millis();
      return;
    case kInitWait: 
      if(millis() - wifiTime > 1000) {
        wifiState = kStarting;
      }
      return;
    case kStarting:
      // ALWAYS enable WiFi power management for optimal battery life
      WiFi.setSleep(true);
      Serial.println("WiFi power management enabled for battery optimization");

      // CRITICAL: Always setup WiFi first (like tare button scenario)
      // This ensures all WiFi subsystems are properly initialized
      // Then disable it cleanly if needed (replicating tare button sequence)
      Serial.println("FORCING WiFi initialization to replicate tare button scenario...");
      setupWiFiNonBlocking(); // Use forced setup to bypass state checks
      wifiState = kStartWait;
      return;
    case kStartWait:
      // Wait for WiFi to fully stabilize after BLE is already running
      
      if(setupWiFiNonBlocking()) {
          wifiState = kConnected;
      }
      return;
    case kConnected:
      Serial.printf("Version: %s\n", ESP.getSdkVersion());
      setupWebServer(scale, flowRate, bluetoothScale, oledDisplay, batteryMonitor);
        // CRITICAL: After full initialization, check if WiFi should be disabled
      // This exactly replicates the tare button scenario: WiFi started, then disabled
      Serial.println("=== POST-INITIALIZATION WiFi STATE CHECK ===");
      if (!loadWiFiEnabledState()) {
        Serial.println("WiFi should be disabled - applying clean shutdown like tare button");
        Serial.println("(WiFi was initialized first, now disabling cleanly)");
        
        // Small delay to ensure all systems are stable (like tare button timing)
        delay(100);
        
        // Now call disableWiFi() exactly like tare button does
        disableWiFi();
        
        Serial.println("WiFi cleanly disabled - 0.05A power consumption expected");
        wifiState = kDisabled;
        bluetoothScale.end();
        delay(100);
        setCpuFrequencyMhz(10); // ESP plus 1 touch sensor: 10MHz 13.6mA, 20MHz 14.6mA, 40MHz 17.8mA, 80MHz 26.4mA, 160MHz 31.3mA, 240MHz 37.1mA - reduce CPU frequency for power optimization when WiFi and bluetooth are off
        Serial.printf("CPU frequency set to: %dMHz for power optimization\n", getCpuFrequencyMhz());
      } 
      else {
        Serial.println("WiFi should remain enabled - no action needed");
        wifiState = kActive;
      }
      return;
    case kActive:
      return;
    case kDisabled:
      return;
  }
}

void loop() {
  static unsigned long lastWeightUpdate = 0;
  static unsigned long lastWiFiCheck = 0;
  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastDisplayBrightnessUpdate = 0;
  static int displayBrightness = 1;
  
  // Update weight at reduced frequency for power optimization
  if (millis() - lastWeightUpdate >= 50) { // Reduced from 20ms to 50ms (20Hz from 50Hz)
    float weight = scale.getWeight();
    flowRate.update(weight);
    lastWeightUpdate = millis();
    //displayBrightness++;
    //if (displayBrightness >= 256) { // Every 20 weight updates (1 second at 50Hz)
    //  displayBrightness = 0;
    //}
    //oledDisplay.setBrightness(displayBrightness);
  }

  /*if (millis() - lastDisplayBrightnessUpdate >= 2000) {
    lastDisplayBrightnessUpdate = millis();
    if(displayBrightness == 1) {
      displayBrightness = 255;
    } 
    else if(displayBrightness == 255) {
      displayBrightness = 128;
    }
    else {
      displayBrightness = 1;
    }
    
    oledDisplay.setBrightness(displayBrightness);
  }*/
  
  static unsigned long lastBLEUpdate = 0;
  
  if(wifiState < kActive) {
    initWifi();
  }
  else {
    // Check WiFi status every 30 seconds for debugging
    if (millis() - lastWiFiCheck >= 30000) {
      printWiFiStatus();
      lastWiFiCheck = millis();
    }
    
    // Maintain WiFi AP stability
    maintainWiFi();
  }
  
  // Update Bluetooth less frequently to reduce BLE interference and power usage
  if (millis() - lastBLEUpdate >= 100) { // Reduced from 50ms to 100ms (10Hz from 20Hz)
    bluetoothScale.update();
    lastBLEUpdate = millis();
  }
  
  // Update touch sensor
  touchSensor.update();
  
  // Update power manager
  powerManager.update();
  
  // Update battery monitor
  batteryMonitor.update();
  
  // Update display less frequently for power saving
  if (millis() - lastDisplayUpdate >= 100) { // Reduced display refresh rate to 10Hz
    if (millis() - lastDisplayUpdate > 150) { // if refresh was delayed significantly keep a gap for next update, else keep 100ms intervals
      lastDisplayUpdate = millis();
    }
    else {
      lastDisplayUpdate += 100;
    }

    oledDisplay.update();
  }
  
  // Increased delay for better power efficiency while maintaining responsiveness
    delay(10); // Optimized delay: 10ms for good responsiveness with power savings
}
