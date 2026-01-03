#include <Arduino.h>

// =============================================================
//   STM32 UNIVERSAL FORENSIC LAB (F1 / F4 / Clones)
//   Ali Raheem 03/01/2026
// =============================================================

// --- Register Definitions ---
#define SCB_CPUID       (*(volatile uint32_t *)0xE000ED00)
#define FLASH_SIZE_REG  ((uint16_t *)0x1FFFF7E0) 
#define DBGMCU_IDCODE   (*(volatile uint32_t *)0xE0042000)

// --- Architecture Globals ---
bool isArchF4 = false; 
uint32_t BKP_STATE_ADDR = 0; // Where we store the current step
uint32_t BKP_RES1_ADDR  = 0; // General Results (RAM/RNG)
uint32_t BKP_RES2_ADDR  = 0; // Timer Bitmask

// --- Result Bitmasks (Res1) ---
#define RES_RNG_OK      (1 << 0)
#define RES_RAM_20K     (1 << 1)
#define RES_RAM_64K     (1 << 2)
#define RES_FLASH_128K  (1 << 3)

// --- Timer Definition ---
struct TimerDef {
  const char* name;
  uint8_t id;         
  uint8_t busType;    // 1=APB1, 2=APB2
  
  // F1 Specs (M3)
  uint32_t f1_base;
  uint8_t  f1_bit;    
  
  // F4 Specs (M4)
  uint32_t f4_base;
  uint8_t  f4_bit;
};

// 14 Timers (Covers F103 C6/C8/RC/ZE/VE and F401/411/407)
const TimerDef TIMERS[] = {
  { "TIM1", 1, 2,  0x40012C00, 11,     0x40010000, 0 },
  { "TIM2", 2, 1,  0x40000000, 0,      0x40000000, 0 },
  { "TIM3", 3, 1,  0x40000400, 1,      0x40000400, 1 },
  { "TIM4", 4, 1,  0x40000800, 2,      0x40000800, 2 },
  { "TIM5", 5, 1,  0x40000C00, 3,      0x40000C00, 3 }, 
  { "TIM6", 6, 1,  0x40001000, 4,      0x40001000, 4 }, 
  { "TIM7", 7, 1,  0x40001400, 5,      0x40001400, 5 }, 
  { "TIM8", 8, 2,  0x40013400, 13,     0x40010400, 1 }, 
  { "TIM9", 9, 2,  0,          0,      0x40014000, 16 },
  { "TIM10",10,2,  0,          0,      0x40014400, 17 },
  { "TIM11",11,2,  0,          0,      0x40014800, 18 },
  { "TIM12",12,1,  0,          0,      0x40001800, 6 }, 
  { "TIM13",13,1,  0,          0,      0x40001C00, 7 }, 
  { "TIM14",14,1,  0,          0,      0x40002000, 8 }  
};
#define NUM_TIMERS (sizeof(TIMERS)/sizeof(TimerDef))

// --- Helper Functions ---

void detectArch() {
  uint32_t cpuid = SCB_CPUID;
  uint32_t part = (cpuid >> 4) & 0xFFF;
  if (part == 0xC24) { // Cortex-M4
    isArchF4 = true;
    BKP_STATE_ADDR = 0x40002850; // RTC_BKP0R
    BKP_RES1_ADDR  = 0x40002854; // RTC_BKP1R
    BKP_RES2_ADDR  = 0x40002858; // RTC_BKP2R
  } else { // Cortex-M3 (F1)
    isArchF4 = false;
    BKP_STATE_ADDR = 0x40006C04; // BKP_DR1
    BKP_RES1_ADDR  = 0x40006C08; // BKP_DR2
    BKP_RES2_ADDR  = 0x40006C0C; // BKP_DR3
  }
}

// Registers Helpers
void setReg(uint32_t addr, uint32_t val) { (*(volatile uint32_t *)addr) = val; }
uint32_t getReg(uint32_t addr) { return (*(volatile uint32_t *)addr); }

void unlockBackup() {
  if (isArchF4) {
    (*(volatile uint32_t *)0x40023840) |= (1 << 28); // F4 PWREN
    (*(volatile uint32_t *)0x40007000) |= (1 << 8);  // PWR_CR DBP
  } else {
    (*(volatile uint32_t *)0x4002101C) |= (1 << 28) | (1 << 27); // F1 PWREN+BKPEN
    (*(volatile uint32_t *)0x40007000) |= (1 << 8);  // PWR_CR DBP
  }
}

