/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "sdmmc.h"
#include "sdmmc_config.h"
#include "driver/sdmmc_host.h"

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

esp_err_t Sdmmc_Init(void)
{
    /* gSdmmcHost đã có giá trị đúng ngay từ khai báo (sdmmc_config.c, dùng thẳng
       SDMMC_HOST_DEFAULT()) - không cần gán lại ở đây. Chỉ còn gSdmmcSlotConfig cần áp macro
       mặc định rồi ghi đè theo board, xem lý do trong sdmmc_config.c */
    gSdmmcSlotConfig = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();

    /* Độ rộng bus theo cấu hình board (xem SDMMC_BUS_WIDTH, sdmmc_config.h) - 1-bit thì
       không dùng các chân data D1/D2/D3 */
    gSdmmcSlotConfig.width = SDMMC_BUS_WIDTH;
    gSdmmcSlotConfig.d1 = -1;
    gSdmmcSlotConfig.d2 = -1;
    gSdmmcSlotConfig.d3 = -1;

    /* Cấu hình chân GPIO thật của board */
    gSdmmcSlotConfig.clk = SDMMC_CLK_PIN;
    gSdmmcSlotConfig.cmd = SDMMC_CMD_PIN;
    gSdmmcSlotConfig.d0 = SDMMC_D0_PIN;

    return ESP_OK;
}

esp_err_t Sdmmc_Mount(const char *mountPoint, const esp_vfs_fat_sdmmc_mount_config_t *pMountConfig,
                      sdmmc_card_t **ppCard)
{
    return esp_vfs_fat_sdmmc_mount(mountPoint, &gSdmmcHost, &gSdmmcSlotConfig, pMountConfig, ppCard);
}
