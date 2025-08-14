#include "wasm_export.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define PATH_SEPARATOR "\\"
#define mkdir(path, mode) _mkdir(path)
#define isatty _isatty
#define fileno _fileno
#define strdup _strdup
#define strcasecmp _stricmp
#else
#include <libgen.h>
#include <sys/time.h>
#include <unistd.h>
#define PATH_SEPARATOR "/"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
#define LIKELY(x) (!!(x))
#define UNLIKELY(x) (!!(x))
#else
#define LIKELY(x) (!!(x))
#define UNLIKELY(x) (!!(x))
#endif

#define HAKO_STACK_SIZE (256 * 1024)
#define HAKO_HEAP_SIZE (512 * 1024)
#define HAKO_ERROR_BUFFER_SIZE 256
#define HAKO_ALL_INTRINSICS 0
#define HAKO_MAX_HOST_FUNCTIONS 64
#define HAKO_MAX_TIMERS 256
#define HAKO_REPL_BUFFER_SIZE 4096
#define HAKO_MAX_JSON_MODULES 32

typedef enum {
  HAKO_TYPE_UNDEFINED = 0,
  HAKO_TYPE_OBJECT = 1,
  HAKO_TYPE_STRING = 2,
  HAKO_TYPE_SYMBOL = 3,
  HAKO_TYPE_BOOLEAN = 4,
  HAKO_TYPE_NUMBER = 5,
  HAKO_TYPE_BIGINT = 6,
  HAKO_TYPE_FUNCTION = 7
} HakoTypeOf;

typedef enum {
  HAKO_PROMISE_PENDING = 0,
  HAKO_PROMISE_FULFILLED = 1,
  HAKO_PROMISE_REJECTED = 2
} HakoPromiseState;

typedef struct {
  uint32_t value;
  bool is_error;
  char *error_message;
} HakoEvalResult;

typedef struct {
  int id;
  uint32_t callback;
  uint64_t trigger_time;
  bool active;
} HakoTimer;

typedef struct {
  uint32_t module_ptr;
  char *name;
  char *json_path;
} HakoJSONModule;

typedef enum {
  HAKO_MODE_FILE,
  HAKO_MODE_EVAL,
  HAKO_MODE_REPL,
  HAKO_MODE_COMPILE,
  HAKO_MODE_RUN_BYTECODE
} HakoExecutionMode;

typedef enum {
  MODULE_SOURCE_STRING = 0,
  MODULE_SOURCE_PRECOMPILED = 1,
  MODULE_SOURCE_ERROR = 2
} ModuleSourceType;

typedef struct {
  uint32_t type;
  union {
    uint32_t source_code;
    uint32_t module_def;
  } data;
} ModuleSource;

static uint8_t hako_wasm[] = {
#embed "hako.wasm"
};
static const size_t hako_wasm_size = sizeof(hako_wasm);

typedef struct {
  wasm_function_inst_t new_runtime;
  wasm_function_inst_t free_runtime;
  wasm_function_inst_t new_context;
  wasm_function_inst_t free_context;
  wasm_function_inst_t malloc;
  wasm_function_inst_t free;
  wasm_function_inst_t eval;
  wasm_function_inst_t get_last_error;
  wasm_function_inst_t is_job_pending;
  wasm_function_inst_t execute_pending_job;
  wasm_function_inst_t enable_module_loader;
  wasm_function_inst_t compile_to_bytecode;
  wasm_function_inst_t eval_bytecode;
  wasm_function_inst_t get_undefined;
  wasm_function_inst_t get_null;
  wasm_function_inst_t get_true;
  wasm_function_inst_t get_false;
  wasm_function_inst_t new_object;
  wasm_function_inst_t new_array;
  wasm_function_inst_t new_string;
  wasm_function_inst_t new_function;
  wasm_function_inst_t new_float64;
  wasm_function_inst_t htypeof;
  wasm_function_inst_t is_null;
  wasm_function_inst_t is_array;
  wasm_function_inst_t is_promise;
  wasm_function_inst_t is_error;
  wasm_function_inst_t to_cstring;
  wasm_function_inst_t free_cstring;
  wasm_function_inst_t to_json;
  wasm_function_inst_t get_float64;
  wasm_function_inst_t free_value_pointer;
  wasm_function_inst_t dup_value_pointer;
  wasm_function_inst_t get_global_object;
  wasm_function_inst_t get_prop;
  wasm_function_inst_t set_prop;
  wasm_function_inst_t call;
  wasm_function_inst_t promise_state;
  wasm_function_inst_t promise_result;
  wasm_function_inst_t new_cmodule;
  wasm_function_inst_t add_module_export;
  wasm_function_inst_t set_module_export;
  wasm_function_inst_t set_module_private_value;
  wasm_function_inst_t get_module_private_value;
  wasm_function_inst_t dump;
  wasm_function_inst_t argv_get_pointer;
  wasm_function_inst_t parse_json;
} HakoFunctionTable;

typedef uint32_t (*HakoHostFunctionCallback)(wasm_exec_env_t exec_env,
                                             uint32_t ctx, uint32_t this_ptr,
                                             int argc, uint32_t *argv,
                                             int32_t func_id);

typedef struct {
  int32_t id;
  const char *name;
  HakoHostFunctionCallback callback;
  void *user_data;
} HakoHostFunction;

typedef struct {
  HakoHostFunction functions[HAKO_MAX_HOST_FUNCTIONS];
  size_t count;
  int32_t next_id;
} HakoHostFunctionRegistry;

typedef struct {
  wasm_module_t module;
  wasm_module_inst_t instance;
  wasm_exec_env_t exec_env;
  uint32_t js_runtime;
  uint32_t js_context;
  HakoFunctionTable funcs;
  HakoHostFunctionRegistry host_functions;
  char *current_module_dir;
  bool module_loader_enabled;
  HakoJSONModule *json_modules[HAKO_MAX_JSON_MODULES];
  size_t json_module_count;
  HakoTimer timers[HAKO_MAX_TIMERS];
  int next_timer_id;
  FILE *output_stream;
  FILE *error_stream;
  bool debug_mode;
  int repl_line_number;
} HakoRuntime;

static HakoRuntime *g_runtime = NULL;

static char *hako_strdup(const char *str) {
  if (!str)
    return NULL;
  size_t len = strlen(str) + 1;
  char *copy = malloc(len);
  if (copy) {
    memcpy(copy, str, len);
  }
  return copy;
}

static uint64_t hako_get_current_time_ms(void) {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t time = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return time / 10000;
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static void hako_host_registry_init(HakoHostFunctionRegistry *registry) {
  registry->count = 0;
  registry->next_id = -32768;
}

static int32_t hako_host_registry_add(HakoHostFunctionRegistry *registry,
                                      const char *name,
                                      HakoHostFunctionCallback callback,
                                      void *user_data) {
  if (registry->count >= HAKO_MAX_HOST_FUNCTIONS) {
    return 0;
  }

  HakoHostFunction *func = &registry->functions[registry->count++];
  func->id = registry->next_id++;
  func->name = name;
  func->callback = callback;
  func->user_data = user_data;

  return func->id;
}

static HakoHostFunction *
hako_host_registry_get(HakoHostFunctionRegistry *registry, int32_t id) {
  for (size_t i = 0; i < registry->count; i++) {
    if (registry->functions[i].id == id) {
      return &registry->functions[i];
    }
  }
  return NULL;
}

static uint32_t hako_allocate_in_wasm(HakoRuntime *runtime, size_t size) {
  if (UNLIKELY(!runtime->funcs.malloc))
    return 0;

  uint32_t argv[2] = {runtime->js_context, (uint32_t)size};

  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env, runtime->funcs.malloc,
                                       2, argv))) {
    if (runtime->debug_mode) {
      const char *exception = wasm_runtime_get_exception(runtime->instance);
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] HAKO_Malloc failed: %s\n", exception ?: "Unknown");
    }
    return 0;
  }
  return argv[0];
}

static void hako_free_in_wasm(HakoRuntime *runtime, uint32_t ptr) {
  if (!ptr || !runtime->funcs.free)
    return;

  uint32_t argv[2] = {runtime->js_context, ptr};
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env, runtime->funcs.free,
                                       2, argv))) {
    if (runtime->debug_mode) {
      const char *exception = wasm_runtime_get_exception(runtime->instance);
      fprintf(runtime->error_stream ?: stderr, "[ERROR] HAKO_Free failed: %s\n",
              exception ?: "Unknown");
    }
  }
}

static uint32_t hako_allocate_string(HakoRuntime *runtime, const char *str) {
  if (!str)
    return 0;
  size_t len = strlen(str) + 1;
  uint32_t ptr = hako_allocate_in_wasm(runtime, len);
  if (ptr) {
    char *dest = wasm_runtime_addr_app_to_native(runtime->instance, ptr);
    if (dest) {
      memcpy(dest, str, len);
    } else {
      hako_free_in_wasm(runtime, ptr);
      return 0;
    }
  }
  return ptr;
}

static uint32_t hako_allocate_bytes(HakoRuntime *runtime, const void *data,
                                    size_t size) {
  if (!data || size == 0)
    return 0;
  uint32_t ptr = hako_allocate_in_wasm(runtime, size);
  if (ptr) {
    void *dest = wasm_runtime_addr_app_to_native(runtime->instance, ptr);
    if (dest) {
      memcpy(dest, data, size);
    } else {
      hako_free_in_wasm(runtime, ptr);
      return 0;
    }
  }
  return ptr;
}

