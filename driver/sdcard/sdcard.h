#ifndef SDCARD_H
#define SDCARD_H

#include <stdint.h>

#define MAX_SONGS 200

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char song_path[128];
    char frame_path[128];
} song_db_t;

typedef struct {
    char song_name[64];
    uint8_t song_index;
} song_info_t;

// RAM
extern song_info_t song_list[MAX_SONGS];
extern uint16_t song_count;

// API
esp_err_t sdcard_mount(void);
void scan_sdcard_and_create_db(const char *base_path);
void read_db_file(void);
int get_song_by_index(uint16_t index, song_db_t *out);
void sdcard_task(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // SDCARD_H