#include "display_backend.h"

#include "hub75.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "factory_display_s3";

Hub75Config MakeDisplayConfig(uint8_t brightness) {
    Hub75Config config{};

    // Exact 64x32 P5/four-scan configuration from the user's working S3 test.
    config.panel_width = 64;
    config.panel_height = 32;
    config.scan_wiring = Hub75ScanWiring::SCAN_1_8_32PX_FULL;
    config.shift_driver = Hub75ShiftDriver::FM6126A;
    config.double_buffer = true;
    config.layout_rows = 1;
    config.layout_cols = 1;
    config.layout = Hub75PanelLayout::TOP_LEFT_DOWN_ZIGZAG;
    config.min_refresh_rate = 150;
    config.brightness = brightness;


// ESP32-S3 DevKitC-1 P5 wiring from S3_P5_test.
config.pins.r1 = 4;
config.pins.g1 = 5;
config.pins.b1 = 6;
config.pins.r2 = 7;
config.pins.g2 = 15;
config.pins.b2 = 16;
config.pins.a = 11;
config.pins.b = 12;
config.pins.c = 13;
config.pins.d = -1;
config.pins.e = -1;
config.pins.lat = 9;
config.pins.oe = 10;
config.pins.clk = 8;

	
	
/*
//============================ HUB75 ETH Development Board ================================

// Upper RGB
config.pins.r1 = 33;
config.pins.g1 = 34;
config.pins.b1 = 35;

// Lower RGB
config.pins.r2 = 36;
config.pins.g2 = 37;
config.pins.b2 = 38;

// Address
config.pins.a = 1;
config.pins.b = 2;
config.pins.c = 15;
config.pins.d = -1;
config.pins.e = -1;

// Control
config.pins.lat = 16;
config.pins.oe  = 21;
config.pins.clk = 47;

//==========================================================================================
*/
	

    return config;
}

Hub75Driver* s_driver = nullptr;

}  // namespace

extern "C" bool factory_display_backend_init(uint8_t brightness) {
    static Hub75Config config = MakeDisplayConfig(brightness);
    static Hub75Driver driver(config);
    s_driver = &driver;

    if (!s_driver->begin()) {
        ESP_LOGE(kTag, "Hub75Driver::begin failed");
        s_driver = nullptr;
        return false;
    }

    s_driver->set_brightness(brightness);
    s_driver->clear();
    s_driver->flip_buffer();
    s_driver->clear();

    ESP_LOGI(kTag,
             "ESP32-S3 P5 backend ready: 64x32 1/8-full FM6126A GDMA pins R1=4 G1=5 B1=6 R2=7 G2=15 B2=16 A=11 B=12 C=13 LAT=9 OE=10 CLK=8 brightness=%u",
             static_cast<unsigned>(brightness));
    return true;
}

extern "C" void factory_display_backend_clear(void) {
    if (s_driver) s_driver->clear();
}

extern "C" void factory_display_backend_fill_rect(int x, int y, int width, int height,
                                                    uint8_t r, uint8_t g, uint8_t b) {
    if (!s_driver || width <= 0 || height <= 0) return;
    s_driver->fill(static_cast<uint16_t>(x), static_cast<uint16_t>(y),
                   static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                   r, g, b);
}

extern "C" void factory_display_backend_flip(void) {
    if (s_driver) s_driver->flip_buffer();
}