static char *hako_value_to_string(HakoRuntime *runtime, uint32_t value,
                                  int json_indent) {
  if (UNLIKELY(!runtime->funcs.htypeof)) {
    return hako_strdup("[error: typeof not available]");
  }

  if (runtime->funcs.is_error) {
    uint32_t error_argv[2] = {runtime->js_context, value};
    if (LIKELY(wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.is_error, 2, error_argv)) &&
        error_argv[0] != 0) {
      if (runtime->funcs.dump) {
        uint32_t dump_argv[2] = {runtime->js_context, value};
        if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env,
                                          runtime->funcs.dump, 2, dump_argv))) {
          uint32_t str_ptr = dump_argv[0];
          char *str =
              wasm_runtime_addr_app_to_native(runtime->instance, str_ptr);
          char *result = hako_strdup(str ? str : "[error]");

          if (runtime->funcs.free_cstring) {
            uint32_t free_argv[2] = {runtime->js_context, str_ptr};
            if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                                 runtime->funcs.free_cstring, 2,
                                                 free_argv))) {
              if (runtime->debug_mode) {
                fprintf(runtime->error_stream ?: stderr,
                        "[ERROR] free_cstring failed\n");
              }
            }
          }

          return result;
        }
      }
    }
  }

  if (runtime->funcs.is_null) {
    uint32_t null_argv[1] = {value};
    if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env, runtime->funcs.is_null,
                                      1, null_argv)) &&
        null_argv[0] != 0) {
      return hako_strdup("null");
    }
  }

  uint32_t argv[2] = {runtime->js_context, value};

  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.htypeof, 2, argv))) {
    return hako_strdup("[error: typeof failed]");
  }

  HakoTypeOf type = (HakoTypeOf)argv[0];

  switch (type) {
  case HAKO_TYPE_UNDEFINED:
    return hako_strdup("undefined");

  case HAKO_TYPE_STRING:
  case HAKO_TYPE_BOOLEAN:
  case HAKO_TYPE_NUMBER:
  case HAKO_TYPE_BIGINT:
  case HAKO_TYPE_SYMBOL:
    goto use_to_cstring;

  case HAKO_TYPE_OBJECT:
  case HAKO_TYPE_FUNCTION: {
    if (runtime->funcs.to_json) {
      uint32_t indent_argv[3] = {runtime->js_context, value,
                                 (uint32_t)json_indent};

      if (UNLIKELY(!wasm_runtime_call_wasm(
              runtime->exec_env, runtime->funcs.to_json, 3, indent_argv))) {
        goto use_to_cstring;
      }

      uint32_t json_value = indent_argv[0];

      if (runtime->funcs.get_last_error) {
        uint32_t error_argv[2] = {runtime->js_context, json_value};
        if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env,
                                          runtime->funcs.get_last_error, 2,
                                          error_argv)) &&
            error_argv[0] != 0) {
          if (runtime->funcs.free_value_pointer) {
            uint32_t free_argv[2] = {runtime->js_context, error_argv[0]};
            if (UNLIKELY(!wasm_runtime_call_wasm(
                    runtime->exec_env, runtime->funcs.free_value_pointer, 2,
                    free_argv))) {
              if (runtime->debug_mode) {
                fprintf(runtime->error_stream ?: stderr,
                        "[ERROR] free_value_pointer failed\n");
              }
            }
            free_argv[1] = json_value;
            if (UNLIKELY(!wasm_runtime_call_wasm(
                    runtime->exec_env, runtime->funcs.free_value_pointer, 2,
                    free_argv))) {
              if (runtime->debug_mode) {
                fprintf(runtime->error_stream ?: stderr,
                        "[ERROR] free_value_pointer failed\n");
              }
            }
          }
          goto use_to_cstring;
        }
      }

      if (runtime->funcs.to_cstring) {
        uint32_t str_argv[2] = {runtime->js_context, json_value};
        if (LIKELY(wasm_runtime_call_wasm(
                runtime->exec_env, runtime->funcs.to_cstring, 2, str_argv))) {
          uint32_t str_ptr = str_argv[0];
          char *str =
              wasm_runtime_addr_app_to_native(runtime->instance, str_ptr);
          char *result = hako_strdup(str ? str : "[null]");

          if (runtime->funcs.free_cstring) {
            uint32_t free_argv[2] = {runtime->js_context, str_ptr};
            if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                                 runtime->funcs.free_cstring, 2,
                                                 free_argv))) {
              if (runtime->debug_mode) {
                fprintf(runtime->error_stream ?: stderr,
                        "[ERROR] free_cstring failed\n");
              }
            }
          }

          if (runtime->funcs.free_value_pointer) {
            uint32_t free_argv[2] = {runtime->js_context, json_value};
            if (UNLIKELY(!wasm_runtime_call_wasm(
                    runtime->exec_env, runtime->funcs.free_value_pointer, 2,
                    free_argv))) {
              if (runtime->debug_mode) {
                fprintf(runtime->error_stream ?: stderr,
                        "[ERROR] free_value_pointer failed\n");
              }
            }
          }

          return result;
        }

        if (runtime->funcs.free_value_pointer) {
          uint32_t free_argv[2] = {runtime->js_context, json_value};
          if (UNLIKELY(!wasm_runtime_call_wasm(
                  runtime->exec_env, runtime->funcs.free_value_pointer, 2,
                  free_argv))) {
            if (runtime->debug_mode) {
              fprintf(runtime->error_stream ?: stderr,
                      "[ERROR] free_value_pointer failed\n");
            }
          }
        }
      }
    }
    break;
  }

  default:
    break;
  }

use_to_cstring:
  if (UNLIKELY(!runtime->funcs.to_cstring)) {
    return hako_strdup("[error: no conversion available]");
  }

  uint32_t str_argv[2] = {runtime->js_context, value};
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.to_cstring, 2, str_argv))) {
    return hako_strdup("[error: ToCString failed]");
  }

  uint32_t str_ptr = str_argv[0];
  if (str_ptr == 0) {
    return hako_strdup("[null]");
  }

  char *str = wasm_runtime_addr_app_to_native(runtime->instance, str_ptr);
  char *result = hako_strdup(str ? str : "[invalid]");

  if (runtime->funcs.free_cstring) {
    uint32_t free_argv[2] = {runtime->js_context, str_ptr};
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_cstring, 2, free_argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_cstring failed\n");
      }
    }
  }

  return result;
}

static uint32_t hako_console_log_callback(wasm_exec_env_t exec_env,
                                          uint32_t ctx, uint32_t this_ptr,
                                          int argc, uint32_t *argv,
                                          int32_t func_id) {
  HakoRuntime *runtime = g_runtime;
  if (!runtime) {
    return 0;
  }

  FILE *out = runtime->output_stream ?: stdout;

  for (int i = 0; i < argc; i++) {
    if (i > 0)
      fprintf(out, " ");

    char *str = hako_value_to_string(runtime, argv[i], 0);
    fprintf(out, "%s", str);
    free(str);
  }

  fprintf(out, "\n");
  fflush(out);

  if (runtime->funcs.get_undefined) {
    uint32_t undef_argv[1];
    if (LIKELY(wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.get_undefined, 0, undef_argv))) {
      return undef_argv[0];
    }
  }

  return 0;
}

static uint32_t hako_setTimeout_callback(wasm_exec_env_t exec_env, uint32_t ctx,
                                         uint32_t this_ptr, int argc,
                                         uint32_t *argv, int32_t func_id) {

  HakoRuntime *runtime = g_runtime;
  if (!runtime || argc < 2) {
    return 0;
  }

  uint32_t callback = argv[0];

  double delay = 0;
  if (runtime->funcs.get_float64) {
    uint32_t float_argv[2] = {ctx, argv[1]};
    if (LIKELY(wasm_runtime_call_wasm(exec_env, runtime->funcs.get_float64, 2,
                                      float_argv))) {
      delay = *(double *)&float_argv[0];
    }
  }

  HakoTimer *timer = NULL;
  for (int i = 0; i < HAKO_MAX_TIMERS; i++) {
    if (!runtime->timers[i].active) {
      timer = &runtime->timers[i];
      break;
    }
  }

  if (!timer) {
    return 0;
  }

  if (runtime->funcs.dup_value_pointer) {
    uint32_t dup_argv[2] = {ctx, callback};
    if (LIKELY(wasm_runtime_call_wasm(
            exec_env, runtime->funcs.dup_value_pointer, 2, dup_argv))) {
      callback = dup_argv[0];
    } else {
      return 0;
    }
  }

  timer->id = runtime->next_timer_id++;
  timer->callback = callback;
  timer->trigger_time = hako_get_current_time_ms() + (uint64_t)delay;
  timer->active = true;

  if (runtime->funcs.new_float64) {
    uint32_t result_argv[3];
    result_argv[0] = ctx;

    double id_double = (double)timer->id;
    uint64_t id_bits = *(uint64_t *)&id_double;
    result_argv[1] = (uint32_t)(id_bits & 0xFFFFFFFF);
    result_argv[2] = (uint32_t)((id_bits >> 32) & 0xFFFFFFFF);

    if (LIKELY(wasm_runtime_call_wasm(exec_env, runtime->funcs.new_float64, 3,
                                      result_argv))) {
      return result_argv[0];
    }
  }

  return 0;
}

static uint32_t hako_clearTimeout_callback(wasm_exec_env_t exec_env,
                                           uint32_t ctx, uint32_t this_ptr,
                                           int argc, uint32_t *argv,
                                           int32_t func_id) {
  HakoRuntime *runtime = g_runtime;
  if (!runtime || argc < 1) {
    return 0;
  }

  double id_double = 0;
  if (runtime->funcs.get_float64) {
    uint32_t float_argv[2] = {ctx, argv[0]};
    if (LIKELY(wasm_runtime_call_wasm(exec_env, runtime->funcs.get_float64, 2,
                                      float_argv))) {
      id_double = *(double *)&float_argv[0];
    }
  }

  int timer_id = (int)id_double;

  for (int i = 0; i < HAKO_MAX_TIMERS; i++) {
    if (runtime->timers[i].active && runtime->timers[i].id == timer_id) {
      runtime->timers[i].active = false;

      if (runtime->funcs.free_value_pointer) {
        uint32_t free_argv[2] = {ctx, runtime->timers[i].callback};
        if (UNLIKELY(!wasm_runtime_call_wasm(
                exec_env, runtime->funcs.free_value_pointer, 2, free_argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] free_value_pointer failed\n");
          }
        }
      }

      break;
    }
  }

  if (runtime->funcs.get_undefined) {
    uint32_t undef_argv[1];
    if (LIKELY(wasm_runtime_call_wasm(exec_env, runtime->funcs.get_undefined, 0,
                                      undef_argv))) {
      return undef_argv[0];
    }
  }

  return 0;
}

static void hako_process_timers(HakoRuntime *runtime) {
  uint64_t current_time = hako_get_current_time_ms();

  for (int i = 0; i < HAKO_MAX_TIMERS; i++) {
    HakoTimer *timer = &runtime->timers[i];
    if (timer->active && current_time >= timer->trigger_time) {
      if (runtime->funcs.call) {
        uint32_t undefined = 0;
        if (runtime->funcs.get_undefined) {
          uint32_t undef_argv[1];
          if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env,
                                            runtime->funcs.get_undefined, 0,
                                            undef_argv))) {
            undefined = undef_argv[0];
          }
        }

        uint32_t call_argv[4] = {runtime->js_context, timer->callback,
                                 undefined, 0};
        if (UNLIKELY(!wasm_runtime_call_wasm(
                runtime->exec_env, runtime->funcs.call, 4, call_argv))) {
          if (runtime->debug_mode) {
            const char *exception =
                wasm_runtime_get_exception(runtime->instance);
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] Timer callback failed: %s\n",
                    exception ?: "Unknown");
          }
        } else if (call_argv[0] != 0 && runtime->funcs.free_value_pointer) {
          uint32_t free_argv[2] = {runtime->js_context, call_argv[0]};
          if (UNLIKELY(!wasm_runtime_call_wasm(
                  runtime->exec_env, runtime->funcs.free_value_pointer, 2,
                  free_argv))) {
            if (runtime->debug_mode) {
              fprintf(runtime->error_stream ?: stderr,
                      "[ERROR] free_value_pointer failed\n");
            }
          }
        }
      }

      timer->active = false;

      if (runtime->funcs.free_value_pointer) {
        uint32_t free_argv[2] = {runtime->js_context, timer->callback};
        if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                             runtime->funcs.free_value_pointer,
                                             2, free_argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] free_value_pointer failed\n");
          }
        }
      }
    }
  }
}

