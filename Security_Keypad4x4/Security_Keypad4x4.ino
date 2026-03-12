#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <EEPROM.h>
#include <Servo.h>

#define BUZZER_PIN 12
#define SERVO_PIN 3

/* ---------- LCD ---------- */
LiquidCrystal_I2C lcd(0x27, 16, 2);   // change to 0x3F if needed

/* ---------- Servo ---------- */
Servo doorServo;

/*
  Adjust these two angles for your real lock:
  LOCK_POS = locked position
  OPEN_POS = unlocked/open position
*/
const int LOCK_POS = 90;
const int OPEN_POS = 0;
const unsigned long SERVO_OPEN_TIME_MS = 10000;
const unsigned long SERVO_CLOSING_TIME_MS = 1500;
const unsigned long DOOR_CLOSED_INFO_MS = 2000;

/* ---------- Keypad ---------- */
const byte ROWS = 4;
const byte COLS = 4;

char hexaKeys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {4, 5, 6, 7};
byte colPins[COLS] = {11, 10, 9, 8};

Keypad keypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS);

/* ---------- Settings ---------- */
const byte MAX_PASS_LEN = 8;
const byte MIN_PASS_LEN = 4;
const byte MAX_TRIES = 3;

const unsigned long HASH_HOLD_TIME_MS = 2000;
const unsigned long INFO_TIME_MS = 1500;

const unsigned long BASE_LOCKOUT_MS = 30000UL;
const unsigned long STEP_LOCKOUT_MS = 30000UL;

/* ---------- Volume ---------- */
/*
  0..100
  Best for passive buzzer: 20..100
*/
int volumePercent = 70;

/* ---------- EEPROM ---------- */
const int EEPROM_MAGIC_ADDR = 0;
const byte EEPROM_MAGIC_VAL = 0x5A;
const int EEPROM_DATA_ADDR = 10;

struct SecurityData {
  char password[MAX_PASS_LEN + 1];
  byte failCount;
  byte lockoutLevel;
  byte lockoutActive;
  unsigned long lockoutDurationMs;
};

SecurityData sec;

/*
  Uncomment these lines ONE TIME to clear EEPROM, upload, then comment again.

  for (int i = 0; i < EEPROM.length(); i++) EEPROM.write(i, 0);
*/

/* ---------- State Machine ---------- */
enum SystemState {
  ST_ENTER_PASSWORD,
  ST_INFO,
  ST_UNLOCKED,
  ST_CLOSING,
  ST_LOCKED_OUT,
  ST_VERIFY_OLD_PASSWORD,
  ST_ENTER_NEW_PASSWORD,
  ST_CONFIRM_NEW_PASSWORD
};

SystemState state = ST_ENTER_PASSWORD;
SystemState nextStateAfterInfo = ST_ENTER_PASSWORD;

/* ---------- Input ---------- */
char inputBuffer[MAX_PASS_LEN + 1] = "";
byte inputLen = 0;
char newPasswordBuffer[MAX_PASS_LEN + 1] = "";

/* ---------- Timers ---------- */
unsigned long infoUntil = 0;
unsigned long closingUntil = 0;
unsigned long lockoutEnd = 0;

/* ---------- Servo runtime ---------- */
bool servoIsOpen = false;
unsigned long servoCloseAt = 0;

/* ---------- LCD cache ---------- */
String lcdLine0 = "";
String lcdLine1 = "";
bool lcdDirty = true;
unsigned long lastShownRemainSec = 999999;

/* =========================================================
   BUZZER ENGINE
   Non-blocking tone sequence + non-blocking volume PWM gating
   ========================================================= */

struct ToneStep {
  unsigned int freq;
  unsigned int durMs;
};

/* Key touch: soft short click */
const ToneStep SEQ_TOUCH[] = {
  {1500, 25}
};

/* Clear key */
const ToneStep SEQ_CLEAR[] = {
  {850, 45}
};

/* Access granted: very different, happy rising melody */
const ToneStep SEQ_GRANTED[] = {
  {1200, 70},
  {1600, 70},
  {2100, 90},
  {2600, 140}
};

/* Access denied: low and short */
const ToneStep SEQ_DENIED[] = {
  {500, 130},
  {380, 180}
};

