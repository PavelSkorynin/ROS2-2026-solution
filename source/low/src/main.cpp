#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Nordic UART Service UUIDs (common and easy to test against)
static const char *kDeviceName = "esp32dev";
static const char *kServiceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *kRxUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // Write
static const char *kTxUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // Notify

BLECharacteristic *g_txCharacteristic = nullptr;
BLEAdvertising *g_advertising = nullptr;
bool g_deviceConnected = false;
uint32_t g_lastAdvRestartMs = 0;

// DC motors JGA25-370 via L298N
// Left motor
static const int kLeftEnPin = 12;  // PWM
static const int kLeftAPin = 14;   // IN1
static const int kLeftBPin = 13;   // IN2
// Right motor
static const int kRightEnPin = 2;  // PWM
static const int kRightAPin = 4;   // IN3
static const int kRightBPin = 15;  // IN4

// PWM configuration (ESP32 LEDC)
static const int kLeftPwmChannel = 0;
static const int kRightPwmChannel = 1;
static const int kPwmFreq = 20000;       // 20 kHz to avoid audible noise
static const int kPwmResolution = 8;     // 0..255

static const bool kRightInverted = true;

struct MotorCommand {
  int8_t left = 0;
  int8_t right = 0;
  uint32_t end_ms = 0;
  bool active = false;
};

MotorCommand g_cmd;

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

static uint8_t powerToPwm(int8_t power) {
  if (power == 0) {
    return 0;
  }
  int p = clampAbsPower(power);
  int pwm = (p * 255) / 127;
  if (pwm > 255) {
    pwm = 255;
  }
  return static_cast<uint8_t>(pwm);
}

static int8_t applyRightInversion(int8_t power) {
  return kRightInverted ? static_cast<int8_t>(-power) : power;
}

static void applyMotor(int pwmChannel, int aPin, int bPin, int8_t power) {
  uint8_t pwm = powerToPwm(power);
  if (power == 0 || pwm == 0) {
    // Brake / freewheel: no PWM, both direction pins LOW
    ledcWrite(pwmChannel, 0);
    digitalWrite(aPin, LOW);
    digitalWrite(bPin, LOW);
    return;
  }

  if (power > 0) {
    digitalWrite(aPin, HIGH);
    digitalWrite(bPin, LOW);
  } else {
    digitalWrite(aPin, LOW);
    digitalWrite(bPin, HIGH);
  }
  ledcWrite(pwmChannel, pwm);
}

static void applyLeftMotor(int8_t power) {
  applyMotor(kLeftPwmChannel, kLeftAPin, kLeftBPin, power);
}

static void applyRightMotor(int8_t power) {
  applyMotor(kRightPwmChannel, kRightAPin, kRightBPin, power);
}

static void motorSelfTest() {
  Serial.println("Motor self-test start");

  const int8_t testPower = 80;

  // Left motor: forward then backward
  applyLeftMotor(testPower);
  delay(1000);
  applyLeftMotor(-testPower);
  delay(1000);
  applyLeftMotor(0);
  delay(200);

  // Right motor: forward then backward
  applyRightMotor(testPower);
  delay(1000);
  applyRightMotor(-testPower);
  delay(1000);
  applyRightMotor(0);
  delay(200);

  Serial.println("Motor self-test done");
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

  // Motor pins
  pinMode(kLeftAPin, OUTPUT);
  pinMode(kLeftBPin, OUTPUT);
  pinMode(kRightAPin, OUTPUT);
  pinMode(kRightBPin, OUTPUT);

  // Configure PWM for motor enable pins
  ledcSetup(kLeftPwmChannel, kPwmFreq, kPwmResolution);
  ledcAttachPin(kLeftEnPin, kLeftPwmChannel);
  ledcSetup(kRightPwmChannel, kPwmFreq, kPwmResolution);
  ledcAttachPin(kRightEnPin, kRightPwmChannel);

  // Ensure motors are stopped initially
  applyLeftMotor(0);
  applyRightMotor(0);

  // Motor self-test: spin each motor in both directions for one second
  motorSelfTest();

  // Stop motors after self-test
  applyLeftMotor(0);
  applyRightMotor(0);

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
}

void loop() {
  if (g_cmdPending) {
    int8_t left = 0;
    int8_t right = 0;
    uint16_t time_ms = 0;
    portENTER_CRITICAL(&g_cmdMux);
    left = g_pendingLeft;
    right = applyRightInversion(g_pendingRight);
    time_ms = g_pendingTimeMs;
    g_cmdPending = false;
    portEXIT_CRITICAL(&g_cmdMux);

    g_cmd.left = left;
    g_cmd.right = right;
    g_cmd.end_ms = millis() + time_ms;
    g_cmd.active = (time_ms > 0) && (left != 0 || right != 0);

    uint8_t left_pwm = powerToPwm(left);
    uint8_t right_pwm = powerToPwm(right);
    Serial.printf("left_pwm=%u right_pwm=%u\n",
                  static_cast<unsigned>(left_pwm),
                  static_cast<unsigned>(right_pwm));
    applyLeftMotor(left);
    applyRightMotor(right);
  }

  if (g_cmd.active && millis() >= g_cmd.end_ms) {
    g_cmd.active = false;
    applyLeftMotor(0);
    applyRightMotor(0);
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