static int hako_json_module_init(wasm_exec_env_t exec_env, uint32_t ctx,
                                 uint32_t module_ptr) {
  HakoRuntime *runtime = g_runtime;
  if (!runtime)
    return -1;

  if (!runtime->funcs.get_module_private_value ||
      !runtime->funcs.set_module_export) {
    return -1;
  }

  uint32_t argv[2] = {ctx, module_ptr};
  if (UNLIKELY(!wasm_runtime_call_wasm(
          exec_env, runtime->funcs.get_module_private_value, 2, argv))) {
    return -1;
  }
  uint32_t json_value = argv[0];

  uint32_t export_name_ptr = hako_allocate_string(runtime, "default");
  if (export_name_ptr) {
    uint32_t export_argv[4] = {ctx, module_ptr, export_name_ptr, json_value};
    if (UNLIKELY(!wasm_runtime_call_wasm(
            exec_env, runtime->funcs.set_module_export, 4, export_argv))) {
      hako_free_in_wasm(runtime, export_name_ptr);
      if (runtime->funcs.free_value_pointer) {
        uint32_t free_argv[2] = {ctx, json_value};
        if (UNLIKELY(!wasm_runtime_call_wasm(
                exec_env, runtime->funcs.free_value_pointer, 2, free_argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] free_value_pointer failed\n");
          }
        }
      }
      return -1;
    }
    hako_free_in_wasm(runtime, export_name_ptr);
  }

  if (runtime->funcs.free_value_pointer) {
    uint32_t free_argv[2] = {ctx, json_value};
    if (UNLIKELY(!wasm_runtime_call_wasm(
            exec_env, runtime->funcs.free_value_pointer, 2, free_argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
  }

  return 0;
}

static HakoJSONModule *hako_create_json_module(HakoRuntime *runtime,
                                               const char *name,
                                               const char *json_path) {
  if (!runtime->funcs.new_cmodule || !runtime->funcs.parse_json) {
    return NULL;
  }

  FILE *f = fopen(json_path, "r");
  if (!f) {
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0) {
    fclose(f);
    return NULL;
  }

  char *json_content = malloc(size + 1);
  if (!json_content) {
    fclose(f);
    return NULL;
  }

  if (fread(json_content, 1, size, f) != (size_t)size) {
    free(json_content);
    fclose(f);
    return NULL;
  }
  json_content[size] = '\0';
  fclose(f);

  HakoJSONModule *module = calloc(1, sizeof(HakoJSONModule));
  if (!module) {
    free(json_content);
    return NULL;
  }

  module->name = hako_strdup(name);
  module->json_path = hako_strdup(json_path);

  uint32_t name_ptr = hako_allocate_string(runtime, name);
  if (!name_ptr) {
    free(json_content);
    free(module->name);
    free(module->json_path);
    free(module);
    return NULL;
  }

  uint32_t argv[2] = {runtime->js_context, name_ptr};
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.new_cmodule, 2, argv))) {
    hako_free_in_wasm(runtime, name_ptr);
    free(json_content);
    free(module->name);
    free(module->json_path);
    free(module);
    return NULL;
  }

  module->module_ptr = argv[0];
  hako_free_in_wasm(runtime, name_ptr);

  uint32_t json_ptr = hako_allocate_string(runtime, json_content);
  uint32_t filename_ptr = hako_allocate_string(runtime, json_path);
  free(json_content);

  if (!json_ptr || !filename_ptr) {
    if (json_ptr)
      hako_free_in_wasm(runtime, json_ptr);
    if (filename_ptr)
      hako_free_in_wasm(runtime, filename_ptr);
    free(module->name);
    free(module->json_path);
    free(module);
    return NULL;
  }

  uint32_t parse_argv[4] = {runtime->js_context, json_ptr, (uint32_t)size,
                            filename_ptr};

  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.parse_json, 4, parse_argv))) {
    hako_free_in_wasm(runtime, json_ptr);
    hako_free_in_wasm(runtime, filename_ptr);
    free(module->name);
    free(module->json_path);
    free(module);
    return NULL;
  }

  uint32_t json_value = parse_argv[0];
  hako_free_in_wasm(runtime, json_ptr);
  hako_free_in_wasm(runtime, filename_ptr);

  if (runtime->funcs.set_module_private_value) {
    uint32_t set_argv[3] = {runtime->js_context, module->module_ptr,
                            json_value};
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.set_module_private_value, 3,
            set_argv))) {
      if (runtime->funcs.free_value_pointer) {
        uint32_t free_argv[2] = {runtime->js_context, json_value};
        if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                             runtime->funcs.free_value_pointer,
                                             2, free_argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] free_value_pointer failed\n");
          }
        }
      }
      free(module->name);
      free(module->json_path);
      free(module);
      return NULL;
    }
  }

  if (runtime->funcs.free_value_pointer) {
    uint32_t free_argv[2] = {runtime->js_context, json_value};
    if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                         runtime->funcs.free_value_pointer, 2,
                                         free_argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
  }

  if (runtime->funcs.add_module_export) {
    uint32_t export_name_ptr = hako_allocate_string(runtime, "default");
    if (export_name_ptr) {
      uint32_t export_argv[3] = {runtime->js_context, module->module_ptr,
                                 export_name_ptr};
      if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                           runtime->funcs.add_module_export, 3,
                                           export_argv))) {
        hako_free_in_wasm(runtime, export_name_ptr);
        free(module->name);
        free(module->json_path);
        free(module);
        return NULL;
      }
      hako_free_in_wasm(runtime, export_name_ptr);
    }
  }

  hako_host_registry_add(&runtime->host_functions, name,
                         (HakoHostFunctionCallback)hako_json_module_init,
                         module);

  return module;
}

static uint32_t hako_call_function(wasm_exec_env_t exec_env, uint32_t ctx,
                                   uint32_t this_ptr, int argc,
                                   uint32_t argv_ptr, int32_t func_id) {
  HakoRuntime *runtime = g_runtime;
  if (!runtime)
    return 0;

  HakoHostFunction *func =
      hako_host_registry_get(&runtime->host_functions, func_id);
  if (!func || !func->callback) {
    return 0;
  }

  uint32_t *argv = NULL;
  if (argc > 0) {
    argv = malloc(sizeof(uint32_t) * argc);
    if (!argv) {
      return 0;
    }

    if (!runtime->funcs.argv_get_pointer) {
      free(argv);
      return 0;
    }

    for (int i = 0; i < argc; i++) {
      uint32_t get_argv_args[2] = {argv_ptr, (uint32_t)i};
      if (UNLIKELY(!wasm_runtime_call_wasm(
              exec_env, runtime->funcs.argv_get_pointer, 2, get_argv_args))) {
        free(argv);
        return 0;
      }
      argv[i] = get_argv_args[0];
    }
  }

  uint32_t result =
      func->callback(exec_env, ctx, this_ptr, argc, argv, func_id);

  if (argv) {
    free(argv);
  }

  return result;
}

static int hako_interrupt_handler(wasm_exec_env_t exec_env, uint32_t rt,
                                  uint32_t ctx, uint32_t opaque) {
  return 0;
}

static bool hako_check_json_attributes(HakoRuntime *runtime,
                                       uint32_t attributes_ptr) {
  if (attributes_ptr == 0)
    return false;

  if (!runtime->funcs.get_prop || !runtime->funcs.new_string ||
      !runtime->funcs.to_cstring || !runtime->funcs.free_cstring ||
      !runtime->funcs.free_value_pointer) {
    return false;
  }

  uint32_t type_str_ptr = hako_allocate_string(runtime, "type");
  if (!type_str_ptr)
    return false;

  uint32_t argv[3] = {runtime->js_context, type_str_ptr};
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.new_string, 2, argv))) {
    hako_free_in_wasm(runtime, type_str_ptr);
    return false;
  }
  uint32_t type_key = argv[0];
  hako_free_in_wasm(runtime, type_str_ptr);

  argv[0] = runtime->js_context;
  argv[1] = attributes_ptr;
  argv[2] = type_key;

  bool is_json = false;
  if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env, runtime->funcs.get_prop,
                                    3, argv))) {
    uint32_t type_value = argv[0];

    uint32_t str_argv[2] = {runtime->js_context, type_value};
    if (LIKELY(wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.to_cstring, 2, str_argv))) {
      uint32_t str_ptr = str_argv[0];
      char *str = wasm_runtime_addr_app_to_native(runtime->instance, str_ptr);
      if (str && strcmp(str, "json") == 0) {
        is_json = true;
      }

      uint32_t free_argv[2] = {runtime->js_context, str_ptr};
      if (UNLIKELY(!wasm_runtime_call_wasm(
              runtime->exec_env, runtime->funcs.free_cstring, 2, free_argv))) {
        if (runtime->debug_mode) {
          fprintf(runtime->error_stream ?: stderr,
                  "[ERROR] free_cstring failed\n");
        }
      }
    }

    uint32_t free_argv[2] = {runtime->js_context, type_value};
    if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                         runtime->funcs.free_value_pointer, 2,
                                         free_argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
  }

  uint32_t free_argv[2] = {runtime->js_context, type_key};
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.free_value_pointer, 2,
                                       free_argv))) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] free_value_pointer failed\n");
    }
  }

  return is_json;
}

