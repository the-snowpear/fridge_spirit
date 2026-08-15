# 当前 main 分支固定为 P4 板载 ES8311；S3/INMP441 完整实现已在 s3-legacy 分支存档。
# 不在此处判断 CONFIG_IDF_TARGET_*，因为 IDF 6 展开 REQUIRES 时尚未加载 sdkconfig。
idf_component_register(
    SRCS "fridge_audio_p4.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer freertos heap log codec_es8311 fy_sp703
)