/* Lockout alarm */
const ToneStep SEQ_LOCKOUT[] = {
  {320, 180},
  {240, 220},
  {320, 180}
};

/* Password changed OK */
const ToneStep SEQ_CHANGE_OK[] = {
  {1400, 60},
  {1800, 60},
  {2200, 90}
};

/* Hold # detected */
const ToneStep SEQ_HOLD_HASH[] = {
  {1700, 100}
};

const ToneStep* activeSeq = nullptr;
byte activeSeqLen = 0;
byte activeSeqIndex = 0;
bool seqPlaying = false;
unsigned long toneStepEnd = 0;

/* volume gating */
const unsigned long pwmPeriodUs = 2000;   // 2 ms
bool pwmToneOn = false;
unsigned long pwmTimerUs = 0;
unsigned int currentToneFreq = 0;

void buzzerStopRaw() {
  noTone(BUZZER_PIN);
  pwmToneOn = false;
  currentToneFreq = 0;
}

void buzzerStartRaw(unsigned int freq) {
  currentToneFreq = freq;

  if (volumePercent <= 0) {
    noTone(BUZZER_PIN);
    pwmToneOn = false;
    return;
  }

  if (volumePercent >= 100) {
    tone(BUZZER_PIN, currentToneFreq);
    pwmToneOn = true;
    return;
  }

  tone(BUZZER_PIN, currentToneFreq);
  pwmToneOn = true;
  pwmTimerUs = micros();
}

void playSequence(const ToneStep* seq, byte len) {
  activeSeq = seq;
  activeSeqLen = len;
  activeSeqIndex = 0;
  seqPlaying = true;

  buzzerStartRaw(activeSeq[0].freq);
  toneStepEnd = millis() + activeSeq[0].durMs;
}

void stopSequence() {
  seqPlaying = false;
  buzzerStopRaw();
}

void updateVolumeGate() {
  if (!seqPlaying || currentToneFreq == 0) return;

  if (volumePercent <= 0) {
    if (pwmToneOn) {
      noTone(BUZZER_PIN);
      pwmToneOn = false;
    }
    return;
  }

  if (volumePercent >= 100) {
    if (!pwmToneOn) {
      tone(BUZZER_PIN, currentToneFreq);
      pwmToneOn = true;
    }
    return;
  }

  unsigned long nowUs = micros();
  unsigned long onTime = (pwmPeriodUs * (unsigned long)volumePercent) / 100UL;
  unsigned long offTime = pwmPeriodUs - onTime;

  if (onTime == 0) {
    if (pwmToneOn) {
      noTone(BUZZER_PIN);
      pwmToneOn = false;
    }
    return;
  }

  if (offTime == 0) {
    if (!pwmToneOn) {
      tone(BUZZER_PIN, currentToneFreq);
      pwmToneOn = true;
    }
    return;
  }

  if (pwmToneOn) {
    if ((unsigned long)(nowUs - pwmTimerUs) >= onTime) {
      noTone(BUZZER_PIN);
      pwmToneOn = false;
      pwmTimerUs = nowUs;
    }
  } else {
    if ((unsigned long)(nowUs - pwmTimerUs) >= offTime) {
      tone(BUZZER_PIN, currentToneFreq);
      pwmToneOn = true;
      pwmTimerUs = nowUs;
    }
  }
}

void updateBuzzer() {
  if (seqPlaying) {
    if ((long)(millis() - toneStepEnd) >= 0) {
      activeSeqIndex++;
      if (activeSeqIndex >= activeSeqLen) {
        stopSequence();
      } else {
        buzzerStartRaw(activeSeq[activeSeqIndex].freq);
        toneStepEnd = millis() + activeSeq[activeSeqIndex].durMs;
      }
    }
  }

  updateVolumeGate();
}

/* ---------- Servo helpers ---------- */
void servoLockNow() {
  doorServo.write(LOCK_POS);
  servoIsOpen = false;
}

void servoOpenFor(unsigned long durationMs) {
  doorServo.write(OPEN_POS);
  servoIsOpen = true;
  servoCloseAt = millis() + durationMs;
}

