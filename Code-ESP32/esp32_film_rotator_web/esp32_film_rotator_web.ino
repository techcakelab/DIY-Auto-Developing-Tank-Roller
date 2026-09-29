#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// OLED Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// L298N Motor Driver Pins
#define ENA_PIN 26
#define IN1_PIN 27
#define IN2_PIN 16
#define PWM_CHANNEL 0

// Hardware Pins
#define BUTTON_PIN    13   // Start / Stop
#define BUTTON2_PIN   14   // Reset
#define ENC_SW_PIN    25   // KY-040 Push button -> toggle TIME <-> RPM
#define ENC_CLK_PIN   32   // KY-040 CLK
#define ENC_DT_PIN    33   // KY-040 DT
#define ENCODER_A_PIN 34   // Motor encoder channel A
#define ENCODER_B_PIN 35   // Motor encoder channel B

// -- Motor timing ----------------------------------------------------------
const unsigned long FWD_TIME_MS  = 10000;
const unsigned long REV_TIME_MS  = 10000;
const unsigned long PAUSE_TIME_MS = 1000;
const float ENCODER_PPR = 1980.0;

// -- PID -------------------------------------------------------------------
float Kp = 10.0, Ki = 5.0, Kd = 0.2;
float integral = 0, lastError = 0;
unsigned long lastPidTime = 0;
long lastEncoderCount = 0;
float currentRPM = 0;
int currentPWM = 0;

// -- Runtime variables -----------------------------------------------------
float TARGET_RPM = 55.0;
unsigned long TOTAL_TIME_MS = 180000UL; // default 3 minutes

// -- Motor encoder ISR -----------------------------------------------------
volatile long encoderCount = 0;
void IRAM_ATTR handleEncoder() {
  if (digitalRead(ENCODER_A_PIN) == digitalRead(ENCODER_B_PIN)) encoderCount++;
  else encoderCount--;
}

// -- KY-040 encoder ISR ----------------------------------------------------
volatile int userEncoderDelta = 0;
void IRAM_ATTR handleUserEncoder() {
  static bool lastClk = HIGH;
  bool clk = digitalRead(ENC_CLK_PIN);
  if (clk != lastClk) {
    if (clk == LOW) {
      if (digitalRead(ENC_DT_PIN) == HIGH) userEncoderDelta++;
      else userEncoderDelta--;
    }
  }
  lastClk = clk;
}

// -- Edit mode -------------------------------------------------------------
enum EditMode { EDIT_TIME, EDIT_RPM };
EditMode editMode = EDIT_TIME;

// -- State machine ---------------------------------------------------------
enum State { IDLE, RUNNING_FWD, PAUSING_FWD, RUNNING_REV, PAUSING_REV, FINISHED };
State currentState = IDLE;

unsigned long processStartTime = 0;
unsigned long stateStartTime = 0;
unsigned long remainingTimeMs = 0;

// -- Debounce --------------------------------------------------------------
bool lastBtnState   = HIGH;
bool lastBtn2State  = HIGH;
bool lastEncSwState = HIGH;
unsigned long lastBtnTime   = 0;
unsigned long lastBtn2Time  = 0;
unsigned long lastEncSwTime = 0;
const unsigned long DEBOUNCE_MS = 200;

// -- Motor helpers ---------------------------------------------------------
void setMotor(int speed, bool forward) {
  if (speed == 0) {
    ledcWrite(ENA_PIN, 0);
    digitalWrite(IN1_PIN, LOW);
    digitalWrite(IN2_PIN, LOW);
  } else {
    ledcWrite(ENA_PIN, speed);
    if (forward) {
      digitalWrite(IN1_PIN, HIGH);
      digitalWrite(IN2_PIN, LOW);
    } else {
      digitalWrite(IN1_PIN, LOW);
      digitalWrite(IN2_PIN, HIGH);
    }
  }
}

void resetPID() {
  integral = 0; lastError = 0; currentPWM = 0;
  noInterrupts(); lastEncoderCount = encoderCount; interrupts();
  lastPidTime = millis();
}

