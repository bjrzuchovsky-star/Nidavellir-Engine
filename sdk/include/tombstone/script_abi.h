/* TombStone script ABI (C). The engine hands every compiled script module
 * (C++ via script_sdk.h, C# via TombStone.Scripting) one TsScriptApi; the
 * module exports the ts_script_* entry points below. Plain C types only, so
 * any compiler / language can sit on either side. Bump TS_SCRIPT_ABI on any
 * change. */
#ifndef TOMBSTONE_SCRIPT_ABI_H
#define TOMBSTONE_SCRIPT_ABI_H

#include <stdint.h>

#define TS_SCRIPT_ABI 1

#ifdef __cplusplus
extern "C" {
#endif

enum TsScriptHook {
  TS_HOOK_START = 0,
  TS_HOOK_TICK = 1,          /* number = dt */
  TS_HOOK_TRIGGER_ENTER = 2, /* entity = other */
  TS_HOOK_TRIGGER_EXIT = 3,
  TS_HOOK_INTERACT = 4,      /* entity = rider */
  TS_HOOK_RELOAD = 5,
  TS_HOOK_DESTROY = 6,
  TS_HOOK_HAND = 7,          /* text = "hook\thand", number */
  TS_HOOK_UI = 8,            /* text = "widget\ttext", number = value, entity = change */
  TS_HOOK_COUNT = 9
};

enum TsUiKind { TS_UI_SHOW = 0, TS_UI_HIDE = 1, TS_UI_SET_TEXT = 2, TS_UI_SET_VALUE = 3 };
enum TsLogLevel { TS_LOG_INFO = 0, TS_LOG_WARN = 1, TS_LOG_ERROR = 2 };

typedef struct TsScriptApi {
  uint32_t abi;  /* TS_SCRIPT_ABI */
  void* world;   /* first argument of every call */
  uint64_t (*tick)(void* w);
  uint64_t (*find)(void* w, const char* name);
  uint64_t (*player)(void* w, int slot);                 /* 0-based */
  int (*position)(void* w, uint64_t id, float* x, float* y);
  void (*move)(void* w, uint64_t id, float x, float y);
  void (*set_visible)(void* w, uint64_t id, int on);
  void (*set_collider)(void* w, uint64_t id, int on);
  int (*play)(void* w, uint64_t id, const char* clip, int hold);
  uint64_t (*spawn)(void* w, const char* tmpl, float x, float y);
  void (*destroy)(void* w, uint64_t id);
  int (*get_number)(void* w, uint64_t id, const char* key, double* out);
  void (*set_number)(void* w, uint64_t id, const char* key, double v);
  int (*get_text)(void* w, uint64_t id, const char* key, char* buf, int cap); /* len, -1 */
  void (*set_text)(void* w, uint64_t id, const char* key, const char* v);
  void (*toast)(void* w, int slot, const char* text, double seconds, uint64_t from); /* -1 all */
  void (*log)(void* w, int level, const char* text, uint64_t entity);
  void (*sound)(void* w, const char* clip, float volume, uint64_t entity);
  void (*music)(void* w, const char* clip, float fade);
  void (*fx_burst)(void* w, const char* preset, uint64_t entity);
  void (*ui)(void* w, int kind, const char* target, const char* text, double value);
  uint32_t (*buttons)(void* w, int slot);                /* 0-based, kButton* bits */
} TsScriptApi;

/* Module exports (one module per project holds every script class).
 *   int   ts_script_abi(void);
 *   const char* ts_script_classes(void);         "door\nlantern\n"
 *   void* ts_script_create(const char* cls, const TsScriptApi* api,
 *                          uint64_t self, const char* props);
 *        props: "name\ttype\tvalue\n" lines, type n / b / t
 *   void  ts_script_destroy(void* s);
 *   int   ts_script_has(void* s, int hook);
 *   void  ts_script_call(void* s, int hook, uint64_t entity, double number,
 *                        const char* text);
 *   int   ts_script_save(void* s, char* buf, int cap);  bytes needed
 *   void  ts_script_load(void* s, const char* data, int len);
 */

#ifdef __cplusplus
}
#endif

#endif
