/*
 * IRIX UDP mouse/keyboard receiver.
 *
 * Listens on UDP port 5005 for events sent by the patched PiKVM otg HID plugin
 * and injects them into the X server with the XTEST extension.
 *
 * Written in strict C89 so it builds with both MIPSpro and gcc:
 *   cc     -o mouse mouse.c -lXtst -lXext -lX11    (MIPSpro, n32)
 *   cc -64 -o mouse mouse.c -lXtst -lXext -lX11    (MIPSpro, 64-bit)
 *   gcc    -o mouse mouse.c -lXtst -lXext -lX11
 * Keep the library order: the 64-bit libXtst is a static archive and needs
 * libXext and libX11 listed after it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#define PORT 5005
#define BUF_SIZE 4096

/* Mouse button mappings */
#define LEFT_BUTTON 1
#define MIDDLE_BUTTON 2
#define RIGHT_BUTTON 3

extern char *optarg;

/*
 * Linux evdev key code (as sent by kvmd) to X keysym. The second keysym is a
 * fallback used when the first one isn't in the X server's keymap.
 * Shift/Ctrl/Alt arrive as separate key events, so only unshifted keysyms are listed.
 */
struct key_map {
    int code;
    KeySym keysym;
    KeySym fallback;
};

static const struct key_map KEY_MAP[] = {
    /* Letters */
    {30, XK_a, 0}, {48, XK_b, 0}, {46, XK_c, 0}, {32, XK_d, 0},
    {18, XK_e, 0}, {33, XK_f, 0}, {34, XK_g, 0}, {35, XK_h, 0},
    {23, XK_i, 0}, {36, XK_j, 0}, {37, XK_k, 0}, {38, XK_l, 0},
    {50, XK_m, 0}, {49, XK_n, 0}, {24, XK_o, 0}, {25, XK_p, 0},
    {16, XK_q, 0}, {19, XK_r, 0}, {31, XK_s, 0}, {20, XK_t, 0},
    {22, XK_u, 0}, {47, XK_v, 0}, {17, XK_w, 0}, {45, XK_x, 0},
    {21, XK_y, 0}, {44, XK_z, 0},

    /* Digits */
    {2, XK_1, 0}, {3, XK_2, 0}, {4, XK_3, 0}, {5, XK_4, 0}, {6, XK_5, 0},
    {7, XK_6, 0}, {8, XK_7, 0}, {9, XK_8, 0}, {10, XK_9, 0}, {11, XK_0, 0},

    /* Punctuation (US layout) */
    {12, XK_minus, 0}, {13, XK_equal, 0}, {26, XK_bracketleft, 0},
    {27, XK_bracketright, 0}, {43, XK_backslash, 0}, {39, XK_semicolon, 0},
    {40, XK_apostrophe, 0}, {41, XK_grave, 0}, {51, XK_comma, 0},
    {52, XK_period, 0}, {53, XK_slash, 0}, {86, XK_less, 0},

    /* Editing and whitespace */
    {28, XK_Return, 0}, {1, XK_Escape, 0}, {14, XK_BackSpace, 0},
    {15, XK_Tab, 0}, {57, XK_space, 0},

    /* Modifiers and locks */
    {42, XK_Shift_L, 0}, {54, XK_Shift_R, XK_Shift_L},
    {29, XK_Control_L, 0}, {97, XK_Control_R, XK_Control_L},
    {56, XK_Alt_L, 0}, {100, XK_Alt_R, XK_Alt_L},
    {125, XK_Meta_L, 0}, {126, XK_Meta_R, XK_Meta_L},
    {58, XK_Caps_Lock, 0}, {69, XK_Num_Lock, 0}, {70, XK_Scroll_Lock, 0},

    /* Function keys */
    {59, XK_F1, 0}, {60, XK_F2, 0}, {61, XK_F3, 0}, {62, XK_F4, 0},
    {63, XK_F5, 0}, {64, XK_F6, 0}, {65, XK_F7, 0}, {66, XK_F8, 0},
    {67, XK_F9, 0}, {68, XK_F10, 0}, {87, XK_F11, 0}, {88, XK_F12, 0},
    {183, XK_F13, 0}, {184, XK_F14, 0}, {185, XK_F15, 0}, {186, XK_F16, 0},
    {187, XK_F17, 0}, {188, XK_F18, 0}, {189, XK_F19, 0}, {190, XK_F20, 0},
    {191, XK_F21, 0}, {192, XK_F22, 0}, {193, XK_F23, 0}, {194, XK_F24, 0},

    /* Navigation */
    {110, XK_Insert, 0}, {111, XK_Delete, 0}, {102, XK_Home, 0},
    {107, XK_End, 0}, {104, XK_Prior, 0}, {109, XK_Next, 0},
    {103, XK_Up, 0}, {108, XK_Down, 0}, {105, XK_Left, 0}, {106, XK_Right, 0},
    {99, XK_Print, XK_Sys_Req}, {119, XK_Pause, XK_Break}, {438, XK_Menu, 0},

    /* Keypad */
    {98, XK_KP_Divide, 0}, {55, XK_KP_Multiply, 0}, {74, XK_KP_Subtract, 0},
    {78, XK_KP_Add, 0}, {96, XK_KP_Enter, XK_Return},
    {82, XK_KP_0, XK_KP_Insert}, {79, XK_KP_1, XK_KP_End},
    {80, XK_KP_2, XK_KP_Down}, {81, XK_KP_3, XK_KP_Next},
    {75, XK_KP_4, XK_KP_Left}, {76, XK_KP_5, XK_KP_Begin},
    {77, XK_KP_6, XK_KP_Right}, {71, XK_KP_7, XK_KP_Home},
    {72, XK_KP_8, XK_KP_Up}, {73, XK_KP_9, XK_KP_Prior},
    {83, XK_KP_Decimal, XK_KP_Delete},

    /* Japanese keys */
    {124, XK_yen, 0},
#ifdef XK_Henkan_Mode
    {92, XK_Henkan_Mode, 0},
#endif
#ifdef XK_Muhenkan
    {94, XK_Muhenkan, 0},
#endif
#ifdef XK_Hiragana_Katakana
    {90, XK_Hiragana_Katakana, 0},
#endif

    /*
     * Media and power keys (mute, volume, power) have no standard X11R6
     * keysyms on IRIX and are intentionally not mapped.
     */
    {0, 0, 0}
};