void applyPID(bool forward) {
  unsigned long now = millis();
  unsigned long dt = now - lastPidTime;
  if (dt >= 100) {
    noInterrupts(); long enc = encoderCount; interrupts();
    long delta = abs(enc - lastEncoderCount);
    lastEncoderCount = enc; lastPidTime = now;

    currentRPM = (delta / ENCODER_PPR) * (60000.0 / dt);
    float error = TARGET_RPM - currentRPM;
    integral += error * (dt / 1000.0);
    integral = constrain(integral, -200, 200);
    float derivative = (error - lastError) / (dt / 1000.0);
    lastError = error;

    float output = (Kp * error) + (Ki * integral) + (Kd * derivative);
    currentPWM += (int)output;
    currentPWM = constrain(currentPWM, 0, 4095);
    setMotor(currentPWM > 50 ? currentPWM : 0, forward);
  }
}

void startProcess() {
  currentState = RUNNING_FWD;
  processStartTime = millis();
  stateStartTime = millis();
  remainingTimeMs = TOTAL_TIME_MS;
  resetPID();
}

void stopProcess() {
  currentState = IDLE;
  setMotor(0, true);
  currentRPM = 0;
  integral = 0;
}

// -- OLED helpers ----------------------------------------------------------
void drawBar(int x, int y, int w, int h, float pct) {
  display.drawRect(x, y, w, h, SH110X_WHITE);
  int fill = (int)(constrain(pct, 0, 100) / 100.0 * (w - 2));
  if (fill > 0) display.fillRect(x + 1, y + 1, fill, h - 2, SH110X_WHITE);
}

void updateOLED() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  if (currentState == IDLE) {
    // -- Setup Screen ------------------------------------------------------
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("FILM ROTATOR  SETUP");
    display.drawLine(0, 9, 128, 9, SH110X_WHITE);

    // Time
    float timeMins = TOTAL_TIME_MS / 60000.0;
    uint8_t m = (uint8_t)timeMins;
    uint8_t s = (uint8_t)((timeMins - m) * 60);

    display.setTextSize(1);
    display.setCursor(0, 13);
    if (editMode == EDIT_TIME) display.print("> TIME:");
    else                       display.print("  TIME:");

    display.setTextSize(2);
    display.setCursor(56, 11);
    display.printf("%02d:%02d", m, s);

    // RPM
    display.setTextSize(1);
    display.setCursor(0, 32);
    if (editMode == EDIT_RPM) display.print("> RPM: ");
    else                      display.print("  RPM: ");

    display.setTextSize(2);
    display.setCursor(56, 30);
    display.printf("%3d", (int)TARGET_RPM);

    // Hint
    display.setTextSize(1);
    display.setCursor(0, 55);
    display.print("SW:switch  BTN1:START");

  } else if (currentState == FINISHED) {
    // -- Finished Screen ---------------------------------------------------
    display.setTextSize(2);
    display.setCursor(10, 16);
    display.print("FINISHED!");
    display.setTextSize(1);
    display.setCursor(10, 50);
    display.print("BTN1 = restart");

  } else {
    // -- Running Screen ----------------------------------------------------
    const char* dirStr = "PAUSE";
    if (currentState == RUNNING_FWD) dirStr = "FWD >>>";
    else if (currentState == RUNNING_REV) dirStr = "<<< REV";

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(dirStr);
    display.drawLine(0, 9, 128, 9, SH110X_WHITE);

    // Countdown
    display.setTextSize(2);
    display.setCursor(0, 12);
    uint8_t m = (remainingTimeMs / 1000) / 60;
    uint8_t s = (remainingTimeMs / 1000) % 60;
    display.printf("%02d:%02d", m, s);

    // Progress bar
    float prog = (float)(TOTAL_TIME_MS - remainingTimeMs) / TOTAL_TIME_MS * 100.0;
    drawBar(0, 34, 128, 6, prog);

    // RPM (actual / target)
    display.setTextSize(1);
    display.setCursor(0, 44);
    display.printf("%.1f / %d RPM", currentRPM, (int)TARGET_RPM);

    display.setCursor(0, 55);
    display.print("BTN1=STOP  BTN2=edit RPM");
  }

  display.display();
}

