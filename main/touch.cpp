

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <esp_log.h>
#include <driver/timer.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl/lvgl.h>
#include "events.h"
#include "display.h"


#define TOUCH_CS 14
#define TOUCH_IRQ 27

#define HAVE_TOUCHPAD

#define TS_MINX 370
#define TS_MINY 470
#define TS_MAXX 3700
#define TS_MAXY 3600

static const char* TAG = "TOUCH";

XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
TS_Point oldPoint;
extern TaskHandle_t touchScreenTaskHandle;

void touch_screen_input(lv_indev_drv_t * drv, lv_indev_data_t*data)
{
  if (ts.tirqTouched()){
    if (ts.touched()) {
      vTaskDelay(pdMS_TO_TICKS(30)); // debounce the touch gesture
      if (ts.touched()){
        auto p = ts.getPoint();
        p.x = map(p.x, TS_MINX, TS_MAXX, 0, 320);
        p.y = map(p.y, TS_MINY, TS_MAXY, 0, 240);
        ESP_LOGI(TAG, "Touched at %d,%d with pressure %d", p.x, p.y, p.z);
        oldPoint = p;
        data->state = LV_INDEV_STATE_PR;
        data->point.x = p.x;
        data->point.y = p.y;

        xTaskNotify(touchScreenTaskHandle, 0x01, eSetBits);
        return;
      }
    }
  }

  data->state = LV_INDEV_STATE_REL;
  data->point.x = oldPoint.x;
  data->point.y = oldPoint.y;
}

extern bool night_mode;
void setDisplayBacklight(bool);

static void turn_off_screen_cb(void*){
  if (night_mode){
    setDisplayBacklight(false);
  }
}

void touchScreenTask(void*) {
  if ( !ts.begin(displayTaskHandle, DISPLAY_NOTIFY_TOUCH) ) {
    ESP_LOGE(TAG, "Cannot setup touchscreen");
    vTaskDelete(NULL);
    return;
  }
  ts.setRotation(3);

  esp_timer_handle_t timer = NULL;
  esp_timer_create_args_t timer_args = {
    .callback = turn_off_screen_cb,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "turn_off_scr",
    .skip_unhandled_events = true
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));

  for (;;){
    // clear bits on entry to avoid multiple events at once when screen is
    // touched
    if (pdTRUE == xTaskNotifyWait(ULONG_LONG_MAX, ULONG_LONG_MAX, NULL, portMAX_DELAY) ){
      if (night_mode) {
        setDisplayBacklight(true);

        esp_timer_stop(timer); // stop timer if already running, no error check on purpose
        ESP_ERROR_CHECK(esp_timer_start_once(timer, 5000 * 1000));
      }
    }
    lv_point_t touchedPoint = {
      .x = oldPoint.x,
      .y = oldPoint.y
    };
    if ( lv_scr_act() == lv_indev_search_obj(lv_scr_act(), &touchedPoint) ) {
      // user touched the screen, and did not hit any object, so let's send
      // the touched event to home assistant
      events.postTouchedEvent();
      vTaskDelay(pdMS_TO_TICKS(1000)); // wait a bit to avoid repeat sending for same touch event
      xTaskNotifyStateClear(NULL);
    }
  }
}
