#include "view_management.h"
#include <Arduino.h>

Menu* create_view(const char* title, void (*function)(void*,void*,void*,void*), Menu* next) {
    Menu* menu = (Menu*)malloc(sizeof(Menu));
    menu->title = title;
    menu->function = function;
    menu->next = next;

    return menu;
}



