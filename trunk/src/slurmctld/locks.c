/*****************************************************************************\
 *  locks.c - semaphore functions for slurmctld
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Moe Jette <jette@llnl.gov>, Randy Sanchez <rsancez@llnl.gov>
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>

#include <src/slurmctld/locks.h>
#include <src/slurmctld/slurmctld.h>

pthread_mutex_t locks_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t locks_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
slurmctld_lock_flags_t slurmctld_locks;
int kill_thread = 0;

void wr_rdlock (lock_datatype_t datatype);
void wr_rdunlock (lock_datatype_t datatype);
void wr_wrlock (lock_datatype_t datatype);
void wr_wrunlock (lock_datatype_t datatype);

/* init_locks - create locks used for slurmctld data structure access control */
void
init_locks ( void )
{
	/* just clear all semaphores */
	memset ((void *)&slurmctld_locks, 0, sizeof (slurmctld_locks) );
}

/* lock_slurmctld - Issue the required lock requests in a well defined order
 * Returns 0 on success, -1 on failure */
void 
lock_slurmctld (slurmctld_lock_t lock_levels)
{
	if (lock_levels.config == READ_LOCK)
		wr_rdlock (CONFIG_LOCK);
	else if (lock_levels.config == WRITE_LOCK)
		wr_wrlock (CONFIG_LOCK);

	if (lock_levels.job == READ_LOCK)
		wr_rdlock (JOB_LOCK);
	else if (lock_levels.job == WRITE_LOCK)
		wr_wrlock (JOB_LOCK);

	if (lock_levels.node == READ_LOCK)
		wr_rdlock (NODE_LOCK);
	else if (lock_levels.node == WRITE_LOCK)
		wr_wrlock (NODE_LOCK);

	if (lock_levels.partition == READ_LOCK)
		wr_rdlock (PART_LOCK);
	else if (lock_levels.partition == WRITE_LOCK)
		wr_wrlock (PART_LOCK);
}

/* unlock_slurmctld - Issue the required unlock requests in a well defined order */
void 
unlock_slurmctld (slurmctld_lock_t lock_levels)
{
	if (lock_levels.partition == READ_LOCK)
		wr_rdunlock (PART_LOCK);
	else if (lock_levels.partition == WRITE_LOCK)
		wr_wrunlock (PART_LOCK);

	if (lock_levels.node == READ_LOCK)
		wr_rdunlock (NODE_LOCK);
	else if (lock_levels.node == WRITE_LOCK)
		wr_wrunlock (NODE_LOCK);

	if (lock_levels.job == READ_LOCK)
		wr_rdunlock (JOB_LOCK);
	else if (lock_levels.job == WRITE_LOCK)
		wr_wrunlock (JOB_LOCK);

	if (lock_levels.config == READ_LOCK)
		wr_rdunlock (CONFIG_LOCK);
	else if (lock_levels.config == WRITE_LOCK)
		wr_wrunlock (CONFIG_LOCK);
}

/* wr_rdlock - Issue a read lock on the specified data type */
void 
wr_rdlock (lock_datatype_t datatype)
{
	pthread_mutex_lock (&locks_mutex);
	while (1) {
		if ((slurmctld_locks.entity [write_wait_lock (datatype)] == 0) &&
		    (slurmctld_locks.entity [write_lock (datatype)] == 0)) {
			slurmctld_locks.entity [read_lock (datatype)]++;
			break;
		} 
		else {	/* wait for state change and retry */
			pthread_cond_wait (&locks_cond, &locks_mutex);
			if (kill_thread)
				pthread_exit (NULL);
		}
	}
	pthread_mutex_unlock (&locks_mutex);
}

/* wr_rdunlock - Issue a read unlock on the specified data type */
void
wr_rdunlock (lock_datatype_t datatype)
{
	pthread_mutex_lock (&locks_mutex);
	slurmctld_locks.entity [read_lock (datatype)]--;
	pthread_mutex_unlock (&locks_mutex);
	pthread_cond_broadcast (&locks_cond);
}

/* wr_wrlock - Issue a write lock on the specified data type */
void
wr_wrlock (lock_datatype_t datatype)
{
	pthread_mutex_lock (&locks_mutex);
	slurmctld_locks.entity [write_wait_lock (datatype)]++;

	while (1) {
		if ((slurmctld_locks.entity [read_lock (datatype)] == 0) &&
		    (slurmctld_locks.entity [write_lock (datatype)] == 0)) {
			slurmctld_locks.entity [write_lock (datatype)]++;
			slurmctld_locks.entity [write_wait_lock (datatype)]--;
			break;
		} 
		else {	/* wait for state change and retry */
			pthread_cond_wait (&locks_cond, &locks_mutex);
			if (kill_thread)
				pthread_exit (NULL);
		}
	}
	pthread_mutex_unlock (&locks_mutex);
}

/* wr_wrunlock - Issue a write unlock on the specified data type */
void
wr_wrunlock (lock_datatype_t datatype)
{
	pthread_mutex_lock (&locks_mutex);
	slurmctld_locks.entity [write_lock (datatype)]--;
	pthread_mutex_unlock (&locks_mutex);
	pthread_cond_broadcast (&locks_cond);
}

/* get_lock_values - Get the current value of all locks */
void
get_lock_values (slurmctld_lock_flags_t *lock_flags)
{
	if (lock_flags == NULL)
		fatal ("get_lock_values passed null pointer");

	memcpy ((void *)lock_flags, (void *) &slurmctld_locks, sizeof (slurmctld_locks) );
}

/* kill_locked_threads - Kill all threads waiting on semaphores */
void
kill_locked_threads ( void )
{
	kill_thread = 1;
	pthread_cond_broadcast (&locks_cond);
}

/* locks used for saving state of slurmctld */
void
lock_state_files ( void )
{
	pthread_mutex_lock (&state_mutex);
}
void
unlock_state_files ( void )
{
	pthread_mutex_unlock (&state_mutex);
}

