/*
 * storage_backend_btrfs.c: btrfs subvolume / snapshot backed storage
 *
 * Copyright (C) 2007-2012 Red Hat, Inc.
 * Copyright (C) 2013 Oskari Saarenmaa
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Oskari Saarenmaa <os@ohmu.fi>
 */

#include <config.h>

#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage_backend.h"
#include "viralloc.h"
#include "virfile.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_STORAGE

static char *
btrfsPoolVolPath(virStoragePoolObjPtr pool,
                 const char *name)
{
    char *res = NULL;
    int rc = -1;
    if (STREQLEN(name, pool->def->target.path,
                 strlen(pool->def->target.path))) {
        rc = VIR_STRDUP(res, name);
    } else {
        rc = virAsprintf(&res, "%s/%s", pool->def->target.path, name);
    }
    return (rc < 0) ? NULL : res;
}

static int
btrfsPoolCheck(virConnectPtr conn ATTRIBUTE_UNUSED,
               virStoragePoolObjPtr pool,
               bool *isActive)
{
    *isActive = (access(pool->def->target.path, R_OK | X_OK) == 0);
    return 0;
}

struct btrfsVolS {
    int count;
    struct {
        char *name;
        char *uuid;
    } *vols;
};

static int
btrfsPoolFindVols(virStoragePoolObjPtr pool,
                  char **const groups,
                  void *data)
{
    struct btrfsVolS *bvols = data;
    virStorageVolDefPtr vol = NULL;
    int idx, ret = -1;

    if (VIR_ALLOC(vol) < 0)
        goto cleanup;

    if (VIR_STRDUP(vol->name, groups[2]) < 0)
        goto cleanup;

    vol->type = VIR_STORAGE_VOL_DIR;
    if (virAsprintf(&vol->target.path, "%s/%s",
                    pool->def->target.path,
                    vol->name) == -1)
        goto cleanup;

    if (VIR_STRDUP(vol->key, vol->target.path) < 0)
        goto cleanup;

    /* Store uuid and name in case a snapshot targets this volume */
    if (VIR_REALLOC_N(bvols->vols, bvols->count+1) < 0)
        goto cleanup;
    idx = bvols->count++;
    bvols->vols[idx].name = NULL;
    bvols->vols[idx].uuid = NULL;
    if (VIR_STRDUP(bvols->vols[idx].name, groups[2]) < 0 ||
        VIR_STRDUP(bvols->vols[idx].uuid, groups[1]) < 0)
        goto cleanup;

    /* Is this a snapshot? */
    if (STRNEQ(groups[0], "-")) {
        for (idx = 0; idx < bvols->count; idx++) {
            if (STREQ(bvols->vols[idx].uuid, groups[0])) {
                vol->backingStore.path = btrfsPoolVolPath(pool, bvols->vols[idx].name);
                if (vol->backingStore.path == NULL)
                    goto cleanup;
                break;
            }
        }
    }

    if (VIR_REALLOC_N(pool->volumes.objs,
                      pool->volumes.count+1) < 0)
        goto cleanup;

    pool->volumes.objs[pool->volumes.count++] = vol;
    vol = NULL;
    ret = 0;

cleanup:
    virStorageVolDefFree(vol);
    return ret;
}

static int
btrfsPoolRefresh(virConnectPtr conn ATTRIBUTE_UNUSED,
                 virStoragePoolObjPtr pool)
{
    struct statvfs sb;
    struct btrfsVolS bvols;
    int idx, vars[] = {3};
    const char *regexes[] = {
        "^ID [0-9]+ gen [0-9]+ top level [0-9]+ "
           "parent_uuid ([-a-f0-9]+) uuid ([-a-f0-9]+) path (\\S+)$"};
    virCommandPtr cmd = virCommandNewArgList(
        "btrfs", "subvolume", "list", "-uq", pool->def->target.path, NULL);

    bvols.count = 0;
    bvols.vols = NULL;
    if (virStorageBackendRunProgRegex(pool, cmd, 1, regexes, vars,
                                      btrfsPoolFindVols, &bvols, NULL) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to parse btrfs subvolume list"));
        goto cleanup;
    }

    if (statvfs(pool->def->target.path, &sb) < 0) {
        virReportSystemError(errno,
                             _("cannot statvfs path '%s'"),
                             pool->def->target.path);
        goto cleanup;
    }
    pool->def->capacity = ((unsigned long long)sb.f_frsize *
                           (unsigned long long)sb.f_blocks);
    pool->def->available = ((unsigned long long)sb.f_bfree *
                            (unsigned long long)sb.f_frsize);
    pool->def->allocation = pool->def->capacity - pool->def->available;

    return 0;

cleanup:
    virStoragePoolObjClearVols(pool);
    for (idx = 0; idx < bvols.count; idx++) {
        VIR_FREE(bvols.vols[idx].name);
        VIR_FREE(bvols.vols[idx].uuid);
    }
    VIR_FREE(bvols.vols);

    return -1;
}

