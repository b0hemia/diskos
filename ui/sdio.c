/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sdio.h"
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#ifndef SD_IO_FAULT_MARKER
#define SD_IO_FAULT_MARKER "/tmp/.diskos_sd_io_fault"
#endif
static int g_fault;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_users;
static int g_allowed;
static int (*g_media_local)(void);

void sd_io_init(int (*media_local)(void)){
    pthread_mutex_lock(&g_mu);
    assert(g_users == 0);
    g_media_local = media_local;
    g_allowed = 0;
    g_fault = g_fault || access(SD_IO_FAULT_MARKER, F_OK) == 0;
    pthread_mutex_unlock(&g_mu);
}
int sd_io_begin(void){
    pthread_mutex_lock(&g_mu);
    int ok = g_allowed && g_media_local && g_media_local();
    if(ok) g_users++;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void sd_io_end(void){
    pthread_mutex_lock(&g_mu);
    assert(g_users > 0);
    g_users--;
    pthread_mutex_unlock(&g_mu);
}
void sd_io_hold(void){
    pthread_mutex_lock(&g_mu);
    g_allowed = 0;
    g_fault = g_fault || access(SD_IO_FAULT_MARKER, F_OK) == 0;
    pthread_mutex_unlock(&g_mu);
}
unsigned sd_io_active(void){
    pthread_mutex_lock(&g_mu);
    unsigned n = g_users;
    pthread_mutex_unlock(&g_mu);
    return n;
}
int sd_io_resume(void){
    pthread_mutex_lock(&g_mu);
    int ok = !g_fault && g_users == 0 && g_media_local && g_media_local();
    if(ok) g_allowed = 1;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void sd_io_fault(void){
    pthread_mutex_lock(&g_mu);
    g_fault = 1; g_allowed = 0;
    int fd = open(SD_IO_FAULT_MARKER, O_WRONLY|O_CREAT|O_CLOEXEC, 0600);
    if(fd >= 0) close(fd);
    pthread_mutex_unlock(&g_mu);
}
int sd_io_healthy(void){
    pthread_mutex_lock(&g_mu);
    int ok = !g_fault;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int sd_io_allowed(void){
    pthread_mutex_lock(&g_mu);
    int ok = g_allowed;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
