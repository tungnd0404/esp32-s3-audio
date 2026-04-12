#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sdcard.h"
#include "menu.h"
#include "button.h"
#include "double_buffer.h"
#include <strings.h> 

// =========================
// Global variables
// =========================
static const char *TAG = "SDCARD";

/* ============================
     Mount SD 1-bit mode
   ============================
 mount sd card sử dụng SD 1-Bit */
esp_err_t sdcard_mount(void)
{
    esp_err_t ret;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.width = 1;

    slot_config.clk = GPIO_NUM_39;
    slot_config.cmd = GPIO_NUM_38;
    slot_config.d0  = GPIO_NUM_40;

    slot_config.d1 = -1;
    slot_config.d2 = -1;
    slot_config.d3 = -1;

    esp_vfs_fat_sdmmc_mount_config_t mount_config =
    {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;

    ret = esp_vfs_fat_sdmmc_mount("/sdcard",
                                  &host,
                                  &slot_config,
                                  &mount_config,
                                  &card);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG,"SD mount failed");
        return ret;
    }

    ESP_LOGI(TAG,"SD card mounted");

    sdmmc_card_print_info(stdout, card);

    return ESP_OK;
}

// ================= RAM =================
song_info_t song_list[MAX_SONGS];
uint16_t song_count = 0;

// ================= INTERNAL =================
static int has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;

    return strcasecmp(dot, ext) == 0;
}

static void remove_ext(char *dest, const char *src)
{
    strcpy(dest, src);
    char *dot = strrchr(dest, '.');
    if (dot) *dot = '\0';
}

// ================= MAIN =================
void scan_sdcard_and_create_db(const char *base_path)
{
    DIR *dir = opendir(base_path);
    if (!dir) {
        printf("Cannot open dir\n");
        return;
    }

    FILE *db = fopen("/sdcard/songs.db", "wb");
    if (!db) {
        printf("Cannot open db file\n");
        closedir(dir);
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && song_count < MAX_SONGS)
    {
        printf("Found: %s\n", entry->d_name);
        // chỉ lấy file .mp3
        if (!has_ext(entry->d_name, ".mp3"))
            continue;

        char name[64];
        remove_ext(name, entry->d_name);

        char mp3_path[128];
        char bin_path[128];

        snprintf(mp3_path, sizeof(mp3_path), "%s/%s.mp3", base_path, name);
        snprintf(bin_path, sizeof(bin_path), "%s/%s.bin", base_path, name);

        // kiểm tra có file .bin tương ứng
        FILE *f = fopen(bin_path, "rb");
        if (!f) {
            printf("Missing bin for %s\n", name);
            continue;
        }
        fclose(f);

        // ghi DB
        song_db_t record;
        memset(&record, 0, sizeof(record));

        strcpy(record.song_path, mp3_path);
        strcpy(record.frame_path, bin_path);

        fwrite(&record, sizeof(song_db_t), 1, db);

        // lưu RAM
        strcpy(song_list[song_count].song_name, name);
        song_list[song_count].song_index = song_count;

        printf("Added: %s\n", name);

        song_count++;
    }

    fclose(db);
    closedir(dir);

    printf("Total songs: %d\n", song_count);
}

// ================= READ DB =================
void read_db_file(void)
{
    FILE *db = fopen("/sdcard/songs.db", "rb");
    if (!db) {
        printf("Cannot open db\n");
        return;
    }

    song_db_t record;

    while (fread(&record, sizeof(song_db_t), 1, db) == 1)
    {
        printf("MP3: %s\n", record.song_path);
        printf("BIN: %s\n", record.frame_path);
    }

    fclose(db);
}

int get_song_by_index(uint16_t index, song_db_t *out)
{
    if (index >= song_count || out == NULL)
        return -1;

    FILE *db = fopen("/sdcard/songs.db", "rb");
    if (!db) {
        printf("Open DB failed\n");
        return -1;
    }

    // nhảy tới vị trí index
    long offset = index * sizeof(song_db_t);

    if (fseek(db, offset, SEEK_SET) != 0) {
        printf("fseek failed\n");
        fclose(db);
        return -1;
    }

    // đọc 1 record
    if (fread(out, sizeof(song_db_t), 1, db) != 1) {
        printf("fread failed\n");
        fclose(db);
        return -1;
    }

    fclose(db);
    return 0;
}

void sdcard_task(void *pvParameters) 
{    
    song_db_t song;

    double_buffer_init();

    while(1) {

        // đợi play
        xEventGroupWaitBits(system_event_group, SYSTEM_PLAYING_NEW || SYSTEM_NEXT || SYSTEM_PREV, pdTRUE, pdFALSE, portMAX_DELAY);
        // lấy đường dẫn song
        get_song_by_index(cursor, &song);

        double_buffer_close();

        double_buffer_open(song.frame_path);

        while (1) 
        {
            EventBits_t bits_system_event_group = xEventGroupGetBits(system_event_group);

            // THOÁT NGAY nếu có SYSTEM_PLAYING_NEW / SYSTEM_NEXT / SYSTEM_PREV
            if ((bits_system_event_group & SYSTEM_PLAYING_NEW) || (bits_system_event_group & SYSTEM_NEXT) || (bits_system_event_group & SYSTEM_PREV))
            {
                break;
            }

            EventBits_t bits_double_buffer_event = xEventGroupWaitBits(double_buffer_event,
                                               EVT_PRELOAD | EVT_LOAD_MISS,
                                               pdTRUE,
                                               pdFALSE,
                                               portMAX_DELAY);
            if (bits & (EVT_PRELOAD | EVT_LOAD_MISS)) 
            {
                sd_task_load_double_buffer();
            }    
        }    
    }
}