/* X keycodes and mouse buttons pressed by this daemon, so they can be released on reset */
static char keys_down[256];
static char buttons_down[4];

/* Function to simulate mouse button press/release */
void simulate_button(Display *display, const char *button, int press, int verbose) {
    int button_code = 0;

    if (strcmp(button, "left") == 0) {
        button_code = LEFT_BUTTON;
    } else if (strcmp(button, "middle") == 0) {
        button_code = MIDDLE_BUTTON;
    } else if (strcmp(button, "right") == 0) {
        button_code = RIGHT_BUTTON;
    }

    if (button_code == 0) {
        if (verbose) {
            printf("Unknown button: %s\n", button);
        }
        return;
    }

    XTestFakeButtonEvent(display, button_code, press, CurrentTime);
    buttons_down[button_code] = (char)press;
    if (verbose) {
        printf("Button %s %s.\n", button, press ? "pressed" : "released");
    }

    XFlush(display);
}

/* Function to move the mouse cursor */
void move_mouse(Display *display, int x, int y, int verbose) {
    XTestFakeMotionEvent(display, 0, x, y, CurrentTime);
    XFlush(display);
    if (verbose) {
        printf("Mouse moved to position (%d, %d).\n", x, y);
    }
}

/* Function to simulate scroll wheel using Page Up/Down */
void simulate_scroll(Display *display, int scroll_amount, int verbose) {
    KeySym key;
    int num_presses, i;

    key = (scroll_amount > 0) ? XK_Page_Up : XK_Page_Down;
    num_presses = abs(scroll_amount) / 5;

    for (i = 0; i < num_presses; i++) {
        XTestFakeKeyEvent(display, XKeysymToKeycode(display, key), True, CurrentTime);
        XTestFakeKeyEvent(display, XKeysymToKeycode(display, key), False, CurrentTime);
        if (verbose) {
            printf("Simulated %s key press.\n", (scroll_amount > 0) ? "Page Up" : "Page Down");
        }
    }

    XFlush(display);
}

/* Function to look up the X keycode for an evdev key code (0 if not mappable) */
KeyCode lookup_keycode(Display *display, int code, int verbose) {
    const struct key_map *entry;
    KeyCode keycode;
    char *name;

    for (entry = KEY_MAP; entry->code != 0; entry++) {
        if (entry->code == code) {
            keycode = XKeysymToKeycode(display, entry->keysym);
            if (keycode == 0 && entry->fallback != 0) {
                keycode = XKeysymToKeycode(display, entry->fallback);
            }
            if (keycode == 0 && verbose) {
                name = XKeysymToString(entry->keysym);
                printf("Key code %d (%s) is not in the X keymap\n", code, name ? name : "?");
            }
            return keycode;
        }
    }

    if (verbose) {
        printf("Unmapped key code: %d\n", code);
    }
    return 0;
}

