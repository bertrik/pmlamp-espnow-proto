#include <stdint.h>
#include <stdbool.h>

#include <Arduino.h>

#include <espnow.h>

#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <MiniShell.h>


#define printf Serial.printf

#define ESPNOW_CHANNEL 1
#define KEEPALIVE_TIMEOUT   300

// we send the colour to two sets of LEDs: a single LED on pin D2, a star of 7 LEDs on pin D4
#define DATA_PIN_1LED   D2
#define DATA_PIN_7LED   D4

static char esp_id[16];
static uint8_t bcast_mac[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool associated = false;
static unsigned long last_received = 0;
static Adafruit_NeoPixel strip1(1, DATA_PIN_1LED, NEO_GRB + NEO_KHZ400);
static Adafruit_NeoPixel strip7(7, DATA_PIN_7LED, NEO_GRB + NEO_KHZ400);
static MiniShell shell(&Serial);

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
    return 0;
}

static void process_tx(uint8_t * mac, uint8_t status)
{
    printf("tx: %02X:%02X:%02X:%02X:%02X:%02X, stat = 0x%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], status);
}

static void process_rx(uint8_t * mac, uint8_t * data, uint8_t len)
{
    printf("rx %d bytes from: %02X:%02X:%02X:%02X:%02X:%02X\n", len,
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
            uint32_t rgb = strtoul(color + 1, NULL, 16);
            set_color(rgb);
        }
    }
    if (strcmp(msg, "ping") == 0) {
        doc.clear();
        doc["msg"] = "pong";
        String json;
        serializeJson(doc, json);
        send_unicast(mac, json.c_str());
    }
}

static void strip_fill(Adafruit_NeoPixel & strip, uint32_t rgb)
{
    for (int i = 0; i < strip.numPixels(); i++) {
        strip.setPixelColor(i, rgb);
    }
    strip.show();
}

static void set_color(uint32_t rgb)
{
    strip_fill(strip1, rgb);
    strip_fill(strip7, rgb);
}

void setup(void)
{
    Serial.begin(115200);

#ifdef LED_RGB
    strip1.updateType(NEO_BGR);
    strip7.updateType(NEO_BGR);
#endif
#ifdef LED_GRB
    strip1.updateType(NEO_GRB);
    strip7.updateType(NEO_GRB);
#endif
    strip1.begin();
    strip7.begin();

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
    static unsigned long last_second = -1;
    unsigned long second = millis() / 1000;

    // process ESP-NOW events
    if (rx_event.event) {
        last_received = second;
        process_rx(rx_event.mac, rx_event.data, rx_event.len);
        rx_event.event = false;
    }
    if (tx_event.event) {
        process_tx(tx_event.mac, tx_event.status);
        tx_event.event = false;
    }

    // send every second
    if (associated) {
        if ((second - last_received) > KEEPALIVE_TIMEOUT) {
            associated = false;
        }
    } else {
        if (second != last_second) {
            set_color(0x0000FF);
            DynamicJsonDocument doc(500);
            doc["msg"] = "discover";
            doc["id"] = "PMLAMP-" + String(esp_id);

            String json;
            serializeJson(doc, json);

            printf("Sending...\n");
            esp_now_add_peer(bcast_mac, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
            if (!send_broadcast(json.c_str())) {
                printf("send_broadcast failed!\n");
            }

            last_second = second;
            set_color(0x000000);
        }
    }

    // parse command line
    // command line processing
    shell.process(">", commands);
}