static uint32_t hako_load_module(wasm_exec_env_t exec_env, uint32_t rt,
                                 uint32_t ctx, uint32_t module_name_ptr,
                                 uint32_t opaque, uint32_t attributes) {
  HakoRuntime *runtime = g_runtime;
  if (!runtime || !runtime->module_loader_enabled) {
    goto error;
  }

  char *module_name =
      wasm_runtime_addr_app_to_native(runtime->instance, module_name_ptr);
  if (!module_name) {
    goto error;
  }

  bool is_json = hako_check_json_attributes(runtime, attributes);

  if (!is_json && strstr(module_name, ".json") != NULL) {
    if (runtime->debug_mode) {
      fprintf(
          stderr,
          "Error: .json file requested without { type: 'json' } attribute\n");
    }
    goto error;
  }

  if (is_json) {
    for (size_t i = 0; i < runtime->json_module_count; i++) {
      if (runtime->json_modules[i] &&
          strcmp(runtime->json_modules[i]->name, module_name) == 0) {
        uint32_t module_src_ptr =
            hako_allocate_in_wasm(runtime, sizeof(ModuleSource));
        if (module_src_ptr) {
          ModuleSource *module_src = wasm_runtime_addr_app_to_native(
              runtime->instance, module_src_ptr);
          if (module_src) {
            module_src->type = MODULE_SOURCE_PRECOMPILED;
            module_src->data.module_def = runtime->json_modules[i]->module_ptr;
            return module_src_ptr;
          }
          hako_free_in_wasm(runtime, module_src_ptr);
        }
      }
    }
  }

  char *resolved_path = NULL;
  if (runtime->current_module_dir) {
    size_t dir_len = strlen(runtime->current_module_dir);
    size_t name_len = strlen(module_name);
    resolved_path = malloc(dir_len + name_len + 20);
    if (resolved_path) {
      sprintf(resolved_path, "%s%s%s", runtime->current_module_dir,
              PATH_SEPARATOR, module_name);

      struct stat st;

      char *jco_path = malloc(strlen(resolved_path) + 5);
      if (jco_path) {
        strcpy(jco_path, resolved_path);
        strcat(jco_path, ".jco");
        if (stat(jco_path, &st) == 0) {
          FILE *f = fopen(jco_path, "rb");
          free(jco_path);
          if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (size > 0) {
              uint8_t *bytecode = malloc(size);
              if (bytecode && fread(bytecode, 1, size, f) == (size_t)size) {
                fclose(f);
                free(resolved_path);

                uint32_t bytecode_ptr =
                    hako_allocate_bytes(runtime, bytecode, size);
                free(bytecode);

                if (bytecode_ptr) {
                  uint32_t module_src_ptr =
                      hako_allocate_in_wasm(runtime, sizeof(ModuleSource));
                  if (module_src_ptr) {
                    ModuleSource *module_src = wasm_runtime_addr_app_to_native(
                        runtime->instance, module_src_ptr);
                    if (module_src) {
                      module_src->type = MODULE_SOURCE_PRECOMPILED;
                      module_src->data.module_def = bytecode_ptr;
                      return module_src_ptr;
                    }
                    hako_free_in_wasm(runtime, module_src_ptr);
                  }
                  hako_free_in_wasm(runtime, bytecode_ptr);
                }
              } else {
                free(bytecode);
                fclose(f);
              }
            } else {
              fclose(f);
            }
          }
        } else {
          free(jco_path);
        }
      }

      if (is_json) {
        char *json_path = malloc(strlen(resolved_path) + 6);
        if (json_path) {
          strcpy(json_path, resolved_path);
          struct stat st;
          if (stat(json_path, &st) != 0) {
            free(json_path);
            free(resolved_path);
            goto error;
          }

          HakoJSONModule *json_mod =
              hako_create_json_module(runtime, module_name, json_path);
          free(json_path);

          if (json_mod) {
            if (runtime->json_module_count < HAKO_MAX_JSON_MODULES) {
              runtime->json_modules[runtime->json_module_count++] = json_mod;
            }

            uint32_t module_src_ptr =
                hako_allocate_in_wasm(runtime, sizeof(ModuleSource));
            if (module_src_ptr) {
              ModuleSource *module_src = wasm_runtime_addr_app_to_native(
                  runtime->instance, module_src_ptr);
              if (module_src) {
                module_src->type = MODULE_SOURCE_PRECOMPILED;
                module_src->data.module_def = json_mod->module_ptr;
                free(resolved_path);
                return module_src_ptr;
              }
              hako_free_in_wasm(runtime, module_src_ptr);
            }
          }
        }
        free(resolved_path);
        goto error;
      }

      char *mjs_path = malloc(strlen(resolved_path) + 5);
      if (mjs_path) {
        strcpy(mjs_path, resolved_path);
        strcat(mjs_path, ".mjs");
        if (stat(mjs_path, &st) == 0) {
          free(resolved_path);
          resolved_path = mjs_path;
        } else {
          free(mjs_path);
          if (stat(resolved_path, &st) != 0) {
            strcat(resolved_path, ".js");
          }
        }
      }
    }
  }

  if (!resolved_path) {
    goto error;
  }

  FILE *f = fopen(resolved_path, "rb");
  free(resolved_path);
  if (!f) {
    goto error;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0) {
    fclose(f);
    goto error;
  }

  char *source = malloc(size + 1);
  if (!source) {
    fclose(f);
    goto error;
  }

  if (fread(source, 1, size, f) != (size_t)size) {
    free(source);
    fclose(f);
    goto error;
  }

  source[size] = '\0';
  fclose(f);

  uint32_t source_ptr = hako_allocate_string(runtime, source);
  free(source);

  if (!source_ptr) {
    goto error;
  }

  uint32_t module_src_ptr =
      hako_allocate_in_wasm(runtime, sizeof(ModuleSource));
  if (!module_src_ptr) {
    hako_free_in_wasm(runtime, source_ptr);
    goto error;
  }

  ModuleSource *module_src =
      wasm_runtime_addr_app_to_native(runtime->instance, module_src_ptr);
  if (module_src) {
    module_src->type = MODULE_SOURCE_STRING;
    module_src->data.source_code = source_ptr;
  }

  return module_src_ptr;

error:
  uint32_t error_ptr = hako_allocate_in_wasm(runtime, sizeof(ModuleSource));
  if (error_ptr) {
    ModuleSource *error_src =
        wasm_runtime_addr_app_to_native(runtime->instance, error_ptr);
    if (error_src) {
      error_src->type = MODULE_SOURCE_ERROR;
      error_src->data.source_code = 0;
    }
  }
  return error_ptr;
}

static uint32_t hako_normalize_module(wasm_exec_env_t exec_env, uint32_t rt,
                                      uint32_t ctx, uint32_t base_name_ptr,
                                      uint32_t module_name_ptr,
                                      uint32_t opaque) {
  HakoRuntime *runtime = g_runtime;
  if (!runtime)
    return 0;

  char *module_name =
      wasm_runtime_addr_app_to_native(runtime->instance, module_name_ptr);
  if (!module_name)
    return 0;

  return hako_allocate_string(runtime, module_name);
}

static uint32_t hako_resolve_module(wasm_exec_env_t exec_env, uint32_t rt,
                                    uint32_t ctx, uint32_t module_name_ptr,
                                    uint32_t current_module_ptr,
                                    uint32_t opaque) {
  HakoRuntime *runtime = g_runtime;
  if (!runtime)
    return 0;

  char *module_name =
      wasm_runtime_addr_app_to_native(runtime->instance, module_name_ptr);
  if (!module_name)
    return 0;

  return hako_allocate_string(runtime, module_name);
}

static void hako_profile_start(wasm_exec_env_t exec_env, uint32_t ctx,
                               uint32_t event, uint32_t opaque) {}

static void hako_profile_end(wasm_exec_env_t exec_env, uint32_t ctx,
                             uint32_t event, uint32_t opaque) {}

static int hako_module_init(wasm_exec_env_t exec_env, uint32_t ctx,
                            uint32_t m) {
  HakoRuntime *runtime = g_runtime;
  if (runtime) {
    for (size_t i = 0; i < runtime->json_module_count; i++) {
      if (runtime->json_modules[i] &&
          runtime->json_modules[i]->module_ptr == m) {
        return hako_json_module_init(exec_env, ctx, m);
      }
    }
  }
  return 0;
}

static uint32_t hako_class_constructor(wasm_exec_env_t exec_env, uint32_t ctx,
                                       uint32_t new_target, int argc,
                                       uint32_t argv, uint32_t class_id) {
  return 0;
}

static void hako_class_finalizer(wasm_exec_env_t exec_env, uint32_t rt,
                                 uint32_t opaque, uint32_t class_id) {}

static NativeSymbol hako_imports[] = {
    {"call_function", hako_call_function, "(iiiii)i", NULL},
    {"interrupt_handler", hako_interrupt_handler, "(iii)i", NULL},
    {"load_module", hako_load_module, "(iiiii)i", NULL},
    {"normalize_module", hako_normalize_module, "(iiiii)i", NULL},
    {"resolve_module", hako_resolve_module, "(iiiii)i", NULL},
    {"profile_function_start", hako_profile_start, "(iii)", NULL},
    {"profile_function_end", hako_profile_end, "(iii)", NULL},
    {"module_init", hako_module_init, "(ii)i", NULL},
    {"class_constructor", hako_class_constructor, "(iiiii)i", NULL},
    {"class_finalizer", hako_class_finalizer, "(iii)", NULL},
};

