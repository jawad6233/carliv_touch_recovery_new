/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 * Copyright (C) 2014 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "bootloader.h"
#include "common.h"
#include "edifyscripting.h"
#include "cutils/android_reboot.h"
#include "cutils/properties.h"
#include "install.h"
#include "minuictr/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "adb_install.h"
#include "minadbd/adb.h"
#include "fuse_sideload.h"
#include "fuse_sdcard_provider.h"

#include "extendedcommands.h"
#include "flashutils/flashutils.h"
#include "recovery_cmds.h"

struct selabel_handle *sehandle = NULL;

static const struct option OPTIONS[] = {
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "headless", no_argument, NULL, 'h' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
  { "show_text", no_argument, NULL, 't' },
  { "sideload", no_argument, NULL, 'l' },
  { "shutdown_after", no_argument, NULL, 'p' },
  { NULL, 0, NULL, 0 },
};

#define LAST_LOG_FILE "/cache/recovery/last_log"
static const char *CACHE_LOG_DIR = "/cache/recovery";
static const char *COMMAND_FILE = "/cache/recovery/command";
static const char *INTENT_FILE = "/cache/recovery/intent";
static const char *LOG_FILE = "/cache/recovery/log";
static const char *LAST_INSTALL_FILE = "/cache/recovery/last_install";
static const char *CACHE_ROOT = "/cache";
static const char *FILEMANAGER = "/tmp/aromafm.zip";
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";
static const char *TEMPORARY_INSTALL_FILE = "/tmp/last_install";

extern UIParameters ui_parameters;    // from ui.c

#ifdef QCOM_HARDWARE
static void parse_t_daemon_data_files();
#endif
/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *   --set_encrypted_filesystem=on|off - enables / diasables encrypted fs
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_volume() reformats /data
 * 6. erase_volume() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=/cache/some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

// open a given path, mounting partitions as necessary
FILE* fopen_path(const char *path, const char *mode) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1, sehandle);

    FILE *fp = fopen(path, mode);
    if (fp == NULL && path != COMMAND_FILE) LOGE("Can't open %s\n", path);
    return fp;
}

// close a file, log an error if the error indicator is set
static void check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("Error in %s\n(%s)\n", name, strerror(errno));
    fclose(fp);
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static void get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    if (device_flash_type() == MTD || device_flash_type() == MMC) {
        get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure
    }

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", (int)sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", (int)sizeof(boot.status), boot.status);
    }

    struct stat file_info;

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1 && 0 != stat("/tmp/.ignorebootmessage", &file_info)) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("Bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *token;
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                token = strtok(buf, "\r\n");
                if (token != NULL) {
                    (*argv)[*argc] = strdup(token);  // Strip newline.
                } else {
                    --*argc;
                }
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    set_bootloader_message(&boot);
}

void set_sdcard_update_bootloader_message() {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    set_bootloader_message(&boot);
}

// How much of the temp log we have copied to the copy in cache.
static long tmplog_offset = 0;

static void copy_log_file(const char* source, const char* destination, int append) {
    FILE *log = fopen_path(destination, append ? "a" : "w");
    if (log == NULL) {
        LOGE("Can't open %s\n", destination);
    } else {
        FILE *tmplog = fopen(TEMPORARY_LOG_FILE, "r");
        if (tmplog == NULL) {
            LOGE("Can't open %s\n", TEMPORARY_LOG_FILE);
        } else {
            if (append) {
                fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            }
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            if (append) {
                tmplog_offset = ftell(tmplog);
            }
            check_and_fclose(tmplog, TEMPORARY_LOG_FILE);
        }
        check_and_fclose(log, destination);
    }
}

