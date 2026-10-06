#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFUserNotification.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    KONE_VENDOR_ID = 0x1e7d,
    KONE_PRODUCT_ID = 0x2c8b,
    KONE_EVENT_USAGE_PAGE = 0xff00,
    KONE_CONFIG_USAGE_PAGE = 0xff01,
};

struct feature_report {
    CFIndex id;
    CFIndex payload_length;
};

/* Payload sizes decoded from the device's HID report descriptor. IOHID adds
 * the report-ID byte, so read buffers must be one byte larger. */
static const struct feature_report feature_reports[] = {
    {0x04, 3},   {0x05, 3},    {0x06, 173}, {0x07, 124},
    {0x08, 1042},{0x09, 8},    {0x0d, 121}, {0x0e, 5},
    {0x0f, 5},   {0x10, 15},   {0x11, 12},  {0x12, 63},
    /* This report stalls on reads; keep it last so it cannot affect others. */
    {0x0a, 3},
};

enum {
    BUTTON_REPORT_ID = 0x07,
    BUTTON_REPORT_LENGTH = 125,
    BUTTON_ENTRY_OFFSET = 3,
    BUTTON_ENTRY_LENGTH = 4,
    BUTTON_ENTRY_COUNT = 30,
};

/* The normal layer is stored in firmware order; the Easy-Shift layer is in
 * physical button order. Values are zero-based entry indexes. */
static const int normal_entry_for_button[15] = {
    0, 1, 2, 3, 4, 5, 6, 11, 12, 9, 10, 7, 8, 13, 14,
};

static void add_number(CFMutableDictionaryRef dictionary, CFStringRef key, int value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(dictionary, key, number);
    CFRelease(number);
}

static int property_int(IOHIDDeviceRef device, CFStringRef key, int fallback) {
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    int result = fallback;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &result);
    }
    return result;
}

static void property_string(IOHIDDeviceRef device, CFStringRef key,
                            char *output, size_t output_size, const char *fallback) {
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    if (value && CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString((CFStringRef)value, output, (CFIndex)output_size,
                           kCFStringEncodingUTF8)) {
        return;
    }
    snprintf(output, output_size, "%s", fallback);
}

static IOHIDDeviceRef find_device(IOHIDManagerRef *manager_out, int usage_page,
                                  const char *description) {
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault,
                                                  kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef matching = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    add_number(matching, CFSTR(kIOHIDVendorIDKey), KONE_VENDOR_ID);
    add_number(matching, CFSTR(kIOHIDProductIDKey), KONE_PRODUCT_ID);
    add_number(matching, CFSTR(kIOHIDPrimaryUsagePageKey), usage_page);
    IOHIDManagerSetDeviceMatching(manager, matching);
    CFRelease(matching);

    IOReturn result = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "Could not open HID manager (0x%08x).\n", result);
        CFRelease(manager);
        return NULL;
    }

    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (!devices || CFSetGetCount(devices) == 0) {
        fprintf(stderr, "No wired ROCCAT Kone XP %s interface found.\n", description);
        if (devices) CFRelease(devices);
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        CFRelease(manager);
        return NULL;
    }

    const void **values = calloc((size_t)CFSetGetCount(devices), sizeof(*values));
    CFSetGetValues(devices, values);
    IOHIDDeviceRef device = (IOHIDDeviceRef)values[0];
    free(values);
    if (device) CFRetain(device);
    CFRelease(devices);
    *manager_out = manager;
    return device;
}

static IOHIDDeviceRef find_configuration_device(IOHIDManagerRef *manager_out) {
    return find_device(manager_out, KONE_CONFIG_USAGE_PAGE, "configuration");
}

static IOHIDDeviceRef open_configuration_device(IOHIDManagerRef *manager_out) {
    IOHIDDeviceRef device = find_configuration_device(manager_out);
    if (!device) return NULL;
    IOReturn result = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (result == kIOReturnSuccess) return device;
    fprintf(stderr, "Could not open Kone XP configuration interface (0x%08x).\n",
            result);
    CFRelease(device);
    IOHIDManagerClose(*manager_out, kIOHIDOptionsTypeNone);
    CFRelease(*manager_out);
    *manager_out = NULL;
    return NULL;
}

