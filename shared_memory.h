/*
 * Created by Ruijie Fang on 2/19/18.
 * Copyright (c) 2018 Ruijie Fang.
 * Open-sourced under the MIT License.
 * Contact: rui.jie.fang [at] gmail.com
 */

#ifndef P2PMD_SHARED_MEMORY_H
#define P2PMD_SHARED_MEMORY_H

#include <semaphore.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <errno.h>
/***************************************************************************************************************\
|*  Theory of single segment producer-consumer using binary semaphores                                         *|
|***************************************************************************************************************|
|*  We define the states of the two semaphores as a 2-tuple (Readable, Writable)                               *|
|*  So we have in total 4 combinations: (0,0), (1,0), (1,1), (0,1)                                             *|
|*  We only use three states and ignore (1,1), which won't happen (presumably...)                              *|
|*                                                                                                             *|
|*  The precondition of a producer write is: (R,W)=(0,1) [Not readable, writable]                              *|
|*      The state during producer write is: (R,W)=(0,0) [Not readable, not writable]                           *|
|*      The postcondition of a producer write is: (R,W)=(1,0) [Not writable, readable]                         *|
|*  The precondition of a consumer read is: (R,W)=(1,0) [Readable, not writable]                               *|
|*      The state during consumer write is: (R,W)=(0,0) [Not readable, not writable]                           *|
|*      The postcondition of a producer write is: (R,W)=(0,1) [Not readable, writable]                         *|
|*  This guarantees the execution sequence of W-R-W-R-...-W-R; the ternary logic shows that                    *|
|*  it is imposible to have a W after a W or an R after an R. The ND-FSM representation of this is:            *|
|*    +------+                     +----+                                                                      *|
|*    |    [[W]] ---- (1,0) ----> [R]   |                                                                      *|
|*  (0,0)___^ ^_______(0,1)_______/ ^__(0,0)                                                                   *|
|*                                                                                                             *|
|*  The deterministic representation using 2-tuples is:                                                        *|
|*    -> (0,1)* -(0,0)-> (1,0)                                                                                 *|
|*       (1,0)  -(0,0)-> (0,1)*                                                                                *|
\***************************************************************************************************************/

/************************************************************\
|* Shared memory layout                                     *|
|************************************************************|
|* 1) unsigned id                                           *|
|* 2) size_t chunk_size                                     *|
|* 3) size_t total                                          *|
|* 4) void*  data                                           *|
\************************************************************/

#define MAX_BYTES 4000000
#define _MODE 0777
static const size_t maxlen = MAX_BYTES - sizeof(unsigned) - sizeof(size_t) * 2 - sizeof(void *);
typedef enum _status {
    Sent = 1, Acked = 2
} status_t;
typedef struct _shared_memory {
    int fd;
    void *data;
    sem_t *r_sem;
    sem_t *w_sem;
    size_t size;
    int id;
    size_t total;
} shared_memory_t;

static inline int
create_shared_memory(shared_memory_t *self, const char *name, const char *write_sem_name, const char *read_sem_name)
{
    /* precondition: shared memory block never existed */
    /* postcondition: (R,W) = (0,1)                    */
    assert(self);
    assert(name);
    assert(write_sem_name);
    assert(read_sem_name);
    /* create producer's semaphore (write_sem) */
    self->w_sem = sem_open(write_sem_name, O_CREAT | O_RDWR, _MODE, 1);
    if (self->w_sem == SEM_FAILED) {
        sem_unlink(write_sem_name);
        return 1;
    }
    /* create consumer's semaphore (read_sem) */
    self->r_sem = sem_open(read_sem_name, O_CREAT | O_RDWR, _MODE, 0);
    if (self->r_sem == SEM_FAILED) {
        sem_unlink(read_sem_name);
        sem_unlink(write_sem_name);
        return 1;
    }

    self->fd = shm_open(name, O_CREAT | O_RDWR, _MODE);
    if (self->fd < 0) {
        sem_unlink(write_sem_name);
        sem_unlink(read_sem_name);
        shm_unlink(name);
        return 1;
    }
    if (ftruncate(self->fd, MAX_BYTES) < 0) {
        if (errno == EINVAL) goto ftruncate_failed_but_einval;
        shm_unlink(name);
        sem_unlink(write_sem_name);
        sem_unlink(read_sem_name);
        return 1;
    }
ftruncate_failed_but_einval:
    self->data = mmap(NULL, MAX_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, self->fd, 0);
    if (self->data == MAP_FAILED) {
        sem_unlink(write_sem_name);
        sem_unlink(read_sem_name);
        shm_unlink(name);
        return 1;
    }
    return 0;
}

static inline int
open_shared_memory(shared_memory_t *self, const char *name, const char *write_sem_name, const char *read_sem_name)
{
    /* precondition: shared memory block exists /\ (R,W) = (0,1)    */
    /* postcondition: fetch the existing shared memory block; does not change (R,W) */
    assert(self);
    assert(name);
    assert(write_sem_name);
    assert(read_sem_name);
    self->w_sem = sem_open(write_sem_name, O_RDWR | O_CREAT, _MODE, 1);
    if (self->w_sem == SEM_FAILED) {
        sem_unlink(write_sem_name);
        return 1;
    }
    self->r_sem = sem_open(read_sem_name, O_RDWR | O_CREAT, _MODE, 0);
    if (self->r_sem == SEM_FAILED) {
        sem_unlink(write_sem_name);
        sem_unlink(read_sem_name);
        return 1;
    }
    self->fd = shm_open(name, O_RDWR | O_CREAT, _MODE);
    if (self->fd < 0) {
        sem_unlink(write_sem_name);
        sem_unlink(read_sem_name);
        shm_unlink(name);
        return 1;
    }
    if (ftruncate(self->fd, MAX_BYTES) < 0) {
        if (errno == EINVAL) goto ftruncate_failed_but_einval1;
        shm_unlink(name);
        sem_unlink(write_sem_name);
        sem_unlink(read_sem_name);
        return 1;
    }
ftruncate_failed_but_einval1:
    self->data = mmap(NULL, MAX_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, self->fd, 0);
    if (self->data == MAP_FAILED) {
        shm_unlink(name);
        sem_unlink(write_sem_name);
        sem_unlink(read_sem_name);
        return 1;
    }
    return 0;
}

