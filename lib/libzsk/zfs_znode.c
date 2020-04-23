
struct objset;
struct cred;
struct nvlist;
struct dmu_tx;

typedef struct objset objset_t;
typedef struct cred cred_t;
typedef struct nvlist nvlist_t;
typedef struct dmu_tx dmu_tx_t;

void
zfs_create_fs(objset_t *os, cred_t *cr, nvlist_t *zplprops, dmu_tx_t *tx)
{
}
