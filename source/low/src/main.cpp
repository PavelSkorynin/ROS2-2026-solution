#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <FastAccelStepper.h>

// Nordic UART Service UUIDs (common and easy to test against)
static const char *kDeviceName = "esp32dev";
static const char *kServiceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *kRxUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // Write
static const char *kTxUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // Notify

BLECharacteristic *g_txCharacteristic = nullptr;
BLEAdvertising *g_advertising = nullptr;
bool g_deviceConnected = false;
uint32_t g_lastAdvRestartMs = 0;

// A4988 pins
static const int kLeftEnPin = 25;
static const int kLeftStepPin = 26;
static const int kLeftDirPin = 27;
static const int kRightEnPin = 32;
static const int kRightStepPin = 33;
static const int kRightDirPin = 14;

static const int kMaxStepHz = 50; // max step frequency at power=127
static const int kMinStepHz = 10;  // minimum frequency to start moving
static const int kAccelHzPerSec = 2000; // ramp rate
static const bool kStartupTest = true;
static const int kTestSteps = 200;
static const int kTestSpeedHz = 200;
static const int kTestAccelHzPerSec = 800;

struct MotorCommand {
  int8_t left = 0;
  int8_t right = 0;
  uint32_t end_ms = 0;
  bool active = false;
};

MotorCommand g_cmd;

FastAccelStepperEngine g_engine;
FastAccelStepper *g_leftStepper = nullptr;
FastAccelStepper *g_rightStepper = nullptr;

portMUX_TYPE g_cmdMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_cmdPending = false;
volatile int8_t g_pendingLeft = 0;
volatile int8_t g_pendingRight = 0;
volatile uint16_t g_pendingTimeMs = 0;

static int clampAbsPower(int8_t power) {
  int p = abs(static_cast<int>(power));
  if (p > 127) {
    p = 127;
  }
  return p;
}

static int powerToFreq(int8_t power) {
  if (power == 0) {
    return 0;
  }
  int p = clampAbsPower(power);
  int freq = (p * kMaxStepHz) / 127;
  if (freq < kMinStepHz) {
    freq = kMinStepHz;
  }
  return freq;
}

static void applyStepper(FastAccelStepper *stepper, int8_t power, int freq) {
  if (stepper == nullptr) {
    return;
  }
  if (power == 0 || freq == 0) {
    stepper->forceStopAndNewPosition(stepper->getCurrentPosition());
    return;
  }
  stepper->setSpeedInHz(freq);
  if (power > 0) {
    stepper->runForward();
  } else {
    stepper->runBackward();
  }
}

static void runStartupTest(FastAccelStepper *stepper, const char *name) {
  if (stepper == nullptr) {
    return;
  }
  Serial.printf("Test %s: +%d steps\n", name, kTestSteps);
  stepper->setSpeedInHz(kTestSpeedHz);
  stepper->setAcceleration(kTestAccelHzPerSec);
  stepper->move(kTestSteps);
  while (stepper->isRunning()) {
    delay(1);
  }
  delay(200);
  Serial.printf("Test %s: -%d steps\n", name, kTestSteps);
  stepper->move(-kTestSteps);
  while (stepper->isRunning()) {
    delay(1);
  }
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    g_deviceConnected = true;
  }

  void onDisconnect(BLEServer *server) override {
    g_deviceConnected = false;
    if (g_advertising != nullptr) {
      g_advertising->start();
      g_lastAdvRestartMs = millis();
    }
    Serial.println("Disconnected, advertising restarted");
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    std::string value = characteristic->getValue();
    if (value.size() == 0) {
      return;
    }

    if (value.size() != 4) {
      Serial.printf("Invalid command size: %u\n", static_cast<unsigned>(value.size()));
      return;
    }

    int8_t left = static_cast<int8_t>(value[0]);
    int8_t right = static_cast<int8_t>(value[1]);
    uint16_t time_ms = static_cast<uint8_t>(value[2]) |
                       (static_cast<uint16_t>(static_cast<uint8_t>(value[3])) << 8);

    Serial.printf("cmd L=%d R=%d T=%u ms\n", left, right, time_ms);

    portENTER_CRITICAL(&g_cmdMux);
    g_pendingLeft = left;
    g_pendingRight = right;
    g_pendingTimeMs = time_ms;
    g_cmdPending = true;
    portEXIT_CRITICAL(&g_cmdMux);
  }
};

void setup() {
  Serial.begin(115200);

  BLEDevice::init(kDeviceName);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(kServiceUuid);

  g_txCharacteristic = service->createCharacteristic(
      kTxUuid, BLECharacteristic::PROPERTY_NOTIFY);
  g_txCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *rxCharacteristic = service->createCharacteristic(
      kRxUuid, BLECharacteristic::PROPERTY_WRITE |
                   BLECharacteristic::PROPERTY_WRITE_NR);
  rxCharacteristic->setCallbacks(new RxCallbacks());

  service->start();

  g_advertising = BLEDevice::getAdvertising();
  g_advertising->addServiceUUID(kServiceUuid);
  g_advertising->setScanResponse(false);
  g_advertising->setMinPreferred(0x06);
  g_advertising->setMinPreferred(0x12);
  g_advertising->start();
  g_lastAdvRestartMs = millis();

  Serial.println("BLE control service started");

  g_engine.init();
  g_leftStepper = g_engine.stepperConnectToPin(kLeftStepPin);
  g_rightStepper = g_engine.stepperConnectToPin(kRightStepPin);

  if (g_leftStepper != nullptr) {
    g_leftStepper->setDirectionPin(kLeftDirPin);
    g_leftStepper->setEnablePin(kLeftEnPin, true);
    g_leftStepper->setAutoEnable(true);
    g_leftStepper->setAcceleration(kAccelHzPerSec);
  }

  if (g_rightStepper != nullptr) {
    g_rightStepper->setDirectionPin(kRightDirPin);
    g_rightStepper->setEnablePin(kRightEnPin, true);
    g_rightStepper->setAutoEnable(true);
    g_rightStepper->setAcceleration(kAccelHzPerSec);
  }

  if (kStartupTest) {
    runStartupTest(g_leftStepper, "left");
    runStartupTest(g_rightStepper, "right");
  }
}

void loop() {
  if (g_cmdPending) {
    int8_t left = 0;
    int8_t right = 0;
    uint16_t time_ms = 0;
    portENTER_CRITICAL(&g_cmdMux);
    left = g_pendingLeft;
    right = g_pendingRight;
    time_ms = g_pendingTimeMs;
    g_cmdPending = false;
    portEXIT_CRITICAL(&g_cmdMux);

    g_cmd.left = left;
    g_cmd.right = right;
    g_cmd.end_ms = millis() + time_ms;
    g_cmd.active = (time_ms > 0) && (left != 0 || right != 0);

    int left_hz = powerToFreq(left);
    int right_hz = powerToFreq(right);
    Serial.printf("left_hz=%d right_hz=%d\n", left_hz, right_hz);
    applyStepper(g_leftStepper, left, left_hz);
    applyStepper(g_rightStepper, right, right_hz);
  }

  if (g_cmd.active && millis() >= g_cmd.end_ms) {
    g_cmd.active = false;
    applyStepper(g_leftStepper, 0, 0);
    applyStepper(g_rightStepper, 0, 0);
  }
  if (!g_deviceConnected && g_advertising != nullptr) {
    uint32_t now = millis();
    if (now - g_lastAdvRestartMs > 2000) {
      g_advertising->start();
      g_lastAdvRestartMs = now;
    }
  }
  delay(50);
}
