#ifndef VIEW_MANAGEMENT_H
#define VIEW_MANAGEMENT_H

#define MAX_ROWS 20
#define MAX_COLS 10

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Menu {
    const char* title;
    void (*function) (void*,void*,void*,void*);
    struct Menu* next;
} Menu;


struct Menu_table{
    char data[MAX_ROWS][MAX_COLS];
    int rows;
    int cols;
};

Menu* create_view(const char* title, void (*function)(void*,void*,void*,void*), Menu* next);

#ifdef __cplusplus
}
#endif

#endif // VIEW_MANAGEMENT_H
