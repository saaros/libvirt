/*
 * Copyright (C) 2010-2013 Red Hat, Inc.
 * Copyright IBM Corp. 2008
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
 * Authors:
 *     David L. Leskovec <dlesko at linux.vnet.ibm.com>
 *     Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <sys/wait.h>

#include "virnetdevveth.h"
#include "viralloc.h"
#include "virlog.h"
#include "vircommand.h"
#include "virerror.h"
#include "virfile.h"
#include "virstring.h"
#include "virutil.h"

#define VIR_FROM_THIS VIR_FROM_NONE

/* Functions */
/**
 * virNetDevVethCreate:
 * @veth1: pointer to name for parent end of veth pair
 * @veth2: pointer to return name for container end of veth pair
 * @mac: mac address of the device
 *
 * Creates a veth device pair using the ip command:
 * ip link add veth1 type veth peer name veth2
 *
 * If veth1 or veth2 points to NULL on entry, they will be a valid interface
 * on return.  The name is generated based on the mac address given.
 *
 * Returns 0 on success or -1 in case of error
 */
int virNetDevVethCreate(char** veth1, char** veth2, const virMacAddrPtr mac)
{
    const char *argv[] = {
        "ip", "link", "add", NULL, "type", "veth", "peer", "name", NULL, NULL
    };
    bool veth1_alloc = false;
    bool veth2_alloc = false;

    VIR_DEBUG("Host: %s guest: %s", NULLSTR(*veth1), NULLSTR(*veth2));

    if (*veth1 == NULL) {
        /* Use the last three bytes of the mac address as our if name */
        if (virAsprintf(veth1, "veth%02x%02x%02x",
                        mac->addr[3], mac->addr[4], mac->addr[5]) < 0)
            return -1;
        VIR_DEBUG("Assigned host: %s", *veth1);
        veth1_alloc = true;
    }
    argv[3] = *veth1;

    if (*veth2 == NULL) {
        /* Append a 'p' to veth1 if name */
        if (virAsprintf(veth2, "%sp", *veth1) < 0) {
            if (veth1_alloc)
                VIR_FREE(*veth1);
            return -1;
        }
        VIR_DEBUG("Assigned guest: %s", *veth2);
        veth2_alloc = true;
    }
    argv[8] = *veth2;

    VIR_DEBUG("Create Host: %s guest: %s", *veth1, *veth2);
    if (virRun(argv, NULL) < 0) {
        if (veth1_alloc)
            VIR_FREE(*veth1);
        if (veth2_alloc)
            VIR_FREE(*veth2);
        return -1;
    }

    return 0;
}

/**
 * virNetDevVethDelete:
 * @veth: name for one end of veth pair
 *
 * This will delete both veth devices in a pair.  Only one end needs to
 * be specified.  The ip command will identify and delete the other veth
 * device as well.
 * ip link del veth
 *
 * Returns 0 on success or -1 in case of error
 */
int virNetDevVethDelete(const char *veth)
{
    const char *argv[] = {"ip", "link", "del", veth, NULL};

    VIR_DEBUG("veth: %s", veth);

    return virRun(argv, NULL);
}
