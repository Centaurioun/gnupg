/* secmem.c  -	memory allocation from a secure heap
 *	Copyright (c) 1997 by Werner Koch (dd9jn)
 *
 * This file is part of G10.
 *
 * G10 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * G10 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#if defined(HAVE_MLOCK) || defined(HAVE_MMAP)
  #include <unistd.h>
  #include <sys/mman.h>
  #include <sys/types.h>
#endif

#include "types.h"
#include "memory.h"
#include "util.h"


#define DEFAULT_POOLSIZE 8196

typedef struct memblock_struct MEMBLOCK;
struct memblock_struct {
    unsigned size;
    union {
	MEMBLOCK *next;
	long align_dummy;
	char d[1];
    } u;
};



static void  *pool;
static int   pool_okay;
static int   pool_is_mmapped;
static size_t poolsize; /* allocated length */
static size_t poollen;	/* used length */
static MEMBLOCK *unused_blocks;
static unsigned max_alloced;
static unsigned cur_alloced;
static unsigned max_blocks;
static unsigned cur_blocks;


static void
lock_pool( void *p, size_t n )
{
  #ifdef HAVE_MLOCK
    uid_t uid;
    int err;

    err = mlock( p, n );
    if( err && errno )
	err = errno;

    uid = getuid();
    if( uid && !geteuid() ) {
	if( setuid( uid ) )
	    log_fatal("failed to reset uid: %s\n", strerror(errno));
    }

    if( err ) {
	log_error("can�t lock memory: %s\n", strerror(err));
	log_info("Warning: using insecure memory!\n");
    }

  #else
    log_info("Please note that you don�t have secure memory on this system\n");
  #endif
}


static void
init_pool( size_t n)
{
    poolsize = n;

  #if HAVE_MMAP && defined(MAP_ANONYMOUS)
    poolsize = (poolsize + 4095) & ~4095;
    pool = mmap( 0, poolsize, PROT_READ|PROT_WRITE,
			      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if( pool == (void*)-1 )
	log_error("can�t mmap pool of %u bytes: %s - using malloc\n",
			    (unsigned)poolsize, strerror(errno));
    else {
	pool_is_mmapped = 1;
	pool_okay = 1;
    }

  #endif
    if( !pool_okay ) {
	pool = malloc( poolsize );
	if( !pool )
	    log_fatal("can�t allocate memory pool of %u bytes\n",
						       (unsigned)poolsize);
	else
	    pool_okay = 1;
    }
    lock_pool( pool, poolsize );
    poollen = 0;
}


/* concatenate unused blocks */
static void
compress_pool(void)
{

}


void
secmem_init( size_t n )
{
    if( n < DEFAULT_POOLSIZE )
	n = DEFAULT_POOLSIZE;
    if( !pool_okay )
	init_pool(n);
    else
	log_error("Oops, secure memory pool already initialized\n");
}


void *
secmem_malloc( size_t size )
{
    MEMBLOCK *mb, *mb2;
    int compressed=0;

    if( !pool_okay )
	init_pool(DEFAULT_POOLSIZE);

    /* blocks are always a multiple of 32 */
    size += sizeof(MEMBLOCK);
    size = ((size + 31) / 32) * 32;

  retry:
    /* try to get it from the used blocks */
    for(mb = unused_blocks,mb2=NULL; mb; mb2=mb, mb = mb->u.next )
	if( mb->size >= size ) {
	    if( mb2 )
		mb2->u.next = mb->u.next;
	    else
		unused_blocks = mb->u.next;
	    goto leave;
	}
    /* allocate a new block */
    if( (poollen + size <= poolsize) ) {
	mb = pool + poollen;
	poollen += size;
	mb->size = size;
    }
    else if( !compressed ) {
	compressed=1;
	compress_pool();
	goto retry;
    }
    else
	return NULL;

  leave:
    cur_alloced += mb->size;
    cur_blocks++;
    if( cur_alloced > max_alloced )
	max_alloced = cur_alloced;
    if( cur_blocks > max_blocks )
	max_blocks = cur_blocks;
    return &mb->u.d;
}


void
secmem_free( void *a )
{
    MEMBLOCK *mb;
    size_t size;

    if( !a )
	return;

    mb = (MEMBLOCK*)((char*)a - ((size_t) &((MEMBLOCK*)0)->u.d));
    size = mb->size;
    memset(mb, 0xff, size );
    memset(mb, 0xaa, size );
    memset(mb, 0x55, size );
    memset(mb, 0x00, size );
    mb->size = size;
    mb->u.next = unused_blocks;
    unused_blocks = mb;
    cur_blocks--;
    cur_alloced -= size;
}

void
secmem_term()
{
    if( !pool_okay )
	return;

    memset( pool, 0xff, poolsize);
    memset( pool, 0xaa, poolsize);
    memset( pool, 0x55, poolsize);
    memset( pool, 0x00, poolsize);
  #if HAVE_MMAP
    if( pool_is_mmapped )
	munmap( pool, poolsize );
  #endif
    pool = NULL;
    pool_okay = 0;
    poolsize=0;
    poollen=0;
    unused_blocks=NULL;
}


void
secmem_dump_stats()
{
    fprintf(stderr,
		"secmem usage: %u/%u bytes in %u/%u blocks of pool %lu/%lu\n",
		cur_alloced, max_alloced, cur_blocks, max_blocks,
		(ulong)poollen, (ulong)poolsize );
}

