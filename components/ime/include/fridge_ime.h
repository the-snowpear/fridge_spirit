// 冰箱小精灵 26 键全拼输入法接口。
// 词库启动时从 TF 卡载入 PSRAM；支持全拼前缀联想、简拼与选词后联想。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_IME_MAX_PINYIN 32
#define FRIDGE_IME_MAX_CANDIDATES 12
#define FRIDGE_IME_MAX_CANDIDATE_BYTES 64

typedef struct {
    bool dictionary_loaded;
    bool association_mode; // true=选词后联想，false=拼音/简拼查询
    char preedit[FRIDGE_IME_MAX_PINYIN + 1];
    size_t candidate_count;
    size_t candidate_total;
    size_t candidate_page;
    size_t candidate_page_count;
    char candidates[FRIDGE_IME_MAX_CANDIDATES][FRIDGE_IME_MAX_CANDIDATE_BYTES];
    char last_error[96];
} fridge_ime_snapshot_t;

esp_err_t fridge_ime_init(void);
// UI 可能早于 TF 卡挂载；由 UI 任务定时调用此接口，挂载完成后幂等加载词库到 PSRAM。
esp_err_t fridge_ime_try_load_tf_dictionary(void);
esp_err_t fridge_ime_attach_textarea(lv_obj_t *textarea);
// 设置当前编辑器每页候选数；范围 1..FRIDGE_IME_MAX_CANDIDATES，词库与排序池不受影响。
esp_err_t fridge_ime_set_candidate_page_size(size_t page_size);
void fridge_ime_feed_letter(char letter);
void fridge_ime_backspace(void);
void fridge_ime_clear_preedit(void);
esp_err_t fridge_ime_select_candidate(size_t index);
void fridge_ime_get_snapshot(fridge_ime_snapshot_t *out);
esp_err_t fridge_ime_next_candidate_page(void);
esp_err_t fridge_ime_previous_candidate_page(void);

#ifdef __cplusplus
}
#endif