// Rename last_log -> last_log.1 -> last_log.2 -> ... -> last_log.$max
// Overwrites any existing last_log.$max.
static void rotate_last_logs(int max) {
    char oldfn[256];
    char newfn[256];

    int i;
    for (i = max-1; i >= 0; --i) {
        snprintf(oldfn, sizeof(oldfn), (i==0) ? LAST_LOG_FILE : (LAST_LOG_FILE ".%d"), i);
        snprintf(newfn, sizeof(newfn), LAST_LOG_FILE ".%d", i+1);
        // ignore errors
        rename(oldfn, newfn);
    }
}

static void copy_logs() {
    // Copy logs to cache so the system can find out what happened.
    copy_log_file(TEMPORARY_LOG_FILE, LOG_FILE, true);
    copy_log_file(TEMPORARY_LOG_FILE, LAST_LOG_FILE, false);
    copy_log_file(TEMPORARY_INSTALL_FILE, LAST_INSTALL_FILE, false);
    chmod(LOG_FILE, 0600);
    chown(LOG_FILE, 1000, 1000);   // system user
    chmod(LAST_LOG_FILE, 0640);
    chmod(LAST_INSTALL_FILE, 0644);
    sync();
}

// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void finish_recovery(const char *send_intent) {
    // By this point, we're ready to return to the main system...
    ensure_path_mounted(INTENT_FILE);
    if (send_intent != NULL) {
        FILE *fp = fopen_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("Can't open %s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    copy_logs();

    // Reset to normal system boot so recovery won't cycle indefinitely.
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    set_bootloader_message(&boot);

    // Remove the command file, so recovery won't repeat indefinitely.
    if (ensure_path_mounted(COMMAND_FILE) != 0 ||
        (unlink(COMMAND_FILE) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    sync();  // For good measure.
}

typedef struct _saved_log_file {
    char* name;
    struct stat st;
    unsigned char* data;
    struct _saved_log_file* next;
} saved_log_file;

static int erase_volume(const char *volume) {
    bool is_cache = (strcmp(volume, CACHE_ROOT) == 0);

    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();

    saved_log_file* head = NULL;

    if (is_cache) {
        // If we're reformatting /cache, we load any
        // "/cache/recovery/last*" files into memory, so we can restore
        // them after the reformat.

        ensure_path_mounted(volume);

        DIR* d;
        struct dirent* de;
        d = opendir(CACHE_LOG_DIR);
        if (d) {
            char path[PATH_MAX];
            strcpy(path, CACHE_LOG_DIR);
            strcat(path, "/");
            int path_len = strlen(path);
            while ((de = readdir(d)) != NULL) {
                if (strncmp(de->d_name, "last", 4) == 0) {
                    saved_log_file* p = (saved_log_file*) malloc(sizeof(saved_log_file));
                    strcpy(path+path_len, de->d_name);
                    p->name = strdup(path);
                    if (stat(path, &(p->st)) == 0) {
                        // truncate files to 512kb
                        if (p->st.st_size > (1 << 19)) {
                            p->st.st_size = 1 << 19;
                        }
                        p->data = (unsigned char*) malloc(p->st.st_size);
                        FILE* f = fopen(path, "rb");
                        fread(p->data, 1, p->st.st_size, f);
                        fclose(f);
                        p->next = head;
                        head = p;
                    } else {
                        free(p);
                    }
                }
            }
            closedir(d);
        } else {
            if (errno != ENOENT) {
                printf("opendir failed: %s\n", strerror(errno));
            }
        }
    }

    ui_print("[*] Formatting %s...\n", volume);

    ensure_path_unmounted(volume);
    int result = format_volume(volume);

    if (is_cache) {
        while (head) {
            FILE* f = fopen_path(head->name, "wb");
            if (f) {
                fwrite(head->data, 1, head->st.st_size, f);
                fclose(f);
                chmod(head->name, head->st.st_mode);
                chown(head->name, head->st.st_uid, head->st.st_gid);
            }
            free(head->name);
            free(head->data);
            saved_log_file* temp = head->next;
            free(head);
            head = temp;
        }

        // Any part of the log we'd copied to cache is now gone.
        // Reset the pointer so we copy from the beginning of the temp
        // log.
        tmplog_offset = 0;
        copy_logs();
    }

    ui_set_background(BACKGROUND_ICON_CLOCKWORK);
    ui_reset_progress();
    return result;
}

#ifdef QCOM_HARDWARE
// copy from philz cwm recovery
static void parse_t_daemon_data_files() {
    // Devices with Qualcomm Snapdragon 800 do some shenanigans with RTC.
    // They never set it, it just ticks forward from 1970-01-01 00:00,
    // and then they have files /data/system/time/ats_* with 64bit offset
    // in miliseconds which, when added to the RTC, gives the correct time.
    // So, the time is: (offset_from_ats + value_from_RTC)
    // There are multiple ats files, they are for different systems? Bases?
    // Like, ats_1 is for modem and ats_2 is for TOD (time of day?).
    // Look at file time_genoff.h in CodeAurora, qcom-opensource/time-services

    const char *paths[] = {"/data/system/time/", "/data/time/"};
    char ats_path[PATH_MAX] = "";
    DIR *d;
    FILE *f;
    uint64_t offset = 0;
    struct timeval tv;
    struct dirent *dt;

	if (!is_encrypted_data() && ensure_path_mounted("/data") != 0) {
        LOGI("parse_t_daemon_data_files: failed to mount /data\n");
        return;
    }

    // Prefer ats_2, it seems to be the one we want according to logcat on hammerhead
    // - it is the one for ATS_TOD (time of day?).
    // However, I never saw a device where the offset differs between ats files.
    size_t i;
    for (i = 0; i < (sizeof(paths)/sizeof(paths[0])); ++i) {
        DIR *d = opendir(paths[i]);
        if (!d)
            continue;

        while ((dt = readdir(d))) {
            if (dt->d_type != DT_REG || strncmp(dt->d_name, "ats_", 4) != 0)
                continue;

            if (strlen(ats_path) == 0 || strcmp(dt->d_name, "ats_2") == 0)
                sprintf(ats_path, "%s%s", paths[i], dt->d_name);
        }

        closedir(d);
    }

    if (strlen(ats_path) == 0) {
        LOGI("parse_t_daemon_data_files: no ats files found, leaving time as-is!\n");
        return;
    }

    f = fopen(ats_path, "r");
    if (!f) {
        LOGI("parse_t_daemon_data_files: failed to open file %s\n", ats_path);
        return;
    }

    if (fread(&offset, sizeof(offset), 1, f) != 1) {
        LOGI("parse_t_daemon_data_files: failed load uint64 from file %s\n", ats_path);
        fclose(f);
        return;
    }
    fclose(f);

    //Samsung S5 set date to 2014-xx-xx at boot(init).
    //We set the base date for RTC time.
    f = fopen("/sys/class/rtc/rtc0/since_epoch", "r");
    if (f != NULL) {
        long int rtc_offset;
        fscanf(f, "%ld", &rtc_offset);
        tv.tv_sec = rtc_offset;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);
        fclose(f);
        LOGI("applying rtc time %ld\n", rtc_offset);
    }
    LOGI("setting time offset from file %s, offset %llu\n", ats_path, offset);

    gettimeofday(&tv, NULL);
    tv.tv_sec += offset / 1000;
    tv.tv_usec += (offset % 1000) * 1000;

    while(tv.tv_usec >= 1000000) {
        ++tv.tv_sec;
        tv.tv_usec -= 1000000;
    }

    settimeofday(&tv, NULL);
}
#endif

static const char** prepend_title(const char** headers) {
    const char* title[] = { EXPAND(RECOVERY_VERSION), NULL };

    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 0;
    const char** p;
    for (p = title; *p; ++p, ++count);
    for (p = headers; *p; ++p, ++count);

    const char** new_headers = malloc((count+1) * sizeof(const char*));
    const char** h = new_headers;
    for (p = title; *p; ++p, ++h) *h = *p;
    for (p = headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

int get_menu_selection(const char** headers, char** items, int menu_only,
                   int initial_selection) {
    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui_clear_key_queue();

    int item_count = ui_start_menu(headers, items, initial_selection);
    int selected = initial_selection;
    int chosen_item = -1;

    while (chosen_item < 0 && chosen_item != GO_BACK) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == -1) {   // ui_wait_key() timed out
            if (ui_text_ever_visible()) {
                continue;
            } else {
                LOGI("timed out waiting for key input; rebooting.\n");
                ui_end_menu();
                return ITEM_REBOOT;
            }
        }
        else if (key == -2) { 
            return GO_BACK;
        }
        else if (key == -3) { 
            return REFRESH;
        }

        int action = ui_handle_key(key, visible);

        int old_selected = selected;
        selected = ui_get_selected_item();

        if (action < 0) {
            switch (action) {
                case HIGHLIGHT_UP:
                    --selected;
                    selected = ui_menu_select(selected);
                    break;
                case HIGHLIGHT_DOWN:
                    ++selected;
                    selected = ui_menu_select(selected);
                    break;
                case HIGHLIGHT_ON_TOUCH:
                    selected = ui_menu_touch_select();
                    break;
                case SELECT_ITEM:
                    chosen_item = selected;
                    if (ui_is_showing_back_button()) {
                        if (chosen_item == item_count) {
                            chosen_item = GO_BACK;
                        }
                    }
                    break;
                case NO_ACTION:
                    break;
                case GO_BACK:
                    chosen_item = GO_BACK;
                    break;
            }
        } else if (!menu_only) {
            chosen_item = action;
        }
    }

    ui_end_menu();
    ui_clear_key_queue();
    return chosen_item;
}

static int compare_string(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

//=========================================/
//=            Wipe menu part             =/
//=              carliv@xda               =/
//=========================================/

void wipe_preflash(int confirm) {
	
	if (confirm && !confirm_selection("Confirm wipe as in preflash?", "Yes - Wipe All!"))
		return;
	if (!confirm_selection("It will wipe system too!!!", "Yes - I want it this way."))
		return;
    ui_print("\n-- Wiping System...\n");
    device_wipe_system();
    erase_volume("/system");
    ui_print("System wipe complete.\n");
    sleep(1);
    if (is_data_media()) preserve_data_media(1);
	if (!is_encrypted_data()) {
	    ui_print("\n-- Wiping data...\n");
	    device_wipe_data();
	    erase_volume("/data");
	    if (has_datadata()) {
	        erase_volume("/datadata");
	    }
	} else {
		ui_print("\n-- Data not formated because it is encrypted. If you format it you will loose encryption. If you want to do it use the Wipe data option in this menu.\n");
	} 
    if (volume_for_path("/sd-ext") != NULL) erase_volume("/sd-ext");
    if (get_android_secure_path() != NULL) erase_volume(get_android_secure_path());
    ui_print("Data wipe complete.\n");
    sleep(1);
    ui_print("\n-- Wiping cache...\n");
    device_wipe_cache();
    erase_volume("/cache");
    ui_print("Cache wipe complete.\n");
    sleep(1);
	if (!is_encrypted_data()) ensure_path_mounted("/data");
	if (volume_for_path("/sd-ext") != NULL) ensure_path_mounted("/sd-ext");
	ensure_path_mounted("/cache");
	device_wipe_dalvik_cache();
	ui_print("\n-- Wiping dalvik-cache...\n");
	__system("rm -rf /data/dalvik-cache");
	__system("rm -rf /cache/dalvik-cache");
	if (volume_for_path("/sd-ext") != NULL) __system("rm -rf /sd-ext/dalvik-cache");
	ui_print("Dalvik Cache wiped.\n");
	if (!is_encrypted_data()) ensure_path_unmounted("/data");
	if (volume_for_path("/sd-ext") != NULL) ensure_path_unmounted("/sd-ext"); 
	ui_print("\nPreflash wipe complete. Don't reboot to Android right now with \"Reboot phone\" --first option in menu, because there is no system files in it. Either flash a new ROM or restore a backup to avoid troubles!!!.\n");
	sleep(1);   
}

void wipe_data(int confirm) {
	
	if (confirm && !confirm_selection("Confirm wipe all user data?", "Yes - Wipe All Data"))
		return;
	if (!confirm_selection("Are you sure?", "Yes"))
		return;

	if (is_encrypted_data()) {
		if (!confirm_selection("Are you sure? You will loose encryption!", "Yes"))
			return;
		ui_print("\n-- Formating data. Encryption will be lost...\n");
		if (is_data_media()) preserve_data_media(1);
		device_wipe_data();
	    erase_volume("/data");
	    set_encryption_state(0);
	    if (has_datadata()) {
	        erase_volume("/datadata");
	    }
	} else {
		ui_print("\n-- Wiping data...\n");
		if (is_data_media()) preserve_data_media(1);
	    device_wipe_data();
	    erase_volume("/data");
	    if (has_datadata()) {
	        erase_volume("/datadata");
	    }
	}
    erase_volume("/cache");
    if (volume_for_path("/sd-ext") != NULL) erase_volume("/sd-ext");
    if (get_android_secure_path() != NULL) erase_volume(get_android_secure_path());
    ui_print("Data wipe complete.\n");
}

void wipe_cache(int confirm) {
    if (confirm && !confirm_selection( "Confirm wipe cache?", "Yes - Wipe cache"))
        return;
        
    ui_print("\n-- Wiping cache...\n");
    device_wipe_cache();
    erase_volume("/cache");
    ui_print("Cache wipe complete.\n");
}

void wipe_dalvik_cache(int confirm) {
    if (confirm && !confirm_selection( "Confirm wipe?", "Yes - Wipe Dalvik Cache")) {
		ui_print("Skipping dalvik cache wipe...\n");
		return;
	}
        
    if (!is_encrypted_data()) ensure_path_mounted("/data");
	if (volume_for_path("/sd-ext") != NULL) ensure_path_mounted("/sd-ext");
	ensure_path_mounted("/cache");
	device_wipe_dalvik_cache();
	ui_print("\n-- Wiping dalvik-cache...\n");
	__system("rm -rf /data/dalvik-cache");
	__system("rm -rf /cache/dalvik-cache");
	if (volume_for_path("/sd-ext") != NULL) __system("rm -rf /sd-ext/dalvik-cache");
	if (!is_encrypted_data()) ensure_path_unmounted("/data");
	if (volume_for_path("/sd-ext") != NULL) ensure_path_unmounted("/sd-ext");
	ui_print("Dalvik Cache wiped.\n");
}

int install_zip(const char* packagefilepath) {
    ui_print("\n-- Installing: %s\n", packagefilepath);
    set_sdcard_update_bootloader_message();

    int wipe_cache = 0;
    int status = install_package(packagefilepath, &wipe_cache, TEMPORARY_INSTALL_FILE, true);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        copy_logs();
        ui_set_background(BACKGROUND_ICON_ERROR);
        LOGE("Installation aborted.\n");
        return 1;
    } else if (wipe_cache && erase_volume("/cache")) {
        LOGE("Cache wipe (requested by package) failed.\n");
    }
#ifdef ENABLE_LOKI
    if (loki_support_enabled) {
        ui_print("Checking if loki-fying is needed\n");
        status = loki_check();
        if (status != INSTALL_SUCCESS) {
            ui_set_background(BACKGROUND_ICON_ERROR);
            return 1;
        }
    }
#endif

    ui_set_background(BACKGROUND_ICON_NONE);
    ui_print("\nInstall from sdcard complete.\n");
    return 0;
}

int enter_sideload_mode(int* wipe_cache) {

    ensure_path_mounted(CACHE_ROOT);
    start_sideload(wipe_cache, TEMPORARY_INSTALL_FILE);

    static const char* headers[] = {  "ADB Sideload",
                                "",
                                NULL
    };

    static char* list[] = { "Cancel sideload", NULL };
    
    int status = INSTALL_NONE;
    int item = get_menu_selection(headers, list, 0, 0);
    if (item != GO_BACK) {
        stop_sideload();
    }
    status = wait_sideload();

    if (status >= 0 && status != INSTALL_NONE) {
        if (status != INSTALL_SUCCESS) {
            ui_set_background(BACKGROUND_ICON_ERROR);
            ui_print("Installation aborted.\n");
        } else if (!ui_text_visible()) {
            return status;  // reboot if logs aren't visible
        } else {
            ui_print("\nInstall from ADB complete.\n");
        }
    }
    return status;
}

static void headless_wait() {
    ui_show_text(0);
    const char** headers = prepend_title((const char**)MENU_HEADERS);
    for(;;) {
        finish_recovery(NULL);
        get_menu_selection(headers, MENU_ITEMS, 0, 0);
    }
}

int ui_root_menu = 0;
static void prompt_and_wait(int status) {
    const char** headers = prepend_title((const char**)MENU_HEADERS);

    switch (status) {
        case INSTALL_SUCCESS:
        case INSTALL_NONE:
            ui_set_background(BACKGROUND_ICON_CLOCKWORK);
            break;

        case INSTALL_ERROR:
        case INSTALL_CORRUPT:
            ui_set_background(BACKGROUND_ICON_ERROR);
            break;
    }

    for (;;) {
        finish_recovery(NULL);
        ui_root_menu = 1;
        ui_reset_progress();
       
        int chosen_item = get_menu_selection(headers, MENU_ITEMS, 0, 0);
        ui_root_menu = 0;

        // device-specific code may take some action here.  It may
        // return one of the core actions handled in the switch
        // statement below.
        chosen_item = device_perform_action(chosen_item);

        int ret = 0;

        for (;;) {
        switch (chosen_item) {
            case ITEM_REBOOT:
                return;

            case ITEM_APPLY_ZIP:
                show_install_update_menu();
                break;
                
            case ITEM_WIPE_MENU:
                show_wipe_menu();
                break;    

            case ITEM_NANDROID:
                show_nandroid_menu();
                break;

            case ITEM_PARTITION:
                show_partition_menu();
                break;

            case ITEM_ADVANCED:
                show_advanced_menu();
                break;
                
            case ITEM_CARLIV:
                show_carliv_menu();
                break;  
                
            case ITEM_POWER:
                show_power_menu();
                break;  
                
            case ITEM_CUSTOM:
                toggle_vibration();
                break;     
        }
            if (ret == REFRESH) {
                ret = 0;
                continue;
            }
            break;
        }
    }
}

static void print_property(const char *key, const char *name, void *cookie) {
    printf("%s=%s\n", key, name);
}

static void setup_adbd() {
    struct stat f;
    static char *key_src = "/data/misc/adb/adb_keys";
    static char *key_dest = "/adb_keys";

    // Mount /data and copy adb_keys to root if it exists
    if (!is_encrypted_data()) ensure_path_mounted("/data");
    if (stat(key_src, &f) == 0) {
        FILE *file_src = fopen(key_src, "r");
        if (file_src == NULL) {
            LOGE("Can't open %s\n", key_src);
        } else {
            FILE *file_dest = fopen(key_dest, "w");
            if (file_dest == NULL) {
                LOGE("Can't open %s\n", key_dest);
            } else {
                char buf[4096];
                while (fgets(buf, sizeof(buf), file_src)) fputs(buf, file_dest);
                check_and_fclose(file_dest, key_dest);

                // Enable secure adbd
                property_set("ro.adb.secure", "1");
            }
            check_and_fclose(file_src, key_src);
        }
    }
    preserve_data_media(0);
    if (!is_encrypted_data()) ensure_path_unmounted("/data");
    preserve_data_media(1);

    // Trigger (re)start of adb daemon
    property_set("service.adb.root", "1");
}

// call a clean reboot
void reboot_main_system(int cmd, int flags, char *arg) {
    write_recovery_version();
    verify_root_and_recovery();
    finish_recovery(NULL); // sync() in here
    android_reboot(cmd, flags, arg);
}

int main(int argc, char **argv) {

    if (argc == 2 && strcmp(argv[1], "--adbd") == 0) {
        adb_main();
        return 0;
    }

    // Recovery needs to install world-readable files, so clear umask
    // set by init
    umask(0);

    char* command = argv[0];
    char* stripped = strrchr(argv[0], '/');
    if (stripped)
        command = stripped + 1;

    if (strcmp(command, "recovery") != 0)
    {
        struct recovery_cmd cmd = get_command(command);
        if (cmd.name)
            return cmd.main_func(argc, argv);

#ifdef BOARD_RECOVERY_HANDLES_MOUNT
        if (!strcmp(command, "mount") && argc == 2)
        {
            load_volume_table();
            return ensure_path_mounted(argv[1]);
        }
#endif
        if (!strcmp(command, "setup_adbd")) {
            load_volume_table();
            setup_adbd();
            return 0;
        }
        if (!strcmp(command, "start")) {
            property_set("ctl.start", argv[1]);
            return 0;
        }
        if (!strcmp(command, "stop")) {
            property_set("ctl.stop", argv[1]);
            return 0;
        }
        return busybox_driver(argc, argv);
    }    
    
    __system("/sbin/postrecoveryboot.sh");

    int is_user_initiated_recovery = 0;
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);
    printf("Starting recovery on %s\n", ctime(&start));

    device_ui_init(&ui_parameters);
    ui_init();
    ui_print(EXPAND(RECOVERY_VERSION)" * "EXPAND(RECOVERY_DEVICE)"\n");
    ui_print("Compiled by "EXPAND(RECOVERY_BUILD_USER)"@"EXPAND(RECOVERY_BUILD_HOST)" on: "EXPAND(RECOVERY_BUILD_DATE)"\n");
    
    load_volume_table();
    encrypted_data_mounted = 0;
	data_is_decrypted = 0;
    process_volumes();
#ifdef QCOM_HARDWARE
    parse_t_daemon_data_files();
#endif
    LOGI("Processing arguments.\n");
    ensure_path_mounted(LAST_LOG_FILE);
    rotate_last_logs(10);
    get_args(&argc, &argv);

    const char *send_intent = NULL;
    const char *update_package = NULL;
    int wipe_data = 0, wipe_cache = 0;
    int sideload = 0;
    int headless = 0;
    int shutdown_after = 0;

    LOGI("Checking arguments.\n");
    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
        case 'w':
#ifndef BOARD_RECOVERY_ALWAYS_WIPES
        wipe_data = wipe_cache = 1;
#endif
        break;
        case 'h':
            ui_set_background(BACKGROUND_ICON_NONE);
            ui_show_text(0);
            headless = 1;
            break;
        case 'c': wipe_cache = 1; break;
        case 't': ui_show_text(1); break;
        case 'l': sideload = 1; break;
        case 'p': shutdown_after = 1; break;
        case '?':
            LOGE("Invalid command argument\n");
            continue;
        }
    }

    struct selinux_opt seopts[] = {
      { SELABEL_OPT_PATH, "/file_contexts" }
    };

    sehandle = selabel_open(SELABEL_CTX_FILE, seopts, 1);

    if (!sehandle) {
        fprintf(stderr, "Warning: No file_contexts\n");
    } else {
		LOGI("Selinux enabled!\n");
	}

    device_recovery_start();

    printf("Command:");
    for (arg = 0; arg < argc; arg++) {
        printf(" \"%s\"", argv[arg]);
    }
    printf("\n");

    if (update_package) {
        // For backwards compatibility on the cache partition only, if
        // we're given an old 'root' path "CACHE:foo", change it to
        // "/cache/foo".
        if (strncmp(update_package, "CACHE:", 6) == 0) {
            int len = strlen(update_package) + 10;
            char* modified_path = (char*)malloc(len);
            if (modified_path) {
                strlcpy(modified_path, "/cache/", len);
                strlcat(modified_path, update_package+6, len);
                printf("(replacing path \"%s\" with \"%s\")\n",
                       update_package, modified_path);
                update_package = modified_path;
            }
            else
                printf("modified_path allocation failed\n");
        }
    }
    printf("\n");

    property_list(print_property, NULL);
    printf("\n");

    int status = INSTALL_SUCCESS;

    if (update_package != NULL) {
        status = install_package(update_package, &wipe_cache, TEMPORARY_INSTALL_FILE, true);
        if (status == INSTALL_SUCCESS && wipe_cache) {
            if (erase_volume("/cache")) {
                LOGE("Cache wipe (requested by package) failed.\n");
            }
        }
        if (status != INSTALL_SUCCESS) {
            ui_print("Installation aborted.\n");
        }
    } else if (wipe_data) {
        if (device_wipe_data()) status = INSTALL_ERROR;
        preserve_data_media(0);
        if (erase_volume("/data")) status = INSTALL_ERROR;
        preserve_data_media(1);
        if (has_datadata() && erase_volume("/datadata")) status = INSTALL_ERROR;
        if (wipe_cache && erase_volume("/cache")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) {
            ui_print("Data wipe failed.\n");
        }
    } else if (wipe_cache) {
        if (wipe_cache && erase_volume("/cache")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) {
            ui_print("Cache wipe failed.\n");
        }
    } else if (sideload) {
        status = enter_sideload_mode(&wipe_cache);
    } else {
        LOGI("Checking for extendedcommand...\n");
        status = INSTALL_NONE;  // No command specified
        // we are starting up in user initiated recovery here
        // let's set up some default options
        vibration_enabled = 0;
        signature_check_enabled = 0;
        md5_check_enabled = 0;
        script_assert_enabled = 0;
        ui_get_rainbow_mode = 0;
        is_user_initiated_recovery = 1;
        if (!headless) {
            ui_set_show_text(1);
            ui_set_background(BACKGROUND_ICON_CLOCKWORK);
        }

        if (extendedcommand_file_exists()) {
            LOGI("Running extendedcommand...\n");
            int ret;
            if (0 == (ret = run_and_remove_extendedcommand())) {
                status = INSTALL_SUCCESS;
                ui_set_show_text(0);
            }
            else {
                if (ret != 0) handle_failure();
            }
        } else {
            LOGI("Skipping execution of extendedcommand, file not found...\n");
        }
    }

    if (headless) {
        headless_wait();
    }
    if (status == INSTALL_ERROR || status == INSTALL_CORRUPT) {
        copy_logs();
        handle_failure();
    }
    else if (status != INSTALL_SUCCESS || ui_text_visible()) {
        prompt_and_wait(status);
    }

    // We reach here when in main menu we choose reboot main system or for some wipe commands on start

    // Otherwise, get ready to boot the main system...
    finish_recovery(send_intent);
    if (shutdown_after) {
        ui_print("Shutting down...\n");
        reboot_main_system(ANDROID_RB_POWEROFF, 0, 0);
    } else {
        ui_print("Rebooting...\n");
        reboot_main_system(ANDROID_RB_RESTART, 0, 0);
    }
    return EXIT_SUCCESS;
}