bool updateServo() {
  if (servoIsOpen && (long)(millis() - servoCloseAt) >= 0) {
    servoLockNow();
    return true;
  }

  return false;
}

/* ---------- EEPROM helpers ---------- */
void saveSecurityData() {
  EEPROM.put(EEPROM_DATA_ADDR, sec);
}

void loadSecurityData() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) {
    strcpy(sec.password, "1234");
    sec.failCount = 0;
    sec.lockoutLevel = 0;
    sec.lockoutActive = 0;
    sec.lockoutDurationMs = 0;

    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
    saveSecurityData();
  } else {
    EEPROM.get(EEPROM_DATA_ADDR, sec);

    if (strlen(sec.password) < MIN_PASS_LEN || strlen(sec.password) > MAX_PASS_LEN) {
      strcpy(sec.password, "1234");
      sec.failCount = 0;
      sec.lockoutLevel = 0;
      sec.lockoutActive = 0;
      sec.lockoutDurationMs = 0;
      saveSecurityData();
    }
  }
}

/* ---------- General helpers ---------- */
void clearInput() {
  inputLen = 0;
  inputBuffer[0] = '\0';
}

bool samePassword(const char* a, const char* b) {
  return strcmp(a, b) == 0;
}

String starsFromInput() {
  String s = "";
  for (byte i = 0; i < inputLen; i++) s += '*';
  return s;
}

String fit16(const String& s) {
  if (s.length() >= 16) return s.substring(0, 16);
  String out = s;
  while (out.length() < 16) out += ' ';
  return out;
}

String center16(const String& s) {
  if (s.length() >= 16) return s.substring(0, 16);

  int leftPad = (16 - s.length()) / 2;
  String out = "";

  for (int i = 0; i < leftPad; i++) out += ' ';
  out += s;
  while (out.length() < 16) out += ' ';

  return out;
}

void setDisplay(const String& l0, const String& l1) {
  String n0 = fit16(l0);
  String n1 = fit16(l1);

  if (n0 != lcdLine0 || n1 != lcdLine1) {
    lcdLine0 = n0;
    lcdLine1 = n1;
    lcdDirty = true;
  }
}

void refreshLCDIfNeeded() {
  if (!lcdDirty) return;

  lcd.setCursor(0, 0);
  lcd.print(lcdLine0);
  lcd.setCursor(0, 1);
  lcd.print(lcdLine1);
  lcdDirty = false;
}

void changeState(SystemState newState) {
  if (state != newState) {
    state = newState;
    lcdDirty = true;
    lastShownRemainSec = 999999;
  }
}

void showInfo(const String& l0, const String& l1, unsigned long duration, SystemState afterState) {
  setDisplay(l0, l1);
  infoUntil = millis() + duration;
  nextStateAfterInfo = afterState;
  changeState(ST_INFO);
}

void updateDisplayForState() {
  switch (state) {
    case ST_ENTER_PASSWORD:
      setDisplay("Enter Password", center16(starsFromInput()));
      break;

    case ST_VERIFY_OLD_PASSWORD:
      setDisplay("Old Password", center16(starsFromInput()));
      break;

    case ST_ENTER_NEW_PASSWORD:
      setDisplay("New Password", center16(starsFromInput()));
      break;

    case ST_CONFIRM_NEW_PASSWORD:
      setDisplay("Confirm Pass", center16(starsFromInput()));
      break;

    case ST_UNLOCKED:
      setDisplay("ACCESS GRANTED", "Door Open");
      break;

    case ST_CLOSING:
      setDisplay("ACCESS GRANTED", "Closing...");
      break;

    case ST_LOCKED_OUT: {
      unsigned long now = millis();
      unsigned long remainMs = (lockoutEnd > now) ? (lockoutEnd - now) : 0;
      unsigned long remainSec = (remainMs + 999) / 1000;

      if (remainSec != lastShownRemainSec) {
        lastShownRemainSec = remainSec;
        setDisplay("LOCKED OUT", "Wait " + String(remainSec) + " sec");
      }
      break;
    }

    case ST_INFO:
      break;
  }
}

/* ---------- Screen state helpers ---------- */
void enterPasswordScreen() {
  clearInput();
  changeState(ST_ENTER_PASSWORD);
  updateDisplayForState();
}

