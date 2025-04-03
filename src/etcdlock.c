/* Copyright 2025 EasyStack, Inc. */
/* This file provides functions for libvirt using etcdlock */

#include <errno.h>       
#include <fcntl.h>       
#include <pthread.h>     
#include <signal.h>      
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/time.h>   
#include <sys/types.h>
#include <sys/wait.h> 

#include <curl/curl.h>

#include "etcdlock_manager_internal.h"
#include "etcdlock_manager_rv.h"
#include "libvirt_helper.h"
#include "monotime.h"
#include "list.h"
#include "etcdlock.h"
#include "timeouts.h"
#include "watchdog.h"


#define BASE_URL "http://localhost:8080"
#define MAX_URL_LENGTH 256

// 响应数据的结构体
struct ResponseData {
    char *data;
    size_t size;
};

// 写入回调函数
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct ResponseData *resp = (struct ResponseData *)userp;

    char *ptr = realloc(resp->data, resp->size + realsize + 1);
    if (!ptr) {
        printf("内存不足\n");
        return 0;
    }

    resp->data = ptr;
    memcpy(&(resp->data[resp->size]), contents, realsize);
    resp->size += realsize;
    resp->data[resp->size] = 0;

    return realsize;
}

// 获取锁
static int acquire_lock(const char *key, const char *value) {
    CURL *curl;
    CURLcode res;
    char url[MAX_URL_LENGTH];
    struct ResponseData resp = {0};
    struct curl_slist *headers = NULL;
    char post_data[100];

    // 初始化curl
    curl = curl_easy_init();
    if (!curl) {
        printf("curl初始化失败\n");
        return -1;
    }

    // 构造URL和POST数据
    snprintf(url, sizeof(url), "%s/lock/%s", BASE_URL, key);
    snprintf(post_data, sizeof(post_data), "value=%s", value);

    // 设置请求头
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    // 设置curl选项
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    // 执行请求
    res = curl_easy_perform(curl);
    
    // 清理
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        printf("获取锁失败: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return -1;
    }

    printf("获取锁响应: %s\n", resp.data);
    free(resp.data);
    return ETCDLOCK_OK;
}

// 释放锁
int release_lock(const char *key) {
    CURL *curl;
    CURLcode res;
    char url[MAX_URL_LENGTH];
    struct ResponseData resp = {0};

    curl = curl_easy_init();
    if (!curl) {
        printf("curl初始化失败\n");
        return -1;
    }

    snprintf(url, sizeof(url), "%s/unlock/%s", BASE_URL, key);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        printf("释放锁失败: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return -1;
    }

    printf("释放锁响应: %s\n", resp.data);
    free(resp.data);
    return ETCDLOCK_OK;
}

// 续约
static int keep_alive(const char *key, const char *value) {
    CURL *curl;
    CURLcode res;
    char url[MAX_URL_LENGTH];
    struct ResponseData resp = {0};
    char post_data[100];

    curl = curl_easy_init();
    if (!curl) {
        printf("curl初始化失败\n");
        return -1;
    }

    // 构造URL和POST数据
    snprintf(url, sizeof(url), "%s/keepalive/%s", BASE_URL, key);
    snprintf(post_data, sizeof(post_data), "value=%s", value);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        printf("续约失败: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return -1;
    }

    printf("续约响应: %s\n", resp.data);
    free(resp.data);
    return ETCDLOCK_OK;
}

/*
 * if etcdlock_result is success:
 * new record saving last_success (timestamp in keepalive),
 * if etcdlock_result is timeout:
 * increment next_timeouts in prev record
 * if etcdlock_result is failure:
 * increment next_errors in prev record
 */
static void save_keepalive_history(struct etcdlock *elk, int etcdlock_result,
    uint64_t last_success)
{
    struct keepalive_history *hi;

    if (!elk->keepalive_history_size || !elk->keepalive_history)
        return;

    if (etcdlock_result == ETCDLOCK_OK) {
        hi = &elk->keepalive_history[elk->keepalive_history_next];

        hi->timestamp = last_success;

        elk->keepalive_history_prev = elk->keepalive_history_next;
        elk->keepalive_history_next++;
        if (elk->keepalive_history_next >= elk->keepalive_history_size)
            elk->keepalive_history_next = 0;
    } else {
        hi = &elk->keepalive_history[elk->keepalive_history_prev];

        hi->next_errors++;
    }
}

