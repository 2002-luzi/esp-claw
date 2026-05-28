/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "tts_provider.h"

#include <string.h>

extern const tts_provider_t tts_provider_xiao_mimo;

static const tts_provider_t *const s_providers[] = {
    &tts_provider_xiao_mimo,
};

const tts_provider_t *tts_provider_find(const char *name)
{
    const char *target = (name && name[0]) ? name : "xiao_mimo";

    for (size_t i = 0; i < sizeof(s_providers) / sizeof(s_providers[0]); i++) {
        if (strcmp(s_providers[i]->name, target) == 0) {
            return s_providers[i];
        }
    }
    return NULL;
}
