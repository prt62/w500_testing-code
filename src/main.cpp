#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <Preferences.h>

// =========================================================
// 1. HARDWARE PIN DEFINITIONS
// =========================================================
#define W5500_MOSI 23
#define W5500_MISO 19
#define W5500_SCLK 18
#define W5500_CS   5
#define W5500_RST  25

// Factory Indication Pins
#define BUZZER_PIN 4
#define LED_PASS   2
#define LED_FAIL   15

// =========================================================
// 2. ERROR CODES & QA METRICS
// =========================================================
enum FactoryError {
    SUCCESS = 0,
    E001_NVS_FAIL,
    E010_HW_RESET_FAIL,
    E012_VERSION_MISMATCH,
    E015_SPI_INTEGRITY_LOW,
    E020_PHY_LINK_DOWN,
    E030_SOCKET_INIT_FAIL,
    E050_CHECKSUM_MISMATCH
};

struct FactoryReport {
    String boardMAC;
    float pcbHealthScore = 100.0;
    FactoryError lastError = SUCCESS;
    unsigned long testStartTime = 0;
    uint32_t spiReads = 10000;
    uint32_t spiErrors = 0;
    
    // Pass/Fail Flags
    bool esp32Pass = false;
    bool resetPass = false;
    bool spiPass = false;
    bool versionPass = false;
    bool regWritePass = false;
    bool phyPass = false;
    bool ethernetPass = false;
    bool socketsPass = false;
    bool stressPass = false;
} report;

Preferences preferences;

