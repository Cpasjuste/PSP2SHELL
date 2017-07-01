/*
	PSP2SHELL
	Copyright (C) 2016, Cpasjuste

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/modulemgr.h>
#include <libk/stdio.h>
#include <libk/stdarg.h>
#include <libk/string.h>
#include <libk/stdbool.h>

#include "kp2s_hooks.h"

#ifdef __USB__

#include "../psp2shell_m/include/psp2shell.h"
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/cpu.h>
#include "usbhostfs/usbasync.h"
#include "usbhostfs/usbhostfs.h"
#include "../common/p2s_msg.h"
#include "../common/p2s_cmd.h"

extern bool kp2s_ready;

static bool quit = false;
static SceUID u_sema = -1;
static SceUID k_sema = -1;

static SceUID thid_wait = -1;
static P2S_CMD kp2s_cmd;
volatile static int k_buf_lock = 0;

static struct AsyncEndpoint g_endp;
static struct AsyncEndpoint g_stdout;

static int usbShellInit(void) {

    int ret = usbAsyncRegister(ASYNC_SHELL, &g_endp);
    printf("usbAsyncRegister: ASYNC_SHELL = %i\n", ret);
    ret = usbAsyncRegister(ASYNC_STDOUT, &g_stdout);
    printf("usbAsyncRegister: ASYNC_STDOUT = %i\n", ret);

    printf("usbWaitForConnect\n");
    ret = usbWaitForConnect();
    printf("usbWaitForConnect: %i\n", ret);

    return 0;
}

static void welcome() {

    PRINT("\n\n                     ________         .__           .__  .__   \n");
    PRINT("______  ____________ \\_____  \\   _____|  |__   ____ |  | |  |  \n");
    PRINT("\\____ \\/  ___/\\____ \\ /  ____/  /  ___/  |  \\_/ __ \\|  | |  |  \n");
    PRINT("|  |_> >___ \\ |  |_> >       \\  \\___ \\|   Y  \\  ___/|  |_|  |__\n");
    PRINT("|   __/____  >|   __/\\_______ \\/____  >___|  /\\___  >____/____/\n");
    PRINT("|__|       \\/ |__|           \\/     \\/     \\/     \\/ %s\n\n", "0.0");
    PRINT("\r\n");
}

int kp2s_print_stdout(const char *data, size_t size) {

    if (!usbhostfs_connected()) {
        return -1;
    }

    int ret = usbAsyncWrite(ASYNC_STDOUT, data, size);
    return ret;
}

int kp2s_print_stdout_user(const char *data, size_t size) {

    int state = 0;
    ENTER_SYSCALL(state);

    if (!usbhostfs_connected()) {
        EXIT_SYSCALL(state);
        return -1;
    }

    char kbuf[size];
    memset(kbuf, 0, size);
    ksceKernelMemcpyUserToKernel(kbuf, (uintptr_t) data, size);

    int ret = usbAsyncWrite(ASYNC_STDOUT, kbuf, size);

    EXIT_SYSCALL(state);
    return ret;
}

int kp2s_print_color(int color, const char *fmt, ...) {

    if (!usbhostfs_connected()) {
        return -1;
    }

    char buffer[P2S_KMSG_SIZE];
    memset(buffer, 0, P2S_KMSG_SIZE);
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, P2S_KMSG_SIZE, fmt, args);
    va_end(args);

    p2s_msg_send(ASYNC_SHELL, color, buffer);

    return len;
}

int kp2s_print_color_user(int color, const char *data, size_t size) {

    int state = 0;
    ENTER_SYSCALL(state);

    if (!usbhostfs_connected()) {
        PRINT_ERR("kp2s_print_color_user(k): !usbhostfs_connected\n");
        EXIT_SYSCALL(state);
        return -1;
    }

    char buffer[size + 1];
    memset(buffer, 0, size + 1);
    ksceKernelMemcpyUserToKernel(buffer, (uintptr_t) data, size);
    buffer[size + 1] = '\0';
    p2s_msg_send(ASYNC_SHELL, color, buffer);

    EXIT_SYSCALL(state);

    return size;
}

int kp2s_wait_cmd(P2S_CMD *cmd) {

    int state = 0;
    int ret = -1;

    ENTER_SYSCALL(state);

    //PRINT_ERR("ksceKernelWaitSema: u_sema\n");
    ksceKernelWaitSema(u_sema, 1, NULL);
    //PRINT_ERR("ksceKernelWaitSema: u_sema received\n");

    if (kp2s_cmd.type > CMD_START) {
        ksceKernelMemcpyKernelToUser((uintptr_t) cmd, &kp2s_cmd, sizeof(P2S_CMD));
        kp2s_cmd.type = 0;
        ret = 0;
    }

    ksceKernelSignalSema(k_sema, 1);

    EXIT_SYSCALL(state);

    return ret;
}

static int thread_wait_cmd(SceSize args, void *argp) {

    printf("thread_wait_cmd start\n");
    //ksceKernelDelayThread(1000*1000*5);

    int res = usbhostfs_start();
    if (res != 0) {
        printf("module_start: usbhostfs_start failed\n");
        return -1;
    }

    usbShellInit();
    welcome();

    //set_hooks();

    while (!quit) {

        res = p2s_cmd_receive(ASYNC_SHELL, &kp2s_cmd);
        //PRINT_ERR("p2s_cmd_receive: %i", kp2s_cmd.type);

        if (res != 0) {
            if (!usbhostfs_connected()) {
                PRINT_ERR("p2s_cmd_receive failed, waiting for usb...\n");
                usbWaitForConnect();
            } else {
                PRINT_ERR("p2s_cmd_receive failed, unknow error...\n");
            }
        } else {

            switch (kp2s_cmd.type) {



                default:
                    if (!kp2s_ready) {
                        PRINT_ERR("psp2shell main user module not loaded \n");
                    } else {
                        ksceKernelSignalSema(u_sema, 1);
                        ksceKernelWaitSema(k_sema, 0, NULL);
                    }
                    break;
            }
        }
    }

    printf("thread_wait_cmd end\n");
    return 0;
}

#endif

void _start() __attribute__ ((weak, alias ("module_start")));

int module_start(SceSize argc, const void *args) {

#ifdef __USB__
    u_sema = ksceKernelCreateSema("p2s_sem_u", 0, 0, 1, NULL);
    k_sema = ksceKernelCreateSema("p2s_sem_k", 0, 0, 1, NULL);

    thid_wait = ksceKernelCreateThread("kp2s_wait_cmd", thread_wait_cmd, 64, 0x6000, 0, 0x10000, 0);
    if (thid_wait >= 0) {
        ksceKernelStartThread(thid_wait, 0, NULL);
    }
#endif

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {

    delete_hooks();

#ifdef __USB__
    quit = true;

    if (u_sema >= 0) {
        ksceKernelDeleteSema(u_sema);
    }
    if (k_sema >= 0) {
        ksceKernelDeleteSema(k_sema);
    }
    //ksceKernelDeleteThread(thid_wait);
    //ksceKernelWaitThreadEnd(thid_wait, 0, 0);

    printf("module_stop: usbhostfs_stop\n");
    int res = usbhostfs_stop();
    if (res != 0) {
        printf("module_stop: usbhostfs_stop failed\n");
    }
#endif

    return SCE_KERNEL_STOP_SUCCESS;
}