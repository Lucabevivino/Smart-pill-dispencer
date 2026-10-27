#include <Arduino.h>
#include <LiquidCrystal.h>
#include "view_management.h"
#include "node_management.h"
#include "protocol.h"
#include "hub_comms.h"

// --- Network / time configuration (edit these) -------------------------------
#define WIFI_SSID          "your-ssid"
#define WIFI_PASSWORD      "your-password"
#define TZ_GMT_OFFSET_SEC  3600    // e.g. +3600 = UTC+1 (CET)
#define TZ_DST_OFFSET_SEC  3600    // daylight saving, 0 if unused


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
#define LCD_RS  23
#define LCD_EN  22
#define LCD_D4  21
#define LCD_D5  19
#define LCD_D6  18
#define LCD_D7  4

#define LCD_COLS 16
#define LCD_ROWS 2

#define VIEW_1 "List of pills"
#define VIEW_2 "Add a pill"
#define VIEW_3 "Refill a pill"
#define VIEW_4 "Remove a pill"
#define VIEW_5 "Daily schedule"

// Navigation buttons (active-low: button -> GND, internal pull-up).
#define BUTTON_UP      13   // previous view / go back (inside a view)
#define BUTTON_DOWN    14   // next view
#define BUTTON_SELECT  2    // enter the highlighted view


#define MAX_NAME_LEN 5

// day_schedule / week_schedule come from protocol.h (shared with the Clip).

struct pill_data {
    char name[MAX_NAME_LEN + 1];   // nickname + terminator
    uint8_t taken;
    uint8_t total;
    uint8_t remaining;
    uint8_t node_id;               // 1-based clip id (0 = unassigned)
    week_schedule schedule;
};

const char* const DAY_NAMES[DAYS_PER_WEEK] =
    {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

pill_data* pill_list = NULL;
pill_data  pending_pill;   // pill assembled during "Add a pill", before node association
char view_1[15] = "";
char view_2[15] = "";
int pill_registered = 0;

// --- Pill list helpers ---

void pill_list_add(const pill_data* pill) {
    pill_list = (pill_data*)realloc(pill_list, (pill_registered + 1) * sizeof(pill_data));
    pill_list[pill_registered] = *pill;
    pill_registered++;
}

void pill_list_remove(int index) {
    for (int i = index; i < pill_registered - 1; i++) {
        pill_list[i] = pill_list[i + 1];
    }
    pill_registered--;
}

int total_intakes() {
    int total = 0;
    for (int p = 0; p < pill_registered; p++) {
        for (int d = 0; d < DAYS_PER_WEEK; d++) {
            total += pill_list[p].schedule.days[d].count;
        }
    }
    return total;
}

bool intake_at(int idx, int* day, int* hour, int* minute, const char** name) {
    for (int p = 0; p < pill_registered; p++) {
        for (int d = 0; d < DAYS_PER_WEEK; d++) {
            int c = pill_list[p].schedule.days[d].count;
            if (idx < c) {
                *day = d;
                *hour = pill_list[p].schedule.days[d].hours[idx];
                *minute = pill_list[p].schedule.days[d].minutes[idx];
                *name = pill_list[p].name;
                return true;
            }
            idx -= c;
        }
    }
    return false;
}

LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

// Defined further down; used by the view functions above.
void configure_schedule(week_schedule* schedule);
char read_buttons();
int  select_value(const char* prompt, int value, int min, int max, int step);
bool confirm_yes_no(const char* prompt);
void enter_name(char* out);
int  select_pill(const char* title);

// All view functions share the Menu function-pointer type.
void function1(void* arg1, void* arg2, void* arg3, void* arg4) {
    if (pill_registered == 0) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("No pills yet");
        delay(1500);
        return;
    }

    int selected = 0;
    while (true) {
        pill_data* pill = &pill_list[selected];

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("> ");
        lcd.print(pill->name);

        lcd.setCursor(0, 1);
        lcd.print("Tkn ");
        lcd.print(pill->taken);
        lcd.print("/");
        lcd.print(pill->total);
        lcd.print(" Lft ");
        lcd.print(pill->remaining);

        char btn = read_buttons();
        if (btn == 'U') {
            selected = (selected - 1 + pill_registered) % pill_registered;
        } else if (btn == 'D') {
            selected = (selected + 1) % pill_registered;
        } else {   // SELECT exits the pill browser
            return;
        }
    }
}

