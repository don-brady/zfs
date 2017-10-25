/*
 * Lic
 */

#ifndef	_ZT_UTIL_H
#define	_ZT_UTIL_H

#include <sys/fs/zfs.h>
#include <sys/types.h>

#ifdef	__cplusplus
extern "C" {
#endif


typedef struct objset objset_t;


extern void ztu_init(const char *pool);
extern void ztu_fini(void);


extern int ztu_open_objset(const char *name, void **handle, int *count);
extern int ztu_close_objset(void *handle);
extern const char * ztu_object_type(void *handle, uint64_t object);
extern int ztu_objset_name(uint64_t object, char *bufptr, size_t buflen);


typedef void (ztu_for_each_zap_cb_t)(const char *name, uint64_t value,
    uint_t index, void *arg);

extern void ztu_for_each_zap(objset_t *os, uint64_t object,
    ztu_for_each_zap_cb_t *callback, void *arg);


extern boolean_t ztu_object_isdir(void *handle, uint64_t object);


#ifdef	__cplusplus
}
#endif

#endif	/* _ZT_UTIL_H */