static inline int
write_shared_memory(shared_memory_t *self, void *data, size_t len, unsigned id, size_t total)
{
    /* Precondition: (R,W) = (0,1) */
    /* State: (R,W) = (0,0) */
    /* Postcondition: (R,W) = (1,0) */
    assert(self);
    assert(data);
    assert(len > 0 && (len <= MAX_BYTES - sizeof(size_t) * 2 - sizeof(unsigned)));
    /* (R,W) = (0,0) */
    int rsemv, wsemv;
    sem_getvalue(self->r_sem, &rsemv);
    sem_getvalue(self->w_sem, &wsemv);
    sem_wait(self->w_sem);
    /* In process */
    memcpy(self->data, &id, sizeof(unsigned));
    memcpy((self->data + (sizeof(unsigned))), &len, sizeof(size_t));
    memcpy((self->data + (sizeof(unsigned) + sizeof(size_t))), &total, sizeof(size_t));
    memcpy((self->data + (sizeof(unsigned) + sizeof(size_t) + sizeof(size_t))), data, len);
    /* (R,W) = (1,0) */
    sem_post(self->r_sem);
    return 0;
}

static inline int
read_shared_memory(shared_memory_t *self, void **data, size_t *len, size_t *total_chunk, unsigned *id)
{
    /* Precondition: (R,W) = (1,0) */
    /* State: (R,W) = (0,0) */
    /* Postcondition: (R,W) = (0,1) */
    assert(self);
    assert(len);
    assert(total_chunk);
    assert(id);
    /* (R,W) = (0,0) */
    int rsemv, wsemv;
    sem_getvalue(self->r_sem, &rsemv);
    sem_getvalue(self->w_sem, &wsemv);
    sem_wait(self->r_sem);
    /* In process */
    *id = *((unsigned *) self->data);
    *len = *((size_t *) (self->data + (sizeof(unsigned))));
    *total_chunk = *((size_t *) (self->data + (sizeof(unsigned) + sizeof(size_t))));
    if (*len <= 0 || *len > MAX_BYTES) {
        return 1;
    }
    *data = malloc(*len);
    if (*data == NULL) {
        return 1;
    }
    memcpy(*data, (self->data + (sizeof(unsigned) + sizeof(size_t) + sizeof(size_t))), *len);
    /* (R,W) = (0,1) */
    memset(self->data, 0, MAX_BYTES);
    sem_post(self->w_sem);
    return 0;
}

static inline int
send_item(shared_memory_t *self, void *data, size_t len)
{
    assert(self);
    assert(self->data);
    assert(self->r_sem);
    assert(self->w_sem);
    size_t chunks = 1;
    size_t ustart, ulen;
    if (len > maxlen)
        chunks = len / maxlen;
    if (chunks * maxlen < len)
        ++chunks;
    size_t i;
    for (i = 0; i < chunks; ++i) {
        ustart = i * maxlen;
        ulen = chunks == 1 ? len : (i == chunks - 1 ? (len - (maxlen * i)) : maxlen);
        int r = write_shared_memory(self, data + ustart, ulen, (unsigned) i, chunks);
        if (r != 0) {
            return 1;
        }
    }
    return 0;
}

static inline int
recv_item(shared_memory_t *self, void **data, size_t *len)
{
    assert(self);
    assert(self->data);
    assert(self->r_sem);
    assert(self->w_sem);
    size_t total_chunk;
    size_t size_per_chunk;
    size_t per_chunk_size;
    size_t ttotal;
    void *temp;
    unsigned id;
    int rt = read_shared_memory(self, &temp, &size_per_chunk, &total_chunk, &id);
    if (rt != 0) {
        free(temp);
        dzlog_debug("recv failure");
        return 1;
    }
    *data = malloc(total_chunk * size_per_chunk);
    memcpy(*data, temp, size_per_chunk);
    free(temp);
    size_t i;
    for (i = 1; i < total_chunk - 1; ++i) {
        void *_temp;
        rt = read_shared_memory(self, &_temp, &per_chunk_size, &ttotal, &id);
        memcpy(((*data) + i * size_per_chunk), _temp, per_chunk_size);
        free(_temp);
        if (rt != 0) {
            free(*data);
        }
    }
    if (total_chunk > 1) {
        void *_temp;
        rt = read_shared_memory(self, &_temp, &per_chunk_size, &ttotal, &id);
        memcpy(((*data) + i * size_per_chunk), _temp, per_chunk_size);
        if (rt != 0) {
            free(*data);
            return 1;
        }
        if (per_chunk_size != size_per_chunk) {
            *data = realloc(*data, (total_chunk - 1) * size_per_chunk + per_chunk_size);
        }
        *len = (total_chunk - 1) * size_per_chunk + per_chunk_size;
    } else
        *len = total_chunk * size_per_chunk;
    return 0;
}

static inline void
close_shared_memory(const char *name, const char *wsem_name, const char *rsem_name)
{
    assert(name);
    assert(wsem_name);
    assert(rsem_name);
    shm_unlink(name);
    sem_unlink(wsem_name);
    sem_unlink(rsem_name);
}

#endif //P2PMD_SHARED_MEMORY_H
