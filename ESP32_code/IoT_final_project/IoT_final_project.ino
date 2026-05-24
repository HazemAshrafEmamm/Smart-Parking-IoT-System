#include <WiFi.h>
#include <FirebaseESP32.h> 
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h> 
#include <time.h> 
#include <Wire.h> 
#include <LiquidCrystal_I2C.h> 

//==============================================
//==============================================
#define WIFI_SSID       "*****"  
#define WIFI_PASSWORD   "**********" 

const char* FIREBASE_HOST_URL = "smart-parking-****-default-rtdb.firebaseio.com"; 
const char* FIREBASE_SECRET = "**************************************************"; 

#define RST_PIN   5  
#define SS_PIN    15 
#define MOSI_PIN  23 
#define MISO_PIN  19 
#define SCK_PIN   18 

#define SERVO_PIN 25  
#define CLOSE_POS 90   
#define OPEN_POS  0  

#define IR_SLOT_1     32  
#define IR_SLOT_2     33  
#define IR_SLOT_3     26  
#define IR_SLOT_4     14  
#define IR_SLOT_5     34 

int ledGreenPins[] = {17, 2, 13, 4, 3}; // Slot 1, 2, 3, 4, 5
int ledRedPins[]   = {16, 27,  12, 0, 1}; // Slot 1, 2, 3, 4, 5

const String MASTER_KEY = "1DC81C2F"; 
const String SUBSCRIBERS[] = { 
  "22491103", "2EAD0B03", "84861B03", "F32CB8D9" 
}; 

const int TOTAL_SLOTS = 5; 
int availableSlots = TOTAL_SLOTS;
const long OCCUPANCY_CONFIRMATION_TIME_MS = 3000; 

//==============================================
//==============================================
FirebaseConfig firebaseConfig;
FirebaseAuth firebaseAuth;
FirebaseData fbdo;
FirebaseJson json; 

MFRC522 mfrc522(SS_PIN, RST_PIN); 
Servo gateServo; 
LiquidCrystal_I2C lcd(0x27, 16, 2); 

int irPins[] = {IR_SLOT_1, IR_SLOT_2, IR_SLOT_3, IR_SLOT_4, IR_SLOT_5};
String slotNames[] = {"Slot_1", "Slot_2", "Slot_3", "Slot_4", "Slot_5"};

long lastSlotChangeTime[TOTAL_SLOTS] = {0, 0, 0, 0, 0};
bool lastKnownState[TOTAL_SLOTS] = {false, false, false, false, false}; 
String lastRegisteredUID = "";

//==============================================
//          (Helper Functions)
//==============================================
void openGate() {
  gateServo.write(OPEN_POS);
  delay(3000); 
}

void closeGate() {
  gateServo.write(CLOSE_POS);
}

String uidToString(byte *buffer, byte bufferSize) {
  String cardUID = "";
  for (byte i = 0; i < bufferSize; i++) {
    cardUID += (buffer[i] < 0x10 ? "0" : "");
    cardUID += String(buffer[i], HEX);
  }
  cardUID.toUpperCase();
  return cardUID;
}

unsigned long getCurrentUnixTime() {
    time_t now = time(NULL);
    if (now < 1000000000) return 0;
    return (unsigned long)now;
}

String getFormattedTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) { return "Time Error"; }
    char timeString[64];
    strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(timeString);
}

void displayWelcome(String uid) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Welcome!");
  lcd.setCursor(0, 1);
  lcd.print("UID: ");
  lcd.print(uid.substring(0, 8)); 
  delay(3000); 
}

void notifySlotOccupancy(int slotIndex, String uid) {
    if (uid.length() == 0 || uid == "N/A") return;
    json.clear();
    json.set("timestamp", getFormattedTime());
    json.set("message", "Car in " + slotNames[slotIndex]);
    json.set("slot", slotNames[slotIndex]);
    json.set("uid", uid);
    Firebase.push(fbdo, "/Notifications/Occupancy", json);
}

void updateSlotsStatus() {
  int currentAvailable = 0;
  unsigned long currentTime = millis();

  for (int i = 0; i < TOTAL_SLOTS; i++) {
    bool currentSensorState = !digitalRead(irPins[i]); 
    String slotPath = "/Parking_Slots/" + slotNames[i];
    
    // جلب الحالة من فايربيز للتحقق
    String firebaseStatus = "Empty"; 
    if (Firebase.getString(fbdo, slotPath + "/Status")) { 
        firebaseStatus = fbdo.stringData(); 
    }
    bool isCurrentlyOccupied = (firebaseStatus != "Empty");

    if (currentSensorState != lastKnownState[i]) {
      lastSlotChangeTime[i] = currentTime;
      lastKnownState[i] = currentSensorState;
    } else if (currentTime - lastSlotChangeTime[i] >= OCCUPANCY_CONFIRMATION_TIME_MS) {
      if (currentSensorState && !isCurrentlyOccupied) { 
        String confirmedUID = "N/A";
        if (lastRegisteredUID.length() > 0 && lastRegisteredUID != "N/A") {
             confirmedUID = lastRegisteredUID;
             lastRegisteredUID = ""; 
        }
        Firebase.setString(fbdo, slotPath + "/Status", "Occupied");
        Firebase.setString(fbdo, slotPath + "/UID", confirmedUID);
        digitalWrite(ledRedPins[i], HIGH); 
        digitalWrite(ledGreenPins[i], LOW);
        notifySlotOccupancy(i, confirmedUID);
      } else if (!currentSensorState && isCurrentlyOccupied) { 
        Firebase.setString(fbdo, slotPath + "/Status", "Empty");
        Firebase.setString(fbdo, slotPath + "/UID", "N/A");
        digitalWrite(ledRedPins[i], LOW);  
        digitalWrite(ledGreenPins[i], HIGH);
      }
    }
    
    String currentSlotUID = "N/A";
    if (Firebase.getString(fbdo, slotPath + "/UID")) {
        currentSlotUID = fbdo.stringData();
    }
    if (currentSlotUID == "N/A") currentAvailable++;
  }
  
  availableSlots = currentAvailable;
  Firebase.setInt(fbdo, "/Parking_Info/Available_Slots", availableSlots);

  lcd.setCursor(0, 0);
  lcd.print("Parking System  ");
  lcd.setCursor(0, 1);
  lcd.print("Available: ");
  lcd.print(availableSlots);
  lcd.print("   ");
}