void oldPasswordScreen() {
  clearInput();
  changeState(ST_VERIFY_OLD_PASSWORD);
  updateDisplayForState();
}

void newPasswordScreen() {
  clearInput();
  changeState(ST_ENTER_NEW_PASSWORD);
  updateDisplayForState();
}

void confirmPasswordScreen() {
  clearInput();
  changeState(ST_CONFIRM_NEW_PASSWORD);
  updateDisplayForState();
}

/* ---------- Security logic ---------- */
void startLockout(unsigned long durationMs) {
  sec.lockoutActive = 1;
  sec.lockoutDurationMs = durationMs;
  sec.failCount = 0;
  saveSecurityData();

  lockoutEnd = millis() + durationMs;
  changeState(ST_LOCKED_OUT);
  updateDisplayForState();
  playSequence(SEQ_LOCKOUT, sizeof(SEQ_LOCKOUT) / sizeof(SEQ_LOCKOUT[0]));
}

void clearLockout() {
  sec.lockoutActive = 0;
  sec.lockoutDurationMs = 0;
  sec.failCount = 0;
  saveSecurityData();

  showInfo("System Ready", "Enter Password", 1000, ST_ENTER_PASSWORD);
}

void processCorrectPassword() {
  sec.failCount = 0;
  saveSecurityData();

  servoOpenFor(SERVO_OPEN_TIME_MS);
  changeState(ST_UNLOCKED);
  updateDisplayForState();
  playSequence(SEQ_GRANTED, sizeof(SEQ_GRANTED) / sizeof(SEQ_GRANTED[0]));
}

void processWrongPassword() {
  sec.failCount++;
  saveSecurityData();

  if (sec.failCount >= MAX_TRIES) {
    sec.lockoutLevel++;
    saveSecurityData();

    unsigned long penalty = BASE_LOCKOUT_MS + (unsigned long)(sec.lockoutLevel - 1) * STEP_LOCKOUT_MS;
    startLockout(penalty);
  } else {
    playSequence(SEQ_DENIED, sizeof(SEQ_DENIED) / sizeof(SEQ_DENIED[0]));
    showInfo("ACCESS DENIED",
             "Try " + String(sec.failCount) + "/" + String(MAX_TRIES),
             INFO_TIME_MS,
             ST_ENTER_PASSWORD);
  }
}

void submitInput() {
  if (inputLen == 0) return;

  switch (state) {
    case ST_ENTER_PASSWORD:
      if (samePassword(inputBuffer, sec.password)) {
        processCorrectPassword();
      } else {
        processWrongPassword();
      }
      clearInput();
      break;

    case ST_VERIFY_OLD_PASSWORD:
      if (samePassword(inputBuffer, sec.password)) {
        clearInput();
        newPasswordScreen();
        playSequence(SEQ_CHANGE_OK, sizeof(SEQ_CHANGE_OK) / sizeof(SEQ_CHANGE_OK[0]));
      } else {
        clearInput();
        playSequence(SEQ_DENIED, sizeof(SEQ_DENIED) / sizeof(SEQ_DENIED[0]));
        showInfo("Old Pass Wrong", "Try Again", INFO_TIME_MS, ST_ENTER_PASSWORD);
      }
      break;

    case ST_ENTER_NEW_PASSWORD:
      if (inputLen < MIN_PASS_LEN) {
        clearInput();
        playSequence(SEQ_DENIED, sizeof(SEQ_DENIED) / sizeof(SEQ_DENIED[0]));
        showInfo("Too Short", "Min 4 chars", INFO_TIME_MS, ST_ENTER_NEW_PASSWORD);
      } else {
        strcpy(newPasswordBuffer, inputBuffer);
        clearInput();
        confirmPasswordScreen();
      }
      break;

    case ST_CONFIRM_NEW_PASSWORD:
      if (samePassword(inputBuffer, newPasswordBuffer)) {
        strcpy(sec.password, newPasswordBuffer);
        saveSecurityData();
        clearInput();
        playSequence(SEQ_CHANGE_OK, sizeof(SEQ_CHANGE_OK) / sizeof(SEQ_CHANGE_OK[0]));
        showInfo("Password Saved", "Success", INFO_TIME_MS, ST_ENTER_PASSWORD);
      } else {
        clearInput();
        playSequence(SEQ_DENIED, sizeof(SEQ_DENIED) / sizeof(SEQ_DENIED[0]));
        showInfo("Not Matched", "Start Again", INFO_TIME_MS, ST_ENTER_PASSWORD);
      }
      break;

    default:
      break;
  }
}

