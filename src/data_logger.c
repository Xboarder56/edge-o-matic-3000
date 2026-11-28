#include "data_logger.h"
#include "config.h"
#include "eom-hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "orgasm_control.h"
#include "orgasm_detection.h"
#include "ui/toast.h"
#include "ui/ui.h"
#include "util/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "data_logger";

enum { SAVE_INTERVAL_MS = 15000, SAMPLES_PER_BLOCK = 32 };

typedef struct {
    unsigned long elapsed_ms;
    long baseline;
    long sustained_ms;
    int sensitivity_threshold;
    uint16_t pressure;
    uint16_t avg_pressure;
    uint16_t arousal;
    uint16_t last_interval_ms;
    uint8_t motor_speed;
    uint8_t detect_state;
    uint8_t peak_count;
    char detect_mode;
} recording_sample_t;

typedef struct sample_block {
    struct sample_block* next;
    size_t count;
    recording_sample_t samples[SAMPLES_PER_BLOCK];
} sample_block_t;

static struct {
    unsigned long last_save_ms;
    sample_block_t* first;
    sample_block_t* last;
    unsigned long recording_start_ms;
    FILE* logfile;
} logger_state;

// The serial protocol starts at average pressure. SD rows prefix it with time and raw pressure.
static int write_readings(FILE* file, const recording_sample_t* sample) {
    return fprintf(
        file, "%d,%d,%d,%d,%d,%c,%ld,%ld,%d,%d\n",
        sample->avg_pressure, sample->arousal, sample->motor_speed,
        sample->sensitivity_threshold, sample->detect_state, sample->detect_mode,
        sample->baseline, sample->sustained_ms, sample->peak_count, sample->last_interval_ms
    );
}

static bool buffer_sample(const recording_sample_t* sample) {
    if (!logger_state.last || logger_state.last->count == SAMPLES_PER_BLOCK) {
        sample_block_t* block = calloc(1, sizeof(*block));
        if (!block) return false;
        if (logger_state.last) {
            logger_state.last->next = block;
        } else {
            logger_state.first = block;
        }
        logger_state.last = block;
    }
    logger_state.last->samples[logger_state.last->count++] = *sample;
    return true;
}

static bool save_recording(void) {
    FILE* file = logger_state.logfile;
    bool ok = !ferror(file);
    while (logger_state.first) {
        sample_block_t* block = logger_state.first;
        for (size_t i = 0; ok && i < block->count; ++i) {
            const recording_sample_t* sample = &block->samples[i];
            ok = fprintf(file, "%lu,%u,", sample->elapsed_ms, (unsigned)sample->pressure) >= 0
                 && write_readings(file, sample) >= 0;
        }
        logger_state.first = block->next;
        free(block);
    }
    logger_state.last = NULL;
    return ok && fflush(file) == 0 && fsync(fileno(file)) == 0;
}

static bool close_recording(bool ok) {
    if (fclose(logger_state.logfile) != 0) ok = false;
    logger_state.logfile = NULL;
    ui_set_icon(UI_ICON_RECORD, -1);
    if (ok) {
        ui_toast("%s", _("Recording stopped."));
    } else {
        ESP_LOGE(TAG, "Failed to save complete recording");
        ui_toast("%s", _("Recording save error!"));
    }
    return ok;
}

void data_logger_init(void) {
    logger_state.logfile = NULL;
    logger_state.recording_start_ms = 0;
}

void data_logger_start_recording(void) {
    if (logger_state.logfile) {
        data_logger_stop_recording();
    }

    ui_toast_blocking("%s", _("Preapring recording..."));

    time_t now;
    struct tm timeinfo;
    char filename_date[32];
    time(&now);

    if (!localtime_r(&now, &timeinfo)) {
        ESP_LOGE(TAG, "Failed to obtain time");
        sniprintf(filename_date, 32, "%lld", (esp_timer_get_time() / 1000UL));
    } else {
        strftime(filename_date, 32, "%Y%m%d-%H%M%S", &timeinfo);
    }

    char* logfile_name = NULL;
    asiprintf(&logfile_name, "%s/log-%s.csv", eom_hal_get_sd_mount_point(), filename_date);

    if (!logfile_name) {
        ESP_LOGE(TAG, "Logfile filename buffer issues.");
        ui_toast("%s", _("Error opening logfile!"));
        return;
    }

    ESP_LOGI(TAG, "Opening logfile: %s", logfile_name);
    logger_state.logfile = fopen(logfile_name, "w+");

    if (!logger_state.logfile) {
        ESP_LOGE(TAG, "Couldn't open logfile to save! (%s)", logfile_name);
        ui_toast("%s", _("Error opening logfile!"));
    } else {
        logger_state.recording_start_ms = (esp_timer_get_time() / 1000UL);
        logger_state.last_save_ms = logger_state.recording_start_ms;

        fprintf(
            logger_state.logfile,
            "millis,pressure,avg_pressure,arousal,motor_speed,sensitivity_threshold,"
            "detect_state,detect_mode_used,baseline,sustained_ms,peak_count,last_interval_ms\n"
        );

        ui_set_icon(UI_ICON_RECORD, RECORD_ICON_RECORDING);
        char* fntok = basename(logfile_name);
        ui_toast(_("Recording started:\n%s"), fntok);
    }

    free(logfile_name);
}

bool data_logger_finish_recording(void) {
    if (!logger_state.logfile) return true;
    ui_toast_blocking("%s", _("Stopping..."));
    ESP_LOGI(TAG, "Closing logfile.");
    return close_recording(save_recording());
}

void data_logger_stop_recording(void) {
    data_logger_finish_recording();
}

bool data_logger_is_recording(void) {
    return logger_state.logfile != NULL;
}

void data_logger_tick(unsigned long tick_ms) {
    if (!logger_state.logfile && !Config.classic_serial) return;

    recording_sample_t sample = {
        .avg_pressure = orgasm_control_get_average_pressure(),
        .arousal = orgasm_control_get_arousal(),
        .motor_speed = eom_hal_get_motor_speed(),
        .sensitivity_threshold = Config.sensitivity_threshold,
        .detect_state = orgasm_detection_get_state(),
        .detect_mode = orgasm_detection_last_was_rhythmic() ? 'R' : 'S',
        .baseline = orgasm_detection_get_baseline(),
        .sustained_ms = orgasm_detection_get_sustained_ms(),
        .peak_count = orgasm_detection_get_peak_count(),
        .last_interval_ms = orgasm_detection_get_last_interval_ms(),
    };

    if (Config.classic_serial) write_readings(stdout, &sample);
    if (!logger_state.logfile) return;

    sample.elapsed_ms = tick_ms - logger_state.recording_start_ms;
    sample.pressure = orgasm_control_get_last_pressure();
    if (!buffer_sample(&sample)) {
        ESP_LOGE(TAG, "Out of memory buffering recording");
        save_recording();
        close_recording(false);
        return;
    }

    if (tick_ms - logger_state.last_save_ms >= SAVE_INTERVAL_MS) {
        logger_state.last_save_ms = tick_ms;
        if (!save_recording()) close_recording(false);
    }
}
