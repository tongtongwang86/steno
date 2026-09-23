/* Host-side stubs so steno_display.c can be compiled and rendered offline.
 * Not part of the firmware build. SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#define ARG_UNUSED(x) (void)(x)
#define IS_ENABLED(x) (x##_ENABLED_)
#define CONFIG_ZMK_BATTERY_REPORTING_ENABLED_ 1
#define CONFIG_ZMK_BLE_ENABLED_ 1
#define CONFIG_ZMK_BATTERY_REPORTING 1
#define CONFIG_ZMK_BLE 1
#define CONFIG_STENO_DISPLAY_LOG_LEVEL 3
#define CONFIG_STENO_DISPLAY_COALESCE_MS 8
#define CONFIG_STENO_DISPLAY_STACK_SIZE 1024
#define CONFIG_STENO_DISPLAY_THREAD_PRIORITY 10

/* kernel */
struct device { int dummy; };
#define DT_CHOSEN(x) 0
#define DEVICE_DT_GET(x) (&sim_dev)
extern const struct device sim_dev;
static inline bool device_is_ready(const struct device *d){ (void)d; return true; }

#define K_FOREVER 0
#define K_MUTEX_DEFINE(n) int n
#define K_SEM_DEFINE(n,a,b) int n
static inline int k_mutex_lock(int *m, int t){ (void)m;(void)t; return 0; }
static inline int k_mutex_unlock(int *m){ (void)m; return 0; }
static inline void k_sem_give(int *s){ (void)s; }
static inline int k_sem_take(int *s, int t){ (void)s;(void)t; return 0; }
static inline void k_sem_reset(int *s){ (void)s; }
static inline void k_msleep(int ms){ (void)ms; }
#define K_THREAD_DEFINE(tid,ss,fn,a,b,c,prio,opt,dly) \
	void (*tid##_unused)(void*,void*,void*) = fn

/* logging */
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...)  do { fprintf(stderr, "ERR  "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOG_WRN(...)  do { fprintf(stderr, "WRN  "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

/* display */
enum display_pixel_format { PIXEL_FORMAT_MONO01 = BIT(1), PIXEL_FORMAT_MONO10 = BIT(2) };
enum display_screen_info {
	SCREEN_INFO_MONO_VTILED = BIT(0),
	SCREEN_INFO_MONO_MSB_FIRST = BIT(1),
};
struct display_buffer_descriptor { uint32_t buf_size; uint16_t width, height, pitch; bool frame_incomplete; };
struct display_capabilities {
	uint16_t x_resolution, y_resolution;
	uint32_t supported_pixel_formats, screen_info;
	enum display_pixel_format current_pixel_format;
	int current_orientation;
};
#define ENOSYS 38
extern uint8_t sim_panel[1024];
static inline int display_write(const struct device *d, uint16_t x, uint16_t y,
				const struct display_buffer_descriptor *desc, const void *buf)
{
	(void)d;
	if (desc->pitch != desc->width) return -22;
	if (y & 7) return -22;
	if (desc->height & 7) return -22;
	if (x != 0 || desc->width != 128) return -22;
	memcpy(&sim_panel[(y/8)*128], buf, desc->buf_size);
	return 0;
}
static inline int display_blanking_on(const struct device *d){ (void)d; return 0; }
static inline int display_blanking_off(const struct device *d){ (void)d; return 0; }
static inline int display_set_pixel_format(const struct device *d, enum display_pixel_format f){ (void)d;(void)f; return 0; }
static inline void display_get_capabilities(const struct device *d, struct display_capabilities *c){
	(void)d; memset(c,0,sizeof(*c));
	c->x_resolution=128; c->y_resolution=64;
	c->screen_info = SCREEN_INFO_MONO_VTILED;  /* MSB_FIRST deliberately clear */
}

/* zmk */
typedef struct { int t; } zmk_event_t;
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_LISTENER(m,cb) void *m##_listener_ref = (void *)(cb)
#define ZMK_SUBSCRIPTION(m,e) int m##_##e##_sub_unused = 0
enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };
struct zmk_activity_state_changed { enum zmk_activity_state state; };
static inline const struct zmk_activity_state_changed *as_zmk_activity_state_changed(const zmk_event_t *e){ (void)e; return NULL; }
enum zmk_transport { ZMK_TRANSPORT_NONE=0, ZMK_TRANSPORT_USB=1, ZMK_TRANSPORT_BLE=2 };
struct zmk_endpoint_instance { enum zmk_transport transport; };
extern struct zmk_endpoint_instance sim_ep;
static inline struct zmk_endpoint_instance zmk_endpoint_get_selected(void){ return sim_ep; }
extern uint8_t sim_soc;
static inline uint8_t zmk_battery_state_of_charge(void){ return sim_soc; }
static inline int zmk_ble_active_profile_index(void){ return 1; }
static inline bool zmk_ble_active_profile_is_connected(void){ return true; }
