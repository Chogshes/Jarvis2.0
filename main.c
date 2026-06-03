#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include "lvgl/lvgl.h"
#include "ui.h"
//#include "video_player.h"

// 时钟初始化 (TODO: 实现时间显示)
static void ui_clock_init(void) {}
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"

#define DISP_BUF_SIZE  (800 * 480)

static void lvgl_init(void) //初始化LVGL
{
    lv_init();

    static lv_color_t buf1[DISP_BUF_SIZE];
    static lv_color_t buf2[DISP_BUF_SIZE];
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, DISP_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.hor_res   = 800;
    disp_drv.ver_res   = 480;

    fbdev_init();
    disp_drv.flush_cb = fbdev_flush;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    evdev_init();
    indev_drv.read_cb = evdev_read;
    lv_indev_drv_register(&indev_drv);
}


uint32_t custom_tick_get(void)//获取当前时间戳（毫秒）
{
    static uint64_t start_ms = 0;
    if (start_ms == 0) {
        struct timeval tv_start;
        gettimeofday(&tv_start, NULL);
        start_ms = (tv_start.tv_sec * 1000000 + tv_start.tv_usec) / 1000;
    }
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t now_ms = (tv_now.tv_sec * 1000000 + tv_now.tv_usec) / 1000;
    return (uint32_t)(now_ms - start_ms);
}

int main(void)
{
    lvgl_init();
    ui_init();
    ui_clock_init();
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }
    return 0;
}
