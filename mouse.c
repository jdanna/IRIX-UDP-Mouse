#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

#define PORT 5005
#define BUF_SIZE 4096

// Mouse button mappings
#define LEFT_BUTTON 1
#define MIDDLE_BUTTON 2
#define RIGHT_BUTTON 3

// Function to simulate mouse button press/release
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
    if (verbose) {
        printf("Button %s %s.\n", button, press ? "pressed" : "released");
    }

    XFlush(display);
}

// Function to move the mouse cursor
void move_mouse(Display *display, int x, int y, int verbose) {
    XTestFakeMotionEvent(display, 0, x, y, CurrentTime);
    XFlush(display);
    if (verbose) {
        printf("Mouse moved to position (%d, %d).\n", x, y);
    }
}

// Function to simulate scroll wheel using Page Up/Down
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

// Function to parse command-line arguments
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
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len;
    char buffer[BUF_SIZE];
    int verbose = 0;
    char *x_display;
    Display *display;

    parse_args(argc, argv, &verbose, &x_display);

    // Open X display
    display = XOpenDisplay(x_display);
    if (display == NULL) {
        fprintf(stderr, "Unable to open X display %s\n", x_display);
        exit(EXIT_FAILURE);
    }

    // Create UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set up server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Bind the socket
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    if (verbose) {
        printf("Listening on UDP port %d...\n", PORT);
    }

    addr_len = sizeof(client_addr);

    // Main loop
    while (1) {
        // Receive data
        len = recvfrom(sockfd, buffer, BUF_SIZE - 1, 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recvfrom failed");
            }
            continue;
        }

        buffer[len] = '\0';

        if (verbose) {
            printf("Received: %s\n", buffer);
        }

        // Handle scroll wheel
        if (strncmp(buffer, "WHEEL_", 6) == 0) {
            int scroll_value;
            if (sscanf(buffer + 6, "%d", &scroll_value) == 1) {
                simulate_scroll(display, scroll_value, verbose);
            } else if (verbose) {
                printf("Invalid scroll value: %s\n", buffer + 6);
            }
        }
        // Handle button presses/releases
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
        // Handle mouse movement
        else {
            if (sscanf(buffer, "%d_%d", &x, &y) == 2) {
                move_mouse(display, x, y, verbose);
            } else if (verbose) {
                printf("Invalid message format: %s\n", buffer);
            }
        }
    }

    // Cleanup
    close(sockfd);
    XCloseDisplay(display);

    return 0;
}