void enableWatchdog() {
  IWDG->KR = 0x5555; IWDG->PR = 3; IWDG->RLR = 1250; IWDG->KR = 0xCCCC;
}
void pet() { IWDG->KR = 0xAAAA; }

// Memory Probe
bool probeMem(uint32_t addr, bool readonly) {
  if (addr == 0) return false; 
  volatile uint32_t *p = (uint32_t*)addr;
  if (readonly) {
    uint32_t v = *p; return (v != 0xFFFFFFFF && v != 0);
  } else {
    uint32_t o = *p; *p = 0xDEADBEEF; 
    bool m = (*p == 0xDEADBEEF); *p = o; return m;
  }
}

// Hardware Timer Probe
bool probeTimHW(int idx) {
  TimerDef t = TIMERS[idx];
  uint32_t base = isArchF4 ? t.f4_base : t.f1_base;
  uint8_t  bit  = isArchF4 ? t.f4_bit  : t.f1_bit;
  if (base == 0) return false; 

  // Enable Clock
  uint32_t rccAddr;
  if (isArchF4) rccAddr = (t.busType == 1) ? 0x40023840 : 0x40023844;
  else          rccAddr = (t.busType == 1) ? 0x4002101C : 0x40021018;

  (*(volatile uint32_t *)rccAddr) |= (1 << bit);
  
  // Probe Prescaler (Offset 0x28)
  volatile uint32_t *psc = (uint32_t*)(base + 0x28);
  *psc = 0xAA55;
  if (*psc == 0xAA55) { *psc = 0; return true; }
  return false;
}