static bool hako_initialize_function_table(HakoRuntime *runtime) {
  runtime->funcs.new_runtime =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_NewRuntime");
  runtime->funcs.free_runtime =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_FreeRuntime");
  runtime->funcs.new_context =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_NewContext");
  runtime->funcs.free_context =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_FreeContext");
  runtime->funcs.malloc =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_Malloc");
  runtime->funcs.free =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_Free");
  runtime->funcs.eval =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_Eval");
  runtime->funcs.get_last_error =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_GetLastError");
  runtime->funcs.is_job_pending =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_IsJobPending");
  runtime->funcs.execute_pending_job =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_ExecutePendingJob");
  runtime->funcs.enable_module_loader = wasm_runtime_lookup_function(
      runtime->instance, "HAKO_RuntimeEnableModuleLoader");
  runtime->funcs.compile_to_bytecode =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_CompileToByteCode");
  runtime->funcs.eval_bytecode =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_EvalByteCode");
  runtime->funcs.get_undefined =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_GetUndefined");
  runtime->funcs.get_null =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_GetNull");
  runtime->funcs.get_true =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_GetTrue");
  runtime->funcs.get_false =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_GetFalse");
  runtime->funcs.new_object =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_NewObject");
  runtime->funcs.new_array =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_NewArray");
  runtime->funcs.new_string =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_NewString");
  runtime->funcs.new_function =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_NewFunction");
  runtime->funcs.new_float64 =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_NewFloat64");
  runtime->funcs.htypeof =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_TypeOf");
  runtime->funcs.is_null =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_IsNull");
  runtime->funcs.is_array =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_IsArray");
  runtime->funcs.is_promise =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_IsPromise");
  runtime->funcs.is_error =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_IsError");
  runtime->funcs.to_cstring =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_ToCString");
  runtime->funcs.free_cstring =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_FreeCString");
  runtime->funcs.to_json =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_ToJson");
  runtime->funcs.get_float64 =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_GetFloat64");
  runtime->funcs.free_value_pointer =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_FreeValuePointer");
  runtime->funcs.dup_value_pointer =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_DupValuePointer");
  runtime->funcs.get_global_object =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_GetGlobalObject");
  runtime->funcs.get_prop =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_GetProp");
  runtime->funcs.set_prop =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_SetProp");
  runtime->funcs.call =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_Call");
  runtime->funcs.promise_state =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_PromiseState");
  runtime->funcs.promise_result =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_PromiseResult");
  runtime->funcs.new_cmodule =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_NewCModule");
  runtime->funcs.add_module_export =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_AddModuleExport");
  runtime->funcs.set_module_export =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_SetModuleExport");
  runtime->funcs.set_module_private_value = wasm_runtime_lookup_function(
      runtime->instance, "HAKO_SetModulePrivateValue");
  runtime->funcs.get_module_private_value = wasm_runtime_lookup_function(
      runtime->instance, "HAKO_GetModulePrivateValue");
  runtime->funcs.dump =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_Dump");
  runtime->funcs.argv_get_pointer = wasm_runtime_lookup_function(
      runtime->instance, "HAKO_ArgvGetJSValueConstPointer");
  runtime->funcs.parse_json =
      wasm_runtime_lookup_function(runtime->instance, "HAKO_ParseJson");

  if (!runtime->funcs.new_runtime || !runtime->funcs.new_context ||
      !runtime->funcs.malloc || !runtime->funcs.free || !runtime->funcs.eval) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] Critical functions not found\n");
    }
    return false;
  }

  return true;
}

static bool hako_setup_console(HakoRuntime *runtime) {
  if (!runtime->funcs.get_global_object || !runtime->funcs.new_object ||
      !runtime->funcs.new_string || !runtime->funcs.new_function ||
      !runtime->funcs.set_prop || !runtime->funcs.free_value_pointer) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] Required functions for console setup not found\n");
    }
    return false;
  }

  int32_t console_log_id = hako_host_registry_add(
      &runtime->host_functions, "console.log", hako_console_log_callback, NULL);
  if (!console_log_id) {
    return false;
  }

  uint32_t argv[5];

  argv[0] = runtime->js_context;
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.get_global_object, 1, argv))) {
    return false;
  }
  uint32_t global_obj = argv[0];

  argv[0] = runtime->js_context;
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.new_object, 1, argv))) {
    argv[0] = runtime->js_context;
    argv[1] = global_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    return false;
  }
  uint32_t console_obj = argv[0];

  uint32_t log_name_ptr = hako_allocate_string(runtime, "log");
  if (!log_name_ptr) {
    argv[0] = runtime->js_context;
    argv[1] = console_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = global_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    return false;
  }

  argv[0] = runtime->js_context;
  argv[1] = (uint32_t)console_log_id;
  argv[2] = log_name_ptr;

  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.new_function, 3, argv))) {
    hako_free_in_wasm(runtime, log_name_ptr);
    argv[0] = runtime->js_context;
    argv[1] = console_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = global_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    return false;
  }
  uint32_t log_func = argv[0];
  hako_free_in_wasm(runtime, log_name_ptr);

  uint32_t log_prop_name_ptr = hako_allocate_string(runtime, "log");
  if (!log_prop_name_ptr) {
    argv[0] = runtime->js_context;
    argv[1] = log_func;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = console_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = global_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    return false;
  }

  argv[0] = runtime->js_context;
  argv[1] = log_prop_name_ptr;
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.new_string, 2, argv))) {
    hako_free_in_wasm(runtime, log_prop_name_ptr);
    argv[0] = runtime->js_context;
    argv[1] = log_func;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = console_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = global_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    return false;
  }
  uint32_t log_prop_name = argv[0];
  hako_free_in_wasm(runtime, log_prop_name_ptr);

  argv[0] = runtime->js_context;
  argv[1] = console_obj;
  argv[2] = log_prop_name;
  argv[3] = log_func;

  bool set_success = wasm_runtime_call_wasm(runtime->exec_env,
                                            runtime->funcs.set_prop, 4, argv);

  argv[0] = runtime->js_context;
  argv[1] = log_prop_name;
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] free_value_pointer failed\n");
    }
  }

  if (UNLIKELY(!set_success)) {
    argv[0] = runtime->js_context;
    argv[1] = log_func;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = console_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = global_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    return false;
  }

  uint32_t console_name_ptr = hako_allocate_string(runtime, "console");
  if (!console_name_ptr) {
    argv[0] = runtime->js_context;
    argv[1] = log_func;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = console_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = global_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    return false;
  }

  argv[0] = runtime->js_context;
  argv[1] = console_name_ptr;
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.new_string, 2, argv))) {
    hako_free_in_wasm(runtime, console_name_ptr);
    argv[0] = runtime->js_context;
    argv[1] = log_func;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = console_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    argv[1] = global_obj;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
    return false;
  }
  uint32_t console_name = argv[0];
  hako_free_in_wasm(runtime, console_name_ptr);

  argv[0] = runtime->js_context;
  argv[1] = global_obj;
  argv[2] = console_name;
  argv[3] = console_obj;

  set_success = wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.set_prop, 4, argv);

  argv[0] = runtime->js_context;
  argv[1] = console_name;
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] free_value_pointer failed\n");
    }
  }

  argv[1] = log_func;
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] free_value_pointer failed\n");
    }
  }

  argv[1] = console_obj;
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] free_value_pointer failed\n");
    }
  }

  argv[1] = global_obj;
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] free_value_pointer failed\n");
    }
  }

  return set_success;
}

static bool hako_setup_timers(HakoRuntime *runtime) {
  if (!runtime->funcs.get_global_object || !runtime->funcs.new_function ||
      !runtime->funcs.new_string || !runtime->funcs.set_prop ||
      !runtime->funcs.free_value_pointer) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] Required functions for timer setup not found\n");
    }
    return false;
  }

  int32_t setTimeout_id = hako_host_registry_add(
      &runtime->host_functions, "setTimeout", hako_setTimeout_callback, NULL);
  int32_t clearTimeout_id =
      hako_host_registry_add(&runtime->host_functions, "clearTimeout",
                             hako_clearTimeout_callback, NULL);

  if (!setTimeout_id || !clearTimeout_id) {
    return false;
  }

  uint32_t argv[5];

  argv[0] = runtime->js_context;
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.get_global_object, 1, argv))) {
    return false;
  }
  uint32_t global_obj = argv[0];

  uint32_t setTimeout_name_ptr = hako_allocate_string(runtime, "setTimeout");
  if (setTimeout_name_ptr) {
    argv[0] = runtime->js_context;
    argv[1] = (uint32_t)setTimeout_id;
    argv[2] = setTimeout_name_ptr;

    if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env,
                                      runtime->funcs.new_function, 3, argv))) {
      uint32_t setTimeout_func = argv[0];

      argv[0] = runtime->js_context;
      argv[1] = setTimeout_name_ptr;
      if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env,
                                        runtime->funcs.new_string, 2, argv))) {
        uint32_t setTimeout_name = argv[0];

        argv[0] = runtime->js_context;
        argv[1] = global_obj;
        argv[2] = setTimeout_name;
        argv[3] = setTimeout_func;
        if (UNLIKELY(!wasm_runtime_call_wasm(
                runtime->exec_env, runtime->funcs.set_prop, 4, argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] Failed to set setTimeout property\n");
          }
        }

        argv[0] = runtime->js_context;
        argv[1] = setTimeout_name;
        if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                             runtime->funcs.free_value_pointer,
                                             2, argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] free_value_pointer failed\n");
          }
        }
      }

      argv[0] = runtime->js_context;
      argv[1] = setTimeout_func;
      if (UNLIKELY(!wasm_runtime_call_wasm(
              runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
        if (runtime->debug_mode) {
          fprintf(runtime->error_stream ?: stderr,
                  "[ERROR] free_value_pointer failed\n");
        }
      }
    }
    hako_free_in_wasm(runtime, setTimeout_name_ptr);
  }

  uint32_t clearTimeout_name_ptr =
      hako_allocate_string(runtime, "clearTimeout");
  if (clearTimeout_name_ptr) {
    argv[0] = runtime->js_context;
    argv[1] = (uint32_t)clearTimeout_id;
    argv[2] = clearTimeout_name_ptr;

    if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env,
                                      runtime->funcs.new_function, 3, argv))) {
      uint32_t clearTimeout_func = argv[0];

      argv[0] = runtime->js_context;
      argv[1] = clearTimeout_name_ptr;
      if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env,
                                        runtime->funcs.new_string, 2, argv))) {
        uint32_t clearTimeout_name = argv[0];

        argv[0] = runtime->js_context;
        argv[1] = global_obj;
        argv[2] = clearTimeout_name;
        argv[3] = clearTimeout_func;
        if (UNLIKELY(!wasm_runtime_call_wasm(
                runtime->exec_env, runtime->funcs.set_prop, 4, argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] Failed to set clearTimeout property\n");
          }
        }

        argv[0] = runtime->js_context;
        argv[1] = clearTimeout_name;
        if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                             runtime->funcs.free_value_pointer,
                                             2, argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] free_value_pointer failed\n");
          }
        }
      }

      argv[0] = runtime->js_context;
      argv[1] = clearTimeout_func;
      if (UNLIKELY(!wasm_runtime_call_wasm(
              runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
        if (runtime->debug_mode) {
          fprintf(runtime->error_stream ?: stderr,
                  "[ERROR] free_value_pointer failed\n");
        }
      }
    }
    hako_free_in_wasm(runtime, clearTimeout_name_ptr);
  }

  argv[0] = runtime->js_context;
  argv[1] = global_obj;
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
    if (runtime->debug_mode) {
      fprintf(runtime->error_stream ?: stderr,
              "[ERROR] free_value_pointer failed\n");
    }
  }

  return true;
}

