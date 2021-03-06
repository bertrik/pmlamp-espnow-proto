#include <stdint.h>
#include <stdbool.h>

#include <Arduino.h>

#include <espnow.h>

#include "editline.h"
#include "cmdproc.h"

#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

#define printf Serial.printf

#define ESPNOW_CHANNEL 1

static char esp_id[16];
static char editline[256];
static uint8_t bcast_mac[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint8_t peer_mac[6];

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

static bool send_unicast(uint8_t * mac, const char *data)
{
    printf("send_unicast to %02X:%02X:%02X:%02X:%02X:%02X... %s\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], data);

    esp_now_add_peer(mac, ESP_NOW_ROLE_SLAVE, ESPNOW_CHANNEL, NULL, 0);
    int rc = esp_now_send((u8 *) mac, (u8 *) data, (u8) strlen(data));
    if (rc != 0) {
        printf("esp_now_send fail %d\n", rc);
    }
    return rc == 0;
}

// sends a unicast packet, blocks until transmit result is available
static bool send_unicast_blocking(uint8_t * mac, const char *data)
{
    tx_event.status = -1;
    tx_event.event = false;
    if (send_unicast(mac, data)) {
        while (!tx_event.event) yield();
        tx_event.event = false;
    }
    return tx_event.status == 0;
}

static int do_led(int argc, char *argv[])
{
    static struct color_entry_t {
        char c;
        const char *name;
        const char *rgb;
    } color_table[] = {
        {'r', "red", "FF0000" },
        {'g', "green", "00FF00" },
        {'b', "blue", "0000FF" },
        {'c', "cyan", "00FFFF" },
        {'m', "magenta", "FF00FF" },
        {'y', "yellow", "FFFF00" },
        {'k', "black", "000000" },
        {'w', "white", "FFFFFF" },
        {'\0'}
    };

    if (argc < 2) {
        printf("Available colors:\n");
        for (color_entry_t *entry = color_table; entry->c != 0; entry++) {
            printf("%c %8s %s\n", entry->c, entry->name, entry->rgb);
        }
        return 0;
    }

    char *color = argv[1];
    const char *rgb = "000000";
    if (strlen(color) == 1) {
        for (color_entry_t *entry = color_table; entry->c != 0; entry++) {
            if (*color == entry->c) {
                printf("Selecting color '%s'\n", entry->name);
                rgb = entry->rgb;
            }
        }
    } else if (strlen(color) == 6) {
        rgb = color;
    }

    DynamicJsonDocument doc(500);
    doc["msg"] = "data";
    doc["color"] = "#" + String(rgb);
    String json;
    serializeJson(doc, json);

    const char *cmd = json.c_str();
    bool ok = send_unicast_blocking(peer_mac, cmd);
    return ok ? CMD_OK : -1;
}

static int do_ping(int argc, char *argv[])
{
    DynamicJsonDocument doc(500);
    doc["msg"] = "ping";
    String json;
    serializeJson(doc, json);
    const char *cmd = json.c_str();
    bool ok = send_unicast_blocking(peer_mac, cmd);
    return ok ? CMD_OK : -1;
}

static int do_help(int argc, char *argv[]);
const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { "led", do_led, "<hexcode|colorcode> Send LED value" },
    { "ping", do_ping, "Pings the lamp" },
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

// copies data to be processed up by non-callback code
static void tx_callback(uint8_t * mac, uint8_t status)
{
    memcpy(tx_event.mac, mac, 6);
    tx_event.status = status;
    tx_event.event = true;
}

// handles transmitted data
static void process_tx(uint8_t * mac, uint8_t status)
{
    printf("tx: %02X:%02X:%02X:%02X:%02X:%02X, stat = 0x%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], status);
}

// copies data to be processed by non-callback code
static void rx_callback(uint8_t * mac, uint8_t * data, uint8_t len)
{
    memcpy(rx_event.mac, mac, 6);
    memcpy(rx_event.data, data, len);
    rx_event.len = len;
    rx_event.event = true;
}

// handled received data
static void process_rx(uint8_t * mac, uint8_t * data, uint8_t len)
{
    printf("rx %d bytes from: %02X:%02X:%02X:%02X:%02X:%02X: ", len,
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
    // handle "discover" packet
    const char *msg = doc["msg"];
    if (strcmp(msg, "discover") == 0) {
        // send back "associate"
        doc.clear();
        doc["msg"] = "associate";
        doc["id"] = "SENSOR-" + String(esp_id);
        String rsp;
        serializeJson(doc, rsp);
        const char *response = rsp.c_str();
        if (send_unicast_blocking(mac, response)) {
            memcpy(peer_mac, mac, 6);
        } else {
            printf("failed to send associate message\n");
        }
    }
}

void setup(void)
{
    Serial.begin(115200);
    EditInit(editline, sizeof(editline));

    // get ESP id
    sprintf(esp_id, "%08X", ESP.getChipId());
    printf("ESP ID: %s\n", esp_id);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_add_peer(bcast_mac, ESP_NOW_ROLE_CONTROLLER, ESPNOW_CHANNEL, NULL, 0);
    esp_now_register_recv_cb(rx_callback);
    esp_now_register_send_cb(tx_callback);

    // simulate being connected over WiFi at the same time   
    printf("Connecting ...");
    WiFi.begin("Freifunk-disabled");
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