static IOHIDDeviceRef open_event_device(IOHIDManagerRef *manager_out) {
    IOHIDDeviceRef device = find_device(manager_out, KONE_EVENT_USAGE_PAGE,
                                        "button-event");
    if (!device) return NULL;
    IOReturn result = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (result == kIOReturnSuccess) return device;
    fprintf(stderr, "Could not open Kone XP button-event interface (0x%08x).\n",
            result);
    CFRelease(device);
    IOHIDManagerClose(*manager_out, kIOHIDOptionsTypeNone);
    CFRelease(*manager_out);
    *manager_out = NULL;
    return NULL;
}

static void close_configuration_device(IOHIDDeviceRef device,
                                       IOHIDManagerRef manager) {
    IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
    CFRelease(device);
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
}

static unsigned short checksum16(const unsigned char *bytes, size_t length) {
    unsigned int sum = 0;
    for (size_t i = 0; i < length; ++i) sum += bytes[i];
    return (unsigned short)(sum & 0xffff);
}

static int valid_button_report(const unsigned char *report, CFIndex length,
                               int expected_profile) {
    if (length != BUTTON_REPORT_LENGTH || report[0] != BUTTON_REPORT_ID ||
        report[1] != BUTTON_REPORT_LENGTH || report[2] > 4 ||
        (expected_profile >= 0 && report[2] != expected_profile)) return 0;
    unsigned short expected = checksum16(report, BUTTON_REPORT_LENGTH - 2);
    unsigned short actual = (unsigned short)(report[BUTTON_REPORT_LENGTH - 2] |
        (report[BUTTON_REPORT_LENGTH - 1] << 8));
    return expected == actual;
}

static int read_status(IOHIDDeviceRef device, const char *operation) {
    unsigned char report[4] = {0x04, 0, 0, 0};
    CFIndex length = sizeof(report);
    /* Captured ROCCAT writes wait roughly 150 ms before reading status. */
    usleep(160000);
    IOReturn result = IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature,
                                            0x04, report, &length);
    /* Byte 1 is the returned payload length, not the result code.  Simple
     * commands return 04 01 00 00 while Kone XP's 125-byte button-table
     * write returns 04 03 00 00.  Both are successful: the payload bytes
     * themselves are zero. */
    if (result != kIOReturnSuccess || length != 4 || report[0] != 0x04 ||
        report[1] == 0x00 || report[1] > 0x03 ||
        report[2] != 0x00 || report[3] != 0x00) {
        fprintf(stderr, "%s was not accepted by the mouse", operation);
        if (result != kIOReturnSuccess) fprintf(stderr, " (0x%08x)", result);
        else fprintf(stderr, " (status %02x %02x %02x %02x)",
                     report[0], report[1], report[2], report[3]);
        fputs(".\n", stderr);
        return 1;
    }
    return 0;
}

static int print_raw_status(void) {
    IOHIDManagerRef manager = NULL;
    IOHIDDeviceRef device = open_configuration_device(&manager);
    if (!device) return 1;
    unsigned char report[4] = {0x04, 0, 0, 0};
    CFIndex length = sizeof(report);
    IOReturn result = IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature,
                                            0x04, report, &length);
    if (result == kIOReturnSuccess && length == 4)
        printf("%02x %02x %02x %02x\n", report[0], report[1], report[2], report[3]);
    else
        fprintf(stderr, "Could not read status (0x%08x, length %ld).\n",
                result, (long)length);
    close_configuration_device(device, manager);
    return result == kIOReturnSuccess && length == 4 ? 0 : 1;
}

static int select_profile_block(IOHIDDeviceRef device, int profile,
                                unsigned char selector) {
    unsigned char request[4] = {0x04, (unsigned char)profile, selector, 0x00};
    IOReturn result = IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature,
                                            0x04, request, sizeof(request));
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "Could not select profile %d (0x%08x).\n", profile + 1,
                result);
        return 1;
    }
    return read_status(device, "Selecting the onboard profile data");
}