static HakoRuntime *hako_runtime_create(void) {
  HakoRuntime *runtime = calloc(1, sizeof(HakoRuntime));
  if (!runtime) {
    return NULL;
  }

  hako_host_registry_init(&runtime->host_functions);

  runtime->next_timer_id = 1;
  for (int i = 0; i < HAKO_MAX_TIMERS; i++) {
    runtime->timers[i].active = false;
  }

  g_runtime = runtime;

  char error_buf[HAKO_ERROR_BUFFER_SIZE];

  RuntimeInitArgs init_args = {0};
  init_args.mem_alloc_type = Alloc_With_System_Allocator;

  if (!wasm_runtime_full_init(&init_args)) {
    goto error;
  }

  size_t import_count = sizeof(hako_imports) / sizeof(hako_imports[0]);
  if (!wasm_runtime_register_natives("hako", hako_imports, import_count)) {
    wasm_runtime_destroy();
    goto error;
  }

  runtime->module = wasm_runtime_load(hako_wasm, hako_wasm_size, error_buf,
                                      sizeof(error_buf));
  if (!runtime->module) {
    fprintf(stderr, "Failed to load module: %s\n", error_buf);
    wasm_runtime_destroy();
    goto error;
  }

  runtime->instance =
      wasm_runtime_instantiate(runtime->module, HAKO_STACK_SIZE, HAKO_HEAP_SIZE,
                               error_buf, sizeof(error_buf));
  if (!runtime->instance) {
    fprintf(stderr, "Failed to instantiate: %s\n", error_buf);
    wasm_runtime_unload(runtime->module);
    wasm_runtime_destroy();
    goto error;
  }

  runtime->exec_env =
      wasm_runtime_create_exec_env(runtime->instance, HAKO_STACK_SIZE);
  if (!runtime->exec_env) {
    wasm_runtime_deinstantiate(runtime->instance);
    wasm_runtime_unload(runtime->module);
    wasm_runtime_destroy();
    goto error;
  }

  if (!hako_initialize_function_table(runtime)) {
    goto error;
  }

  uint32_t argv[2];
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.new_runtime, 0, argv))) {
    goto error;
  }
  runtime->js_runtime = argv[0];

  if (runtime->funcs.enable_module_loader) {
    argv[0] = runtime->js_runtime;
    argv[1] = 1;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.enable_module_loader, 2, argv))) {
      if (runtime->debug_mode) {
        fprintf(stderr, "[WARNING] Failed to enable module loader\n");
      }
    } else {
      runtime->module_loader_enabled = true;
    }
  }

  argv[0] = runtime->js_runtime;
  argv[1] = HAKO_ALL_INTRINSICS;
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.new_context, 2, argv))) {
    goto error;
  }
  runtime->js_context = argv[0];

  if (!hako_setup_console(runtime)) {
    if (runtime->debug_mode) {
      fprintf(stderr, "[WARNING] Failed to setup console\n");
    }
  }

  if (!hako_setup_timers(runtime)) {
    if (runtime->debug_mode) {
      fprintf(stderr, "[WARNING] Failed to setup timers\n");
    }
  }

  runtime->repl_line_number = 1;

  return runtime;

error:
  if (runtime->exec_env) {
    wasm_runtime_destroy_exec_env(runtime->exec_env);
  }
  if (runtime->instance) {
    wasm_runtime_deinstantiate(runtime->instance);
  }
  if (runtime->module) {
    wasm_runtime_unload(runtime->module);
  }
  free(runtime);
  g_runtime = NULL;
  return NULL;
}

static void hako_runtime_destroy(HakoRuntime *runtime) {
  if (!runtime)
    return;

  uint32_t argv[2];

  for (int i = 0; i < HAKO_MAX_TIMERS; i++) {
    if (runtime->timers[i].active && runtime->funcs.free_value_pointer) {
      argv[0] = runtime->js_context;
      argv[1] = runtime->timers[i].callback;
      if (UNLIKELY(!wasm_runtime_call_wasm(
              runtime->exec_env, runtime->funcs.free_value_pointer, 2, argv))) {
        if (runtime->debug_mode) {
          fprintf(runtime->error_stream ?: stderr,
                  "[ERROR] free_value_pointer failed\n");
        }
      }
    }
  }

  for (size_t i = 0; i < runtime->json_module_count; i++) {
    if (runtime->json_modules[i]) {
      free(runtime->json_modules[i]->name);
      free(runtime->json_modules[i]->json_path);
      free(runtime->json_modules[i]);
    }
  }

  if (runtime->js_context && runtime->funcs.free_context) {
    argv[0] = runtime->js_context;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_context, 1, argv))) {
      if (runtime->debug_mode) {
        const char *exception = wasm_runtime_get_exception(runtime->instance);
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_context failed: %s\n", exception ?: "Unknown");
      }
    }
  }

  if (runtime->js_runtime && runtime->funcs.free_runtime) {
    argv[0] = runtime->js_runtime;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.free_runtime, 1, argv))) {
      if (runtime->debug_mode) {
        const char *exception = wasm_runtime_get_exception(runtime->instance);
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_runtime failed: %s\n", exception ?: "Unknown");
      }
    }
  }

  if (runtime->current_module_dir) {
    free(runtime->current_module_dir);
  }

  if (runtime->exec_env) {
    wasm_runtime_destroy_exec_env(runtime->exec_env);
  }
  if (runtime->instance) {
    wasm_runtime_deinstantiate(runtime->instance);
  }
  if (runtime->module) {
    wasm_runtime_unload(runtime->module);
  }

  wasm_runtime_destroy();

  g_runtime = NULL;

  free(runtime);
}

static uint32_t hako_await(HakoRuntime *runtime, uint32_t promise_value) {
  if (!runtime->funcs.is_promise || !runtime->funcs.promise_state ||
      !runtime->funcs.promise_result) {
    return promise_value;
  }

  uint32_t argv[4] = {runtime->js_context, promise_value, 0, 0};
  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                       runtime->funcs.is_promise, 2, argv)) ||
      argv[0] == 0) {
    return promise_value;
  }

  uint32_t ctx_ptr = hako_allocate_in_wasm(runtime, sizeof(uint32_t));
  if (!ctx_ptr) {
    return promise_value;
  }

  while (true) {
    hako_process_timers(runtime);

    if (runtime->funcs.is_job_pending && runtime->funcs.execute_pending_job) {
      while (true) {
        argv[0] = runtime->js_runtime;

        if (UNLIKELY(!wasm_runtime_call_wasm(
                runtime->exec_env, runtime->funcs.is_job_pending, 1, argv)) ||
            argv[0] == 0) {
          break;
        }

        argv[0] = runtime->js_runtime;
        argv[1] = 1;
        argv[2] = ctx_ptr;

        if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                             runtime->funcs.execute_pending_job,
                                             3, argv))) {
          break;
        }

        if (argv[0] != 0 && runtime->funcs.free_value_pointer) {
          uint32_t free_argv[2] = {runtime->js_context, argv[0]};
          if (UNLIKELY(!wasm_runtime_call_wasm(
                  runtime->exec_env, runtime->funcs.free_value_pointer, 2,
                  free_argv))) {
            if (runtime->debug_mode) {
              fprintf(runtime->error_stream ?: stderr,
                      "[ERROR] free_value_pointer failed\n");
            }
          }
        }
      }
    }

    argv[0] = runtime->js_context;
    argv[1] = promise_value;
    if (UNLIKELY(!wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.promise_state, 2, argv))) {
      hako_free_in_wasm(runtime, ctx_ptr);
      return promise_value;
    }

    HakoPromiseState state = (HakoPromiseState)argv[0];

    if (state != HAKO_PROMISE_PENDING) {
      break;
    }

    bool has_active_timers = false;
    for (int i = 0; i < HAKO_MAX_TIMERS; i++) {
      if (runtime->timers[i].active) {
        has_active_timers = true;
        break;
      }
    }

    bool has_pending_jobs = false;
    if (runtime->funcs.is_job_pending) {
      argv[0] = runtime->js_runtime;
      if (LIKELY(wasm_runtime_call_wasm(
              runtime->exec_env, runtime->funcs.is_job_pending, 1, argv)) &&
          argv[0] != 0) {
        has_pending_jobs = true;
      }
    }

    if (!has_active_timers && !has_pending_jobs) {
      hako_free_in_wasm(runtime, ctx_ptr);
      return promise_value;
    }
  }

  hako_free_in_wasm(runtime, ctx_ptr);

  argv[0] = runtime->js_context;
  argv[1] = promise_value;
  if (UNLIKELY(!wasm_runtime_call_wasm(
          runtime->exec_env, runtime->funcs.promise_result, 2, argv))) {
    return promise_value;
  }

  uint32_t result = argv[0];

  if (runtime->funcs.free_value_pointer) {
    uint32_t free_argv[2] = {runtime->js_context, promise_value};
    if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                         runtime->funcs.free_value_pointer, 2,
                                         free_argv))) {
      if (runtime->debug_mode) {
        fprintf(runtime->error_stream ?: stderr,
                "[ERROR] free_value_pointer failed\n");
      }
    }
  }

  return result;
}

