/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_tts.h"

#include <stdio.h>
#include <stdlib.h>

#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "claw_cap.h"
#include "esp_console.h"

static struct {
    struct arg_lit *speak;
    struct arg_lit *enqueue_front;
    struct arg_lit *status;
    struct arg_lit *clear_pending;
    struct arg_lit *stop_current;
    struct arg_str *text;
    struct arg_str *source;
    struct arg_end *end;
} tts_args;

static int cap_tts_call(const char *cap_name, const char *input_json)
{
    char *output = NULL;
    esp_err_t err;
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
    };

    output = calloc(1, 4096);
    if (!output) {
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call(cap_name, input_json, &ctx, output, 4096);
    if (err != ESP_OK) {
        printf("%s\n", output[0] ? output : esp_err_to_name(err));
        free(output);
        return 1;
    }

    printf("%s\n", output);
    free(output);
    return 0;
}

static int tts_func(int argc, char **argv)
{
    cJSON *root = NULL;
    char *input_json = NULL;
    const char *cap_name = NULL;
    int nerrors = arg_parse(argc, argv, (void **) &tts_args);
    int operation_count;
    int rc;

    if (nerrors != 0) {
        arg_print_errors(stderr, tts_args.end, argv[0]);
        return 1;
    }

    operation_count = tts_args.speak->count + tts_args.enqueue_front->count +
                      tts_args.status->count + tts_args.clear_pending->count +
                      tts_args.stop_current->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    if (tts_args.speak->count || tts_args.enqueue_front->count) {
        if (!tts_args.text->count) {
            printf("'--speak' and '--enqueue-front' require '--text'\n");
            return 1;
        }

        root = cJSON_CreateObject();
        if (!root) {
            printf("Out of memory\n");
            return 1;
        }

        cJSON_AddStringToObject(root, "text", tts_args.text->sval[0]);
        if (tts_args.source->count) {
            cJSON_AddStringToObject(root, "source", tts_args.source->sval[0]);
        }

        cap_name = tts_args.speak->count ? "tts_speak" : "tts_enqueue_front";
    } else {
        root = cJSON_CreateObject();
        if (!root) {
            printf("Out of memory\n");
            return 1;
        }

        if (tts_args.status->count) {
            cap_name = "tts_status";
        } else if (tts_args.clear_pending->count) {
            cap_name = "tts_clear_pending";
        } else {
            cap_name = "tts_stop_current";
        }
    }

    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        printf("Out of memory\n");
        return 1;
    }

    rc = cap_tts_call(cap_name, input_json);
    free(input_json);
    return rc;
}

void register_cap_tts(void)
{
    tts_args.speak = arg_lit0(NULL, "speak", "Append a TTS utterance to the tail queue");
    tts_args.enqueue_front = arg_lit0(NULL,
                                      "enqueue-front",
                                      "Insert a TTS utterance at the front of the pending queue");
    tts_args.status = arg_lit0(NULL, "status", "Show the TTS runtime status");
    tts_args.clear_pending = arg_lit0(NULL,
                                      "clear-pending",
                                      "Clear queued TTS utterances without stopping active playback");
    tts_args.stop_current = arg_lit0(NULL,
                                     "stop-current",
                                     "Stop the current active TTS utterance");
    tts_args.text = arg_str0("t", "text", "<text>", "TTS text content");
    tts_args.source = arg_str0("s", "source", "<source>", "Optional request source tag");
    tts_args.end = arg_end(8);

    const esp_console_cmd_t tts_cmd = {
        .command = "tts",
        .help = "TTS operations.\n"
        "Examples:\n"
        " tts --speak --text \"hello\"\n"
        " tts --enqueue-front --text \"urgent\" --source alarm\n"
        " tts --status\n"
        " tts --clear-pending\n"
        " tts --stop-current\n",
        .func = tts_func,
        .argtable = &tts_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&tts_cmd));
}