static int read_button_report(IOHIDDeviceRef device, int profile,
                              unsigned char report[BUTTON_REPORT_LENGTH]) {
    if (profile >= 0 && select_profile_block(device, profile, 0x90) != 0) return 1;
    memset(report, 0, BUTTON_REPORT_LENGTH);
    CFIndex length = BUTTON_REPORT_LENGTH;
    IOReturn result = IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature,
                                            BUTTON_REPORT_ID, report, &length);
    if (result != kIOReturnSuccess) {
        if (profile >= 0) fprintf(stderr, "Could not read Profile %d buttons (0x%08x).\n", profile + 1, result);
        else fprintf(stderr, "Could not read the active profile buttons (0x%08x).\n", result);
        return 1;
    }
    if (!valid_button_report(report, length, profile)) {
        if (profile >= 0) fprintf(stderr, "Profile %d returned an invalid button table.\n", profile + 1);
        else fputs("The active profile returned an invalid button table.\n", stderr);
        return 1;
    }
    return 0;
}

static int select_active_profile(IOHIDDeviceRef device, int profile) {
    unsigned char request[4] = {0x05, 0x04, (unsigned char)profile, 0x05};
    IOReturn result = IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature,
                                           0x05, request, sizeof(request));
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "Could not activate Profile %d (0x%08x).\n", profile + 1,
                result);
        return 1;
    }
    return read_status(device, "Activating the edited profile");
}

static int commit_changes(IOHIDDeviceRef device) {
    unsigned char request[6] = {0x0e, 0x06, 0x01, 0x00, 0x00, 0xff};
    IOReturn result = IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature,
                                           0x0e, request, sizeof(request));
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "Could not commit the onboard change (0x%08x).\n", result);
        return 1;
    }
    return read_status(device, "Committing the onboard change");
}

static int button_entry_index(const char *layer, int button) {
    if (button < 1 || button > 15) return -1;
    if (strcmp(layer, "standard") == 0) return normal_entry_for_button[button - 1];
    if (strcmp(layer, "shift") == 0) return 15 + normal_entry_for_button[button - 1];
    return -1;
}

static int encode_action(unsigned char entry[4], const char *action,
                         const char *value) {
    memset(entry, 0, BUTTON_ENTRY_LENGTH);
    if (strcmp(action, "disabled") == 0) return 0;
    if (strcmp(action, "left") == 0) { entry[2] = 1; entry[3] = 1; return 0; }
    if (strcmp(action, "right") == 0) { entry[2] = 2; entry[3] = 1; return 0; }
    if (strcmp(action, "middle") == 0) { entry[2] = 3; entry[3] = 1; return 0; }
    if (strcmp(action, "forward") == 0) { entry[2] = 5; entry[3] = 1; return 0; }
    if (strcmp(action, "back") == 0) { entry[2] = 6; entry[3] = 1; return 0; }
    if (strcmp(action, "tilt-left") == 0) { entry[2] = 7; entry[3] = 1; return 0; }
    if (strcmp(action, "tilt-right") == 0) { entry[2] = 8; entry[3] = 1; return 0; }
    if (strcmp(action, "scroll-up") == 0) { entry[2] = 9; entry[3] = 1; return 0; }
    if (strcmp(action, "scroll-down") == 0) { entry[2] = 10; entry[3] = 1; return 0; }
    if (strcmp(action, "dpi-up") == 0) { entry[2] = 2; entry[3] = 2; return 0; }
    if (strcmp(action, "dpi-down") == 0) { entry[2] = 3; entry[3] = 2; return 0; }
    if (strcmp(action, "play-pause") == 0) { entry[2] = 4; entry[3] = 3; return 0; }
    if (strcmp(action, "previous-track") == 0) { entry[2] = 2; entry[3] = 3; return 0; }
    if (strcmp(action, "next-track") == 0) { entry[2] = 3; entry[3] = 3; return 0; }
    if (strcmp(action, "volume-up") == 0) { entry[2] = 7; entry[3] = 3; return 0; }
    if (strcmp(action, "volume-down") == 0) { entry[2] = 8; entry[3] = 3; return 0; }
    if (strcmp(action, "profile-cycle") == 0) { entry[2] = 1; entry[3] = 8; return 0; }
    if (strcmp(action, "easy-shift") == 0) { entry[2] = 1; entry[3] = 10; return 0; }
    if (strcmp(action, "key") == 0 && value) {
        char *end = NULL;
        long usage = strtol(value, &end, 0);
        if (*value && end && *end == '\0' && usage >= 4 && usage <= 231) {
            entry[1] = (unsigned char)usage;
            entry[3] = 6;
            return 0;
        }
    }
    return 1;
}

