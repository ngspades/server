// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/PluginNet.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_QUEUE 64 /* a lane's waiting and finished together */

typedef struct net_job
{
    uint32_t        id;
    char            method[16];
    char*           url;
    char**          headers;
    int             header_count;
    char*           body;
    size_t          body_len;
    int             timeout_s;
    int             ip_family;
    struct net_job* next;
} net_job_t;

typedef struct net_done
{
    plugin_net_result_t r;
    struct net_done*    next;
} net_done_t;

typedef struct
{
    pthread_cond_t wake;
    net_job_t *    jobs, *jobs_tail;
    net_done_t *   done_head, *done_tail;
    int            in_flight; /* queued, running or finished unread */
    int            started;
    pthread_t      thread;
} net_lane_state_t;

static pthread_mutex_t  net_lock = PTHREAD_MUTEX_INITIALIZER;
static net_lane_state_t net_lanes[NET_LANES];
static uint32_t         net_next_id = 1;

int plugin_net_host(const char* url, char* host, size_t size)
{
    const char* p;
    if (!strncmp(url, "https://", 8)) {
        p = url + 8;
    } else if (!strncmp(url, "http://", 7)) {
        p = url + 7;
    } else {
        return 0;
    }
    size_t n = 0;
    /* user:password@ never goes through: it would hide the real host */
    for (const char* c = p; *c && *c != '/' && *c != '?' && *c != '#'; c++) {
        if (*c == '@') {
            return 0;
        }
    }
    while (p[n] && p[n] != '/' && p[n] != ':' && p[n] != '?' && p[n] != '#') {
        n++;
    }
    if (n == 0 || n >= size) {
        return 0;
    }
    for (size_t k = 0; k < n; k++) {
        host[k] = (char) tolower((unsigned char) p[k]);
    }
    host[n] = '\0';
    return 1;
}

#ifdef ENABLE_CURL /* only the lanes' threads free a job (the web build has none) */
static void _net_job_free(net_job_t* j)
{
    for (int k = 0; k < j->header_count; k++) {
        free(j->headers[k]);
    }
    free(j->headers);
    free(j->url);
    free(j->body);
    free(j);
}
#endif

static void _net_finish(net_lane_t lane, uint32_t id, long status, char* body, size_t len, const char* error)
{
    net_done_t* d = calloc(1, sizeof(*d));
    if (d == NULL) {
        free(body);
        return;
    }
    d->r.id       = id;
    d->r.status   = status;
    d->r.body     = body;
    d->r.body_len = len;
    snprintf(d->r.error, sizeof d->r.error, "%s", error != NULL ? error : "");
    pthread_mutex_lock(&net_lock);
    net_lane_state_t* l = &net_lanes[lane];
    if (l->done_tail) {
        l->done_tail->next = d;
    } else {
        l->done_head = d;
    }
    l->done_tail = d;
    pthread_mutex_unlock(&net_lock);
}

int net_done(net_lane_t lane, plugin_net_result_t* out)
{
    pthread_mutex_lock(&net_lock);
    net_lane_state_t* l = &net_lanes[lane];
    net_done_t*       d = l->done_head;
    if (d != NULL) {
        l->done_head = d->next;
        if (l->done_head == NULL) {
            l->done_tail = NULL;
        }
        l->in_flight--;
    }
    pthread_mutex_unlock(&net_lock);
    if (d == NULL) {
        return 0;
    }
    *out = d->r;
    free(d);
    return 1;
}

int plugin_net_done(plugin_net_result_t* out)
{
    return net_done(NET_LANE_PLUGINS, out);
}

uint32_t plugin_net_http(const char*        method,
                         const char*        url,
                         const char* const* headers,
                         int                header_count,
                         const char*        body,
                         size_t             body_len,
                         int                timeout_s)
{
    net_request_t req = {NET_LANE_PLUGINS, method, url, headers, header_count, body, body_len, timeout_s, 0};
    return net_http(&req);
}