static bool hako_compile_to_bytecode(HakoRuntime *runtime, const char *code,
                                    size_t code_len, const char *filename,
                                    const char *output_file) {
 if (!runtime->funcs.compile_to_bytecode) {
   fprintf(stderr, "Error: HAKO_CompileToByteCode not found\n");
   return false;
 }

 uint32_t code_ptr = hako_allocate_string(runtime, code);
 uint32_t filename_ptr = hako_allocate_string(runtime, filename);

 if (!code_ptr || !filename_ptr) {
   if (code_ptr) hako_free_in_wasm(runtime, code_ptr);
   if (filename_ptr) hako_free_in_wasm(runtime, filename_ptr);
   fprintf(stderr, "Error: Out of memory\n");
   return false;
 }

 // Allocate 4 bytes for uint32_t, not sizeof(size_t)
 uint32_t size_ptr = hako_allocate_in_wasm(runtime, 4);
 if (!size_ptr) {
   hako_free_in_wasm(runtime, code_ptr);
   hako_free_in_wasm(runtime, filename_ptr);
   fprintf(stderr, "Error: Out of memory\n");
   return false;
 }

 uint32_t argv[7] = {
     runtime->js_context,
     code_ptr,
     (uint32_t)code_len,
     filename_ptr,
     1,
     0,
     size_ptr
 };

 if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                             runtime->funcs.compile_to_bytecode, 7, argv))) {
   const char *exception = wasm_runtime_get_exception(runtime->instance);
   fprintf(stderr, "Compilation failed: %s\n", exception ?: "Unknown error");
   hako_free_in_wasm(runtime, code_ptr);
   hako_free_in_wasm(runtime, filename_ptr);
   hako_free_in_wasm(runtime, size_ptr);
   return false;
 }

 uint32_t bytecode_ptr = argv[0];

 hako_free_in_wasm(runtime, code_ptr);
 hako_free_in_wasm(runtime, filename_ptr);

 if (!bytecode_ptr) {
   if (runtime->funcs.get_last_error) {
     uint32_t error_argv[2] = {runtime->js_context, 0};
     if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env,
                                runtime->funcs.get_last_error, 2,
                                error_argv)) &&
         error_argv[0] != 0) {
       char *error_str = hako_value_to_string(runtime, error_argv[0], 2);
       fprintf(stderr, "Compilation error: %s\n", error_str);
       free(error_str);
       if (runtime->funcs.free_value_pointer) {
         uint32_t free_argv[2] = {runtime->js_context, error_argv[0]};
         if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                    runtime->funcs.free_value_pointer, 2,
                                    free_argv))) {
           if (runtime->debug_mode) {
             fprintf(runtime->error_stream ?: stderr,
                     "[ERROR] free_value_pointer failed\n");
           }
         }
       }
     }
   }
   hako_free_in_wasm(runtime, size_ptr);
   return false;
 }

 // Read as uint32_t
 uint32_t *size_addr = wasm_runtime_addr_app_to_native(runtime->instance, size_ptr);
 if (!size_addr) {
   fprintf(stderr, "Error: Invalid size pointer\n");
   hako_free_in_wasm(runtime, bytecode_ptr);
   hako_free_in_wasm(runtime, size_ptr);
   return false;
 }
 
 size_t bytecode_size = (size_t)(*size_addr);
 hako_free_in_wasm(runtime, size_ptr);

 uint8_t *bytecode = wasm_runtime_addr_app_to_native(runtime->instance, bytecode_ptr);
 if (!bytecode) {
   fprintf(stderr, "Error: Invalid bytecode pointer\n");
   hako_free_in_wasm(runtime, bytecode_ptr);
   return false;
 }

 FILE *f = fopen(output_file, "wb");
 if (!f) {
   fprintf(stderr, "Error: Cannot open output file '%s': %s\n", output_file,
           strerror(errno));
   hako_free_in_wasm(runtime, bytecode_ptr);
   return false;
 }

 size_t written = fwrite(bytecode, 1, bytecode_size, f);
 fclose(f);

 hako_free_in_wasm(runtime, bytecode_ptr);

 if (written != bytecode_size) {
   fprintf(stderr, "Error: Failed to write complete bytecode\n");
   return false;
 }

 printf("Compiled to %s (%zu bytes)\n", output_file, bytecode_size);
 return true;
}

static HakoEvalResult hako_evaluate_bytecode(HakoRuntime *runtime,
                                             const uint8_t *bytecode,
                                             size_t bytecode_size) {
  HakoEvalResult result = {0, false, NULL};

  if (!runtime->funcs.eval_bytecode) {
    result.is_error = true;
    result.error_message = hako_strdup("HAKO_EvalByteCode not found");
    return result;
  }

  uint32_t bytecode_ptr = hako_allocate_bytes(runtime, bytecode, bytecode_size);
  if (!bytecode_ptr) {
    result.is_error = true;
    result.error_message = hako_strdup("Out of memory");
    return result;
  }

  uint32_t argv[4] = {runtime->js_context, bytecode_ptr,
                      (uint32_t)bytecode_size, 0};

  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env, runtime->funcs.eval_bytecode,
                              4, argv))) {
    const char *exception = wasm_runtime_get_exception(runtime->instance);
    hako_free_in_wasm(runtime, bytecode_ptr);
    result.is_error = true;
    result.error_message = hako_strdup(exception ?: "Evaluation failed");
    return result;
  }

  result.value = argv[0];
  hako_free_in_wasm(runtime, bytecode_ptr);

  result.value = hako_await(runtime, result.value);

  if (runtime->funcs.is_error) {
    uint32_t error_argv[2] = {runtime->js_context, result.value};
    if (LIKELY(wasm_runtime_call_wasm(runtime->exec_env, runtime->funcs.is_error, 2,
                               error_argv)) &&
        error_argv[0] != 0) {
      result.is_error = true;
      result.error_message = hako_value_to_string(runtime, result.value, 2);
    }
  }

  return result;
}

static HakoEvalResult hako_evaluate_javascript(HakoRuntime *runtime,
                                               const char *code,
                                               size_t code_len,
                                               const char *filename) {
  HakoEvalResult result = {0, false, NULL};

  if (!runtime->funcs.eval) {
    result.is_error = true;
    result.error_message = hako_strdup("HAKO_Eval not found");
    return result;
  }

  uint32_t code_ptr = hako_allocate_string(runtime, code);
  uint32_t filename_ptr = hako_allocate_string(runtime, filename);

  if (!code_ptr || !filename_ptr) {
    if (code_ptr)
      hako_free_in_wasm(runtime, code_ptr);
    if (filename_ptr)
      hako_free_in_wasm(runtime, filename_ptr);
    result.is_error = true;
    result.error_message = hako_strdup("Out of memory");
    return result;
  }

  uint32_t argv[7] = {
      runtime->js_context, code_ptr, (uint32_t)code_len, filename_ptr, 1, 0, 0};

  if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env, runtime->funcs.eval,
                                       6, argv))) {
    const char *exception = wasm_runtime_get_exception(runtime->instance);
    hako_free_in_wasm(runtime, code_ptr);
    hako_free_in_wasm(runtime, filename_ptr);
    result.is_error = true;
    result.error_message =
        hako_strdup(exception ? exception : "Evaluation failed");
    return result;
  }

  result.value = argv[0];

  hako_free_in_wasm(runtime, code_ptr);
  hako_free_in_wasm(runtime, filename_ptr);

  if (runtime->funcs.get_last_error) {
    uint32_t error_argv[3] = {runtime->js_context, result.value, 0};

    if (LIKELY(wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.get_last_error, 2, error_argv)) &&
        error_argv[0] != 0) {
      result.is_error = true;
      result.error_message = hako_value_to_string(runtime, error_argv[0], 2);

      if (runtime->funcs.free_value_pointer) {
        uint32_t free_argv[2] = {runtime->js_context, error_argv[0]};
        if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                             runtime->funcs.free_value_pointer,
                                             2, free_argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] free_value_pointer failed\n");
          }
        }

        free_argv[1] = result.value;
        if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                             runtime->funcs.free_value_pointer,
                                             2, free_argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] free_value_pointer failed\n");
          }
        }
      }

      result.value = 0;
      return result;
    }
  }

  result.value = hako_await(runtime, result.value);

  if (runtime->funcs.is_error) {
    uint32_t error_argv[2] = {runtime->js_context, result.value};
    if (LIKELY(wasm_runtime_call_wasm(
            runtime->exec_env, runtime->funcs.is_error, 2, error_argv)) &&
        error_argv[0] != 0) {
      result.is_error = true;
      result.error_message = hako_value_to_string(runtime, result.value, 2);
    }
  }

  return result;
}

static char *hako_read_repl_line(void) {
  static char buffer[HAKO_REPL_BUFFER_SIZE];

  if (!fgets(buffer, sizeof(buffer), stdin)) {
    return NULL;
  }

  size_t len = strlen(buffer);
  if (len > 0 && buffer[len - 1] == '\n') {
    buffer[len - 1] = '\0';
  }

  return buffer;
}

static void hako_run_repl(HakoRuntime *runtime) {
  printf("Hako JavaScript Runtime - Interactive Mode\n");
  printf("Type '.exit' to quit, '.help' for help\n\n");

  while (true) {
    printf("hako:%d> ", runtime->repl_line_number);
    fflush(stdout);

    char *line = hako_read_repl_line();
    if (!line) {
      printf("\n");
      break;
    }

    if (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0) {
      break;
    }

    if (strcmp(line, ".help") == 0) {
      printf("Commands:\n");
      printf("  .exit    Exit the REPL\n");
      printf("  .help    Show this help message\n");
      printf("  .clear   Clear the screen\n");
      continue;
    }

    if (strcmp(line, ".clear") == 0) {
#ifdef _WIN32
      system("cls");
#else
      system("clear");
#endif
      continue;
    }

    if (strlen(line) == 0) {
      continue;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "<repl:%d>",
             runtime->repl_line_number);

    HakoEvalResult result =
        hako_evaluate_javascript(runtime, line, strlen(line), filename);

    if (result.is_error) {
      printf("Error: %s\n", result.error_message);
      free(result.error_message);
    } else if (result.value != 0) {
      char *str = hako_value_to_string(runtime, result.value, 2);
      if (strcmp(str, "undefined") != 0 ||
          strstr(line, "console.log") == NULL) {
        printf("%s\n", str);
      }
      free(str);

      if (runtime->funcs.free_value_pointer) {
        uint32_t free_argv[2] = {runtime->js_context, result.value};
        if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                             runtime->funcs.free_value_pointer,
                                             2, free_argv))) {
          if (runtime->debug_mode) {
            fprintf(runtime->error_stream ?: stderr,
                    "[ERROR] free_value_pointer failed\n");
          }
        }
      }
    }

    runtime->repl_line_number++;
  }

  printf("Goodbye!\n");
}

static char *hako_getcwd() {
#ifdef _WIN32
  char *buffer = _getcwd(NULL, 0);
#else
  char *buffer = getcwd(NULL, 0);
#endif
  return buffer;
}

static char *hako_get_directory(const char *filepath) {
#ifdef _WIN32
  char *dir = hako_strdup(filepath);
  if (!dir)
    return NULL;

  char *last_sep = strrchr(dir, '\\');
  if (!last_sep) {
    last_sep = strrchr(dir, '/');
  }

  if (last_sep) {
    *last_sep = '\0';
  } else {
    free(dir);
    return hako_strdup(".");
  }
  return dir;
#else
  char *filepath_copy = hako_strdup(filepath);
  if (!filepath_copy)
    return NULL;

  char *dir = dirname(filepath_copy);
  char *result = hako_strdup(dir);
  free(filepath_copy);
  return result;
#endif
}

static char *hako_read_file(const char *filename, size_t *out_size) {
  FILE *file = fopen(filename, "rb");
  if (!file) {
    return NULL;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }

  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }

  char *buffer = malloc(size + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }

  size_t bytes_read = fread(buffer, 1, size, file);
  fclose(file);

  if (bytes_read != (size_t)size) {
    free(buffer);
    return NULL;
  }

  buffer[size] = '\0';
  if (out_size) {
    *out_size = size;
  }

  return buffer;
}

static uint8_t *hako_read_binary_file(const char *filename, size_t *out_size) {
  FILE *file = fopen(filename, "rb");
  if (!file) {
    return NULL;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }

  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }

  uint8_t *buffer = malloc(size);
  if (!buffer) {
    fclose(file);
    return NULL;
  }

  size_t bytes_read = fread(buffer, 1, size, file);
  fclose(file);

  if (bytes_read != (size_t)size) {
    free(buffer);
    return NULL;
  }

  if (out_size) {
    *out_size = size;
  }

  return buffer;
}

