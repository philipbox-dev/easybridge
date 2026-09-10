#include "spi_bus.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include <string.h>

static const char *TAG = "SPIBUS";

#define MAX_HOSTS 3

static struct {
    bool inited;
    int  sck, mosi, miso;
    int  max_transfer;
} s_bus[MAX_HOSTS];

esp_err_t spi_bus_acquire(uint8_t host, int sck, int mosi, int miso,
                          int max_transfer_sz, spi_host_device_t *out_host)
{
    // У C6/C3 периферия SPI одна: просить второй хост там некуда,
    // и молча свалиться в SPI2 честнее, чем не собраться вовсе.
#if SOC_SPI_PERIPH_NUM > 2
    spi_host_device_t h = (host == 2) ? SPI3_HOST : SPI2_HOST;
#else
    if (host == 2) {
        ESP_LOGW(TAG, "у этого чипа только один SPI — SPI3 сведён в SPI2");
        host = 1;
    }
    spi_host_device_t h = SPI2_HOST;
#endif
    if (out_host) *out_host = h;
    int idx = (host == 2) ? 2 : 1;

    if (s_bus[idx].inited) {
        // Та же шина, те же пины — просто подключаемся к ней.
        if (s_bus[idx].sck == sck && s_bus[idx].mosi == mosi &&
            s_bus[idx].miso == miso) {
            if (max_transfer_sz > s_bus[idx].max_transfer) {
                ESP_LOGW(TAG, "SPI%d поднят с потолком %d байт, запрошено %d — "
                              "режь посылки по spi_bus_max_transfer()",
                         idx + 1, s_bus[idx].max_transfer, max_transfer_sz);
            }
            return ESP_OK;
        }
        ESP_LOGE(TAG, "SPI%d уже поднят на SCK=%d MOSI=%d MISO=%d, "
                      "запрошен SCK=%d MOSI=%d MISO=%d",
                 idx + 1, s_bus[idx].sck, s_bus[idx].mosi, s_bus[idx].miso,
                 sck, mosi, miso);
        return ESP_ERR_INVALID_STATE;
    }

    spi_bus_config_t cfg = {
        .mosi_io_num     = mosi,
        .miso_io_num     = miso,
        .sclk_io_num     = sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = max_transfer_sz,
    };
    esp_err_t e = spi_bus_initialize(h, &cfg, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SPI%d init failed: %s", idx + 1, esp_err_to_name(e));
        return e;
    }
    s_bus[idx].inited = true;
    s_bus[idx].sck = sck; s_bus[idx].mosi = mosi; s_bus[idx].miso = miso;
    s_bus[idx].max_transfer = max_transfer_sz;
    ESP_LOGI(TAG, "SPI%d поднят: SCK=%d MOSI=%d MISO=%d", idx + 1, sck, mosi, miso);
    return ESP_OK;
}

int spi_bus_max_transfer(uint8_t host)
{
    int idx = (host == 2) ? 2 : 1;
    return s_bus[idx].inited ? s_bus[idx].max_transfer : 0;
}