#ifdef ENABLE_CURL

#include <curl/curl.h>
#include <sys/stat.h>
#ifdef SERVER_CA_BUNDLE
/* a CA bundle built in (-DSERVER_CA_BUNDLE=<file>, the static Linux build):
   for a machine with no certificates installed at all (the debian and
   ubuntu container images ship none) */
#include <server_ca_bundle.h>
#endif

#if defined(__linux__)
/* A static libcurl (the release's, built on Alpine) only knows the
   certificate file of the machine it was built on, and every distribution
   keeps its own somewhere else: look for the usual ones once. */
static const char* _net_ca_file(void)
{
    static int         looked;
    static const char* found;
    if (looked) {
        return found;
    }
    looked = 1;
    struct stat st;
    const char* env = getenv("SSL_CERT_FILE");
    if (env != NULL && *env != '\0' && stat(env, &st) == 0) {
        return found = env;
    }
    static const char* const paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Ubuntu, Alpine, Arch, Gentoo */
        "/etc/pki/tls/certs/ca-bundle.crt",                  /* Fedora, RHEL, CentOS */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* Fedora, RHEL */
        "/etc/ssl/ca-bundle.pem",                            /* openSUSE */
        "/etc/ssl/cert.pem",                                 /* Alpine, Void */
        "/etc/pki/tls/cacert.pem",                           /* OpenELEC */
        "/etc/certs/ca-certificates.crt",                    /* Solus */
        "/var/lib/ca-certificates/ca-bundle.pem",            /* openSUSE */
        NULL,
    };
    for (int k = 0; paths[k] != NULL; k++) {
        if (stat(paths[k], &st) == 0 && S_ISREG(st.st_mode)) {
            return found = paths[k];
        }
    }
    return NULL;
}
#endif

void net_curl_tls(void* handle)
{
    CURL* curl = handle;
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
    /* the Windows certificate store, whatever TLS library curl was built on */
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long) CURLSSLOPT_NATIVE_CA);
#elif defined(__linux__)
    const char* ca = _net_ca_file();
    if (ca != NULL) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
        return;
    }
#if defined(SERVER_CA_BUNDLE) && LIBCURL_VERSION_NUM >= 0x074D00
    struct curl_blob blob = {(void*) server_ca_bundle, sizeof server_ca_bundle - 1, CURL_BLOB_NOCOPY};
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
#endif
#else
    (void) curl;
#endif
}

typedef struct
{
    char*  data;
    size_t len;
    int    too_big;
} net_buf_t;

static size_t _net_on_body(char* ptr, size_t size, size_t nmemb, void* user)
{
    net_buf_t* b = user;
    size_t     n = size * nmemb;
    if (b->len + n > PLUGIN_NET_MAX_BODY) {
        b->too_big = 1;
        return 0; /* stops the transfer */
    }
    char* grown = realloc(b->data, b->len + n + 1);
    if (grown == NULL) {
        return 0;
    }
    b->data = grown;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static void _net_run(net_lane_t lane, net_job_t* j)
{
    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        _net_finish(lane, j->id, 0, NULL, 0, "curl_easy_init failed");
        return;
    }
    net_buf_t          buf   = {0};
    struct curl_slist* slist = NULL;
    for (int k = 0; k < j->header_count; k++) {
        slist = curl_slist_append(slist, j->headers[k]);
    }
    char agent[64];
    snprintf(agent, sizeof agent, "%s", GAME_SERVER_TITLE);
    curl_easy_setopt(curl, CURLOPT_URL, j->url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, j->method);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _net_on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long) j->timeout_s);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    /* the master lists the address a call came from: an announce over each
       family is how a server gets both of its rows */
    if (j->ip_family == 4) {
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, (long) CURL_IPRESOLVE_V4);
    } else if (j->ip_family == 6) {
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, (long) CURL_IPRESOLVE_V6);
    }
    /* no redirects: one could lead to a host the plugin may not reach */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    net_curl_tls(curl);
    if (j->body != NULL) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, j->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) j->body_len);
    }
    CURLcode res    = curl_easy_perform(curl);
    long     status = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(slist);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        free(buf.data);
        _net_finish(lane, j->id, 0, NULL, 0, buf.too_big ? "the answer is larger than 1 MB" : curl_easy_strerror(res));
        return;
    }
    if (buf.data == NULL) {
        buf.data = calloc(1, 1);
    }
    _net_finish(lane, j->id, status, buf.data, buf.len, NULL);
}

