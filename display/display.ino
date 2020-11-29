// an SSD1306 display showing current PM values from a PM sensor sending PM-values over ESP-NOW

#include <stdint.h>
#include <stdbool.h>

#include <Arduino.h>
#include <espnow.h>
#include <ESP8266WiFi.h>

#include "editline.h"
#include "cmdproc.h"

#include <Wire.h>
#include "SSD1306Wire.h"

#include <ArduinoJson.h>


#define printf Serial.printf
#define ESPNOW_CHANNEL 1

#define OLED_I2C_ADDR 0x3C

#define PIN_OLED_SCL    D1
#define PIN_OLED_SDA    D2

static char esp_id[16];
static char editline[256];
static uint8_t bcast_mac[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// WEMOS-D1-MINI display shield, 64x48 pixels, apply offset of (32,16) for top-left 
static SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL);

static bool associated = false;

static struct rx_event_t {
    bool event;
    uint8_t mac[6];
    uint8_t data[256];
    size_t len;
} rx_event;

static struct tx_event_t {
    bool event;
    uint8_t mac[6];
    int status;
} tx_event;

// copies data to be processed by non-callback code
static void rx_callback(uint8_t * mac, uint8_t * data, uint8_t len)
{
    memcpy(rx_event.mac, mac, 6);
    memcpy(rx_event.data, data, len);
    rx_event.len = len;
    rx_event.event = true;
}

// copies data to be processed up by non-callback code
static void tx_callback(uint8_t * mac, uint8_t status)
{
    memcpy(tx_event.mac, mac, 6);
    tx_event.status = status;
    tx_event.event = true;
}

static bool send_broadcast(const char *data)
{
    printf("send_broadcast %s...\n", data);

    int rc = esp_now_send((u8 *) bcast_mac, (u8 *) data, strlen(data));
    return rc == 0;
}

static bool send_unicast(uint8_t * mac, const char *data)
{
    esp_now_add_peer(mac, ESP_NOW_ROLE_SLAVE, ESPNOW_CHANNEL, NULL, 0);
    return esp_now_send((u8 *) mac, (u8 *) data, (u8) strlen(data)) == 0;
}

static int do_help(int argc, char *argv[]);
const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { NULL, NULL, NULL }
};

static void show_help(const cmd_t * cmds)
{
    for (const cmd_t * cmd = cmds; cmd->cmd != NULL; cmd++) {
        printf("%10s: %s\n", cmd->name, cmd->help);
    }
}

static int do_help(int argc, char *argv[])
{
    show_help(commands);
    return CMD_OK;
}

static void process_tx(uint8_t * mac, uint8_t status)
{
    printf("tx: %02X:%02X:%02X:%02X:%02X:%02X, stat = 0x%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], status);
}

static void process_rx(uint8_t * mac, uint8_t * data, uint8_t len)
{
    printf("rx %d bytes from: %02X:%02X:%02X:%02X:%02X:%02X:\n", len,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char json[250];
    memcpy(json, data, len);
    json[len] = '\0';
    printf("%s\n", json);

    DynamicJsonDocument doc(500);
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        printf("deserializeJson error\n");
        return;
    }

    const char *msg = doc["msg"];
    if (strcmp(msg, "associate") == 0) {
        associated = true;
    }
    if (strcmp(msg, "data") == 0) {
        printf("received DATA!\n");
        const char *color = doc["color"];
        display.clear();
        display.setFont(ArialMT_Plain_16);
        display.drawString(32, 16, color);
        display.display();
    }
    if (strcmp(msg, "ping") == 0) {
        doc.clear();
        doc["msg"] = "pong";
        String json;
        serializeJson(doc, json);
        send_unicast(mac, json.c_str());
    }
}

void setup(void)
{
    Serial.begin(115200);
    EditInit(editline, sizeof(editline));

    // get ESP id
    sprintf(esp_id, "%08X", ESP.getChipId());
    printf("ESP ID: %s\n", esp_id);

    WiFi.mode(WIFI_AP);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_add_peer(bcast_mac, ESP_NOW_ROLE_SLAVE, ESPNOW_CHANNEL, NULL, 0);
    esp_now_register_recv_cb(rx_callback);
    esp_now_register_send_cb(tx_callback);

    // display init
    display.init();
    display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(32, 16, esp_id);
    display.display();
}

void loop(void)
{
    // process ESP-NOW events
    if (rx_event.event) {
        process_rx(rx_event.mac, rx_event.data, rx_event.len);
        rx_event.event = false;
    }
    if (tx_event.event) {
        process_tx(tx_event.mac, tx_event.status);
        tx_event.event = false;
    }
    // send every second
    static int last_period = -1;
    int period = millis() / 1000;
    if (!associated && (period != last_period)) {
        DynamicJsonDocument doc(500);
        doc["msg"] = "discover";
        doc["id"] = "PMDISPLAY-" + String(esp_id);

        String json;
        serializeJson(doc, json);

        printf("Sending...\n");
        esp_now_add_peer(bcast_mac, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
        if (!send_broadcast(json.c_str())) {
            printf("send_broadcast failed!\n");
        }

        last_period = period;
    }
    // parse command line
    bool haveLine = false;
    if (Serial.available()) {
        char c;
        haveLine = EditLine(Serial.read(), &c);
        Serial.write(c);
    }
    if (haveLine) {
        int result = cmd_process(commands, editline);
        switch (result) {
        case CMD_OK:
            printf("OK\n");
            break;
        case CMD_NO_CMD:
            break;
        case CMD_UNKNOWN:
            printf("Unknown command, available commands:\n");
            show_help(commands);
            break;
        case CMD_ARG:
            printf("Invalid argument(s)\n");
            break;
        default:
            printf("%d\n", result);
            break;
        }
        printf(">");
    }
}