/* acquire lock and keep alive thread function */
void *lock_thread(void *arg_in)
{
    struct etcdlock *elk;
    uint64_t etcdlock_begin, last_success = 0;
    int etcdlock_length, keepalive_interval = 0;
    int keepalive_interval_seconds, keepalive_fail_timeout_seconds;
    int etcdlock_result;
    int stop = 0;
 
    elk = (struct etcdlock *)arg_in;
 
    keepalive_interval_seconds = calc_keepalive_interval_seconds(elk->base_timeout);
    keepalive_fail_timeout_seconds = calc_keepalive_fail_timeout_seconds(elk->base_timeout);

    while (1) {
        pthread_mutex_lock(&elk->mutex);
        stop = elk->thread_stop;
        pthread_mutex_unlock(&elk->mutex);
        if (stop)
            break;
 
        /*
        * wait between each keepalive
        */
        if (monotime() - last_success < keepalive_interval_seconds) {
            sleep(1);
            continue;
        } else {
            /* don't spin too quickly if keepalive is failing
               immediately and repeatedly */
            usleep(500000);
        }
 
 
        /*
         * do a keepalive, measuring length of time spent in keepalive,
         * and the length of time between successful keepalives
         */
        etcdlock_begin = monotime();
 
        etcdlock_result = keep_alive(elk->key, elk->value);
        etcdlock_length = monotime() - etcdlock_begin;
 
        if (etcdlock_result == ETCDLOCK_OK) {
            keepalive_interval = monotime() - last_success;
            last_success = monotime();
        }

        /*
         * publish the results
         */
        pthread_mutex_lock(&elk->mutex);
        elk->lease_status.keepalive_last_result = etcdlock_result;
        elk->lease_status.keepalive_last_attempt = etcdlock_begin;
 
        if (etcdlock_result == ETCDLOCK_OK)
            elk->lease_status.keepalive_last_success = last_success;
 
        /*
         * pet the watchdog
         * (don't update on thread_stop because it's probably unlinked)
         */
 
        if (etcdlock_result == ETCDLOCK_OK && !elk->thread_stop)
            update_watchdog(elk, last_success, keepalive_fail_timeout_seconds);
 
        save_keepalive_history(elk, etcdlock_result, last_success);
        pthread_mutex_unlock(&elk->mutex);
 
        /*
         * log the results
         */
        if (etcdlock_result != ETCDLOCK_OK) {
            /*log_erros(elk, "keepalive error %d etcdlock_length %d last_success %llu",
                etcdlock_result, etcdlock_length, (unsigned long long)last_success);*/
            fprintf(stderr, "keepalive error %d etcdlock_length %d last_success %llu\n",
                etcdlock_result, etcdlock_length, (unsigned long long)last_success);
        } else if (etcdlock_length > keepalive_interval_seconds) {
            /*log_erros(elk, "keepalive %llu etcdlock_length %d too long",
                (unsigned long long)last_success, etcdlock_length);*/
            fprintf(stderr, "keepalive %llu etcdlock_length %d too long\n",
                (unsigned long long)last_success, etcdlock_length);
        } else {
            if (com.debug_keepalive) {
                /*log_etcdlock(elk, "keepalive %llu etcdlock_length %d interval %d",
                    (unsigned long long)last_success, etcdlock_length, keepalive_interval);*/
                fprintf(stdin, "keepalive %llu etcdlock_length %d interval %d\n",
                    (unsigned long long)last_success, etcdlock_length, keepalive_interval);
            }
        }
    }
 
    /* watchdog unlink was done in main_loop when thread_stop was set, to
        get it done as quickly as possible in case the wd is about to fire. */
 
    disconnect_watchdog(elk);

    release_lock(elk->key);
 
    return NULL;
}

