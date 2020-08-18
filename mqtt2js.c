/*
Copyright 2020 Cedric Priscal

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <sys/types.h> // open()
#include <sys/stat.h> // open()
#include <fcntl.h> // open()
#include <unistd.h> // getopt(), read()
#include <string.h> // strdup()
#include <stdlib.h> // strtol(), exit()
#include <stdio.h> // fprintf(), perror(), snprintf()
#include <libgen.h> // basename()
#include <linux/joystick.h> // JS_EVENT_BUTTON, JS_EVENT_AXIS
#include <stdbool.h> // bool, true, false
#include <mosquitto.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <json.h>

#ifndef VERSION
#define VERSION "0.1"
#endif

// Define program arguments with default values
const char* prog_name = "mqtt2js";
const char* device_name = "mqtt2js virtual joystick";
const char* mqtt_server_address = "localhost";
unsigned short mqtt_server_port = 1883;
const char* topic = "/joystick";
bool debug = false;

// Global variables
struct json_tokener *tok;
struct libevdev_uinput *uidev;

// Constants
#define MOSQUITTO_KEEP_ALIVE 60

// Buttons and axis corresponding to an XBOX controller
// /usr/include/linux/input-event-codes.h

#define BTN_COUNT 11
const unsigned int BTN[BTN_COUNT] = {
    BTN_A,
    BTN_B,
    BTN_X,
    BTN_Y,
    BTN_TL, // TL2?
    BTN_TR,
    BTN_SELECT,
    BTN_START,
    BTN_TASK, // XBOX button?
    BTN_THUMBL,
    BTN_THUMBR
};

#define AXIS_COUNT 8
const unsigned int AXIS[AXIS_COUNT] = {
    ABS_HAT0X,
    ABS_HAT0Y,
    ABS_BRAKE, // left trigger
    ABS_HAT1X,
    ABS_HAT1Y,
    ABS_GAS, // right trigger
    ABS_HAT2X,
    ABS_HAT2Y
};

static void help()
{
    fprintf(
        stderr,
        "Usage: %s [OPTION]...\n"
        "Create a virtual joystick controlled by a MQTT topic.\n"
        "\n"
        "  -o MQTT_SERVER_ADDRESS  MQTT server address. Default: %s\n"
        "  -p MQTT_SERVER_PORT     MQTT server port. Default: %hu\n"
        "  -t MQTT_TOPIC           MQTT topic. Default: %s\n"
        "  -d                      display the JSON object on the standard output\n"
        "  -v                      display version and exit\n"
        "  -h                      display this help and exit\n"
        "\n"
        "Copyright 2020 Cedric Priscal\n"
        "https://github.com/cepr/mqtt2js\n"
        "\n",
        prog_name,
        mqtt_server_address,
        mqtt_server_port,
        topic
        );
}

static void version()
{
    fprintf(
        stderr,
        "%s " VERSION "\n"
        "Copyright 2020 Cedric Priscal\n"
        "https://github.com/cepr/js2mqtt\n"
        "\n"
        "   Licensed under the Apache License, Version 2.0 (the \"License\");\n"
        "   you may not use this file except in compliance with the License.\n"
        "   You may obtain a copy of the License at\n"
        "\n"
        "       http://www.apache.org/licenses/LICENSE-2.0\n"
        "\n"
        "   Unless required by applicable law or agreed to in writing, software\n"
        "   distributed under the License is distributed on an \"AS IS\" BASIS,\n"
        "   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n"
        "   See the License for the specific language governing permissions and\n"
        "   limitations under the License.\n"
        "\n",
        prog_name
    );
}

static void mosquitto_assert(int connack_code, const char* msg)
{
    if (connack_code != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "%s: %s\n", msg, mosquitto_connack_string(connack_code));
        exit(EXIT_FAILURE);
    }
}

// MQTT callback
static void on_message(struct mosquitto *mosq, void *user_obj, const struct mosquitto_message *msg)
{
    // Unused parameters
    (void)mosq;
    (void)user_obj;

    // Decode the json object
    json_tokener_reset(tok);
    json_object *obj = json_tokener_parse_ex(tok, (const char*)msg->payload, msg->payloadlen);
    if (obj == NULL) {
        fprintf(stderr, "Invalid JSON object: %.*s\n", msg->payloadlen, (const char*)msg->payload);
        return;
    }

    // Extract fields
    struct json_object* field = NULL;
    if (!json_object_object_get_ex(obj, "value", &field)) {
        fprintf(stderr, "Missing key `value`\n");
        json_object_put(obj);
        return;
    }
    int value = json_object_get_int(field);
    
    if (!json_object_object_get_ex(obj, "type", &field)) {
        fprintf(stderr, "Missing key `type`\n");
        json_object_put(obj);
        return;
    }
    int type = json_object_get_int(field);

    if (!json_object_object_get_ex(obj, "number", &field)) {
        fprintf(stderr, "Missing key `number`\n");
        json_object_put(obj);
        return;
    }
    int number = json_object_get_int(field);

    // Release object
    json_object_put(obj);

    // Debug
    fprintf(stderr, "%d, %d, %d\n", value, type, number);

    if (type == JS_EVENT_BUTTON) {
        if (number < 0 || number >= BTN_COUNT) {
            fprintf(stderr, "Invalid button number: %d\n", number);
        } else {
            int err = libevdev_uinput_write_event(uidev, EV_KEY, BTN[number], value);
            if (err != 0) {
                fprintf(stderr, "libevdev_uinput_write_event() failed, aborting\n");
                exit(EXIT_FAILURE);
            }
            libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        }
    } else if (type == JS_EVENT_AXIS) {
        if (number < 0 || number >= AXIS_COUNT) {
            fprintf(stderr, "Invalid axis number: %d\n", number);
        } else {
            int err = libevdev_uinput_write_event(uidev, EV_ABS, AXIS[number], value);
            if (err != 0) {
                fprintf(stderr, "libevdev_uinput_write_event() failed, aborting\n");
                exit(EXIT_FAILURE);
            }
            libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
        }
    } else {
        fprintf(stderr, "Invalid event `type`: %d\n", type);
    }

}

int main(int argc, char *argv[])
{
    // Extract program name
    if (argc > 0) {
        prog_name = strdup(basename(argv[0]));
    }

    // Parse command line
    int opt;
    while((opt = getopt(argc, argv, "hvdo:p:t:")) != -1) {
        switch(opt) {
            case 'h':
            {
                help();
                exit(EXIT_SUCCESS);
                break;
            }

            case 'v':
            {
                version();
                exit(EXIT_SUCCESS);
                break;
            }

            case 'd':
            {
                debug = true;
                break;
            }

            case 'o':
            {
                mqtt_server_address = strdup(optarg);
                break;
            }

            case 'p':
            {
                char* endptr = NULL;
                mqtt_server_port = (unsigned short)strtol(optarg, &endptr, 10);
                if (endptr == NULL || *endptr != '\0') {
                    fprintf(stderr, "Invalid port specified: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            }

            case 't':
            {
                topic = strdup(optarg);
                break;
            }

            default:
            {
                // getopt already displays a warning
                exit(EXIT_FAILURE);
                break;
            }
        }
    }

    fprintf(
        stderr,
        "%s: listening for topic `%s` from %s:%hu...\n",
        prog_name, topic, mqtt_server_address, mqtt_server_port
    );

    // Initialize the JSON parser
    tok = json_tokener_new();

    // Connect to MQTT
    mosquitto_lib_init();
    struct mosquitto * mosq = mosquitto_new(NULL, true, NULL);
    if (mosq == NULL) {
        perror("mosquitto_new()");
        exit(EXIT_FAILURE);
    }
    mosquitto_assert(mosquitto_connect(mosq, mqtt_server_address, mqtt_server_port, MOSQUITTO_KEEP_ALIVE), "mosquitto_connect()");
    mosquitto_message_callback_set(mosq, &on_message);
    mosquitto_assert(mosquitto_subscribe(mosq, NULL, topic, 2), "mosquitto_subscribe()");

    // Create the virtual joystick
    // https://www.freedesktop.org/software/libevdev/doc/latest/group__uinput.html
    {
        int err;
        struct libevdev *dev;
        dev = libevdev_new();
        libevdev_set_name(dev, device_name);
        libevdev_enable_event_type(dev, EV_KEY);
        for (int i = 0; i < BTN_COUNT; i++) {
            libevdev_enable_event_code(dev, EV_KEY, BTN[i], NULL);
        }
        libevdev_enable_event_type(dev, EV_ABS);
        for (int i = 0; i < AXIS_COUNT; i++) {
            libevdev_enable_event_code(dev, EV_ABS, AXIS[i], NULL);
        }
        err = libevdev_uinput_create_from_device(dev,
                                                LIBEVDEV_UINPUT_OPEN_MANAGED,
                                                &uidev);
        if (err != 0) {
            fprintf(stderr, "libevdev_uinput_create_from_device() failed\n");
            exit(EXIT_FAILURE);
        }
    }

    // Read loop
    mosquitto_loop_forever(mosq, -1, 1);

    // Only way to stop this program is to kill it
    return EXIT_FAILURE;
}