static int
btrfsPoolVolCreate(virConnectPtr conn ATTRIBUTE_UNUSED,
                   virStoragePoolObjPtr pool,
                   virStorageVolDefPtr vol)
{
    vol->type = VIR_STORAGE_VOL_DIR;

    VIR_FREE(vol->target.path);
    vol->target.path = btrfsPoolVolPath(pool, vol->name);
    if (vol->target.path == NULL)
        return -1;

    if (virFileExists(vol->target.path)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume target path '%s' already exists"),
                       vol->target.path);
        return -1;
    }

    VIR_FREE(vol->key);
    return VIR_STRDUP(vol->key, vol->target.path);
}

static int
btrfsPoolVolBuild(virConnectPtr conn ATTRIBUTE_UNUSED,
                  virStoragePoolObjPtr pool,
                  virStorageVolDefPtr vol,
                  unsigned int flags ATTRIBUTE_UNUSED)
{
    int ret = -1;
    char *target_volume = NULL, *snapshot_source = NULL;
    virCommandPtr cmd = virCommandNew("btrfs");
    struct stat st;

    if (!cmd)
        goto cleanup;

    target_volume = btrfsPoolVolPath(pool, vol->target.path);
    if (target_volume == NULL)
        goto cleanup;

    if (vol->backingStore.path == NULL) {
        virCommandAddArgList(cmd, "subvolume", "create", target_volume, NULL);
    } else {
        int accessRetCode = -1;
        snapshot_source = btrfsPoolVolPath(pool, vol->backingStore.path);
        if (snapshot_source == NULL)
            goto cleanup;

        accessRetCode = access(snapshot_source, R_OK | X_OK);
        if (accessRetCode != 0) {
            virReportSystemError(errno,
                                 _("inaccessible backing store volume %s"),
                                 vol->backingStore.path);
            goto cleanup;
        }

        virCommandAddArgList(cmd, "subvolume", "snapshot", snapshot_source,
                             target_volume, NULL);
    }

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;
    if (stat(target_volume, &st) < 0) {
        virReportSystemError(errno,
                             _("failed to create %s"), vol->target.path);
        goto cleanup;
    }
    ret = 0;

cleanup:
    VIR_FREE(target_volume);
    VIR_FREE(snapshot_source);
    virCommandFree(cmd);
    return ret;
}

static int
btrfsPoolVolDelete(virConnectPtr conn ATTRIBUTE_UNUSED,
                   virStoragePoolObjPtr pool,
                   virStorageVolDefPtr vol,
                   unsigned int flags ATTRIBUTE_UNUSED)
{
    int ret = -1;
    char *target_volume = btrfsPoolVolPath(pool, vol->target.path);
    virCommandPtr cmd = virCommandNewArgList("btrfs", "subvolume",
                                             "delete", target_volume, NULL);
    if (target_volume == NULL || cmd == NULL)
        goto cleanup;
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;
    ret = 0;

cleanup:
    VIR_FREE(target_volume);
    virCommandFree(cmd);
    return ret;
}

virStorageBackend virStorageBackendBtrfs = {
    .type = VIR_STORAGE_POOL_BTRFS,

    .checkPool = btrfsPoolCheck,
    .refreshPool = btrfsPoolRefresh,

    .buildVol = btrfsPoolVolBuild,
    .createVol = btrfsPoolVolCreate,
    .deleteVol = btrfsPoolVolDelete,
};
