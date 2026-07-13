idf_component_register(
    SRCS "fridge_tf_http.c"
    INCLUDE_DIRS "include"
    REQUIRES tfcard network esp_http_server esp_timer esp_system
)