static int list_buttons(int profile) {
    IOHIDManagerRef manager = NULL;
    IOHIDDeviceRef device = open_configuration_device(&manager);
    if (!device) return 1;
    unsigned char report[BUTTON_REPORT_LENGTH];
    int status = read_button_report(device, profile, report);
    if (status == 0) {
        for (int button = 1; button <= 15; ++button) {
            int indexes[2] = {normal_entry_for_button[button - 1],
                              15 + normal_entry_for_button[button - 1]};
            for (int layer = 0; layer < 2; ++layer) {
                unsigned char *entry = report + BUTTON_ENTRY_OFFSET +
                    indexes[layer] * BUTTON_ENTRY_LENGTH;
                printf("%d\t%s\t%02x%02x%02x%02x\n", button,
                       layer == 0 ? "standard" : "shift",
                       entry[0], entry[1], entry[2], entry[3]);
            }
        }
    }
    close_configuration_device(device, manager);
    return status;
}

static int active_profile(void) {
    IOHIDManagerRef manager = NULL;
    IOHIDDeviceRef device = open_configuration_device(&manager);
    if (!device) return 1;
    unsigned char report[4] = {0x05, 0, 0, 0};
    CFIndex length = sizeof(report);
    IOReturn result = IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature,
                                            0x05, report, &length);
    int status = 0;
    if (result != kIOReturnSuccess || length != 4 || report[0] != 0x05 ||
        report[1] != 0x04 || report[2] > 4 || report[3] != 0x05) {
        fprintf(stderr, "Could not determine the active onboard profile.\n");
        status = 1;
    } else {
        printf("%u\n", report[2] + 1);
    }
    close_configuration_device(device, manager);
    return status;
}

struct identify_context {
    int index;
};

static void input_report_callback(void *context, IOReturn result, void *sender,
                                  IOHIDReportType type, uint32_t report_id,
                                  uint8_t *report, CFIndex length) {
    (void)sender; (void)report_id;
    struct identify_context *capture = context;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput ||
        length < 4 || capture->index >= 0) return;
    /* Interface 2 reports two little-endian button masks in bytes 0 and 2. */
    unsigned int mask = report[0] | ((unsigned int)report[2] << 8);
    if (report[0] == 0x03 && report[2] == 0xb0) return; /* DPI status packet */
    if (mask == 0) return;
    for (int bit = 0; bit < 15; ++bit) {
        if (mask & (1u << bit)) {
            capture->index = bit;
            CFRunLoopStop(CFRunLoopGetCurrent());
            return;
        }
    }
}

static int identify_button(int timeout_seconds) {
    IOHIDManagerRef manager = NULL;
    /* Button-state interrupts are sent on the vendor page 0xff00 interface.
     * The 0xff01 interface used for onboard configuration exposes only
     * one-byte feature-report notifications and never carries button masks. */
    IOHIDDeviceRef device = open_event_device(&manager);
    if (!device) return 1;
    unsigned char buffer[64] = {0};
    struct identify_context capture = {.index = -1};
    IOHIDDeviceRegisterInputReportCallback(device, buffer, sizeof(buffer),
                                           input_report_callback, &capture);
    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetCurrent(),
                                   kCFRunLoopDefaultMode);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, timeout_seconds, false);
    IOHIDDeviceUnscheduleFromRunLoop(device, CFRunLoopGetCurrent(),
                                     kCFRunLoopDefaultMode);
    int status = 0;
    if (capture.index < 0) {
        fputs("No Kone XP button press was detected.\n", stderr);
        status = 1;
    } else {
        int physical_button = -1;
        for (int button = 0; button < 15; ++button)
            if (normal_entry_for_button[button] == capture.index) physical_button = button + 1;
        if (physical_button < 0) {
            fprintf(stderr, "Detected unsupported hardware button index %d.\n", capture.index);
            status = 1;
        } else {
            printf("%d\t%d\n", capture.index, physical_button);
        }
    }
    close_configuration_device(device, manager);
    return status;
}