void function2(void* arg1, void* arg2, void* arg3, void* arg4) {
    // 1) Find a free clip (node) to associate with the new pill.
    int node_id = node_reserve_free();
    if (node_id == 0) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("No free clip");
        lcd.setCursor(0, 1);
        lcd.print("Remove a pill");
        return;
    }

    // 2) Make the clip blink so the user knows which box to attach, then wait
    //    for them to clip the box on.
    hub_comms_blink(node_id, 10);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Use the blinking");
    lcd.setCursor(0, 1);
    lcd.print("clip #");
    lcd.print(node_id);
    while (read_buttons() != 'S') {}   // SELECT = box clipped, continue

    // 3) Pill identity and contents.
    enter_name(pending_pill.name);
    if (pending_pill.name[0] == '\0') {
        snprintf(pending_pill.name, MAX_NAME_LEN + 1, "Clip%d", node_id);
    }
    pending_pill.node_id = (uint8_t)node_id;
    pending_pill.total = (uint8_t)select_value("Pills per box", 30, 1, 99, 1);
    pending_pill.remaining = pending_pill.total;
    pending_pill.taken = 0;

    // 4) Configure the pill's weekly schedule.
    configure_schedule(&pending_pill.schedule);

    // 5) Send the node its id, schedule and the current time, then store the
    //    pill in the list.
    if (!hub_comms_assign(node_id, &pending_pill.schedule)) {
        node_release(node_id);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Clip offline");
        lcd.setCursor(0, 1);
        lcd.print("Try again");
        return;
    }
    pill_list_add(&pending_pill);

    // 6) Confirmation.
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Pill was added!");
}

void function3(void* arg1, void* arg2, void* arg3, void* arg4) {
    int index = select_pill("Refill");
    if (index < 0) return;   // no pills

    pill_data* pill = &pill_list[index];

    // Tell the user to clip the new box onto this pill's blinking clip.
    // (The ESP-NOW "start blinking" message is not implemented yet.)
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Clip new box on");
    lcd.setCursor(0, 1);
    lcd.print("clip #");
    lcd.print(pill->node_id);
    while (read_buttons() != 'S') {}   // SELECT = new box clipped

    pill->remaining = pill->total;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Refilled!");
}

void function4(void* arg1, void* arg2, void* arg3, void* arg4) {
    int index = select_pill("Remove");
    if (index < 0) return;   // no pills

    pill_data* pill = &pill_list[index];

    char prompt[17];
    snprintf(prompt, sizeof(prompt), "Remove %s?", pill->name);
    if (!confirm_yes_no(prompt)) {
        return;
    }

    hub_comms_remove(pill->node_id);   // tell the clip it has been removed
    node_release(pill->node_id);       // free the clip slot
    pill_list_remove(index);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Removed!");
}

void function5(void* arg1, void* arg2, void* arg3, void* arg4) {
    int total = total_intakes();
    if (total == 0) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("No schedule yet");
        return;
    }

    int idx = 0;
    while (true) {
        int day, hour, minute;
        const char* name;
        intake_at(idx, &day, &hour, &minute, &name);

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(DAY_NAMES[day]);
        lcd.print(" ");
        lcd.print(name);

        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
        lcd.setCursor(0, 1);
        lcd.print(buf);

        char btn = read_buttons();
        if (btn == 'U') {
            idx = (idx - 1 + total) % total;
        } else if (btn == 'D') {
            idx = (idx + 1) % total;
        } else {   // SELECT exits the schedule browser
            return;
        }
    }
}

Menu* setup_views(){

    Menu* view5 = create_view(VIEW_5, function5, NULL);
    Menu* view4 = create_view(VIEW_4, function4, view5);
    Menu* view3 = create_view(VIEW_3, function3, view4);
    Menu* view2 = create_view(VIEW_2, function2, view3);
    Menu* view1 = create_view(VIEW_1, function1, view2);

    return view1;
}

