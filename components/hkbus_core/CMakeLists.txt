idf_component_register(
    SRCS
        "hk_fb.cc"
        "hk_font.cc"
        "hk_model.cc"
        "hk_screen.cc"
        "hk_weather.cc"
        "third_party/qrcodegen.c"
    INCLUDE_DIRS "include" "third_party"
    REQUIRES json
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
