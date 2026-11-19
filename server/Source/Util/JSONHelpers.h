#ifndef JSONHELPERS_H
#define JSONHELPERS_H

#include <Util/Log.h>

/* Reads a member into var; when it is absent var takes fallback and, unless
   the caller says the member is optional, a warning names it. The member's
   name doubles as the local it is read into, so jsonvar must be an
   identifier. */
#define READ_VAR_FROM_JSON(type, fmt, pj, var, jsonvar, description, fallback, optional)                 \
    {                                                                                                    \
        struct json_object* _##var;                                                                      \
        if (!json_object_object_get_ex(pj, #jsonvar, &_##var)) {                                         \
            if (optional == 0) {                                                                         \
                LOG_WARNING("Failed to find " description " in config, falling back to " fmt, fallback); \
            }                                                                                            \
            var = fallback;                                                                              \
        } else                                                                                           \
            var = json_object_get_##type(_##var);                                                        \
    }

#define READ_INT_FROM_JSON(pj, var, jsonvar, description, fallback, optional) \
    READ_VAR_FROM_JSON(int, "%i", pj, var, jsonvar, description, fallback, optional)
#define READ_DOUBLE_FROM_JSON(pj, var, jsonvar, description, fallback, optional) \
    READ_VAR_FROM_JSON(double, "%lf", pj, var, jsonvar, description, fallback, optional)
#define READ_STR_FROM_JSON(pj, var, jsonvar, description, fallback, optional) \
    READ_VAR_FROM_JSON(string, "\"%s\"", pj, var, jsonvar, description, fallback, optional)

/* A stored integer array may be shorter than the current build expects: the
   missing tail is filled from fallback, one warning per element. */
#define READ_INT_ARR_FROM_JSON(pj, var, jsonvar, description, fallback, fbsize, optional)                        \
    {                                                                                                            \
        struct json_object* _##var;                                                                              \
        if (!json_object_object_get_ex(pj, #jsonvar, &_##var)) {                                                 \
            if (optional == 0) {                                                                                 \
                LOG_WARNING("Failed to find " description " in config, falling back to " #fallback);             \
            }                                                                                                    \
            for (int i = 0; i < fbsize; ++i)                                                                     \
                var[i] = fallback[i];                                                                            \
        } else {                                                                                                 \
            int _len_##var = json_object_array_length(_##var);                                                   \
            for (int i = 0; i < fbsize; ++i) {                                                                   \
                if (i < _len_##var) {                                                                            \
                    var[i] = json_object_get_int(json_object_array_get_idx(_##var, i));                          \
                } else {                                                                                         \
                    LOG_WARNING("Config option " #jsonvar "[%i] not found, falling back to %i", i, fallback[i]); \
                    var[i] = fallback[i];                                                                        \
                }                                                                                                \
            }                                                                                                    \
        }                                                                                                        \
    }

#endif