// Block until one of the three buttons is pressed and released.
// Returns 'U' (up), 'D' (down) or 'S' (select).
char read_buttons() {
    while (true) {
        hub_comms_loop();   // keep comms alive while the UI blocks on buttons

        if (digitalRead(BUTTON_UP) == LOW) {
            delay(50);                                      // debounce
            while (digitalRead(BUTTON_UP) == LOW) delay(10); // wait for release
            return 'U';
        }
        if (digitalRead(BUTTON_DOWN) == LOW) {
            delay(50);
            while (digitalRead(BUTTON_DOWN) == LOW) delay(10);
            return 'D';
        }
        if (digitalRead(BUTTON_SELECT) == LOW) {
            delay(50);
            while (digitalRead(BUTTON_SELECT) == LOW) delay(10);
            return 'S';
        }
        delay(10);
    }
}

// ---------------------------------------------------------------------------
// Schedule-configuration UI — UP/DOWN change a value, SELECT confirms.
// ---------------------------------------------------------------------------

// Adjust an integer value with UP/DOWN (wrapping within [min, max]) and confirm
// with SELECT. The prompt is drawn on line 0, the live value on line 1.
int select_value(const char* prompt, int value, int min, int max, int step) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(prompt);

    while (true) {
        lcd.setCursor(0, 1);
        lcd.print("                ");   // clear the 16-char line
        lcd.setCursor(0, 1);
        lcd.print("> ");
        lcd.print(value);

        char btn = read_buttons();
        if (btn == 'U') {
            value += step;
            if (value > max) value = min;
        } else if (btn == 'D') {
            value -= step;
            if (value < min) value = max;
        } else {  // SELECT
            return value;
        }
    }
}

// Ask a yes/no question. UP/DOWN toggles the answer, SELECT confirms.
bool confirm_yes_no(const char* prompt) {
    bool answer = true;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(prompt);

    while (true) {
        lcd.setCursor(0, 1);
        lcd.print("                ");
        lcd.setCursor(0, 1);
        lcd.print(answer ? "Yes" : "No");

        char btn = read_buttons();
        if (btn == 'U' || btn == 'D') {
            answer = !answer;
        } else {  // SELECT
            return answer;
        }
    }
}

// Enter a nickname of up to MAX_NAME_LEN chars. Each slot is cycled with
// UP/DOWN; SELECT advances to the next slot and, after the last one, finishes.
// Trailing spaces are trimmed.
void enter_name(char* out) {
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    const int charset_len = sizeof(charset) - 1;

    char name[MAX_NAME_LEN + 1];
    for (int i = 0; i < MAX_NAME_LEN; i++) name[i] = ' ';
    name[MAX_NAME_LEN] = '\0';

    int pos = 0;
    while (true) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Name:");
        lcd.print(name);
        lcd.setCursor(5 + pos, 1);
        lcd.print("^");

        char btn = read_buttons();
        if (btn == 'U' || btn == 'D') {
            int idx = 0;
            while (charset[idx] != name[pos]) idx++;
            idx += (btn == 'D') ? 1 : -1;
            if (idx < 0) idx = charset_len - 1;
            if (idx >= charset_len) idx = 0;
            name[pos] = charset[idx];
        } else {   // SELECT
            pos++;
            if (pos >= MAX_NAME_LEN) break;
        }
    }

    int end = MAX_NAME_LEN;
    while (end > 0 && name[end - 1] == ' ') end--;
    for (int i = 0; i < end; i++) out[i] = name[i];
    out[end] = '\0';
}

// Navigate the pill list with UP/DOWN; SELECT returns the chosen index.
// Shows a message and returns -1 if there are no pills.
int select_pill(const char* title) {
    if (pill_registered == 0) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("No pills yet");
        delay(1500);
        return -1;
    }

    int selected = 0;
    while (true) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(title);
        lcd.setCursor(0, 1);
        lcd.print("> ");
        lcd.print(pill_list[selected].name);

        char btn = read_buttons();
        if (btn == 'U') {
            selected = (selected - 1 + pill_registered) % pill_registered;
        } else if (btn == 'D') {
            selected = (selected + 1) % pill_registered;
        } else {   // SELECT
            return selected;
        }
    }
}