void addPasswordChar(char key) {
  if (inputLen < MAX_PASS_LEN) {
    inputBuffer[inputLen++] = key;
    inputBuffer[inputLen] = '\0';
    updateDisplayForState();
    playSequence(SEQ_TOUCH, sizeof(SEQ_TOUCH) / sizeof(SEQ_TOUCH[0]));
  }
}

void clearEnteredInput() {
  clearInput();
  updateDisplayForState();
  playSequence(SEQ_CLEAR, sizeof(SEQ_CLEAR) / sizeof(SEQ_CLEAR[0]));
}

/* ---------- Keypad event ---------- */
bool hashHoldTriggered = false;

void keypadEvent(KeypadEvent key) {
  KeyState ks = keypad.getState();

  if (key == '#') {
    if (ks == HOLD) {
      hashHoldTriggered = true;
      if (state == ST_ENTER_PASSWORD && inputLen == 0) {
        oldPasswordScreen();
        playSequence(SEQ_HOLD_HASH, sizeof(SEQ_HOLD_HASH) / sizeof(SEQ_HOLD_HASH[0]));
      }
      return;
    }

    if (ks == RELEASED) {
      if (!hashHoldTriggered) {
        if (state != ST_LOCKED_OUT && state != ST_INFO && state != ST_UNLOCKED && state != ST_CLOSING) {
          submitInput();
        }
      }
      hashHoldTriggered = false;
      return;
    }

    return;
  }

  if (ks != PRESSED) return;
  if (state == ST_LOCKED_OUT || state == ST_INFO || state == ST_UNLOCKED || state == ST_CLOSING) return;

  if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'D')) {
    addPasswordChar(key);
  } else if (key == '*') {
    clearEnteredInput();
  }
}

/* ---------- Setup ---------- */
void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  doorServo.attach(SERVO_PIN);
  servoLockNow();

  lcd.init();
  lcd.backlight();

  keypad.addEventListener(keypadEvent);
  keypad.setHoldTime(HASH_HOLD_TIME_MS);

  loadSecurityData();

  setDisplay("Smart Security", "Booting...");
  refreshLCDIfNeeded();

  if (sec.lockoutActive) {
    lockoutEnd = millis() + sec.lockoutDurationMs;
    changeState(ST_LOCKED_OUT);
  } else {
    changeState(ST_ENTER_PASSWORD);
  }

  updateDisplayForState();
  refreshLCDIfNeeded();
}

/* ---------- Loop ---------- */
void loop() {
  keypad.getKey();
  updateBuzzer();
  bool servoJustClosed = updateServo();

  unsigned long now = millis();

  if (servoJustClosed) {
    closingUntil = now + SERVO_CLOSING_TIME_MS;
    changeState(ST_CLOSING);
    updateDisplayForState();
  }

  switch (state) {
    case ST_INFO:
      if (now >= infoUntil) {
        if (nextStateAfterInfo == ST_ENTER_PASSWORD) {
          enterPasswordScreen();
        } else if (nextStateAfterInfo == ST_ENTER_NEW_PASSWORD) {
          newPasswordScreen();
        } else {
          changeState(nextStateAfterInfo);
          updateDisplayForState();
        }
      }
      break;

    case ST_UNLOCKED:
      break;

    case ST_CLOSING:
      if (now >= closingUntil) {
        showInfo("Door Closed", "Wait few sec", DOOR_CLOSED_INFO_MS, ST_ENTER_PASSWORD);
      }
      break;

    case ST_LOCKED_OUT:
      if (now >= lockoutEnd) {
        clearLockout();
      } else {
        updateDisplayForState();
      }
      break;

    default:
      break;
  }

  refreshLCDIfNeeded();
}
