#pragma once

#include <jasos/types.h>
#include <jasos/config.h>
#include <jasos/status.h>

struct thread;
struct process;

typedef enum object_kind {
    OBJ_DIRECTORY = 1,
    OBJ_PROCESS,
    OBJ_THREAD,
    OBJ_SECTION,
    OBJ_FILE,
    OBJ_DEVICE,
    OBJ_EVENT,
    OBJ_MUTEX,
    OBJ_TIMER,
    OBJ_PIPE,
    OBJ_TYPE_MAX
} object_kind_t;

#define OBJ_PERMANENT      0x1u
#define OBJ_KERNEL_ONLY    0x2u
#define OBJ_DELETE_PENDING 0x4u
#define OBJ_INHERIT        0x8u
#define OBJ_WAITABLE       0x10u

typedef enum disp_type {
    DISP_NOTIFICATION_EVENT = 1,
    DISP_SYNCHRONIZATION_EVENT,
    DISP_MUTANT,
    DISP_TIMER,
    DISP_THREAD,
    DISP_PROCESS
} disp_type_t;

typedef struct wait_block {
    list_t          obj_link;
    list_t          thr_link;
    struct thread  *thread;
    struct dispatcher *object;
    status_t        wake_status;
} wait_block_t;

typedef struct dispatcher {
    disp_type_t type;
    i32         signal_state;
    list_t      wait_list;
    spinlock_t  lock;
} dispatcher_t;

struct object;

typedef void (*obj_delete_fn)(struct object *);
typedef void (*obj_close_fn)(struct object *, access_t);
typedef void (*obj_open_fn)(struct object *, access_t);


typedef struct object_type {
    const char   *name;
    object_kind_t kind;
    usize         body_size;
    access_t      generic_read;
    access_t      generic_write;
    access_t      generic_execute;
    access_t      generic_all;
    obj_delete_fn delete_fn;
    obj_close_fn  close_fn;
    obj_open_fn   open_fn;
    bool          waitable;

} object_type_t;

typedef struct object {
    object_type_t     *type;
    char               name[NAME_MAX];
    struct object     *directory;
    volatile u64       pointer_count;
    volatile u64       handle_count;
    u32                flags;
    kpid_t             owner_pid;
    dispatcher_t      *wait;
    list_t             dir_link;
    u32                dir_hash;
} object_t;

typedef struct directory_object {
    object_t   hdr;
    spinlock_t lock;
    list_t     buckets[37];
    u32        count;
} directory_object_t;

typedef struct event_object {
    object_t     hdr;
    dispatcher_t disp;
    bool         auto_reset;
} event_object_t;

typedef struct mutex_object {
    object_t       hdr;
    dispatcher_t   disp;
    struct thread *owner;
    u32            recursion;
    bool           abandoned;
} mutex_object_t;

typedef struct section_object {
    object_t  hdr;
    u64       size;
    u32       prot;
    u8       *kdata;
} section_object_t;

typedef struct timer_object {
    object_t     hdr;
    dispatcher_t disp;
    u64          due_tick;
    u64          period;
    bool         armed;
    list_t       timer_link;
} timer_object_t;

typedef struct handle_entry {
    object_t *object;
    access_t  access;
    u8        inherit;
    u8        protect_close;
    u32       generation;
} handle_entry_t;

typedef struct handle_table {
    spinlock_t    lock;
    handle_entry_t slots[HANDLE_TABLE_SLOTS];
    u32           used;
} handle_table_t;

void     ob_init(void);
object_t *ob_create(object_type_t *type, const char *name, directory_object_t *dir);
void     ob_reference(object_t *o);
void     ob_dereference(object_t *o);
status_t ob_insert_name(directory_object_t *dir, object_t *o, const char *name);
status_t ob_lookup(const char *path, object_kind_t expect, object_t **out);
directory_object_t *ob_root(void);
directory_object_t *ob_dir_bno(void);
const char *ob_kind_name(object_kind_t k);

object_type_t *ob_type_directory(void);
object_type_t *ob_type_process(void);
object_type_t *ob_type_thread(void);
object_type_t *ob_type_section(void);
object_type_t *ob_type_file(void);
object_type_t *ob_type_device(void);
object_type_t *ob_type_event(void);
object_type_t *ob_type_mutex(void);
object_type_t *ob_type_timer(void);

void     ht_init(handle_table_t *t);
void     ht_destroy(handle_table_t *t);
status_t ht_insert(handle_table_t *t, object_t *o, access_t access, handle_t *out);
status_t ht_lookup(handle_table_t *t, handle_t h, access_t required, object_kind_t expect, object_t **out);
status_t ht_lookup_ex(handle_table_t *t, handle_t h, access_t required, object_kind_t expect, object_t **out, access_t *granted);
status_t ht_close(handle_table_t *t, handle_t h);
status_t ht_duplicate(handle_table_t *src, handle_t h, handle_table_t *dst, access_t access, handle_t *out);
access_t ob_map_generic(object_type_t *type, access_t access);

status_t ob_create_event(const char *name, bool auto_reset, bool initial, event_object_t **out);
status_t ob_create_mutex(const char *name, bool initial_owner, mutex_object_t **out);

status_t pipe_read(object_t *o, void *buf, u64 n, u64 *got);
status_t pipe_write(object_t *o, const void *buf, u64 n, u64 *put);
void     pipe_init_type(void);

void disp_init(dispatcher_t *d, disp_type_t type, i32 state);
void disp_signal(dispatcher_t *d, i32 increment);
void disp_wake_one(dispatcher_t *d, status_t st);
void disp_wake_all(dispatcher_t *d, status_t st);