void handleSubscriber(String uid, bool isMasterKey = false) {
  String activePath = "/Active_Sessions/" + uid; 
  bool isActiveSession = Firebase.get(fbdo, activePath + "/entry_timestamp"); 
  
  if (isMasterKey) {
      lcd.clear(); lcd.print("Master Access");
      openGate();
      delay(5000); 
      closeGate();
      updateSlotsStatus();
      return;
  }
  
  if (isActiveSession) {
    unsigned long entryTimeRaw = 0;
    if (Firebase.get(fbdo, activePath + "/entry_timestamp")) entryTimeRaw = fbdo.intData(); 
    
    unsigned long exitTimeRaw = getCurrentUnixTime(); 
    float durationMinutes = (float)(exitTimeRaw - entryTimeRaw) / 60.0;
    
    json.clear(); 
    json.set("UID", uid);
    json.set("Exit_Time", getFormattedTime()); 
    json.set("Duration_Min", durationMinutes);
    
    if (Firebase.push(fbdo, "/Session_Log", json)) {
        lcd.clear(); lcd.print("Goodbye!"); 
        openGate();
        delay(5000); 
        closeGate();
        Firebase.deleteNode(fbdo, activePath);
        updateSlotsStatus();
    }
  } else {
    if (availableSlots > 0) {
        json.clear(); 
        json.set("status", "Active");
        json.set("entry_timestamp", getCurrentUnixTime()); 
        json.set("entry_formatted_time", getFormattedTime()); 
        
        if (Firebase.set(fbdo, activePath, json)) {
          displayWelcome(uid); 
          openGate();
          lastRegisteredUID = uid;
          delay(5000); 
          closeGate();
          updateSlotsStatus();
        }
    } else {
        lcd.clear(); lcd.print("Parking Full!");
        delay(3000);
        updateSlotsStatus();
    }
  }
}

//==============================================
//             (setup) 
//==============================================
void setup() {
  Serial.begin(115200);
  
  gateServo.attach(SERVO_PIN);
  closeGate(); 

  Wire.begin(21, 22); 
  lcd.init();
  lcd.backlight();
  lcd.print("Initializing...");
  
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN); 
  mfrc522.PCD_Init(); 
  
  for (int i = 0; i < TOTAL_SLOTS; i++) {
    pinMode(irPins[i], INPUT_PULLUP);
    pinMode(ledGreenPins[i], OUTPUT);
    pinMode(ledRedPins[i], OUTPUT);
    digitalWrite(ledRedPins[i], LOW);  
    digitalWrite(ledGreenPins[i], HIGH);
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print(".");}
  
  configTime(2 * 3600, 0, "pool.ntp.org");
  setenv("TZ", "EET-2", 1); 
  tzset();
  
  firebaseConfig.host = FIREBASE_HOST_URL;
  firebaseConfig.signer.tokens.legacy_token = FIREBASE_SECRET; 
  
  // تصحيح الخطأ: تمرير العناوين باستخدام &
  Firebase.begin(&firebaseConfig, &firebaseAuth);
  Firebase.reconnectWiFi(true); 

  for (int i = 0; i < TOTAL_SLOTS; i++) {
      Firebase.setString(fbdo, "/Parking_Slots/" + slotNames[i] + "/Status", "Empty");
      Firebase.setString(fbdo, "/Parking_Slots/" + slotNames[i] + "/UID", "N/A");
  }

  updateSlotsStatus(); 
  lcd.clear();
}

//==============================================
//                     (loop) 
//==============================================
void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    
    static unsigned long lastSlotUpdate = 0;
    if (millis() - lastSlotUpdate > 1000) { 
        updateSlotsStatus();
        lastSlotUpdate = millis();
    }

    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String cardUID = uidToString(mfrc522.uid.uidByte, mfrc522.uid.size);
      bool isSubscriber = false;
      for (int i = 0; i < 4; i++) { 
          if (cardUID == SUBSCRIBERS[i]) { isSubscriber = true; break; }
      }
      
      if (cardUID == MASTER_KEY) {
        handleSubscriber(cardUID, true); 
      } else if (isSubscriber) {
        handleSubscriber(cardUID); 
      } else {
        lcd.clear(); lcd.print("Access Denied!");
        delay(2000);
      }
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
    }
  } else {
    lcd.clear(); lcd.print("WiFi Lost!");
    delay(5000);
  }
}
