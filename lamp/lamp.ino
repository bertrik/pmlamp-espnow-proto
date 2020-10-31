#include <stdint.h>
#include <stdbool.h>

#include <Arduino.h>

#include <espnow.h>

#include "editline.h"
#include "cmdproc.h"

#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <FastLED.h>


#define printf Serial.printf

#define ESPNOW_CHANNEL 1
#define DATA_PIN_1LED   D2

static char esp_id[16];
static char editline[256];
static uint8_t bcast_mac[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool associated = false;
static CRGB leds[1];

static bool send_broadcast(const char *data)
{
    printf("send_broadcast %s...\n", data);

    int rc = esp_now_send((u8 *) bcast_mac, (u8 *) data, strlen(data));
    return rc == 0;
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

static void tx_callback(uint8_t * mac, uint8_t status)
{
    printf("tx: %02X:%02X:%02X:%02X:%02X:%02X, stat = 0x%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], status);
}

static void rx_callback(uint8_t * mac, uint8_t * data, uint8_t len)
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
        if (color[0] == '#') {
            int rgb = strtoul(color + 1, NULL, 16);
            FastLED.showColor(rgb);
        }
    }
}

void setup(void)
{
    Serial.begin(115200);
    EditInit(editline, sizeof(editline));

    FastLED.addLeds < WS2812B, DATA_PIN_1LED, GRB > (leds, 1).setCorrection(TypicalSMD5050);

    // get ESP id
    sprintf(esp_id, "%08X", ESP.getChipId());
    printf("ESP ID: %s\n", esp_id);

    WiFi.mode(WIFI_AP);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_add_peer(bcast_mac, ESP_NOW_ROLE_SLAVE, ESPNOW_CHANNEL, NULL, 0);
    esp_now_register_recv_cb(rx_callback);
    esp_now_register_send_cb(tx_callback);
}

void loop(void)
{
    static int index = 0;

    // send every second
    static int last_period = -1;
    int period = millis() / 1000;
    if (!associated && (period != last_period)) {
        FastLED.showColor(CRGB::Blue);
        DynamicJsonDocument doc(500);
        doc["msg"] = "discover";
        doc["id"] = "PMLAMP-" + String(esp_id);

        String json;
        serializeJson(doc, json);

        int channel = index + 1;
        printf("Sending on channel %d...\n", channel);
        esp_now_add_peer(bcast_mac, ESP_NOW_ROLE_COMBO, channel, NULL, 0);
        if (!send_broadcast(json.c_str())) {
            printf("send_broadcast failed!\n");
        }

        last_period = period;
        index = (index + 1) % 12;
        FastLED.showColor(CRGB::Black);
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