// Configure the intake schedule for a single day. `day_abbr` ("Mon", "All", …)
// is prefixed onto each prompt so the user knows which day is being edited.
void configure_day(day_schedule* day, const char* day_abbr) {
    char prompt[17];

    snprintf(prompt, sizeof(prompt), "%s times/day", day_abbr);
    day->count = (uint8_t)select_value(prompt, 1, 0, MAX_INTAKES_PER_DAY, 1);

    for (int i = 0; i < day->count; i++) {
        snprintf(prompt, sizeof(prompt), "%s d%d hour", day_abbr, i + 1);
        day->hours[i] = (uint8_t)select_value(prompt, 8, 0, 23, 1);

        snprintf(prompt, sizeof(prompt), "%s d%d min", day_abbr, i + 1);
        day->minutes[i] = (uint8_t)select_value(prompt, 0, 0, 55, 5);
    }
}

// Configure a full week. If every day shares the same schedule it is entered
// once and copied across all seven days; otherwise each day is edited in turn.
void configure_schedule(week_schedule* schedule) {
    if (confirm_yes_no("Same every day?")) {
        day_schedule common = {0};
        configure_day(&common, "All");
        for (int d = 0; d < DAYS_PER_WEEK; d++) {
            schedule->days[d] = common;
        }
    } else {
        for (int d = 0; d < DAYS_PER_WEEK; d++) {
            configure_day(&schedule->days[d], DAY_NAMES[d]);
        }
    }
}

// Draw the highlighted view on the bottom row.
void show_selected(Menu* views[], int count, int selected) {
    lcd.setCursor(0, 1);
    lcd.print("                ");   // clear the 16-char line
    lcd.setCursor(0, 1);
    lcd.print(">");
    lcd.print(views[selected]->title);
}

// Inside a view: wait for the "back" gesture (UP) to return to the menu.
// SELECT is left free for future in-view actions.
void wait_for_back() {
    while (true) {
        if (read_buttons() == 'U') {
            return;
        }
        // DOWN and SELECT do nothing inside a view for now.
    }
}

// Navigate the views with UP/DOWN and enter the highlighted view with SELECT.
void navigate_views(Menu* head) {
    // Flatten the linked list into an array for indexing.
    Menu* views[MAX_ROWS];
    int count = 0;
    for (Menu* m = head; m != NULL && count < MAX_ROWS; m = m->next) {
        views[count++] = m;
    }
    if (count == 0) {
        return;
    }

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Select a view");

    int selected = 0;
    show_selected(views, count, selected);

    while (true) {
        char btn = read_buttons();

        if (btn == 'U') {
            selected = (selected - 1 + count) % count;
            show_selected(views, count, selected);
        } else if (btn == 'D') {
            selected = (selected + 1) % count;
            show_selected(views, count, selected);
        } else {  // 'S' -> enter the highlighted view
            views[selected]->function(NULL, NULL, NULL, NULL);
            wait_for_back();

            // Back in the menu: restore the header + highlight.
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Select a view");
            show_selected(views, count, selected);
        }
    }
}

Menu* menu_head = NULL;

void setup() {
    Serial.begin(115200);

    pinMode(BUTTON_UP, INPUT_PULLUP);     // buttons -> GND, internal pull-up
    pinMode(BUTTON_DOWN, INPUT_PULLUP);
    pinMode(BUTTON_SELECT, INPUT_PULLUP);

    lcd.begin(LCD_COLS, LCD_ROWS);   // initialise in 4-bit mode
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Smart pill");
    lcd.setCursor(0, 1);
    lcd.print("hub ready");
    delay(1500);

    // Bring up WiFi + NTP + ESP-NOW (blocks while connecting).
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Connecting WiFi");
    lcd.setCursor(0, 1);
    lcd.print("Please wait");
    if (!hub_comms_begin(WIFI_SSID, WIFI_PASSWORD,
                         TZ_GMT_OFFSET_SEC, TZ_DST_OFFSET_SEC,
                         15000, 10000)) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Comms failed");
        lcd.setCursor(0, 1);
        lcd.print("Restart hub");
        delay(2000);
    }

    menu_head = setup_views();
}

void loop() {
    hub_comms_loop();   // beacons, NTP, peer registration, time requests

    if (menu_head != NULL) {
        navigate_views(menu_head);
    }
}
