#include "hal/gpio_types.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/gpio.h>

#define DHT_PIN GPIO_NUM_32

#define RET_ERR(ret) if (ESP_OK != ret) {\
  printf("DHT ***** error at %s:%d -> %d\n", __FILE__, __LINE__, ret); \
  vTaskDelete(NULL); \
  return; }

enum DHT_Status {
  DHT_STATUS_OK,
  DHT_STATUS_BAD_CHECKSUM,
  DHT_STATUS_INVALID
};

struct DHT_Info {
  float temperature =0.0;
  float relative_humidity =0.0;
  uint64_t polling_interval = pdMS_TO_TICKS(10000);
  DHT_Status status = DHT_STATUS_INVALID;
};

DHT_Info dht_info;

int dht_isr_call_count = 0;
uint64_t dht_pin_raise_time;
bool dht_wait_init_pulse = true;
bool dht_pin_bit_started = false;
uint8_t dht_byte = 0;
uint8_t dht_bits = 0;
QueueHandle_t dht_queue;

void clear_isr_state() {
  dht_isr_call_count = 0;
  dht_wait_init_pulse = true;
  dht_pin_bit_started = false;
  dht_byte = 0;
  dht_bits = 0;
  xQueueReset(dht_queue);
}

void IRAM_ATTR dht_isr_handler(void *) {
  BaseType_t taskWoken = pdFALSE;
  dht_isr_call_count++;
  auto dht_pin_level = gpio_get_level(DHT_PIN);
  if (0 == dht_pin_level) {
    if (dht_pin_bit_started) {
      char bit = ((esp_timer_get_time() - dht_pin_raise_time) > 50) ? 0x01:0x00;
      xQueueSendFromISR(dht_queue, &bit, &taskWoken);
    }
  } else {
    // raising edge
    if (dht_wait_init_pulse) {
      dht_wait_init_pulse = false;
    } else {
      dht_pin_raise_time = esp_timer_get_time();
      dht_pin_bit_started = true;
    }
  }
  if (taskWoken) {
    portYIELD_FROM_ISR();
  }
}

void dht_task(void*) {
  gpio_config_t io_config ={};
  io_config.pin_bit_mask = 1ULL << DHT_PIN;
  io_config.intr_type = GPIO_INTR_DISABLE;
  io_config.mode = GPIO_MODE_DISABLE;
  io_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_config.pull_up_en = GPIO_PULLUP_ENABLE;
  auto ret = gpio_config(&io_config);

  ret = gpio_install_isr_service(ESP_INTR_FLAG_LOWMED); RET_ERR(ret);
  ret = gpio_isr_handler_add(DHT_PIN, dht_isr_handler, NULL); RET_ERR(ret);
  ret = gpio_set_intr_type(DHT_PIN, GPIO_INTR_ANYEDGE); RET_ERR(ret);

  dht_queue = xQueueCreate(40, sizeof(char)); RET_ERR(ret);

  for (;;){
    vTaskDelay(dht_info.polling_interval);
    clear_isr_state();

    // start reading
    ret = gpio_intr_disable(DHT_PIN); RET_ERR(ret);
    ret = gpio_set_direction(DHT_PIN, GPIO_MODE_OUTPUT_OD); RET_ERR(ret);
    ret = gpio_set_level(DHT_PIN, 0); RET_ERR(ret);
    vTaskDelay(1);
    ret = gpio_set_level(DHT_PIN, 1); RET_ERR(ret);
    ret = gpio_set_direction(DHT_PIN, GPIO_MODE_INPUT); RET_ERR(ret);
    ret = gpio_intr_enable(DHT_PIN); RET_ERR(ret);

    char data[5];
    for (int b = 0; b <5; b++) {
      char byte = 0;
      for (int bits=0; bits<8; bits++) {
        char bit = 0;
        if (xQueueReceive(dht_queue, &bit, 500)) {
          byte = (byte <<1) | bit;
          continue;
        }
        printf("DHT: OOPS!\n");
      }
      data[b] = byte;
    }
    if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
      dht_info.status = DHT_STATUS_OK;
      dht_info.relative_humidity = (( (uint16_t)data[0] ) << 8 | data[1] ) * 0.1;
      dht_info.temperature = (((uint16_t)(data[2] & 0x7F)) << 8 | data[3]) * 0.1;
      if (data[2] & 0x80) dht_info.temperature *= -1.0;
      printf("DHT: %4.1f | %5.1f%%\n", dht_info.temperature, dht_info.relative_humidity);
    } else {
      printf("DHT: Checksum FAILED\n");
      dht_info.status = DHT_STATUS_INVALID;
    }
  }

  vTaskDelete(NULL);
}