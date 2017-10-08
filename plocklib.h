#ifndef PLOCKLIB_H
#define PLOCKLIB_H

/*A synchronization library for palloc.
  See http://gcc.gnu.org/onlinedocs/gcc-4.1.0/gcc/Atomic-Builtins.html

  For Terminus Est, converted to use pthreads locks
*/     

#include <pthread.h>

typedef pthread_mutex_t plocklib_simple_t;

static inline void plocklib_acquire_simple_lock(plocklib_simple_t* lock)
{
     pthread_mutex_lock(lock);
}

static inline void plocklib_release_simple_lock(plocklib_simple_t* lock)
{
     pthread_mutex_unlock(lock);
}

typedef pthread_rwlock_t plocklib_rw_lock;

static inline void plocklib_become_reader(plocklib_rw_lock* lock)
{
     pthread_rwlock_rdlock(lock);
}

static inline void plocklib_resign_as_reader(plocklib_rw_lock* lock)
{
     pthread_rwlock_unlock(lock);
}

/*Upon grant of writership, 1 is returned.  0 returned on denial.
  If request is denied, the requester MUST UNDERSTAND
  that he no longer has even reader access to the locked data.
*/
static inline int plocklib_request_writer_promotion(plocklib_rw_lock* lock)
{
     plocklib_resign_as_reader(lock);
     pthread_rwlock_wrlock(lock);

     return 1;
}

/*Resign ownership of writer lock.*/
static inline void plocklib_resign_as_writer(plocklib_rw_lock* lock)
{
     pthread_rwlock_unlock(lock);
}

#endif