int acquire_lock_start(struct etcdlock *elk)
{
    uint64_t etcdlock_begin, last_success = 0;
    int rv;
    int acquire_result, etcdlock_result;
    int keepalive_fail_timeout_seconds;
    int wd_con;

    keepalive_fail_timeout_seconds = calc_keepalive_fail_timeout_seconds(elk->base_timeout);

    etcdlock_begin = monotime();
 
    /* Connect first so we can fail quickly if wdmd is not running. */
    wd_con = connect_watchdog(elk);
    if (wd_con < 0) {
        /*log_erros(elk, "connect_watchdog failed %d", wd_con);*/
        fprintf(stderr, "connect_watchdog failed %d\n", wd_con);
        acquire_result = ETCDLOCK_WD_ERROR;
        etcdlock_result = -1;
        goto set_status;
    }
 
    /*
     * Tell wdmd to open the watchdog device, set the fire timeout and
     * begin the keepalive loop that regularly pets the watchdog.  This
     * only happens for the first client/etcdlock.  This fails if the
     * watchdog device cannot be opened by wdmd or does not support the
     * requested fire timeout.
     *
     * For later clients/etcdlocks, when wdmd already has the watchdog
     * open, this does nothing (just verifies that fire timeout matches
     * what's in use.)
     */
    rv = open_watchdog(wd_con, com.watchdog_fire_timeout);
    if (rv < 0) {
        /*log_erros(elk, "open_watchdog with fire_timeout %d failed %d",
                  com.watchdog_fire_timeout, wd_con);*/
        fprintf(stderr, "open_watchdog with fire_timeout %d failed %d\n",
                  com.watchdog_fire_timeout, wd_con);
        acquire_result = ETCDLOCK_WD_ERROR;
        etcdlock_result = -1;
        disconnect_watchdog(elk);
        goto set_status;
    }
 
    /*
     * acquire the etcd lock
     */
    etcdlock_begin = monotime();
 
    etcdlock_result = acquire_lock(elk->key, elk->value);
 
    if (etcdlock_result == ETCDLOCK_OK)
        last_success = monotime();
 
    acquire_result = etcdlock_result;
 
    /* we need to start the watchdog after we acquire the etcd lock but
        before we allow any pid's to begin running */
    if (etcdlock_result == ETCDLOCK_OK) {
        rv = activate_watchdog(elk, last_success, keepalive_fail_timeout_seconds, wd_con);
        if (rv < 0) {
            /*log_erros(elk, "activate_watchdog failed %d", rv);*/
            fprintf(stderr, "activate_watchdog failed %d\n", rv);
            acquire_result = ETCDLOCK_WD_ERROR;
        }
    } else {
        if (com.use_watchdog)
            close(wd_con);
    }
 
set_status:
    pthread_mutex_lock(&elk->mutex);
    elk->lease_status.acquire_last_result = acquire_result;
    elk->lease_status.acquire_last_attempt = etcdlock_begin;
    if (etcdlock_result == ETCDLOCK_OK)
        elk->lease_status.acquire_last_success = last_success;
    elk->lease_status.keepalive_last_result = acquire_result;
    elk->lease_status.keepalive_last_attempt = etcdlock_begin;
    if (etcdlock_result == ETCDLOCK_OK)
        elk->lease_status.keepalive_last_success = last_success;
    /* First keepalive entry shows the acquire time with 0 latencies. */
    save_keepalive_history(elk, etcdlock_result, last_success);
    pthread_mutex_unlock(&elk->mutex);

    /* If acquire lock successful, start the keepalive thread */
    if (acquire_result == ETCDLOCK_OK) {
        rv = pthread_create(&elk->thread, NULL, lock_thread, elk);
        if (rv) {
            /*log_erros(elk, "acquire lock create keepalive thread failed %d", rv);*/
            fprintf(stderr, "acquire lock create keepalive thread failed %d\n", rv);
            acquire_result = -1;
            goto do_fail;
        }

        /* Add elk to etcdlocks list for main loop to check the lock lease */
        pthread_mutex_lock(&etcdlocks_mutex);
        list_add(&elk->list, &etcdlocks);
        pthread_mutex_unlock(&etcdlocks_mutex);

        /*log_etcdlock(elk, "acquire lock and create keepalive thread success");*/
        fprintf(stdin, "acquire lock and create keepalive thread success\n");
        return 0;
    }
do_fail:
    if (etcdlock_result == ETCDLOCK_OK){
        release_lock(elk->key);
        if (com.use_watchdog)
            close(wd_con);
    }

    pthread_mutex_lock(&etcdlocks_mutex);
    list_del(&elk->list);
    pthread_mutex_unlock(&etcdlocks_mutex);

    return acquire_result;
}

/*
 * check if etcdlock lease has keepalived within timeout
 */
int check_etcdlock_lease(struct etcdlock *elk)
{
    int keepalive_fail_timeout_seconds, keepalive_warn_timeout_seconds;
    uint64_t last_success;
    int gap;
 
    pthread_mutex_lock(&elk->mutex);
    last_success = elk->lease_status.keepalive_last_success;
    pthread_mutex_unlock(&elk->mutex);
 
    gap = monotime() - last_success;
 
    keepalive_fail_timeout_seconds = calc_keepalive_fail_timeout_seconds(elk->base_timeout);
    keepalive_warn_timeout_seconds = calc_keepalive_warn_timeout_seconds(elk->base_timeout);
 
    if (gap >= keepalive_fail_timeout_seconds) {
        /*log_erros(elk, "check_etcdlock_lease failed %d", gap);*/
        fprintf(stderr, "check_etcdlock_lease failed %d\n", gap);
        return -1;
    }
 
    if (gap >= keepalive_warn_timeout_seconds) {
        /*log_erros(elk, "check_etcdlock_lease warning %d last_success %llu",
            gap, (unsigned long long)last_success);*/
        fprintf(stderr, "check_etcdlock_lease warning %d last_success %llu\n",
            gap, (unsigned long long)last_success);
    }
 
    return 0;
}
