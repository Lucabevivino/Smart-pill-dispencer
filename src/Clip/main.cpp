#include <Arduino.h>
#include <LiquidCrystal.h>
#include "view_management.h"


// ---------------------------------------------------------------------------
// 1602 parallel (16-pin) HD44780 LCD — 4-bit mode.
//
// LCD pin  1  VSS -> GND
// LCD pin  2  VDD -> +3.3 V (or +5 V — see note below)
// LCD pin  3  Vo  -> wiper of a 10 kOhm contrast pot (VDD..GND)
// LCD pin  4  RS  -> LCD_RS
// LCD pin  5  RW  -> GND
// LCD pin  6  E   -> LCD_EN
// LCD pin 11  D4  -> LCD_D4
// LCD pin 12  D5  -> LCD_D5
// LCD pin 13  D6  -> LCD_D6
// LCD pin 14  D7  -> LCD_D7
// LCD pin 15  A   -> VDD through a resistor (backlight anode)
// LCD pin 16  K   -> GND (backlight cathode)
//
// NOTE: this is a 3.3 V MCU. The module usually works at 3.3 V VDD (adjust the
// contrast pot); if you power it at 5 V instead, level-shift RS/E/D4-D7 up to
// 5 V logic.
// ---------------------------------------------------------------------------
#define LCD_RS  26
#define LCD_EN  25
#define LCD_D4  33
#define LCD_D5  32
#define LCD_D6  27
#define LCD_D7  14

#define LCD_COLS 16
#define LCD_ROWS 2

#define MENU_1 "List of pills"
#define MENU_2 "Add a pill"
#define MENU_3 "Refill a pill"
#define MENU_4 "Remove a pill"
#define MENU_5 "Daily schedule"
#define MENU_6 "New pill"


LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
// void function1(void* arg1, void* arg2, void* arg3, void* arg4) {
// }
// void function2(void* arg1, void* arg2, void* arg3, void* arg4) {
// }
// void function3(void* arg1, void* arg2, void* arg3, void* arg4) {
// }
// void function4(void* arg1, void* arg2, void* arg3, void* arg4) {
// }
// void function5(void* arg1, void* arg2, void* arg3, void* arg4) {
// }
// void function6(void* arg1, void* arg2, void* arg3, void* arg4) {
// }

// Menu* set_munues(){
//     Menu* menu6 = create_view(MENU_6, function6, NULL, 0);
//     Menu* menu5 = create_view(MENU_5, function5, NULL, 0);
//     Menu* menu4 = create_view(MENU_4, function4, NULL, 0);
//     Menu* menu3 = create_view(MENU_3, function3, NULL, 0);
//     Menu* menu2 = create_view(MENU_2, function2, NULL, 0);
//     Menu* menu1 = create_view(MENU_1, function1, children1, 5);

//     return menu1;
// }
void setup() {
    Serial.begin(115200);

    lcd.begin(LCD_COLS, LCD_ROWS);   // initialise in 4-bit mode
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Smart pill");
    lcd.setCursor(0, 1);
    lcd.print("hub ready");
}

void loop() {
    
}