// --- Main Setup ---
void setup() {
  detectArch(); 
  
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  delay(200);
  
  unlockBackup();
  enableWatchdog();

  uint16_t state = getReg(BKP_STATE_ADDR);
  
  // Wipe Check
  Serial.println("\n\n=== STM32 UNIVERSAL FORENSIC SCAN ===");
  Serial.print("Send 'w' to wipe data. ");
  uint32_t w = millis();
  while(millis() - w < 800) {
    if(Serial.read() == 'w') { 
       setReg(BKP_STATE_ADDR, 0); 
       setReg(BKP_RES1_ADDR, 0); 
       setReg(BKP_RES2_ADDR, 0);
       state=0; Serial.println("[WIPED]"); 
    }
  }
  Serial.println();

  // --- STATE MACHINE ---
  
  // 0. INIT
  if (state == 0) {
    Serial.println("Starting Scan.");
    setReg(BKP_RES1_ADDR, 0);
    setReg(BKP_RES2_ADDR, 0);
    setReg(BKP_STATE_ADDR, 10); 
    delay(50);
  }

  // 10. RNG & CORE
  else if (state == 10) {
    Serial.print("1. Probing RNG. "); Serial.flush();
    setReg(BKP_STATE_ADDR, 20); 
    
    // Probe F4 and F1 locations
    bool found = false;
    if (isArchF4) {
       (*(volatile uint32_t *)0x40023834) |= (1 << 6); 
       if (probeMem(0x50060800, true)) found = true;
    } else {
       (*(volatile uint32_t *)0x40021014) |= (1 << 10); 
       if (probeMem(0x50060800, true)) found = true;
       if (probeMem(0x40025000, true)) found = true;
    }
    if(found) { 
      setReg(BKP_RES1_ADDR, getReg(BKP_RES1_ADDR) | RES_RNG_OK); 
      Serial.println("PASS"); 
    } else Serial.println("FAIL");
  }

  // 20. RAM 20K
  else if (state == 20) {
    pet();
    Serial.print("2. Probing RAM 20KB... "); Serial.flush();
    setReg(BKP_STATE_ADDR, 30);
    if(probeMem(0x20004FFC, false)) { 
      setReg(BKP_RES1_ADDR, getReg(BKP_RES1_ADDR) | RES_RAM_20K); 
      Serial.println("PASS"); 
    } else Serial.println("FAIL (Crash)");
  }
  
  // 30. RAM 64K
  else if (state == 30) {
    pet();
    Serial.print("3. Probing RAM 64KB. "); Serial.flush();
    setReg(BKP_STATE_ADDR, 35);
    if(probeMem(0x2000F000, false)) { 
      setReg(BKP_RES1_ADDR, getReg(BKP_RES1_ADDR) | RES_RAM_64K); 
      Serial.println("PASS"); 
    } else Serial.println("FAIL");
  }

  // 35. FLASH GHOST
  else if (state == 35) {
    pet();
    Serial.print("4. Probing 128KB Flash. "); Serial.flush();
    setReg(BKP_STATE_ADDR, 40);
    if(probeMem(0x08010000, true)) { 
      setReg(BKP_RES1_ADDR, getReg(BKP_RES1_ADDR) | RES_FLASH_128K); 
      Serial.println("PASS"); 
    } else Serial.println("FAIL");
  }

  // 40. TIMER LOOP INIT
  else if (state == 40) {
    setReg(BKP_STATE_ADDR, 100); 
  }
  
  // 100+. TIMER EXECUTION
  else if (state >= 100 && state < (100 + NUM_TIMERS)) {
    pet();
    int tIdx = state - 100;
    Serial.print("5."); Serial.print(tIdx+1); 
    Serial.print(" Probing "); Serial.print(TIMERS[tIdx].name); Serial.print("... ");
    Serial.flush();
    
    setReg(BKP_STATE_ADDR, state + 1);
    
    if (probeTimHW(tIdx)) {
      // Store success bit in RES2
      setReg(BKP_RES2_ADDR, getReg(BKP_RES2_ADDR) | (1 << tIdx));
      Serial.println("PASS");
    } else {
      Serial.println("FAIL");
    }
  }

  // DONE - VERBOSE REPORT
  else {
    pet();
    uint32_t r1 = getReg(BKP_RES1_ADDR);
    uint32_t r2 = getReg(BKP_RES2_ADDR);

   // Serial.println("\n==================================");
    Serial.println("    FINAL FORENSIC REPORT v3.0    ");
  //  Serial.println("==================================");
    
    // 1. CHIP IDENTITY
    Serial.print("Core Architecture: "); 
    if (isArchF4) Serial.println("Cortex-M4 (STM32F4)"); 
    else Serial.println("Cortex-M3 (STM32F1)");
    
    Serial.print("CPUID Register:    0x"); Serial.println(SCB_CPUID, HEX);
    
    // 2. MEMORY
    Serial.println("\n[ MEMORY ]");
    Serial.print("Registered Flash:  "); Serial.print(*FLASH_SIZE_REG); Serial.println(" KB");
    Serial.print("Detected Flash:    "); 
    if(r1 & RES_FLASH_128K) Serial.println("128 KB (Hidden capacity found)");
    else Serial.println("Matches Registered Size");

    Serial.print("Detected SRAM:     ");
    if(r1 & RES_RAM_64K) Serial.println("64 KB (High/Med Density)");
    else if(r1 & RES_RAM_20K) Serial.println("20 KB (Standard C8)");
    else Serial.println("10 KB (Low Density C6)");

    // 3. PERIPHERALS
    Serial.println("\n[ PERIPHERALS ]");
    Serial.print("Hardware RNG:      ");
    if(r1 & RES_RNG_OK) Serial.println("PRESENT (GigaDevice/F4)");
    else Serial.println("NOT PRESENT");

    Serial.println("Hardware Timers:");
    int tCount = 0;
    for(int i=0; i<NUM_TIMERS; i++) {
      if(r2 & (1 << i)) {
        Serial.print(" ["); Serial.print(TIMERS[i].name); Serial.print("]");
        tCount++;
        if(tCount % 5 == 0) Serial.println();
      }
    }
    if(tCount==0) Serial.print(" None Detected!");
    Serial.println();

    // 4. CONCLUSION
    Serial.println("\n[ CONCLUSION ]");
    Serial.print("Identity: ");
    if(isArchF4) Serial.println("STM32F4 Series");
    else if(r1 & RES_RNG_OK) Serial.println("GigaDevice GD32F103 (Performance)");
    else if((r2 & (1<<3)) == 0) Serial.println("CKS/CS32 F103C6 (Low Density Clone)"); // TIM4 missing
    else if(r1 & RES_RAM_64K) Serial.println("STM32F103 High Density (RC/ZE/VE)");
    else Serial.println("STM32F103C8 (Standard Medium Density)");

    Serial.println("\n----------------------------------");
    Serial.println("Process Complete. System Halted.");
    Serial.println("Send 'r' to restart scan.");
    
    // STATIC WAIT LOOP
    while(1) {
      pet(); // Keep watchdog alive
      if(Serial.available()) {
        char c = Serial.read();
        if(c == 'r') {
           setReg(BKP_STATE_ADDR, 0); 
           setReg(BKP_RES1_ADDR, 0); 
           setReg(BKP_RES2_ADDR, 0);
           NVIC_SystemReset();
        }
      }
      delay(100);
    }
  }
}

void loop() {}

