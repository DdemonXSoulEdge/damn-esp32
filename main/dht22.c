#include "dht22.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "DHT22";
static gpio_num_t dht_gpio;

#define DHT_TIMEOUT_US  1000

static inline int wait_for_level(int level, uint32_t timeout_us)
{
    uint32_t start = esp_timer_get_time();
    while (gpio_get_level(dht_gpio) == level) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return -1; 
        }
    }
    return 0;
}

static int dht22_read_raw(int *humidity, int *temperature)
{
    uint8_t data[5] = {0};

    gpio_set_direction(dht_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(dht_gpio, 1);
    esp_rom_delay_us(10);
    gpio_set_direction(dht_gpio, GPIO_MODE_INPUT);

    // Start signal
    gpio_set_direction(dht_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(dht_gpio, 0);
    esp_rom_delay_us(1200);        // 1.2 ms
    gpio_set_level(dht_gpio, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(dht_gpio, GPIO_MODE_INPUT);

    // DHT pull low->high handshake
    if (wait_for_level(1, DHT_TIMEOUT_US) < 0) return -1;
    if (wait_for_level(0, DHT_TIMEOUT_US) < 0) return -1;
    if (wait_for_level(1, DHT_TIMEOUT_US) < 0) return -1;

    // Read 40 bits
    for (int i = 0; i < 40; i++) {
        if (wait_for_level(0, DHT_TIMEOUT_US) < 0) return -1;

        uint32_t t = esp_timer_get_time();

        if (wait_for_level(1, DHT_TIMEOUT_US) < 0) return -1;

        if ((esp_timer_get_time() - t) > 50)
            data[i/8] |= (1 << (7 - (i % 8)));
    }

    // checksum
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        return -2;
    }

    *humidity = (data[0] << 8) | data[1];
    *temperature = (data[2] << 8) | data[3];
    return 0;
}

void dht22_init(gpio_num_t gpio)
{
    dht_gpio = gpio;
    gpio_set_direction(dht_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(dht_gpio, 1);
}

int dht22_read(float *temperature, float *humidity)
{
    int raw_h, raw_t;
    int res = dht22_read_raw(&raw_h, &raw_t);

    if (res < 0) return res;

    *humidity = raw_h / 10.0f;
    *temperature = raw_t / 10.0f;

    return 0;
}