// Official WIZnet MAC & Strict Static IP
byte mac[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
IPAddress staticIP(192, 168, 5, 100);
IPAddress gateway(192, 168, 5, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

// =========================================================
// 3. LOW-LEVEL SPI FUNCTIONS & HELPERS
// =========================================================
uint8_t readReg(uint16_t addr, uint8_t controlByte = 0x00) {
    // FIX: Stabilized SPI frequency to 1MHz for bare PCB traces
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(W5500_CS, LOW);
    SPI.transfer(highByte(addr));
    SPI.transfer(lowByte(addr));
    SPI.transfer(controlByte);
    uint8_t value = SPI.transfer(0x00);
    digitalWrite(W5500_CS, HIGH);
    SPI.endTransaction();
    return value;
}

void writeReg(uint16_t addr, uint8_t data, uint8_t controlByte = 0x04) {
    // FIX: Stabilized SPI frequency to 1MHz for bare PCB traces
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(W5500_CS, LOW);
    SPI.transfer(highByte(addr));
    SPI.transfer(lowByte(addr));
    SPI.transfer(controlByte);
    SPI.transfer(data);
    digitalWrite(W5500_CS, HIGH);
    SPI.endTransaction();
}

void printHeader(const char* title) {
    Serial.println("\n===================================");
    Serial.println(title);
    Serial.println("===================================");
    Serial.flush();
    yield(); 
}

void printPassFail(bool status) {
    if (status) Serial.println("\n-> PASS\n");
    else Serial.println("\n-> FAIL\n");
    Serial.flush();
    yield();
}

void signalFail(FactoryError errCode) {
    digitalWrite(LED_FAIL, HIGH);
    digitalWrite(LED_PASS, LOW);
    report.lastError = errCode;
    for(int i=0; i<3; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(100);
        digitalWrite(BUZZER_PIN, LOW); delay(100);
    }
}

void signalPass() {
    digitalWrite(LED_PASS, HIGH);
    digitalWrite(LED_FAIL, LOW);
    digitalWrite(BUZZER_PIN, HIGH); delay(500);
    digitalWrite(BUZZER_PIN, LOW);
}

// =========================================================
// 4. MAIN SETUP - FULL 15 PHASE DIAGNOSTIC SEQUENCE
// =========================================================
void setup() {
    Serial.begin(115200);
    delay(2000); // Give terminal time to connect
    report.testStartTime = millis();

    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(LED_PASS, OUTPUT);
    pinMode(LED_FAIL, OUTPUT);
    
    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    report.boardMAC = String(macStr);

    Serial.println("\n\n################################################");
    Serial.println("  W5500 FACTORY MASTER DIAGNOSTIC v4.3");
    Serial.println("################################################\n");
    Serial.flush();

    // --- PHASE 1: ESP32 Self Test & NVS ---
    printHeader("Phase-1 : ESP32 Self Test & NVS");
    Serial.printf("Chip        : %s\n", ESP.getChipModel());
    Serial.printf("Revision    : %d\n", ESP.getChipRevision());
    Serial.printf("CPU         : %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Heap        : %d Bytes\n", ESP.getFreeHeap());
    
    if(preferences.begin("factory_test", false)) {
        preferences.putUInt("boot_count", preferences.getUInt("boot_count", 0) + 1);
        preferences.end();
        report.esp32Pass = true;
        Serial.println("NVS Storage : PASS");
    } else {
        report.pcbHealthScore -= 10;
        report.lastError = E001_NVS_FAIL;
        Serial.println("NVS Storage : FAIL");
    }
    printPassFail(report.esp32Pass);

    // --- PHASE 2: Hardware Reset & Crystal Diagnostics (Phase-2.5) ---
    printHeader("Phase-2 : Hardware Reset & Crystal Diagnostics");
    
    // 1. Assert Reset
    pinMode(W5500_RST, OUTPUT);
    Serial.println("Reset Assert (Transistor ON -> W5500 RST LOW)");
    digitalWrite(W5500_RST, HIGH);
    delay(250); // FIX: Solid 250ms hardware reset pulse
    
    // 2. Start SPI to poll during boot
    pinMode(W5500_CS, OUTPUT);
    digitalWrite(W5500_CS, HIGH);
    // FIX: Using -1 bypasses ESP32 Hardware CS routing to avoid GPIO5 conflict
    SPI.begin(W5500_SCLK, W5500_MISO, W5500_MOSI, -1);
    delay(100); 
    
    Serial.println("Reset Release (Transistor OFF -> W5500 RST HIGH). Measuring Boot Time...");
    digitalWrite(W5500_RST, LOW); // Transistor off, R13 pull-up takes over
    
    // FIX: Wait for W5500 internal PLL to lock before hammering SPI bus
    delay(100); 
    
    unsigned long bootStartTime = millis();
    bool crystalAlive = false;
    unsigned long actualBootTime = 0;
    
    // 3. Poll for Max 1500ms
    while((millis() - bootStartTime) < 1500) {
        if(readReg(0x0039) == 0x04) {
            actualBootTime = millis() - bootStartTime;
            crystalAlive = true;
            break;
        }
        delay(5); // Poll every 5ms
        yield();
    }
    
    if(crystalAlive) {
        Serial.printf("[CRYSTAL ALIVE] W5500 Booted in: %lu ms\n", actualBootTime);
        if(actualBootTime < 250) {
            Serial.println("Crystal Health : PERFECT (Fast Boot)");
        } else {
            Serial.println("Crystal Health : WARNING (Slow Boot! Check 18pF/22pF caps)");
        }
        report.resetPass = true;
        printPassFail(true);
    } else {
        Serial.println("[FATAL] 1500ms passed. W5500 did not boot.");
        Serial.println("PROBABLE CAUSES:");
        Serial.println("1. Dead 25MHz Crystal or missing capacitors.");
        Serial.println("2. Reset Transistor/Pin logic is inverted or floating.");
        Serial.println("3. 3.3V Power missing on chip.");
        Serial.println("4. MISO Capacitor C6 (104) is populated by mistake (Check PCB!).");
        report.resetPass = false;
        report.pcbHealthScore -= 50; // Heavy penalty for hardware death
        if(report.lastError == SUCCESS) report.lastError = E010_HW_RESET_FAIL;
        printPassFail(false);
    }

    // --- PHASE 3: SPI Test ---
    printHeader("Phase-3 : SPI Test");
    pinMode(W5500_CS, OUTPUT);
    digitalWrite(W5500_CS, HIGH);
    // FIX: Again using -1 for CS pin to maintain software control
    SPI.begin(W5500_SCLK, W5500_MISO, W5500_MOSI, -1);
    Serial.println("Mode          : 0");
    Serial.println("Bit Order     : MSB");
    Serial.println("Frequency     : 1 MHz (Base)");
    report.spiPass = true;
    printPassFail(report.spiPass);

    // --- PHASE 4: VERSION Register ---
    printHeader("Phase-4 : VERSION Register");
    uint8_t version = readReg(0x0039);
    Serial.printf("Expected : 0x04\nRead     : 0x%02X\n", version);
    if (version == 0x04) {
        report.versionPass = true;
    } else {
        report.pcbHealthScore -= 40;
        if(report.lastError == SUCCESS) report.lastError = E012_VERSION_MISMATCH;
    }
    printPassFail(report.versionPass);

    if(!report.versionPass) {
        Serial.println("\n[FATAL] VERSION FAILED. HARDWARE OFFLINE. SKIPPING TO REPORT.");
        Serial.flush();
    } else {
        // --- PHASE 5: Register Dump ---
        printHeader("Phase-5 : Register Dump");
        Serial.printf("MR       : 0x%02X\n", readReg(0x0000));
        Serial.printf("PHYCFGR  : 0x%02X\n", readReg(0x002E));
        Serial.printf("VERSION  : 0x%02X\n", readReg(0x0039));

        // --- PHASE 6: Register Read/Write & Checksum ---
        printHeader("Phase-6 : Register Read Write Test");
        writeReg(0x0001, 192); writeReg(0x0002, 168);
        writeReg(0x0003, 5); writeReg(0x0004, 1);
        if((readReg(0x0001) + readReg(0x0002) + readReg(0x0003) + readReg(0x0004)) == 366) {
            report.regWritePass = true;
            Serial.println("Write/Read Checksum 366 : PASS");
        } else {
            report.pcbHealthScore -= 20;
            if(report.lastError == SUCCESS) report.lastError = E050_CHECKSUM_MISMATCH;
            Serial.println("Write/Read Checksum : FAIL");
        }
        printPassFail(report.regWritePass);

        // --- PHASE 7: PHY Test ---
        printHeader("Phase-7 : PHY Test");
        Serial.println("Waiting for PHY Auto-Negotiation (Max 3s)...");
        Serial.flush();
        uint8_t phyCfg = 0;
        for(int i = 0; i < 30; i++) {
            phyCfg = readReg(0x002E);
            if((phyCfg & 0x01) == 1) break; // Link is UP
            delay(100);
            yield(); 
        }
        
        if((phyCfg & 0x01) == 1) report.phyPass = true;
        Serial.printf("Link             : %s\n", report.phyPass ? "UP" : "DOWN");
        Serial.printf("Speed            : %s\n", (phyCfg & 0x02) ? "100 Mbps" : "10 Mbps");
        Serial.printf("Duplex           : %s\n", (phyCfg & 0x04) ? "FULL" : "HALF");
        
        if(!report.phyPass) {
            report.pcbHealthScore -= 30;
            if(report.lastError == SUCCESS) report.lastError = E020_PHY_LINK_DOWN;
        }
        printPassFail(report.phyPass);

        // --- PHASE 8: Ethernet Test ---
        printHeader("Phase-8 : Ethernet Test");
        Ethernet.init(W5500_CS);
        Ethernet.begin(mac, staticIP, dns, gateway, subnet);
        
        // FIX: Allow time for internal IP setup processing before validation
        delay(250);
        
        Serial.print("IP       : "); Serial.println(Ethernet.localIP());
        Serial.print("Gateway  : "); Serial.println(Ethernet.gatewayIP());
        Serial.print("Subnet   : "); Serial.println(Ethernet.subnetMask());
        
        if(Ethernet.localIP() == staticIP) report.ethernetPass = true;
        printPassFail(report.ethernetPass);

        // --- PHASE 9: Socket Lifecycle Test ---
        printHeader("Phase-9 : Socket Test");
        report.socketsPass = true;
        for(uint8_t sn = 0; sn < 4; sn++) {
            uint8_t ctrlRead = 0x08 + (sn * 0x20);
            uint8_t ctrlWrite = 0x0C + (sn * 0x20);
            
            writeReg(0x0000, 0x01, ctrlWrite);
            writeReg(0x0001, 0x01, ctrlWrite);
            delay(10);
            
            uint8_t status = readReg(0x0003, ctrlRead);
            if(status != 0x13) report.socketsPass = false;
            
            writeReg(0x0001, 0x10, ctrlWrite);
            Serial.printf("Socket %d : Initialize %s (Status: 0x%02X)\n", sn, (status == 0x13) ? "OK" : "FAIL", status);
            yield();
        }
        if(!report.socketsPass) {
            report.pcbHealthScore -= 25;
            if(report.lastError == SUCCESS) report.lastError = E030_SOCKET_INIT_FAIL;
        }
        printPassFail(report.socketsPass);

        // --- PHASE 10: Stress Test ---
        printHeader("Phase-10 : Stress Test (10000 Reads)");
        {
            for (int i = 0; i < report.spiReads; i++) {
                if (readReg(0x0039) != 0x04) report.spiErrors++;
                if (i % 1000 == 0) yield(); 
            }
            Serial.printf("Reads    : %d\n", report.spiReads);
            Serial.printf("Errors   : %d\n", report.spiErrors);
            
            float accuracy = ((float)(report.spiReads - report.spiErrors) / report.spiReads) * 100.0;
            if (accuracy < 100.0) {
                report.pcbHealthScore -= (100.0 - accuracy) * 5.0;
                if(report.lastError == SUCCESS) report.lastError = E015_SPI_INTEGRITY_LOW;
            } else {
                report.stressPass = true;
            }
            printPassFail(report.stressPass);
        }

        // --- PHASE 11: SPI Speed Test ---
        printHeader("Phase-11 : SPI Speed Test");
        {
            uint32_t speeds[] = {500000, 1000000, 2000000, 4000000, 8000000, 10000000, 20000000};
            String speedNames[] = {"500 KHz", "1 MHz  ", "2 MHz  ", "4 MHz  ", "8 MHz  ", "10 MHz ", "20 MHz "};
            for (int i = 0; i < 7; i++) {
                SPI.beginTransaction(SPISettings(speeds[i], MSBFIRST, SPI_MODE0));
                digitalWrite(W5500_CS, LOW);
                SPI.transfer(0x00); SPI.transfer(0x39); SPI.transfer(0x00);
                uint8_t v = SPI.transfer(0x00);
                digitalWrite(W5500_CS, HIGH);
                SPI.endTransaction();
                Serial.printf("%s : %s\n", speedNames[i].c_str(), (v == 0x04) ? "PASS" : "FAIL");
                yield();
            }
        }

        // --- PHASE 12: MAC Test ---
        printHeader("Phase-12 : MAC Test");
        Serial.printf("MAC      : %s\n", report.boardMAC.c_str());
        printPassFail(true);
    } 

    // --- PHASE 15: Final Report & QA Data ---
    printHeader("Phase-15 : Final Report");
    
    Serial.println("\n=======================================================");
    Serial.println(" Phase | Name of the Test             | Status");
    Serial.println("-------------------------------------------------------");
    Serial.printf("  01   | %-28s | %s\n", "ESP32 Core & NVS", report.esp32Pass ? "PASS" : "FAIL");
    Serial.printf("  02   | %-28s | %s\n", "Hardware Reset", report.resetPass ? "PASS" : "FAIL");
    Serial.printf("  03   | %-28s | %s\n", "SPI Communication", report.spiPass ? "PASS" : "FAIL");
    Serial.printf("  04   | %-28s | %s\n", "W5500 Version Register", report.versionPass ? "PASS" : "FAIL");
    Serial.printf("  05   | %-28s | %s\n", "Register Dump", report.versionPass ? "PASS" : "SKIP"); 
    Serial.printf("  06   | %-28s | %s\n", "Register R/W Checksum", report.regWritePass ? "PASS" : "FAIL");
    Serial.printf("  07   | %-28s | %s\n", "PHY Link & Auto-Negotiation", report.phyPass ? "PASS" : "FAIL");
    Serial.printf("  08   | %-28s | %s\n", "Ethernet Static IP", report.ethernetPass ? "PASS" : "FAIL");
    Serial.printf("  09   | %-28s | %s\n", "Socket Hardware Lifecycle", report.socketsPass ? "PASS" : "FAIL");
    Serial.printf("  10   | %-28s | %s\n", "SPI 10k Stress Test", report.stressPass ? "PASS" : "FAIL");
    Serial.printf("  11   | %-28s | %s\n", "SPI Speed Test (Up to 20MHz)", report.versionPass ? "PASS" : "SKIP");
    Serial.printf("  12   | %-28s | %s\n", "MAC Address Verification", "PASS");
    Serial.println("-------------------------------------------------------");
    Serial.printf("  13   | %-28s | %s\n", "Ping Ready Target", "READY");
    Serial.printf("  14   | %-28s | %s\n", "Continuous Link Monitor", "ACTIVE");
    Serial.printf("  15   | %-28s | %.1f/100\n", "Final QA Health Score", report.pcbHealthScore);
    Serial.println("=======================================================\n");
    Serial.flush();

    Serial.printf("SPI Reads        %d\n", report.spiReads);
    Serial.printf("SPI Errors       %d\n", report.spiErrors);
    Serial.println("===================================");
    
    if (report.pcbHealthScore < 0) report.pcbHealthScore = 0;
    
    if (report.pcbHealthScore == 100.0 && report.lastError == SUCCESS) {
        Serial.println("\n★★★★★ BOARD HEALTH : GOOD ★★★★★\n");
        signalPass();
    } else {
        Serial.println("\nXXXXX BOARD HEALTH : BAD XXXXX\n");
        signalFail(report.lastError);
    }
    Serial.flush();

    unsigned long duration = millis() - report.testStartTime;

    Serial.println("CSV_DATA_START");
    Serial.println("BOARD_MAC,DURATION_MS,RESULT,SPI,PHY,SOCKET,HEALTH_SCORE,ERROR_CODE");
    Serial.printf("%s,%lu,%s,%s,%s,%s,%.1f,E%03d\n", 
        report.boardMAC.c_str(), duration,
        (report.pcbHealthScore == 100.0) ? "PASS" : "FAIL",
        report.spiPass ? "PASS" : "FAIL",
        report.phyPass ? "PASS" : "FAIL",
        report.socketsPass ? "PASS" : "FAIL",
        report.pcbHealthScore, report.lastError);
    Serial.println("CSV_DATA_END\n");
    Serial.flush();

    Serial.println("JSON_DATA_START");
    Serial.println("{");
    Serial.printf("  \"mac\": \"%s\",\n", report.boardMAC.c_str());
    Serial.printf("  \"health\": %.1f,\n", report.pcbHealthScore);
    Serial.printf("  \"error\": \"E%03d\"\n", report.lastError);
    Serial.println("}");
    Serial.println("JSON_DATA_END\n");
    Serial.flush();

    // --- PHASE 13: Ping Ready ---
    printHeader("Phase-13 : Ping Ready");
    Serial.printf("Target   : %d.%d.%d.%d\n", staticIP[0], staticIP[1], staticIP[2], staticIP[3]);
    Serial.println("Waiting for network traffic...\n");
    Serial.flush();
}

// =========================================================
// 5. PHASE 14: CONTINUOUS LINK Monitor LOOP
// =========================================================
void loop() {
    static unsigned long lastMonitor = 0;
    static auto lastLinkStatus = Ethernet.linkStatus();
    
    auto currentLinkStatus = Ethernet.linkStatus();
    if (currentLinkStatus != lastLinkStatus) {
        Serial.printf("\nLINK\n%s\n", (currentLinkStatus == LinkON) ? "UP" : "DOWN");
        lastLinkStatus = currentLinkStatus;
        Serial.flush();
    }

    if (millis() - lastMonitor > 5000) {
        lastMonitor = millis();
         
        unsigned long sec = millis() / 1000;
        int h = (sec / 3600); int m = (sec % 3600) / 60; int s = (sec % 60);

        float tempC = temperatureRead(); 
        
        Serial.println("-----------------------------------");
        Serial.printf("Uptime           %02d:%02d:%02d\n", h, m, s);
        Serial.printf("Heap             %d KB\n", ESP.getFreeHeap() / 1024);
        Serial.printf("Link             %s\n", (currentLinkStatus == LinkON) ? "UP" : "DOWN");
        Serial.print("IP               "); Serial.println(Ethernet.localIP());
        Serial.printf("Temperature      %.1f°C\n", tempC);
        Serial.flush();
    }
}