/* Function to simulate keyboard key press/release */
void simulate_key(Display *display, int code, int press, int verbose) {
    KeyCode keycode;

    keycode = lookup_keycode(display, code, verbose);
    if (keycode == 0) {
        return;
    }

    XTestFakeKeyEvent(display, keycode, press, CurrentTime);
    keys_down[keycode] = (char)press;
    XFlush(display);
    if (verbose) {
        printf("Key %d (X keycode %d) %s.\n", code, (int)keycode, press ? "pressed" : "released");
    }
}

/* Function to release every key and button this daemon is holding down */
void release_all(Display *display, int verbose) {
    int i;

    for (i = 0; i < 256; i++) {
        if (keys_down[i]) {
            XTestFakeKeyEvent(display, (unsigned int)i, False, CurrentTime);
            keys_down[i] = 0;
        }
    }
    for (i = 1; i < 4; i++) {
        if (buttons_down[i]) {
            XTestFakeButtonEvent(display, (unsigned int)i, False, CurrentTime);
            buttons_down[i] = 0;
        }
    }
    XFlush(display);
    if (verbose) {
        printf("Released all keys and buttons.\n");
    }
}

/* Function to parse command-line arguments */
void parse_args(int argc, char *argv[], int *verbose, char **x_display) {
    int opt;
    *x_display = ":0";

    while ((opt = getopt(argc, argv, "vd:")) != -1) {
        switch (opt) {
            case 'v':
                *verbose = 1;
                break;
            case 'd':
                *x_display = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s [-v] [-d display]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
}

int main(int argc, char *argv[]) {
    int sockfd, len, x, y;
    int event_base, error_base, major, minor;
    int running = 1;
    struct sockaddr_in server_addr;
    char buffer[BUF_SIZE];
    int verbose = 0;
    char *x_display;
    Display *display;

    parse_args(argc, argv, &verbose, &x_display);

    /* Line-buffer verbose output so it shows up promptly when redirected to a file */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

    /* Open X display */
    display = XOpenDisplay(x_display);
    if (display == NULL) {
        fprintf(stderr, "Unable to open X display %s\n", x_display);
        exit(EXIT_FAILURE);
    }

    if (!XTestQueryExtension(display, &event_base, &error_base, &major, &minor)) {
        fprintf(stderr, "X server on %s does not support the XTEST extension\n", x_display);
        exit(EXIT_FAILURE);
    }

    /* Create UDP socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    /* Set up server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    /* Bind the socket */
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    if (verbose) {
        printf("Listening on UDP port %d...\n", PORT);
    }

    /* Main loop */
    while (running) {
        /* Receive data (the sender's address isn't needed) */
        len = (int)recv(sockfd, buffer, BUF_SIZE - 1, 0);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                perror("recv failed");
            }
            continue;
        }

        buffer[len] = '\0';

        if (verbose) {
            printf("Received: %s\n", buffer);
        }

        /* Handle keyboard keys (must be checked before buttons, both contain a comma) */
        if (strncmp(buffer, "KEY_", 4) == 0) {
            int code;
            char state[16];

            if (sscanf(buffer + 4, "%d,%15s", &code, state) == 2
                && (strcmp(state, "True") == 0 || strcmp(state, "False") == 0)) {
                simulate_key(display, code, strcmp(state, "True") == 0, verbose);
            } else if (verbose) {
                printf("Invalid key message: %s\n", buffer);
            }
        }
        /* Release everything held down (sent when the PiKVM web session changes) */
        else if (strcmp(buffer, "RESET") == 0) {
            release_all(display, verbose);
        }
        /* Handle scroll wheel */
        else if (strncmp(buffer, "WHEEL_", 6) == 0) {
            int scroll_value;
            if (sscanf(buffer + 6, "%d", &scroll_value) == 1) {
                simulate_scroll(display, scroll_value, verbose);
            } else if (verbose) {
                printf("Invalid scroll value: %s\n", buffer + 6);
            }
        }
        /* Handle button presses/releases */
        else if (strchr(buffer, ',') != NULL) {
            char *comma_pos;
            char *button;
            char *state;

            comma_pos = strchr(buffer, ',');
            *comma_pos = '\0';
            button = buffer;
            state = comma_pos + 1;

            if (strcmp(state, "True") == 0) {
                simulate_button(display, button, 1, verbose);
            } else if (strcmp(state, "False") == 0) {
                simulate_button(display, button, 0, verbose);
            } else if (verbose) {
                printf("Invalid button state: %s\n", state);
            }
        }
        /* Handle mouse movement */
        else {
            if (sscanf(buffer, "%d_%d", &x, &y) == 2) {
                move_mouse(display, x, y, verbose);
            } else if (verbose) {
                printf("Invalid message format: %s\n", buffer);
            }
        }
    }

    /* Cleanup */
    close(sockfd);
    XCloseDisplay(display);

    return 0;
}