// -------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN,    INPUT_PULLUP);
  pinMode(BUTTON2_PIN,   INPUT_PULLUP);
  pinMode(ENC_SW_PIN,    INPUT_PULLUP);
  pinMode(ENC_CLK_PIN,   INPUT_PULLUP);
  pinMode(ENC_DT_PIN,    INPUT_PULLUP);
  pinMode(ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ENCODER_B_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(ENCODER_A_PIN), handleEncoder,    CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK_PIN),   handleUserEncoder, CHANGE);

  Wire.begin();
  if (!display.begin(OLED_ADDRESS, true)) {
    Serial.println("[ERROR] OLED display not found on I2C (address 0x3C)!");
    Serial.println("Please check SDA/SCL wiring and display power supply.");
  } else {
    Serial.println("[OK] OLED display found and initialized successfully.");
  }

  pinMode(IN1_PIN, OUTPUT);
  pinMode(IN2_PIN, OUTPUT);
  
  // Setup PWM for ENA (ESP32 Core 3.x style)
  ledcAttach(ENA_PIN, 1000, 12); // 1 kHz, 12-bit resolution (0-4095)

  setMotor(0, true);

  updateOLED();
}

void loop() {
  unsigned long now = millis();

  // -- Read KY-040 encoder -----------------------------------------------
  noInterrupts(); int delta = userEncoderDelta; userEncoderDelta = 0; interrupts();
  if (delta != 0) {
    if (editMode == EDIT_TIME) {
      // Adjust by 15 seconds (0.25 minutes)
      float mins = TOTAL_TIME_MS / 60000.0 + delta * 0.25;
      mins = constrain(mins, 0.5, 30.0);
      TOTAL_TIME_MS = (unsigned long)(mins * 60000.0);
    } else {
      TARGET_RPM += delta;
      TARGET_RPM = constrain(TARGET_RPM, 10, 100);
    }
  }

  // -- KY-040 SW Button: toggle TIME <-> RPM mode ------------------------
  bool sw = digitalRead(ENC_SW_PIN);
  if (sw == LOW && lastEncSwState == HIGH && (now - lastEncSwTime > DEBOUNCE_MS)) {
    lastEncSwTime = now;
    editMode = (editMode == EDIT_TIME) ? EDIT_RPM : EDIT_TIME;
  }
  lastEncSwState = sw;

  // -- Button 1: Start / Stop --------------------------------------------
  bool btn = digitalRead(BUTTON_PIN);
  if (btn == LOW && lastBtnState == HIGH && (now - lastBtnTime > DEBOUNCE_MS)) {
    lastBtnTime = now;
    if (currentState == IDLE || currentState == FINISHED) startProcess();
    else stopProcess();
  }
  lastBtnState = btn;

  // -- Button 2: Reset to IDLE from anywhere -----------------------------
  bool btn2 = digitalRead(BUTTON2_PIN);
  if (btn2 == LOW && lastBtn2State == HIGH && (now - lastBtn2Time > DEBOUNCE_MS)) {
    lastBtn2Time = now;
    stopProcess(); // stop motor and go to IDLE
    editMode = EDIT_TIME;
  }
  lastBtn2State = btn2;

  // -- State Machine -----------------------------------------------------
  if (currentState != IDLE && currentState != FINISHED) {
    unsigned long elapsed = now - processStartTime;

    if (elapsed >= TOTAL_TIME_MS) {
      setMotor(0, true);
      currentState = FINISHED;
      currentRPM = 0;
    } else {
      remainingTimeMs = TOTAL_TIME_MS - elapsed;
      unsigned long inState = now - stateStartTime;

      switch (currentState) {
        case RUNNING_FWD:
          if (inState >= FWD_TIME_MS) {
            currentState = PAUSING_FWD;
            stateStartTime = now;
            setMotor(0, true);
            currentRPM = 0;
          } else applyPID(true);
          break;

        case PAUSING_FWD:
          if (inState >= PAUSE_TIME_MS) {
            currentState = RUNNING_REV;
            stateStartTime = now;
            resetPID();
          }
          break;

        case RUNNING_REV:
          if (inState >= REV_TIME_MS) {
            currentState = PAUSING_REV;
            stateStartTime = now;
            setMotor(0, true);
            currentRPM = 0;
          } else applyPID(false);
          break;

        case PAUSING_REV:
          if (inState >= PAUSE_TIME_MS) {
            currentState = RUNNING_FWD;
            stateStartTime = now;
            resetPID();
          }
          break;

        default: break;
      }
    }
  }

  // -- Update OLED 4 times/second ----------------------------------------
  static unsigned long lastOled = 0;
  if (now - lastOled >= 250) {
    lastOled = now;
    updateOLED();
  }
}
