/*
 * TALPA test program
 *
 * Copyright (C) 2004-2011 Sophos Limited, Oxford, England.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License Version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if not,
 * write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>

#include "../include/talpa-vettingclient.h"
#include "../src/ifaces/intercept_filters/eintercept_action.h"
#include "../clients/vc.h"


int main(int argc, char *argv[])
{
    char *tmpdir;
    int talpa;
    int rc;
    struct TalpaPacket_VettingDetails* details;
    int status;


    if ( argc == 2 )
    {
        tmpdir = argv[1];
    }
    else
    {
        fprintf(stderr, "Bad usage!\n");
        return 1;
    }

    if ( (talpa = vc_init(0, 2000)) < 0 )
    {
        fprintf(stderr, "Failed to initialize!\n");
        return -1;
    }

    rc = fork();

    if ( !rc )
    {
        if ( execle("./chkext2fops", "./chkext2fops",  tmpdir, "testfile",  "512",  "128", NULL, NULL) )
        {
            fprintf(stderr, "Failed to exec (%d)!\n", errno);
            return -1;
        }
    }
    else if ( rc < 0 )
    {
        fprintf(stderr, "Fork failed!\n");
        return -1;
    }

    details = vc_get(talpa);

    if ( details )
    {
        if ( details->header.payloadLength != (sizeof(struct TalpaPacket_VettingDetails) - sizeof(struct TalpaProtocolHeader) + sizeof(struct TalpaPacketFragment_FileDetails)) )
        {
            fprintf(stderr, "Size error (%u %u)!\n", details->header.payloadLength, (unsigned int) (sizeof(struct TalpaPacket_VettingDetails) - sizeof(struct TalpaProtocolHeader) + sizeof(struct TalpaPacketFragment_FileDetails)));
            vc_exit(talpa);
            wait(NULL);
            return -1;
        }

        if ( vc_respond(talpa, details, TALPA_ALLOW) < 0 )
        {
            fprintf(stderr, "Respond error!\n");
            vc_exit(talpa);
            wait(NULL);
            return -1;
        }
    }

    wait(&status);

    if ( !WIFEXITED(status) )
    {
        fprintf(stderr, "Child error!\n");
        vc_exit(talpa);
        return -1;
    }

    if ( WEXITSTATUS(status) )
    {
        if ( WEXITSTATUS(status) != (10+ENODATA) )
        {
            fprintf(stderr, "Child failed!\n");
            vc_exit(talpa);
            return -1;
        }
    }

    vc_exit(talpa);

    return 0;
}