static void* _net_thread_main(void* arg)
{
    net_lane_t        lane = (net_lane_t) (size_t) arg;
    net_lane_state_t* l    = &net_lanes[lane];
    for (;;) {
        pthread_mutex_lock(&net_lock);
        while (l->jobs == NULL) {
            pthread_cond_wait(&l->wake, &net_lock);
        }
        net_job_t* j = l->jobs;
        l->jobs      = j->next;
        if (l->jobs == NULL) {
            l->jobs_tail = NULL;
        }
        pthread_mutex_unlock(&net_lock);
        _net_run(lane, j);
        _net_job_free(j);
    }
    return NULL;
}

uint32_t net_http(const net_request_t* req)
{
    static int        curl_ready;
    net_lane_state_t* l = &net_lanes[req->lane];
    pthread_mutex_lock(&net_lock);
    if (l->in_flight >= NET_QUEUE) {
        pthread_mutex_unlock(&net_lock);
        return 0;
    }
    if (!curl_ready) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_ready = 1;
    }
    if (!l->started) {
        pthread_cond_init(&l->wake, NULL);
        if (pthread_create(&l->thread, NULL, _net_thread_main, (void*) (size_t) req->lane) != 0) {
            pthread_mutex_unlock(&net_lock);
            return 0;
        }
        pthread_detach(l->thread);
        l->started = 1;
    }
    pthread_mutex_unlock(&net_lock);

    net_job_t* j = calloc(1, sizeof(*j));
    if (j == NULL) {
        return 0;
    }
    snprintf(j->method, sizeof j->method, "%s", req->method);
    j->url       = strdup(req->url);
    j->timeout_s = req->timeout_s;
    j->ip_family = req->ip_family;
    j->headers   = req->header_count > 0 ? calloc((size_t) req->header_count, sizeof(char*)) : NULL;
    for (int k = 0; k < req->header_count; k++) {
        j->headers[k] = strdup(req->headers[k]);
    }
    j->header_count = req->header_count;
    if (req->body != NULL) {
        j->body = malloc(req->body_len + 1);
        memcpy(j->body, req->body, req->body_len);
        j->body[req->body_len] = '\0';
        j->body_len            = req->body_len;
    }
    pthread_mutex_lock(&net_lock);
    j->id = net_next_id++;
    if (net_next_id == 0) {
        net_next_id = 1;
    }
    l->in_flight++;
    if (l->jobs_tail) {
        l->jobs_tail->next = j;
    } else {
        l->jobs = j;
    }
    l->jobs_tail = j;
    pthread_cond_signal(&l->wake);
    uint32_t id = j->id;
    pthread_mutex_unlock(&net_lock);
    return id;
}

#else

void net_curl_tls(void* handle)
{
    (void) handle;
}


/* built without libcurl: an answer at once, saying so */
uint32_t net_http(const net_request_t* req)
{
    net_lane_state_t* l = &net_lanes[req->lane];
    pthread_mutex_lock(&net_lock);
    if (l->in_flight >= NET_QUEUE) {
        pthread_mutex_unlock(&net_lock);
        return 0;
    }
    uint32_t id = net_next_id++;
    l->in_flight++;
    pthread_mutex_unlock(&net_lock);
    _net_finish(req->lane, id, 0, NULL, 0, "this server was built without libcurl");
    return id;
}

#endif