static bool hako_has_extension(const char *filename, const char *ext) {
  size_t filename_len = strlen(filename);
  size_t ext_len = strlen(ext);

  if (filename_len < ext_len) {
    return false;
  }

  return strcasecmp(filename + filename_len - ext_len, ext) == 0;
}

static char *hako_get_output_filename(const char *input_file) {
  size_t len = strlen(input_file);
  char *output = malloc(len + 5);
  if (!output)
    return NULL;

  strcpy(output, input_file);

  char *dot = strrchr(output, '.');
  if (dot && (strcmp(dot, ".js") == 0 || strcmp(dot, ".mjs") == 0)) {
    *dot = '\0';
  }

  strcat(output, ".jco");
  return output;
}

static int hako_execute_eval(HakoRuntime *runtime, const char *code) {

  g_runtime->current_module_dir = hako_getcwd();
  HakoEvalResult result =
      hako_evaluate_javascript(runtime, code, strlen(code), "<eval>");

  if (result.is_error) {
    fprintf(stderr, "%s\n", result.error_message);
    free(result.error_message);
    return EXIT_FAILURE;
  }

  if (result.value != 0) {
    char *str = hako_value_to_string(runtime, result.value, 2);
    printf("%s\n", str);
    free(str);

    if (runtime->funcs.free_value_pointer) {
      uint32_t free_argv[2] = {runtime->js_context, result.value};
      if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                           runtime->funcs.free_value_pointer, 2,
                                           free_argv))) {
        if (runtime->debug_mode) {
          fprintf(runtime->error_stream ?: stderr,
                  "[ERROR] free_value_pointer failed\n");
        }
      }
    }
  }

  return EXIT_SUCCESS;
}

static int hako_execute_file(HakoRuntime *runtime, const char *filename) {
  size_t file_size;
  char *file_content = hako_read_file(filename, &file_size);
  if (!file_content) {
    fprintf(stderr, "Error: Cannot read file '%s': %s\n", filename,
            strerror(errno));
    return EXIT_FAILURE;
  }

  runtime->current_module_dir = hako_get_directory(filename);

  HakoEvalResult result =
      hako_evaluate_javascript(runtime, file_content, file_size, filename);
  free(file_content);

  if (result.is_error) {
    fprintf(stderr, "%s\n", result.error_message);
    free(result.error_message);
    return EXIT_FAILURE;
  }

  if (result.value != 0) {
    char *str = hako_value_to_string(runtime, result.value, 2);
    if (strcmp(str, "undefined") != 0) {
      printf("%s\n", str);
    }
    free(str);

    if (runtime->funcs.free_value_pointer) {
      uint32_t free_argv[2] = {runtime->js_context, result.value};
      if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                           runtime->funcs.free_value_pointer, 2,
                                           free_argv))) {
        if (runtime->debug_mode) {
          fprintf(runtime->error_stream ?: stderr,
                  "[ERROR] free_value_pointer failed\n");
        }
      }
    }
  }

  return EXIT_SUCCESS;
}

static int hako_execute_bytecode_file(HakoRuntime *runtime,
                                      const char *filename) {
  size_t bytecode_size;
  uint8_t *bytecode = hako_read_binary_file(filename, &bytecode_size);
  if (!bytecode) {
    fprintf(stderr, "Error: Cannot read bytecode file '%s': %s\n", filename,
            strerror(errno));
    return EXIT_FAILURE;
  }

  runtime->current_module_dir = hako_get_directory(filename);

  HakoEvalResult result =
      hako_evaluate_bytecode(runtime, bytecode, bytecode_size);
  free(bytecode);

  if (result.is_error) {
    fprintf(stderr, "%s\n", result.error_message);
    free(result.error_message);
    return EXIT_FAILURE;
  }

  if (result.value != 0) {
    char *str = hako_value_to_string(runtime, result.value, 2);
    if (strcmp(str, "undefined") != 0) {
      printf("%s\n", str);
    }
    free(str);

    if (runtime->funcs.free_value_pointer) {
      uint32_t free_argv[2] = {runtime->js_context, result.value};
      if (UNLIKELY(!wasm_runtime_call_wasm(runtime->exec_env,
                                           runtime->funcs.free_value_pointer, 2,
                                           free_argv))) {
        if (runtime->debug_mode) {
          fprintf(runtime->error_stream ?: stderr,
                  "[ERROR] free_value_pointer failed\n");
        }
      }
    }
  }

  return EXIT_SUCCESS;
}

static int hako_compile_file(HakoRuntime *runtime, const char *input_file,
                             const char *output_file) {

  size_t file_size;
  char *file_content = hako_read_file(input_file, &file_size);
  if (!file_content) {
    fprintf(stderr, "Error: Cannot read file '%s': %s\n", input_file,
            strerror(errno));
    return EXIT_FAILURE;
  }

  runtime->current_module_dir = hako_get_directory(input_file);

  char *out_file = (char *)output_file;
  if (!out_file) {
    out_file = hako_get_output_filename(input_file);
    if (!out_file) {
      fprintf(stderr, "Error: Cannot determine output filename\n");
      free(file_content);
      return EXIT_FAILURE;
    }
  }

  bool success = hako_compile_to_bytecode(runtime, file_content, file_size,
                                          input_file, out_file);

  if (!output_file) {
    free(out_file);
  }
  free(file_content);

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void hako_print_usage(const char *program) {
  printf("Usage: %s [options] [file.js|file.mjs|file.jco]\n", program);
  printf("\nExecution modes:\n");
  printf("  (no args)               Start interactive REPL\n");
  printf("  file.js                 Execute JavaScript file\n");
  printf("  file.mjs                Execute JavaScript module\n");
  printf("  file.jco                Execute bytecode file\n");
  printf("  -e, --eval CODE         Evaluate JavaScript code\n");
  printf("\nCompilation:\n");
  printf("  -c, --compile FILE      Compile to bytecode (outputs FILE.jco)\n");
  printf("  -o, --output FILE       Specify output filename for compilation\n");
  printf("\nOptions:\n");
  printf("  -h, --help              Show this help message\n");
  printf("  -d, --debug             Enable debug output\n");
  printf("  --no-module-loader      Disable module loading\n");
  printf("\nExamples:\n");
  printf("  %s                      # Start REPL\n", program);
  printf("  %s script.js            # Run script.js\n", program);
  printf("  %s -e 'console.log(42)' # Evaluate code\n", program);
  printf("  %s -c script.js         # Compile to script.jco\n", program);
  printf("  %s script.jco           # Run compiled bytecode\n", program);
}

int main(int argc, char *argv[]) {
  HakoExecutionMode mode = HAKO_MODE_REPL;
  char *input_file = NULL;
  char *output_file = NULL;
  char *eval_code = NULL;
  bool debug_mode = false;
  bool disable_module_loader = false;

  int i = 1;
  while (i < argc) {
    const char *arg = argv[i];

    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      hako_print_usage(argv[0]);
      return EXIT_SUCCESS;
    } else if (strcmp(arg, "-d") == 0 || strcmp(arg, "--debug") == 0) {
      debug_mode = true;
    } else if (strcmp(arg, "--no-module-loader") == 0) {
      disable_module_loader = true;
    } else if (strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: %s requires an argument\n", arg);
        hako_print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      eval_code = argv[i];
      mode = HAKO_MODE_EVAL;
    } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--compile") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: %s requires an argument\n", arg);
        hako_print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      input_file = argv[i];
      mode = HAKO_MODE_COMPILE;
    } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "Error: %s requires an argument\n", arg);
        hako_print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      output_file = argv[i];
    } else if (arg[0] != '-') {
      input_file = argv[i];
      if (mode != HAKO_MODE_COMPILE) {
        if (hako_has_extension(input_file, ".jco")) {
          mode = HAKO_MODE_RUN_BYTECODE;
        } else {
          mode = HAKO_MODE_FILE;
        }
      }
      break;
    } else {
      fprintf(stderr, "Error: Unknown option '%s'\n", arg);
      hako_print_usage(argv[0]);
      return EXIT_FAILURE;
    }
    i++;
  }

  if (mode == HAKO_MODE_REPL && !isatty(fileno(stdin))) {
    size_t capacity = 4096;
    size_t size = 0;
    char *code = malloc(capacity);
    if (!code) {
      fprintf(stderr, "Error: Out of memory\n");
      return EXIT_FAILURE;
    }

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), stdin)) {
      size_t len = strlen(buffer);
      if (size + len >= capacity) {
        capacity *= 2;
        char *new_code = realloc(code, capacity);
        if (!new_code) {
          free(code);
          fprintf(stderr, "Error: Out of memory\n");
          return EXIT_FAILURE;
        }
        code = new_code;
      }
      memcpy(code + size, buffer, len);
      size += len;
    }
    code[size] = '\0';
    eval_code = code;
    mode = HAKO_MODE_EVAL;
  }

  HakoRuntime *runtime = hako_runtime_create();
  if (!runtime) {
    fprintf(stderr, "Failed to create Hako runtime\n");
    return EXIT_FAILURE;
  }

  runtime->debug_mode = debug_mode;
  if (disable_module_loader) {
    runtime->module_loader_enabled = false;
  }

  int exit_code = EXIT_SUCCESS;

  switch (mode) {
  case HAKO_MODE_REPL:
    hako_run_repl(runtime);
    break;

  case HAKO_MODE_EVAL:
    if (!eval_code) {
      fprintf(stderr, "Error: No code to evaluate\n");
      exit_code = EXIT_FAILURE;
    } else {
      exit_code = hako_execute_eval(runtime, eval_code);
    }
    break;

  case HAKO_MODE_FILE:
    if (!input_file) {
      fprintf(stderr, "Error: No input file specified\n");
      exit_code = EXIT_FAILURE;
    } else {
      exit_code = hako_execute_file(runtime, input_file);
    }
    break;

  case HAKO_MODE_COMPILE:
    if (!input_file) {
      fprintf(stderr, "Error: No input file specified\n");
      exit_code = EXIT_FAILURE;
    } else {
      exit_code = hako_compile_file(runtime, input_file, output_file);
    }
    break;

  case HAKO_MODE_RUN_BYTECODE:
    if (!input_file) {
      fprintf(stderr, "Error: No bytecode file specified\n");
      exit_code = EXIT_FAILURE;
    } else {
      exit_code = hako_execute_bytecode_file(runtime, input_file);
    }
    break;
  }

  hako_runtime_destroy(runtime);

  return exit_code;
}