static int set_action(int profile, const char *layer, int button,
                      const char *action, const char *value) {
    int index = button_entry_index(layer, button);
    if (index < 0) {
        fputs("Invalid layer or button number.\n", stderr);
        return 2;
    }
    unsigned char encoded[BUTTON_ENTRY_LENGTH];
    if (encode_action(encoded, action, value) != 0) {
        fputs("Invalid or unsupported action.\n", stderr);
        return 2;
    }

    IOHIDManagerRef manager = NULL;
    IOHIDDeviceRef device = open_configuration_device(&manager);
    if (!device) return 1;
    unsigned char before[BUTTON_REPORT_LENGTH];
    unsigned char verify[BUTTON_REPORT_LENGTH];
    int status = read_button_report(device, profile, before);
    if (status == 0) {
        memcpy(before + BUTTON_ENTRY_OFFSET + index * BUTTON_ENTRY_LENGTH,
               encoded, BUTTON_ENTRY_LENGTH);
        unsigned short sum = checksum16(before, BUTTON_REPORT_LENGTH - 2);
        before[BUTTON_REPORT_LENGTH - 2] = (unsigned char)(sum & 0xff);
        before[BUTTON_REPORT_LENGTH - 1] = (unsigned char)(sum >> 8);
    }
    if (status == 0) {
        IOReturn result = IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature,
                                               BUTTON_REPORT_ID, before,
                                               BUTTON_REPORT_LENGTH);
        if (result != kIOReturnSuccess) {
            fprintf(stderr, "Writing Profile %d failed (0x%08x).\n", profile + 1, result);
            status = 1;
        }
    }
    if (status == 0) status = read_status(device, "Writing the button assignment");
    if (status == 0) status = select_active_profile(device, profile);
    if (status == 0) status = commit_changes(device);
    if (status == 0) status = read_button_report(device, profile, verify);
    if (status == 0 && memcmp(before, verify, BUTTON_REPORT_LENGTH) != 0) {
        fputs("The mouse did not verify the saved button table.\n", stderr);
        status = 1;
    }
    if (status == 0) printf("Button assignment saved, Profile %d activated, and onboard memory committed.\n", profile + 1);
    close_configuration_device(device, manager);
    return status;
}

static void print_json_string(FILE *file, const char *text) {
    fputc('"', file);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        switch (*p) {
            case '"': fputs("\\\"", file); break;
            case '\\': fputs("\\\\", file); break;
            case '\n': fputs("\\n", file); break;
            case '\r': fputs("\\r", file); break;
            case '\t': fputs("\\t", file); break;
            default:
                if (*p < 0x20) fprintf(file, "\\u%04x", *p);
                else fputc(*p, file);
        }
    }
    fputc('"', file);
}

static int backup(const char *output_path) {
    IOHIDManagerRef manager = NULL;
    IOHIDDeviceRef device = find_configuration_device(&manager);
    if (!device) return 1;

    IOReturn open_result = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (open_result != kIOReturnSuccess) {
        fprintf(stderr, "Could not open Kone XP configuration interface (0x%08x).\n",
                open_result);
        CFRelease(device);
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        CFRelease(manager);
        return 1;
    }

    char product[256];
    property_string(device, CFSTR(kIOHIDProductKey), product, sizeof(product),
                    "ROCCAT Kone XP");
    int version = property_int(device, CFSTR(kIOHIDVersionNumberKey), 0);
    printf("Connected: %s, device revision %d.%02d\n", product,
           (version >> 8) & 0xff, version & 0xff);

    size_t report_count = sizeof(feature_reports) / sizeof(feature_reports[0]);
    unsigned char **payloads = calloc(report_count, sizeof(*payloads));
    CFIndex *lengths = calloc(report_count, sizeof(*lengths));
    IOReturn *results = calloc(report_count, sizeof(*results));
    int status = 0;
    size_t successful_reports = 0;

    for (size_t index = 0; index < report_count; ++index) {
        const struct feature_report *report = &feature_reports[index];
        payloads[index] = calloc((size_t)report->payload_length + 1, 1);
        lengths[index] = report->payload_length + 1;
        results[index] = IOHIDDeviceGetReport(
            device, kIOHIDReportTypeFeature, report->id, payloads[index], &lengths[index]);
        if (results[index] != kIOReturnSuccess) {
            fprintf(stderr, "Reading feature report 0x%02lx failed (0x%08x).\n",
                    report->id, results[index]);
            lengths[index] = 0;
            continue;
        }
        successful_reports++;
        printf("Read report 0x%02lx: %ld bytes\n", report->id, lengths[index]);
    }

    if (successful_reports == 0) status = 1;

    if (status == 0) {
        FILE *file = fopen(output_path, "wx");
        if (!file) {
            fprintf(stderr, "Could not create backup at %s (it may already exist).\n",
                    output_path);
            status = 1;
        } else {
            time_t now = time(NULL);
            struct tm utc;
            gmtime_r(&now, &utc);
            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);

            fputs("{\n  \"formatVersion\": 1,\n  \"capturedAt\": ", file);
            print_json_string(file, timestamp);
            fprintf(file, ",\n  \"vendorID\": %d,\n  \"productID\": %d,\n  \"product\": ",
                    KONE_VENDOR_ID, KONE_PRODUCT_ID);
            print_json_string(file, product);
            fprintf(file, ",\n  \"versionNumber\": %d,\n  \"reportBytesIncludeID\": true,\n  \"reports\": [\n", version);
            for (size_t index = 0; index < report_count; ++index) {
                fprintf(file, "    {\"reportID\": %ld, \"payloadLength\": %ld, \"ioReturn\": \"0x%08x\", \"hex\": \"",
                        feature_reports[index].id, lengths[index], results[index]);
                for (CFIndex byte = 0; byte < lengths[index]; ++byte) {
                    fprintf(file, "%02x", payloads[index][byte]);
                }
                fprintf(file, "\"}%s\n", index + 1 == report_count ? "" : ",");
            }
            fputs("  ]\n}\n", file);
            if (fclose(file) != 0) status = 1;
            if (status == 0) printf("Backup saved to %s\n", output_path);
        }
    }

    for (size_t index = 0; index < report_count; ++index) free(payloads[index]);
    free(payloads);
    free(lengths);
    free(results);
    IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
    CFRelease(device);
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
    return status;
}

static const char *access_name(IOHIDAccessType access) {
    switch (access) {
        case kIOHIDAccessTypeGranted: return "granted";
        case kIOHIDAccessTypeDenied: return "denied";
        case kIOHIDAccessTypeUnknown: return "not requested";
    }
    return "unknown";
}

static int authorize(void) {
    IOHIDAccessType before = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    printf("Input Monitoring access: %s\n", access_name(before));
    if (before == kIOHIDAccessTypeGranted) return 0;
    bool granted = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
    IOHIDAccessType after = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    printf("Permission request result: %s\n", granted ? "granted" : access_name(after));
    if (!granted) {
        puts("Enable this program under System Settings > Privacy & Security > Input Monitoring,");
        puts("then quit and reopen the terminal/app that launched it.");
    }
    return granted ? 0 : 1;
}

static void show_notice(const char *title, const char *message) {
    CFStringRef title_string = CFStringCreateWithCString(
        kCFAllocatorDefault, title, kCFStringEncodingUTF8);
    CFStringRef message_string = CFStringCreateWithCString(
        kCFAllocatorDefault, message, kCFStringEncodingUTF8);
    CFUserNotificationDisplayNotice(0, kCFUserNotificationNoteAlertLevel,
                                    NULL, NULL, NULL, title_string,
                                    message_string, CFSTR("OK"));
    CFRelease(title_string);
    CFRelease(message_string);
}

static int app_backup(const char *executable_path) {
    IOHIDAccessType access = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    if (access != kIOHIDAccessTypeGranted) {
        IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
        show_notice("Kone XP Backup",
                    "macOS has not yet granted Input Monitoring access. Add and enable “Kone XP Backup” in System Settings > Privacy & Security > Input Monitoring, then reopen this app.");
        return 1;
    }

    char resolved[PATH_MAX];
    if (!realpath(executable_path, resolved)) {
        show_notice("Kone XP Backup", "Could not resolve the application path.");
        return 1;
    }
    char *outputs_marker = strstr(resolved, "/outputs/");
    if (!outputs_marker) {
        show_notice("Kone XP Backup", "The app must remain inside the project’s outputs folder.");
        return 1;
    }
    *outputs_marker = '\0';
    char backup_path[PATH_MAX];
    int length = snprintf(backup_path, sizeof(backup_path),
                          "%s/work/kone-xp-backup-initial.json", resolved);
    if (length < 0 || (size_t)length >= sizeof(backup_path)) {
        show_notice("Kone XP Backup", "The backup path is too long.");
        return 1;
    }

    int result = backup(backup_path);
    if (result == 0) {
        show_notice("Kone XP Backup", "The read-only onboard-memory backup was captured successfully.");
    } else {
        show_notice("Kone XP Backup", "The mouse could not be read. Ensure the wired Kone XP is connected, then reopen the app.");
    }
    return result;
}

static void usage(const char *program) {
    printf("Usage: %s backup <output.json>\n", program);
    printf("       %s buttons <profile 1-5>\n", program);
    printf("       %s active-profile\n", program);
    printf("       %s identify [timeout-seconds]\n", program);
    printf("       %s set-action <profile 1-5> <standard|shift> <button 1-15> <action> [key-usage]\n", program);
    printf("       %s authorize\n", program);
    puts("Actions: disabled, left, right, middle, forward, back, tilt-left,");
    puts("tilt-right, scroll-up, scroll-down, dpi-up, dpi-down, play-pause,");
    puts("previous-track, next-track, volume-up, volume-down, profile-cycle,");
    puts("easy-shift, or key followed by a USB HID usage number.");
}

int main(int argc, char **argv) {
    if (argc == 1 && strstr(argv[0], ".app/Contents/MacOS/")) return app_backup(argv[0]);
    if (argc == 3 && strcmp(argv[1], "backup") == 0) return backup(argv[2]);
    if (argc == 3 && strcmp(argv[1], "buttons") == 0) {
        int profile = atoi(argv[2]);
        if (profile < 1 || profile > 5) { usage(argv[0]); return 2; }
        return list_buttons(profile - 1);
    }
    if (argc == 2 && strcmp(argv[1], "active-profile") == 0)
        return active_profile();
    if (argc == 2 && strcmp(argv[1], "raw-status") == 0)
        return print_raw_status();
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "identify") == 0) {
        int timeout = argc == 3 ? atoi(argv[2]) : 10;
        if (timeout < 1 || timeout > 60) { usage(argv[0]); return 2; }
        return identify_button(timeout);
    }
    if ((argc == 6 || argc == 7) && strcmp(argv[1], "set-action") == 0) {
        int profile = atoi(argv[2]);
        int button = atoi(argv[4]);
        if (profile < 1 || profile > 5) { usage(argv[0]); return 2; }
        return set_action(profile - 1, argv[3], button, argv[5],
                          argc == 7 ? argv[6] : NULL);
    }
    if (argc == 2 && strcmp(argv[1], "authorize") == 0) return authorize();
    usage(argv[0]);
    return argc == 1 ? 0 : 2;
}
